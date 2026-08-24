#ifndef PSPRECO_OSK_HOST_H
#define PSPRECO_OSK_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

int osk_host_input(const unsigned short *desc, const unsigned short *initial,
                   unsigned short *out, int cap);

#ifdef __cplusplus
}
#endif
#endif
