/* partmgr internals shared by ptable.c, ptedit.c and ptwrite.c: little-endian
 * fields and the MBR type rules. */
#ifndef PARTMGR_PTINT_H
#define PARTMGR_PTINT_H

#include "ptable.h"

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }

/* 05 (CHS), 0F (LBA) and 85 (Linux) all mean "extended partition". */
static inline bool pt_mbr_extended(uint8_t type) { return type == 0x05 || type == 0x0F || type == 0x85; }

#endif
