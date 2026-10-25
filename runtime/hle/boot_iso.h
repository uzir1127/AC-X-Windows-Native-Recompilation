#ifndef PSPRECO_BOOT_ISO_H
#define PSPRECO_BOOT_ISO_H
#include "pspreco.h"
#include <stddef.h>
/* Mounted ISO -> relocated module, identical to prxload.py --rebase 0x08804000.
 * Only the executable this build was compiled against is accepted. */
int boot_iso_load(u8 *image, u32 capacity, u32 *size,
                  char *error, size_t error_size);
#endif
