/* UEFI (EFI 1.1) compression format. */
#ifndef NESH_EFICOMP_H
#define NESH_EFICOMP_H

#include "rt.h"

/* Returns a malloc'd buffer with the 8-byte header included. */
char *efi_compress(const uint8_t *in, size_t n, size_t *out_len);
/* 0 on success; *out is malloc'd. */
int efi_decompress(const uint8_t *in, size_t n, char **out, size_t *out_len);

#endif
