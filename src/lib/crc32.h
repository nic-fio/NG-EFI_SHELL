/* CRC-32 (IEEE 802.3, the one of GPT headers, ZIP and PNG). */
#ifndef NESH_CRC32_H
#define NESH_CRC32_H

#include "rt.h"

/* crc32_update(0, ...) starts a new checksum; pass the result back to continue. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);

#endif
