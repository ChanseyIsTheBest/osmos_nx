/* osmos_jni.c -- a small, complete JNI environment for libosmos.so.
 *
 * MIT licensed. See LICENSE.
 *
 * The JNIEnv vtable layout below is the standard JNINativeInterface ordering.
 * Slot numbers were checked against the binary rather than assumed: the
 * disassembly of jStringToCStr does
 *
 *     ldr x8, [x0]          ; env->functions
 *     ldr x8, [x8, #0x548]  ; 0x548 / 8 = 169
 *     blr x8
 *
 * and index 169 is GetStringUTFChars, which is exactly what that function is
 * for. Any slot the engine reaches that we have not filled traps with its own
 * index in the message, so an unexpected call names itself instead of jumping
 * to zero.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#include "config.h"
#include "osmos_jni.h"
#include "osmos_paths.h"
#include "osmos_save.h"

volatile int jni_quit_requested = 0;

/* Every engine -> Java call is traced, capped so a call made once per frame
 * cannot fill the SD card. The outbound surface is ten methods, so anything
 * appearing in the trace that is not one of them is a real discovery. */
static int jni_traced;
#define JNI_TRACE_CAP (OSMOS_DIAG ? 120 : 0)

/* ------------------------------------------------------------------ */
/* Object model                                                        */
/* ------------------------------------------------------------------ */

/* Everything the engine holds is one of these. `tag` lets a jobject be
 * identified without a real type system. */
typedef enum { OBJ_CLASS, OBJ_STRING, OBJ_BYTES, OBJ_PLAIN } ObjTag;

typedef struct {
  ObjTag tag;
  char  *name;      /* OBJ_CLASS: the JNI class name */
  char  *utf;       /* OBJ_STRING: NUL-terminated UTF-8 */
  void  *bytes;     /* OBJ_BYTES */
  int    len;
} Obj;

/* A method or field id. The engine only ever compares the pointer, so the
 * struct exists to carry the (class, name, signature) triple to the dispatcher. */
typedef struct {
  const char *cls;
  char name[64];
  char sig[96];
} MethodID;

#define MAX_OBJS 512
#define MAX_IDS  128

static Obj      *objs[MAX_OBJS];
static int       nobjs;
static MethodID  ids[MAX_IDS];
static int       nids;

static Obj *obj_new(ObjTag tag) {
  Obj *o = calloc(1, sizeof(Obj));
  if (!o) return NULL;
  o->tag = tag;
  if (nobjs < MAX_OBJS) objs[nobjs++] = o;
  return o;
}

/* Classes are interned so FindClass("...") twice returns the same jclass and
 * a pointer comparison in the engine behaves. */
static Obj *intern_class(const char *name) {
  for (int i = 0; i < nobjs; i++)
    if (objs[i]->tag == OBJ_CLASS && !strcmp(objs[i]->name, name))
      return objs[i];
  Obj *o = obj_new(OBJ_CLASS);
  if (o) o->name = strdup(name);
  return o;
}

static const char *class_name_of(void *cls) {
  Obj *o = cls;
  return (o && o->tag == OBJ_CLASS && o->name) ? o->name : "?";
}

void *jni_new_string(const char *utf8) {
  Obj *o = obj_new(OBJ_STRING);
  if (o) o->utf = strdup(utf8 ? utf8 : "");
  return o;
}

void *jni_new_bytearray(const void *data, int len) {
  Obj *o = obj_new(OBJ_BYTES);
  if (!o) return NULL;
  o->len = len;
  o->bytes = malloc(len > 0 ? (size_t)len : 1);
  if (o->bytes && data && len > 0) memcpy(o->bytes, data, (size_t)len);
  return o;
}

void *jni_bytearray_data(void *arr, int *len_out) {
  Obj *o = arr;
  if (!o || o->tag != OBJ_BYTES) { if (len_out) *len_out = 0; return NULL; }
  if (len_out) *len_out = o->len;
  return o->bytes;
}

static const char *string_utf(void *s) {
  Obj *o = s;
  return (o && o->tag == OBJ_STRING && o->utf) ? o->utf : "";
}

/* ------------------------------------------------------------------ */
/* The ten calls that go back into "Java"                              */
/* ------------------------------------------------------------------ */

/* Signature-directed argument capture. Reading a va_list positionally without
 * walking it in signature order is undefined on aarch64, where integer and
 * floating arguments come from different register files -- so walk it once,
 * guided by the signature, exactly as the reference ports do. */
typedef struct {
  int   count;
  void *obj[4];
  int32_t i32[4];
} Args;

static void marshal(const char *sig, va_list va, Args *out) {
  memset(out, 0, sizeof(*out));
  if (!sig) return;
  const char *p = strchr(sig, '(');
  if (!p) return;
  p++;
  while (*p && *p != ')' && out->count < 4) {
    int k = out->count;
    switch (*p) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        out->i32[k] = va_arg(va, int32_t); p++; break;
      case 'J':  (void)va_arg(va, int64_t); p++; break;
      case 'F':  (void)va_arg(va, double);  p++; break;  /* promoted */
      case 'D':  (void)va_arg(va, double);  p++; break;
      case '[':
        out->obj[k] = va_arg(va, void *);
        p++;
        if (*p == 'L') {
          while (*p && *p != ';') p++;
          if (*p) p++;
        } else if (*p) {
          p++;
        }
        break;
      case 'L':
        out->obj[k] = va_arg(va, void *);
        while (*p && *p != ';') p++;
        if (*p) p++;
        break;
      default: p++; continue;
    }
    out->count++;
  }
}

/* Toast text arrives as a Java String built from the engine's wchar_t. It is
 * the engine's own user-facing message, so surface it in the log and, when
 * enabled, on screen. */
static void do_toast(const char *text) {
  LOGI("[toast] %s", text ? text : "");
  overlay_note(text);
}

static int64_t dispatch(const char *cls, const char *name, const char *sig,
                        const Args *a, int *handled) {
  *handled = 1;

  /* --- CloudSaveHelper -------------------------------------------------
   * There is no Play Games cloud here. Back it with a local file so that
   * the engine's own merge/restore paths still work coherently, which is
   * cheaper than teaching the engine there is no cloud. */
  if (strstr(cls, "CloudSaveHelper")) {
    if (!strcmp(name, "getGameData")) {
      /* Returning NULL when there is no save is REQUIRED, not tidiness.
       *
       * Cloud_GetData(int&) does:
       *     CallStaticObjectMethod(...)     ; x0 = the byte[]
       *     cbz x0, <return NULL>           ; null  -> "no cloud data"
       *     GetArrayLength -> w22
       *     operator new[](w22); memcpy; str w22, [x19]
       *
       * A zero-length array is NOT null: operator new[](0) returns a valid
       * pointer, so the engine would come back with a non-null buffer and a
       * length of 0 -- "cloud data exists and is empty". Against a game that
       * merges cloud progress with local progress, that is the shape of a bug
       * that quietly erases a save rather than one that crashes. */
      int len = 0;
      void *buf = osmos_save_read_cloud(&len);
      if (!buf || len <= 0) {
        free(buf);
        return 0;                         /* Java null */
      }
      void *arr = jni_new_bytearray(buf, len);
      free(buf);
      return (int64_t)(intptr_t)arr;
    }
    if (!strcmp(name, "saveGameData")) {
      int len = 0;
      void *data = jni_bytearray_data(a->obj[0], &len);
      osmos_save_write_cloud(data, len);
      return 0;
    }
    /* NOTE: sync()V is on PlayGames, not here. This class has exactly the
     * two methods above. */
  }

  /* --- Osmos (the Application class) ---------------------------------- */
  if (!strcmp(cls, "com/hemispheregames/osmos/Osmos")) {
    if (!strcmp(name, "toastLauncher"))  { do_toast(string_utf(a->obj[0])); return 0; }
    if (!strcmp(name, "browserIntentLauncher")) {
      /* No browser. Show the URL so it can be typed on another device --
       * silently dropping it would make the credits and support links look
       * broken rather than unavailable. */
      LOGI("[url] %s", string_utf(a->obj[0]));
      overlay_note(string_utf(a->obj[0]));
      return 0;
    }
    if (!strcmp(name, "emailIntentLauncher")) {
      overlay_note("Support email is not available on this platform.");
      return 0;
    }
    if (!strcmp(name, "resetProgress")) {
      /* On Android this killed and restarted the process. Delete the save and
       * ask the loop to exit; the next launch comes up clean. */
      osmos_save_reset();
      jni_quit_requested = 1;
      return 0;
    }
  }

  /* --- GameActivity --------------------------------------------------- */
  if (strstr(cls, "GameActivity") && !strcmp(name, "promptUpdate")) {
    /* "A newer version is required." There is no store to send anyone to. */
    LOGW("engine asked to prompt for an update; ignoring");
    return 0;
  }

  /* --- InAppPurchaseHelper -------------------------------------------- */
  if (strstr(cls, "InAppPurchaseHelper") && !strcmp(name, "purchaseLightMode")) {
    /* Play billing does not exist here, so the engine can neither be sold
     * anything nor be told what is owned. config.txt decides; see the
     * light_mode entry there and cfg_light_mode_owned() in main.c. */
    LOGI("light mode purchase requested; set light_mode = owned in config.txt");
    overlay_note("Set 'light_mode = owned' in config.txt if you bought this.");
    return 0;
  }

  /* --- AchievementHelper ---------------------------------------------- */
  if (strstr(cls, "AchievementHelper")) {
    if (!strcmp(name, "updateAchievement")) {
      osmos_save_record_achievement(a->i32[0]);
      return 0;
    }
    if (!strcmp(name, "showAchievementScreen")) {
      overlay_note("Achievements are recorded locally in achievements.txt.");
      return 0;
    }
  }

  /* --- PlayGames -------------------------------------------------------
   * The wrapper class ExternalGameService_Init(), _DoLogin() and
   * _IsPlayerLoggedIn() talk to. There is no Play Games here, so the honest
   * answer to every query is "not signed in" -- which is a state the game
   * already handles, since it runs offline on Android too.
   *
   * Returning 0 for isPlayerLoggedIn/isLoginInProcess matters: the engine
   * polls these from ExternalGameService_Tick(), and answering "login in
   * progress" would leave it waiting for a callback that can never arrive. */
  if (strstr(cls, "/PlayGames")) {
    /* The three real methods, taken from the binary rather than guessed:
     *   sync()V            <- ExternalGameService_Init
     *   signIn()V          <- ExternalGameService_DoLogin
     *   isAuthenticated()Z <- ExternalGameService_IsPlayerLoggedIn
     * An earlier version invented isPlayerLoggedIn/isLoginInProcess, which
     * simply never matched.
     *
     * isAuthenticated returning false is the whole point: the engine then
     * takes its offline path, which it has to support on Android anyway. It
     * cannot deadlock waiting for a sign-in callback either -- the three
     * polling functions are compiled out to constants in this build
     * (IsLoginInProcess and IsLoginCompleteAndNil both `mov w0, wzr; ret`,
     * and ExternalGameService_Tick is a bare `ret`). */
    /* Truthfully false: there is no Play Games here, and the game ships to
     * devices without it. Overridable from config.txt only so the claim that
     * this cannot affect anything but a button icon is testable. */
    if (!strcmp(name, "isAuthenticated")) return 0;   /* never signed in */
    if (!strcmp(name, "signIn"))          return 0;   /* no account to sign in */
    if (!strcmp(name, "sync"))            return 0;   /* nothing to sync to */
    return 0;
  }

  /* --- InAppReviewHelper ---------------------------------------------- */
  if (strstr(cls, "InAppReviewHelper")) {
    /* NativeRateApp() -> requestReview()V. There is no store to rate in. */
    if (!strcmp(name, "requestReview")) return 0;
    return 0;
  }

  *handled = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/* JNIEnv                                                              */
/* ------------------------------------------------------------------ */

static void *env_table[256];
static void *env_ptr = env_table;      /* JNIEnv is a pointer to the table */
static void *vm_table[16];
static void *vm_ptr = vm_table;

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  if (jni_traced < JNI_TRACE_CAP) LOGI("jni: FindClass(%s)", name ? name : "?");
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  Obj *o = obj;
  if (o && o->tag == OBJ_STRING) return intern_class("java/lang/String");
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  const char *cn = class_name_of(cls);
  for (int i = 0; i < nids; i++)
    if (ids[i].cls == cn && !strcmp(ids[i].name, name) && !strcmp(ids[i].sig, sig))
      return &ids[i];
  if (nids >= MAX_IDS) return &ids[0];
  MethodID *m = &ids[nids++];
  m->cls = cn;
  snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
  snprintf(m->sig,  sizeof(m->sig),  "%s", sig  ? sig  : "");
  return m;
}

static int64_t call_v(void *mid, va_list va) {
  MethodID *m = mid;
  if (!m) return 0;
  Args a; marshal(m->sig, va, &a);

  const int traced = (jni_traced < JNI_TRACE_CAP);
  if (traced) {
    jni_traced++;
    LOGI("jni -> %s.%s%s", m->cls, m->name, m->sig);
    if (jni_traced == JNI_TRACE_CAP)
      LOGI("jni: trace cap reached; further calls not logged");
  }

  int handled = 0;
  const int64_t r = dispatch(m->cls, m->name, m->sig, &a, &handled);
  if (!handled)
    LOGW("unhandled JNI call %s.%s%s -- returning 0", m->cls, m->name, m->sig);

  /* Paired with the "jni ->" line above. Without a return marker, a log that
   * ends on a call cannot distinguish "stuck inside this call" from "this call
   * finished and the next thing hung", which is exactly the ambiguity that
   * made PlayGames.sync look like the culprit. */
  if (traced) LOGI("jni <- %s.%s returned %lld", m->cls, m->name, (long long)r);
  return r;
}

static void j_CallStaticVoidMethod(void *env, void *cls, void *mid, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, mid); call_v(mid, va); va_end(va);
}
static void j_CallStaticVoidMethodV(void *env, void *cls, void *mid, va_list va) {
  (void)env; (void)cls; call_v(mid, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, void *mid, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, mid);
  int64_t r = call_v(mid, va);
  va_end(va);
  return (void *)(intptr_t)r;
}
static void *j_CallStaticObjectMethodV(void *env, void *cls, void *mid, va_list va) {
  (void)env; (void)cls; return (void *)(intptr_t)call_v(mid, va);
}
static uint8_t j_CallStaticBooleanMethod(void *env, void *cls, void *mid, ...) {
  (void)env; (void)cls;
  va_list va; va_start(va, mid);
  int64_t r = call_v(mid, va);
  va_end(va);
  return r ? 1 : 0;
}
static uint8_t j_CallStaticBooleanMethodV(void *env, void *cls, void *mid, va_list va) {
  (void)env; (void)cls; return call_v(mid, va) ? 1 : 0;
}

static const char *j_GetStringUTFChars(void *env, void *str, uint8_t *copy) {
  (void)env; if (copy) *copy = 0;
  return string_utf(str);
}
static void j_ReleaseStringUTFChars(void *env, void *str, const char *c) {
  (void)env; (void)str; (void)c;      /* the buffer belongs to the Obj */
}
static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env; return jni_new_string(utf);
}
static int32_t j_GetStringUTFLength(void *env, void *str) {
  (void)env; return (int32_t)strlen(string_utf(str));
}
static int32_t j_GetStringLength(void *env, void *str) {
  (void)env; return (int32_t)strlen(string_utf(str));
}

static int32_t j_GetArrayLength(void *env, void *arr) {
  (void)env; int n = 0; jni_bytearray_data(arr, &n); return n;
}
static void *j_NewByteArray(void *env, int32_t len) {
  (void)env; return jni_new_bytearray(NULL, len);
}
static void *j_GetByteArrayElements(void *env, void *arr, uint8_t *copy) {
  (void)env; if (copy) *copy = 0;
  return jni_bytearray_data(arr, NULL);
}
static void j_ReleaseByteArrayElements(void *env, void *arr, void *elems, int32_t mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetByteArrayRegion(void *env, void *arr, int32_t start, int32_t len, void *buf) {
  (void)env;
  int n = 0; unsigned char *d = jni_bytearray_data(arr, &n);
  if (!d || !buf || start < 0 || len < 0 || start + len > n) return;
  memcpy(buf, d + start, (size_t)len);
}
static void j_SetByteArrayRegion(void *env, void *arr, int32_t start, int32_t len, const void *buf) {
  (void)env;
  int n = 0; unsigned char *d = jni_bytearray_data(arr, &n);
  if (!d || !buf || start < 0 || len < 0 || start + len > n) return;
  memcpy(d + start, buf, (size_t)len);
}

static void *j_NewGlobalRef(void *env, void *o)  { (void)env; return o; }
static void  j_DeleteRef(void *env, void *o)     { (void)env; (void)o; }
static int32_t j_EnsureLocalCapacity(void *env, int32_t n) { (void)env; (void)n; return 0; }
static uint8_t j_ExceptionCheck(void *env)       { (void)env; return 0; }
static void  j_ExceptionClear(void *env)         { (void)env; }
static void *j_ExceptionOccurred(void *env)      { (void)env; return NULL; }
static int32_t j_GetVersion(void *env)           { (void)env; return 0x00010006; }

/* Any slot the engine reaches that we did not fill lands here. Filling the
 * whole table with this is the difference between a named diagnostic and a
 * branch to address zero. */
static void j_trap(void) {
  LOGE("libosmos.so called an unimplemented JNIEnv slot");
  abort();
}

/* JavaVM (JNIInvokeInterface). The engine reaches this through alcGetJavaVM
 * and every outbound bridge function starts by attaching the current thread:
 *
 *     bl   alcGetJavaVM
 *     ldr  x8, [x0]           ; vm->functions
 *     mov  x1, sp             ; &env
 *     mov  x2, xzr            ; args = NULL
 *     ldr  x8, [x8, #0x20]    ; slot 4 = AttachCurrentThread
 *     blr  x8
 *
 * and several finish with slot 5 = DetachCurrentThread, called with x0 only.
 *
 * The slot ORDER matters and is easy to get wrong, because it is not the same
 * shape as JNIEnv:
 *     3 DestroyJavaVM  4 AttachCurrentThread  5 DetachCurrentThread
 *     6 GetEnv         7 AttachCurrentThreadAsDaemon
 *
 * An earlier version had GetEnv in slot 4 and AttachCurrentThread in slot 5.
 * Slot 4 survived by coincidence -- GetEnv(vm, out, ver) and
 * AttachCurrentThread(vm, &env, NULL) agree on the first two arguments. Slot 5
 * did not: DetachCurrentThread is called with x0 alone, so the misplaced
 * AttachCurrentThread wrote `&env_ptr` through whatever value happened to be
 * left in x1. A wild eight-byte store on every Cloud_GetData, Cloud_SaveData,
 * LaunchURLInBrowser, LaunchSupportEmail and ShowUpdateRequiredAlert. */
static int32_t vm_AttachCurrentThread(void *vm, void **p_env, void *args) {
  (void)vm; (void)args;
  if (p_env) *p_env = &env_ptr;
  return 0;                                    /* JNI_OK */
}
static int32_t vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}
static int32_t vm_GetEnv(void *vm, void **p_env, int32_t ver) {
  (void)vm; (void)ver;
  if (p_env) *p_env = &env_ptr;
  return 0;
}
static int32_t vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }

void jni_init(void) {
  for (size_t i = 0; i < sizeof(env_table) / sizeof(*env_table); i++)
    env_table[i] = (void *)j_trap;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[17]  = (void *)j_ExceptionClear;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteRef;         /* DeleteGlobalRef */
  env_table[23]  = (void *)j_DeleteRef;         /* DeleteLocalRef  */
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[113] = (void *)j_GetMethodID;       /* GetStaticMethodID */
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  /* 118, not 117. libosmos.so never calls the variadic form directly: it goes
   * through its own inline _JNIEnv::CallStaticBooleanMethod wrapper, which
   * forwards to the ...V slot. Same for Object (115) and Void (142), both of
   * which were already right -- this one was the odd man out and would have
   * hit the trap the first time ExternalGameService_IsPlayerLoggedIn ran. */
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[163] = (void *)j_NewStringUTF;      /* NewString */
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars; /* confirmed: offset 0x548 */
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[176] = (void *)j_NewByteArray;
  env_table[184] = (void *)j_GetByteArrayElements;
  env_table[192] = (void *)j_ReleaseByteArrayElements;
  env_table[200] = (void *)j_GetByteArrayRegion;
  env_table[208] = (void *)j_SetByteArrayRegion;
  env_table[228] = (void *)j_ExceptionCheck;

  for (size_t i = 0; i < sizeof(vm_table) / sizeof(*vm_table); i++)
    vm_table[i] = (void *)j_trap;
  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;        /* engine uses this */
  vm_table[5] = (void *)vm_DetachCurrentThread;        /* engine uses this */
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;        /* ...AsDaemon */

  intern_class("com/hemispheregames/osmos/wrappers/OsmosJNILib");
}

void *jni_env(void) { return &env_ptr; }
void *jni_vm(void)  { return &vm_ptr;  }

void *jni_osmos_class(void) {
  return intern_class("com/hemispheregames/osmos/wrappers/OsmosJNILib");
}
