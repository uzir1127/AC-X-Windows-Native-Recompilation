#ifndef PSPRECO_HOOKS_H
#define PSPRECO_HOOKS_H

#include "pspreco.h"

/* Replace any recompiled function.
 *
 * `orig` is filled with what was there and a hook is expected to call it.
 *
 * Returns 0 on success, negative if the address is not a recompiled function
 * or this is not a moddable build.  A non-moddable build refuses rather than
 * half-working: writing the table there would only affect indirect call
 * sites, so a hook would fire on some calls and not others. */
#define HOOK_OK            0
#define HOOK_ENOTFOUND   (-1)   /* no recompiled function at that address */
#define HOOK_ENOTMODDABLE (-2)  /* built without --moddable */

int  hook_install(u32 guest_addr, RecompFn fn, RecompFn *orig);
/* To uninstall, install the `orig` that was handed back.  There is no
 * hook_remove(): nothing maps an index to its original once the table entry
 * has been overwritten. */

/* Whether direct calls honour the table in this build. */
int  hook_available(void);

/* What hook_install would return, without installing anything. */
int  hook_index(u32 guest_addr);

#endif
