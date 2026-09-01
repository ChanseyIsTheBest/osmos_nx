/* osmos_tune.h -- see osmos_tune.c. MIT licensed. */
#ifndef OSMOS_TUNE_H
#define OSMOS_TUNE_H

#include "so_util.h"

/* Call after so_relocate and BEFORE so_finalize: load_base is writable until
 * finalize maps it as code, and the kernel does not allow a W->X transition
 * afterwards. */
void osmos_tune_apply(so_module *mod);

/* criticalLength is recomputed by nativeChanged from gPixelDensityScale, so
 * this must run AFTER every Changed, not once at load. */
void osmos_tune_deadzone(so_module *mod);

#endif
