/* partmgr: sizes as the user writes and reads them. Units are binary
 * (1K = 1024 bytes, 1M = 1024K...); a number without a unit is in MiB. */
#ifndef PARTMGR_UNITS_H
#define PARTMGR_UNITS_H

#include "../lib/rt.h"

/* Parses "512M", "1.5G", "20 GiB", "300" (MiB), "2048s" (blocks, when
 * BSIZE is given) or "rest" / "all" / "max" (*rest set, *bytes 0).
 * Returns NULL on success or a message for the user. */
const char *pm_parse_size(const char *s, uint32_t bsize, uint64_t *bytes, bool *rest);

/* 512 B, 20.0 KiB, 512.0 MiB, 30.8 GiB, 1.8 TiB */
void pm_fmt_size(char *out, size_t n, uint64_t bytes);

/* An exact form that pm_parse_size reads back to the same value when it is a
 * whole number of MiB or GiB ("1 MiB", "20 GiB"), else pm_fmt_size. */
void pm_fmt_exact(char *out, size_t n, uint64_t bytes);

#endif
