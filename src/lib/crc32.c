/* CRC-32 (IEEE 802.3, the one of GPT headers, ZIP and PNG). Reflected
 * polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF; the table is
 * built on first use. */
#include "crc32.h"

static uint32_t table[256];
static bool ready;

static void build(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    ready = true;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    if (!ready)
        build();
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
