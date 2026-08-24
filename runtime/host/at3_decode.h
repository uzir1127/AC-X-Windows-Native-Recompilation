#ifndef PSPRECO_AT3_DECODE_H
#define PSPRECO_AT3_DECODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct At3Handle At3Handle;

int at3_available(void);

At3Handle *at3_open(int is_plus, const unsigned char *extradata,
                    int extradata_size, int channels, int block_align,
                    int sample_rate);

int at3_decode_frame(At3Handle *h, const unsigned char *in, int in_size,
                     short *out, int out_cap);

void at3_flush(At3Handle *h);

void at3_close(At3Handle *h);

#ifdef __cplusplus
}
#endif
#endif
