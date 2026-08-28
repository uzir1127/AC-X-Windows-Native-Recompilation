#ifndef PSPRECO_SCRIPT_H
#define PSPRECO_SCRIPT_H

#include "pspreco.h"

/* The Lua host.  Scripts live in mods/<name>/scripts/ and are reloaded when
 * they change on disk.
 *
 * Everything a script can do lives in acx_api.c; this is only the binding.
 * Runtime fan-outs go through acx_event_fire rather than a function per event,
 * so adding an event does not touch this header. */

void script_init(void);
void script_tick(CpuCtx *c, u8 *ram);

#endif
