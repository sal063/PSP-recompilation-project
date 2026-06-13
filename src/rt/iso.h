/* ISO9660 disc reader (src/rt/iso.c). */
#ifndef PSP_RECOMP_ISO_H
#define PSP_RECOMP_ISO_H

#include <stdint.h>

int iso_init(void);
/* Resolve a guest path ("disc0:/PSP_GAME/...") to its extent. Returns 0 on success. */
int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size);
/* Read bytes from (lba*2048 + offset). Returns bytes read. */
int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes);

#endif
