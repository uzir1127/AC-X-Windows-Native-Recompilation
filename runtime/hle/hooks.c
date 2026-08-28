/* Replace any recompiled function.
 *
 * recomp_lookup is only consulted for indirect calls, and the emitter turns a
 * `jal` to a known function into a direct C call, so overrides used to reach
 * only the 604 of 5462 functions the engine dispatches through a pointer.
 * Res_GetByName was not among them.
 *
 * A --moddable build changes the emitter rather than the runtime: every direct
 * call becomes
 *
 *     PSPRECO_CALL(idx, func_08806b6c)(c, ram)   ->   g_func_ptr[idx](c, ram)
 *
 * through the sorted table recomp_lookup already bisects, so installing a hook
 * is one store that covers direct and indirect sites alike.  30868 call sites
 * were checked against the table.
 *
 * The cost is a load and an indirect branch per call, hence a separate
 * executable rather than the default.
 */

#include "hooks.h"
#include "../../generated/symbols.h"

#include <stdio.h>

int hook_available(void)
{
#ifdef PSPRECO_MODDABLE
    return 1;
#else
    return 0;
#endif
}

/* The same bisect recomp_lookup does, kept here so a caller can test
 * hookability without also consulting the override chain. */
int hook_index(u32 guest_addr)
{
    size_t lo = 0, hi = PSPRECO_FUNC_COUNT;

    if (guest_addr < PSPRECO_MODULE_BASE) return HOOK_ENOTFOUND;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_func_addr[mid] < guest_addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < PSPRECO_FUNC_COUNT && g_func_addr[lo] == guest_addr)
        return (int)lo;
    return HOOK_ENOTFOUND;
}

int hook_install(u32 guest_addr, RecompFn fn, RecompFn *orig)
{
    int i = hook_index(guest_addr);

    if (i < 0) {
        fprintf(stderr, "[hook] %08x is not a recompiled function\n",
                (unsigned)guest_addr);
        return HOOK_ENOTFOUND;
    }
    if (!hook_available()) {
        /* Without the emitter change this store would only be seen by the
         * indirect call sites, so the hook would fire on some calls and not
         * others.  Refuse instead. */
        fprintf(stderr, "[hook] %08x: this build is not moddable "
                "(rebuild with build.py --moddable)\n", (unsigned)guest_addr);
        return HOOK_ENOTMODDABLE;
    }
#ifdef PSPRECO_MODDABLE
    if (orig) *orig = g_func_ptr[i];
    g_func_ptr[i] = fn;
#else
    (void)fn; (void)orig;
#endif
    return HOOK_OK;
}
