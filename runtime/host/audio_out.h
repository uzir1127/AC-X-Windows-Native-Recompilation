#ifndef PSPRECO_AUDIO_OUT_H
#define PSPRECO_AUDIO_OUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_out_push(int ch, const int16_t *lr, int nframes,
                    int volL, int volR);

int audio_out_queued(int ch);

void audio_out_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
