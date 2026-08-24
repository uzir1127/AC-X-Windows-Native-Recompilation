#ifndef PSPRECO_ISO9660_H
#define PSPRECO_ISO9660_H

#include "pspreco.h"

int iso_mount(const char *path);
int iso_mounted(void);
int iso_is_directory(void);
s64 iso_image_size(void);
int iso_find(const char *path, s64 *out_offset, s64 *out_size);
int iso_read(s64 offset, u8 *ram, u32 guest_addr, u32 length);
int iso_pread(s64 offset, void *dst, u32 length);

#endif
