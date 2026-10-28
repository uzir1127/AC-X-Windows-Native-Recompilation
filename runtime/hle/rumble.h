#ifndef ACX_RUMBLE_H
#define ACX_RUMBLE_H
#include "pspreco.h"
enum { RUMBLE_GUN = 1, RUMBLE_MISSILE = 2, RUMBLE_SPECIAL = 4, RUMBLE_DAMAGE = 8, RUMBLE_STALL = 16 };
void rumble_install(u8 *ram);
/* Read the completed player integration's live speed envelope. */
void rumble_observe_flight(u8 *ram, u32 player, u32 flight);
unsigned rumble_airspeed_level(float native_speed);
unsigned rumble_buffet_level(u8 *ram);
/* Read-only gameplay observer. Inactive polling discards pending effects. */
unsigned rumble_poll(u8 *ram, int active, unsigned *damage);
#endif
