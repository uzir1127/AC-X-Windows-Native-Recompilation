#ifndef ACX_GAME_STATE_H
#define ACX_GAME_STATE_H
#include "pspreco.h"

/* US world pause flag, read by the original getter 089c3a94 and player
 * update dispatcher. Config +0x151 is a pause-menu RESULT: it is 0 both
 * on first entering flight and while paused, then 1 after Resume. */
static inline int acx_flight_active(u8 *ram)
{
    if (!ram) return 0;
    u32 player = mem_r32(ram, 0x08a82ed8u);
    unsigned phase = mem_r8(ram, 0x08a82e08u);
    return player >= 0x08800000u && player < 0x09fff000u
        && (phase == 12 || phase == 22)
        && mem_r32(ram, 0x08b7f1b0u) == 0;
}
#endif
