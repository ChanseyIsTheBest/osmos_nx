/* osmos_jni.h -- the JNI environment libosmos.so expects.
 *
 * MIT licensed. See LICENSE.
 *
 * WHY THIS IS NOT jni_fake.c
 * --------------------------
 * The Sonic Jump and Killer Bean wrappers carry a ~75-180 KB jni_fake.c plus a
 * 38 KB jni_unimpl.h, because a Unity game reaches into Java for dozens of
 * platform classes and you cannot enumerate them ahead of time.
 *
 * Osmos can be enumerated, and it was. Cross-referencing every Java class
 * string in .text against its call sites gives the complete outbound surface:
 *
 *   Cloud_GetData(int&)                     -> CloudSaveHelper.getGameData()[B
 *   Cloud_SaveData(uchar*,int)              -> CloudSaveHelper.saveGameData([B)V
 *                                              CloudSaveHelper.sync()V
 *   LaunchToast(const wchar_t*)             -> Osmos.toastLauncher(String)V
 *   LaunchSupportEmail()                    -> Osmos.emailIntentLauncher()V
 *   LaunchURLInBrowser(const wchar_t*)      -> Osmos.browserIntentLauncher(String)V
 *   LaunchResetProgress()                   -> Osmos.resetProgress()V
 *   ShowUpdateRequiredAlert()               -> GameActivity.promptUpdate()V
 *   LaunchPurchaseLightMode()               -> InAppPurchaseHelper.purchaseLightMode()V
 *   ExternalGameService_UnlockAchievement   -> AchievementHelper.updateAchievement(I)V
 *   ExternalGameService_ShowAchievementScreen
 *                                           -> AchievementHelper.showAchievementScreen()V
 *
 * Ten methods over five classes. Every signature in the binary is one of
 * ()V, ()Z, ()[B, (I)V, ([B)V, (Ljava/lang/String;)V -- that is the complete
 * set of JNI signature strings present in .rodata.
 *
 * So this is a real, small JNI environment rather than a compatibility net,
 * and adapting jni_fake.c would have meant untangling it from android_native.h,
 * unity_stubs.h and the Unity input plumbing for no benefit.
 */

#ifndef OSMOS_JNI_H
#define OSMOS_JNI_H

#include <stdint.h>

/* Set when the engine asks to finish (reset progress, update required). */
extern volatile int jni_quit_requested;

void  jni_init(void);

void *jni_env(void);          /* JNIEnv *  */
void *jni_vm(void);           /* JavaVM *  -- alcGetJavaVM hands this back */

/* The jclass passed as argument 2 of every static native call. The engine
 * never inspects it, but it must be a valid pooled object so that anything
 * doing GetObjectClass on it does not fault. */
void *jni_osmos_class(void);

/* Build a jstring the engine can read with GetStringUTFChars. */
void *jni_new_string(const char *utf8);

/* Osmos stores its cloud save as a Java byte[]. These back that. */
void *jni_new_bytearray(const void *data, int len);
void *jni_bytearray_data(void *arr, int *len_out);

#endif
