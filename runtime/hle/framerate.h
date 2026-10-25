#ifndef PSPRECO_FRAMERATE_H
#define PSPRECO_FRAMERATE_H
#include "pspreco.h"
void framerate_init(u8 *ram);
int framerate_is_60(void);
void framerate_movie_begin(void);
void framerate_movie_end(void);
void framerate_update_delta(u8 *ram, u32 timer);
#endif
