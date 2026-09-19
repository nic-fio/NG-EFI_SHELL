/* UEFI compression format (EFI 1.1 / "version 1"): LZ77 with an 8 KiB window
 * and canonical Huffman codes, as decoded by EFI_DECOMPRESS_PROTOCOL.
 *
 * Stream: UINT32 compressed size, UINT32 original size, then blocks, bits
 * written most significant first. Each block:
 *   16 bits  number of codes in the block
 *   T tree   code lengths of the "length of lengths" alphabet (NT = 19)
 *   C tree   code lengths of literals/match lengths (NC = 510), coded with T
 *   P tree   code lengths of position classes (NP = 14)
 *   codes    literal (0..255) or match length L as 253+L followed by a
 *            position class p and p-1 extra bits (distance - 1). */
#include "eficomp.h"

#define NC 510
#define NT 19
#define NP 14
#define CBIT 9
#define TBIT 5
#define PBIT 4
#define THRESHOLD 3
#define MAXMATCH 256
#define WINDOW 8192
#define MAXLEN 16
#define BLOCK_CODES 16384

/* ---- bit writer ---- */

typedef struct {
    Sbuf *out;
    uint32_t acc;
    int n;
} BitW;

static void putbits(BitW *w, int n, uint32_t v)
{
    while (n > 0) {
        int take = n > 8 ? 8 : n;
        n -= take;
        w->acc = (w->acc << take) | ((v >> n) & ((1u << take) - 1));
        w->n += take;
        while (w->n >= 8) {
            w->n -= 8;
            sb_putc(w->out, (char)(w->acc >> w->n));
        }
    }
}

static void flushbits(BitW *w)
{
    if (w->n)
        putbits(w, 8 - w->n, 0);
}

/* ---- Huffman code lengths, limited to MAXLEN, complete code ---- */

static void huff_lengths(const uint32_t *freq, int n, uint8_t *len)
{
    uint32_t f[NC];
    for (int i = 0; i < n; i++)
        f[i] = freq[i];
    for (;;) {
        /* simple O(n^2) Huffman: n <= 510, called a few times per block */
        int nodes = 0;
        int parent[2 * NC];
        uint32_t w[2 * NC];
        int leaf_of[NC];
        for (int i = 0; i < n; i++) {
            len[i] = 0;
            if (f[i]) {
                w[nodes] = f[i];
                parent[nodes] = -1;
                leaf_of[i] = nodes++;
            } else {
                leaf_of[i] = -1;
            }
        }
        int leaves = nodes;
        if (leaves < 2)
            return; /* 0 or 1 symbol: coded with the special form */
        bool alive[2 * NC];
        for (int i = 0; i < nodes; i++)
            alive[i] = true;
        for (int step = 0; step < leaves - 1; step++) {
            int a = -1, b = -1;
            for (int i = 0; i < nodes; i++) {
                if (!alive[i])
                    continue;
                if (a < 0 || w[i] < w[a]) {
                    b = a;
                    a = i;
                } else if (b < 0 || w[i] < w[b]) {
                    b = i;
                }
            }
            alive[a] = alive[b] = false;
            w[nodes] = w[a] + w[b];
            parent[nodes] = -1;
            parent[a] = parent[b] = nodes;
            alive[nodes] = true;
            nodes++;
        }
        int maxl = 0;
        for (int i = 0; i < n; i++) {
            if (leaf_of[i] < 0)
                continue;
            int d = 0;
            for (int x = leaf_of[i]; parent[x] >= 0; x = parent[x])
                d++;
            len[i] = (uint8_t)d;
            if (d > maxl)
                maxl = d;
        }
        if (maxl <= MAXLEN)
            return;
        for (int i = 0; i < n; i++) /* flatten the distribution and retry */
            if (f[i])
                f[i] = (f[i] + 1) / 2;
    }
}

static void canon_codes(const uint8_t *len, int n, uint16_t *code)
{
    uint32_t count[MAXLEN + 2] = { 0 }, next[MAXLEN + 2];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (int l = 1; l <= MAXLEN; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (int i = 0; i < n; i++)
        if (len[i])
            code[i] = (uint16_t)next[len[i]]++;
}

/* Code lengths for a tree, sending the "single symbol" form when needed. */
static void write_pt(BitW *w, const uint8_t *len, int n, int nbit, int special, int single)
{
    if (single >= 0) {
        putbits(w, nbit, 0);
        putbits(w, nbit, (uint32_t)single);
        return;
    }
    while (n > 0 && !len[n - 1])
        n--;
    putbits(w, nbit, (uint32_t)n);
    for (int i = 0; i < n;) {
        int k = len[i++];
        if (k <= 6)
            putbits(w, 3, (uint32_t)k);
        else
            putbits(w, k - 3, (1u << (k - 3)) - 2);
        if (i == special) {
            while (i < 6 && i < n && !len[i])
                i++;
            putbits(w, 2, (uint32_t)((i - 3) & 3));
        }
    }
}

typedef struct {
    uint16_t sym; /* C symbol */
    uint16_t pos; /* distance - 1, for matches */
} Tok;

static int pos_class(unsigned p)
{
    int c = 0;
    while (p) {
        c++;
        p >>= 1;
    }
    return c; /* 0 for 0, 1 for 1, n for 2^(n-1) <= p < 2^n */
}

static void write_block(BitW *w, const Tok *t, int nt)
{
    uint32_t cf[NC] = { 0 }, pf[NP] = { 0 }, tf[NT] = { 0 };
    for (int i = 0; i < nt; i++) {
        cf[t[i].sym]++;
        if (t[i].sym >= 256)
            pf[pos_class(t[i].pos)]++;
    }
    uint8_t cl[NC], pl[NP], tl[NT];
    uint16_t cc[NC], pc[NP], tc[NT];
    huff_lengths(cf, NC, cl);
    huff_lengths(pf, NP, pl);
    int csingle = -1, psingle = -1, tsingle = -1, used = 0;
    for (int i = 0; i < NC; i++)
        if (cf[i])
            used++, csingle = i;
    if (used != 1)
        csingle = -1;
    used = 0;
    for (int i = 0; i < NP; i++)
        if (pf[i])
            used++, psingle = i;
    if (used != 1)
        psingle = used ? -1 : 0; /* no match at all: any single symbol */

    /* T alphabet: describes the C code lengths */
    int cn = NC;
    while (cn > 0 && !cl[cn - 1])
        cn--;
    if (csingle < 0) {
        for (int i = 0; i < cn;) {
            if (cl[i]) {
                tf[cl[i] + 2]++;
                i++;
                continue;
            }
            int z = 0;
            while (i < cn && !cl[i])
                i++, z++;
            if (z <= 2)
                tf[0] += (uint32_t)z;
            else if (z <= 18)
                tf[1]++;
            else if (z == 19)
                tf[0]++, tf[1]++;
            else
                tf[2]++;
        }
        huff_lengths(tf, NT, tl);
        used = 0;
        for (int i = 0; i < NT; i++)
            if (tf[i])
                used++, tsingle = i;
        if (used != 1)
            tsingle = -1;
    } else {
        memset(tl, 0, sizeof(tl));
        tsingle = 0;
    }
    if (tsingle < 0)
        canon_codes(tl, NT, tc);
    if (csingle < 0)
        canon_codes(cl, NC, cc);
    if (psingle < 0)
        canon_codes(pl, NP, pc);

    putbits(w, 16, (uint32_t)nt);
    write_pt(w, tl, NT, TBIT, 3, tsingle);
    if (csingle >= 0) {
        putbits(w, CBIT, 0);
        putbits(w, CBIT, (uint32_t)csingle);
    } else {
        putbits(w, CBIT, (uint32_t)cn);
        for (int i = 0; i < cn;) {
            if (cl[i]) {
                int s = cl[i] + 2;
                if (tsingle < 0)
                    putbits(w, tl[s], tc[s]);
                i++;
                continue;
            }
            int z = 0;
            while (i < cn && !cl[i])
                i++, z++;
#define TPUT(s) do { if (tsingle < 0) putbits(w, tl[s], tc[s]); } while (0)
            if (z <= 2) {
                for (int k = 0; k < z; k++)
                    TPUT(0);
            } else if (z <= 18) {
                TPUT(1);
                putbits(w, 4, (uint32_t)(z - 3));
            } else if (z == 19) {
                TPUT(0);
                TPUT(1);
                putbits(w, 4, 15);
            } else {
                TPUT(2);
                putbits(w, CBIT, (uint32_t)(z - 20));
            }
#undef TPUT
        }
    }
    write_pt(w, pl, NP, PBIT, -1, psingle);

    for (int i = 0; i < nt; i++) {
        uint16_t s = t[i].sym;
        if (csingle < 0)
            putbits(w, cl[s], cc[s]);
        if (s >= 256) {
            unsigned p = t[i].pos;
            int k = pos_class(p);
            if (psingle < 0)
                putbits(w, pl[k], pc[k]);
            if (k > 1)
                putbits(w, k - 1, p - (1u << (k - 1)));
        }
    }
}

/* ---- compressor ---- */

#define HASH_BITS 13
#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & ((1u << HASH_BITS) - 1))

char *efi_compress(const uint8_t *in, size_t n, size_t *out_len)
{
    Sbuf out;
    sb_init(&out);
    sb_add(&out, "\0\0\0\0\0\0\0\0", 8);
    BitW w = { &out, 0, 0 };
    int32_t *head = xmalloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = xmalloc(sizeof(int32_t) * (n ? n : 1));
    for (int i = 0; i < (1 << HASH_BITS); i++)
        head[i] = -1;
    Tok *toks = xmalloc(sizeof(Tok) * BLOCK_CODES);
    int nt = 0;
    for (size_t i = 0; i < n;) {
        int best = 0;
        size_t bestpos = 0;
        if (i + THRESHOLD <= n) {
            uint32_t h = HASH(in + i);
            int chain = 256;
            for (int32_t j = head[h]; j >= 0 && i - (size_t)j <= WINDOW && chain--; j = prev[j]) {
                size_t max = MIN((size_t)MAXMATCH, n - i);
                size_t l = 0;
                while (l < max && in[j + l] == in[i + l])
                    l++;
                if ((int)l > best) {
                    best = (int)l;
                    bestpos = i - (size_t)j - 1;
                    if (l == max)
                        break;
                }
            }
        }
        size_t adv;
        if (best >= THRESHOLD) {
            toks[nt].sym = (uint16_t)(best + 256 - THRESHOLD);
            toks[nt].pos = (uint16_t)bestpos;
            adv = (size_t)best;
        } else {
            toks[nt].sym = in[i];
            toks[nt].pos = 0;
            adv = 1;
        }
        nt++;
        for (size_t k = 0; k < adv; k++, i++) {
            if (i + THRESHOLD <= n) {
                uint32_t h = HASH(in + i);
                prev[i] = head[h];
                head[h] = (int32_t)i;
            }
        }
        if (nt == BLOCK_CODES) {
            write_block(&w, toks, nt);
            nt = 0;
        }
    }
    if (nt)
        write_block(&w, toks, nt);
    flushbits(&w);
    free(toks);
    free(prev);
    free(head);
    uint32_t csize = (uint32_t)(out.len - 8), osize = (uint32_t)n;
    memcpy(out.s, &csize, 4);
    memcpy(out.s + 4, &osize, 4);
    *out_len = out.len;
    return sb_steal(&out);
}

/* ---- decompressor ---- */

typedef struct {
    const uint8_t *src;
    size_t srclen, bitpos;
    bool bad;
} BitR;

static uint32_t peekbits(BitR *r, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t p = r->bitpos + (size_t)i;
        int bit = p / 8 < r->srclen ? (r->src[p / 8] >> (7 - p % 8)) & 1 : 0;
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}

static uint32_t getbits(BitR *r, int n)
{
    uint32_t v = peekbits(r, n);
    r->bitpos += (size_t)n;
    return v;
}

typedef struct {
    uint8_t len[NC];
    uint16_t code[NC];
    uint16_t cnt[MAXLEN + 1]; /* canonical decoding tables */
    uint16_t sorted[NC];
    int n;
    int single; /* >= 0: every code is this symbol, 0 bits */
} Tree;

static bool tree_build(Tree *t)
{
    /* the code must be complete (as the firmware decoder requires) */
    uint32_t total = 0;
    int used = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->len[i] > MAXLEN)
            return false;
        if (t->len[i]) {
            total += 1u << (MAXLEN - t->len[i]);
            used++;
        }
    }
    if (used && total != (1u << MAXLEN))
        return false;
    memset(t->cnt, 0, sizeof(t->cnt));
    for (int i = 0; i < t->n; i++)
        t->cnt[t->len[i]]++;
    int k = 0;
    for (int l = 1; l <= MAXLEN; l++)
        for (int i = 0; i < t->n; i++)
            if (t->len[i] == l)
                t->sorted[k++] = (uint16_t)i;
    return true;
}

static int tree_decode(BitR *r, const Tree *t)
{
    if (t->single >= 0)
        return t->single;
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAXLEN; l++) {
        code |= (int)getbits(r, 1);
        int count = t->cnt[l];
        if (code - first < count)
            return t->sorted[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    r->bad = true;
    return 0;
}

static void read_pt(BitR *r, Tree *t, int nn, int nbit, int special)
{
    memset(t->len, 0, sizeof(t->len));
    t->n = nn;
    t->single = -1;
    int n = (int)getbits(r, nbit);
    if (!n) {
        t->single = (int)getbits(r, nbit);
        return;
    }
    for (int i = 0; i < n && i < nn;) {
        int c = (int)peekbits(r, 3);
        if (c == 7) {
            r->bitpos += 3;
            while (getbits(r, 1))
                c++;
        } else {
            r->bitpos += 3;
        }
        t->len[i++] = (uint8_t)c;
        if (i == special) {
            int z = (int)getbits(r, 2);
            while (z-- > 0 && i < nn)
                t->len[i++] = 0;
        }
    }
    if (!tree_build(t))
        r->bad = true;
}

int efi_decompress(const uint8_t *in, size_t n, char **out, size_t *out_len)
{
    if (n < 8)
        return -1;
    uint32_t csize, osize;
    memcpy(&csize, in, 4);
    memcpy(&osize, in + 4, 4);
    if (csize > n - 8 || osize > 0x40000000)
        return -1;
    uint8_t *dst = xmalloc(osize ? osize : 1);
    BitR r = { in + 8, csize, 0, false };
    size_t o = 0;
    Tree *tt = xmalloc(sizeof(Tree)), *ct = xmalloc(sizeof(Tree)), *pt = xmalloc(sizeof(Tree));
    while (o < osize && !r.bad) {
        uint32_t codes = getbits(&r, 16);
        read_pt(&r, tt, NT, TBIT, 3);
        /* C lengths */
        memset(ct->len, 0, sizeof(ct->len));
        ct->n = NC;
        ct->single = -1;
        int cn = (int)getbits(&r, CBIT);
        if (!cn) {
            ct->single = (int)getbits(&r, CBIT);
        } else {
            for (int i = 0; i < cn && i < NC && !r.bad;) {
                int c = tree_decode(&r, tt);
                if (c <= 2) {
                    int z = c == 0 ? 1 : c == 1 ? (int)getbits(&r, 4) + 3 : (int)getbits(&r, CBIT) + 20;
                    while (z-- > 0 && i < NC)
                        ct->len[i++] = 0;
                } else {
                    ct->len[i++] = (uint8_t)(c - 2);
                }
            }
            if (!tree_build(ct))
                r.bad = true;
        }
        read_pt(&r, pt, NP, PBIT, -1);
        for (uint32_t k = 0; k < codes && o < osize && !r.bad; k++) {
            int c = tree_decode(&r, ct);
            if (c < 256) {
                dst[o++] = (uint8_t)c;
                continue;
            }
            int len = c - (256 - THRESHOLD);
            int pc = tree_decode(&r, pt);
            uint32_t pos = (uint32_t)pc;
            if (pc > 1)
                pos = (1u << (pc - 1)) + getbits(&r, pc - 1);
            if (pos + 1 > o) {
                r.bad = true;
                break;
            }
            size_t from = o - pos - 1;
            while (len-- > 0 && o < osize)
                dst[o++] = dst[from++];
        }
        if (r.bitpos / 8 > csize + 1)
            r.bad = true;
    }
    free(tt);
    free(ct);
    free(pt);
    if (r.bad) {
        free(dst);
        return -1;
    }
    *out = (char *)dst;
    *out_len = osize;
    return 0;
}
