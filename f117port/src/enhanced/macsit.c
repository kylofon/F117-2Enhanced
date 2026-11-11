/* The Mac art straight from the user's Mac release (Phase 7): a classic StuffIt archive (SIT!, methods 0 and 13, nested
 * archives too) or the resource fork of "F117a Art", decoded in memory. The C twin of tools/mac_art.py (same decoders, same
 * pictures); see macsit.h. */
#include "macsit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ART_NAME "F117a Art"

typedef struct { const u8 *d; size_t n; } Buf;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }

/* ------------------------------------------------------------------ StuffIt method 13 (LZ77 + Huffman) */

typedef struct { const u8 *d; size_t n; size_t p; } Bits;   /* p in bits; reads past the end give zeros */

static u64 peek(const Bits *b)
{
    u64 v = 0;
    const size_t q = b->p >> 3;
    for (int i = 0; i < 8; i++) v |= (u64)(q + (size_t)i < b->n ? b->d[q + (size_t)i] : 0) << (8 * i);
    return v >> (b->p & 7);
}

static u32 get(Bits *b, int n)
{
    const u32 r = (u32)(peek(b) & (((u64)1 << n) - 1));
    b->p += (size_t)n;
    return r;
}

#define MAXL 20
typedef struct { int maxl; s32 *tab; } Code;   /* tab[low maxl bits] = symbol | length << 16, -1 = no code */

static int rev(int x, int n) { int r = 0; for (int i = 0; i < n; i++) { r = r << 1 | (x & 1); x >>= 1; } return r; }

static bool canon(Code *c, const int *lens, int n)
{
    int maxl = 0;
    for (int s = 0; s < n; s++) if (lens[s] > maxl) maxl = lens[s];
    if (maxl < 1 || maxl > MAXL) return false;
    c->maxl = maxl;
    c->tab = malloc(sizeof(s32) << maxl);
    if (!c->tab) return false;
    for (int i = 0; i < 1 << maxl; i++) c->tab[i] = -1;
    int code = 0;
    for (int ln = 1; ln <= maxl; ln++) {
        for (int s = 0; s < n; s++) {
            if (lens[s] != ln) continue;
            const int r = rev(code, ln);
            for (int hi = 0; hi < 1 << (maxl - ln); hi++) {
                s32 *e = &c->tab[r | hi << ln];
                if (*e < 0) *e = s | ln << 16;       /* the shortest match wins, as in the tool */
            }
            code++;
        }
        code <<= 1;
    }
    return true;
}

static int sym(Bits *b, const Code *c)
{
    const s32 e = c->tab[peek(b) & ((1u << c->maxl) - 1)];
    if (e < 0) return -1;
    b->p += (size_t)(e >> 16);
    return e & 0xFFFF;
}

static const u16 meta_codes[37] = { 0x5d8, 0x058, 0x040, 0x0c0, 0x000, 0x078, 0x02b, 0x014, 0x00c, 0x01c, 0x01b, 0x00b, 0x010,
                                    0x020, 0x038, 0x018, 0x0d8, 0xbd8, 0x180, 0x680, 0x380, 0xf80, 0x780, 0x480, 0x080, 0x280,
                                    0x3d8, 0xfd8, 0x7d8, 0x9d8, 0x1d8, 0x004, 0x001, 0x002, 0x007, 0x003, 0x008 };
static const u8 meta_lens[37] = { 11, 8, 8, 8, 8, 7, 6, 5, 5, 5, 5, 6, 5, 6, 7, 7, 9, 12, 10, 11, 11, 12,
                                  12, 11, 11, 11, 12, 12, 12, 12, 12, 5, 2, 2, 3, 4, 5 };

static bool meta_code(Code *c)
{
    c->maxl = 12;
    c->tab = malloc(sizeof(s32) << 12);
    if (!c->tab) return false;
    for (int i = 0; i < 1 << 12; i++) c->tab[i] = -1;
    for (int s = 0; s < 37; s++)                       /* the codes are given bit-reversed already (the tool's dict keys) */
        for (int hi = 0; hi < 1 << (12 - meta_lens[s]); hi++) {
            s32 *e = &c->tab[meta_codes[s] | hi << meta_lens[s]];
            if (*e < 0) *e = s | meta_lens[s] << 16;
        }
    return true;
}

static bool parse_code(Bits *b, int n, const Code *meta, Code *out)
{
    int lens[321 + 80] = { 0 };
    int length = 0, i = 0;
    while (i < n) {
        const int v = sym(b, meta);
        if (v < 0) return false;
        if (v == 31) length = 0;
        else if (v == 32) length++;
        else if (v == 33) length--;
        else if (v == 34) { if (get(b, 1)) lens[i++] = length; }
        else if (v == 35) { for (int k = (int)get(b, 3) + 2; k > 0 && i < n + 70; k--) lens[i++] = length; }
        else if (v == 36) { for (int k = (int)get(b, 6) + 10; k > 0 && i < n + 70; k--) lens[i++] = length; }
        else length = v + 1;
        if (i >= n + 70) return false;
        lens[i++] = length;
    }
    for (int s = 0; s < n; s++) if (lens[s] < 0) lens[s] = 0;
    return canon(out, lens, n);
}

static u8 *sit13(const u8 *d, size_t n, size_t ulen)
{
    Bits b = { d, n, 0 };
    const u32 val = get(&b, 8);
    if (val >> 4) return NULL;                         /* static tables: not used by this archive */
    Code meta = { 0 }, first = { 0 }, second = { 0 }, offc = { 0 };
    u8 *out = malloc(ulen ? ulen : 1);
    size_t o = 0;
    bool ok = out && meta_code(&meta) && parse_code(&b, 321, &meta, &first)
              && ((val & 8) || parse_code(&b, 321, &meta, &second)) && parse_code(&b, (int)(val & 7) + 10, &meta, &offc);
    const Code *sec = (val & 8) ? &first : &second, *cur = &first;
    while (ok && o < ulen) {
        const int v = sym(&b, cur);
        if (v < 0) { ok = false; break; }
        if (v < 0x100) { out[o++] = (u8)v; cur = &first; continue; }
        cur = sec;
        size_t ln;
        if (v < 0x13e) ln = (size_t)(v - 0x100 + 3);
        else if (v == 0x13e) ln = get(&b, 10) + 65;
        else if (v == 0x13f) ln = get(&b, 15) + 65;
        else break;
        const int bl = sym(&b, &offc);
        if (bl < 0) { ok = false; break; }
        const size_t off = bl == 0 ? 1 : bl == 1 ? 2 : ((size_t)1 << (bl - 1)) + get(&b, bl - 1) + 1;
        if (off > o) { ok = false; break; }
        for (size_t k = 0; k < ln && o < ulen; k++, o++) out[o] = out[o - off];
    }
    free(meta.tab); free(first.tab); free(second.tab); free(offc.tab);
    if (!ok || o != ulen) { free(out); return NULL; }
    return out;
}

/* A fork, uncompressed (malloc'd), or NULL */
static u8 *unpack(int m, const u8 *d, size_t n, size_t ulen)
{
    m &= 15;
    if (m == 13) return sit13(d, n, ulen);
    if (m != 0 || n != ulen) return NULL;
    u8 *o = malloc(ulen ? ulen : 1);
    if (o) memcpy(o, d, ulen);
    return o;
}

/* ------------------------------------------------------------------ resource fork */

static bool is_rsrc(Buf f)
{
    if (f.n < 16) return false;
    const u32 doff = be32(f.d), moff = be32(f.d + 4), dlen = be32(f.d + 8), mlen = be32(f.d + 12);
    return doff >= 16 && (u64)doff + dlen <= f.n && (u64)moff + mlen <= f.n && mlen >= 30;
}

/* ------------------------------------------------------------------ CvPc: w, h (BE), LZW min code size, colours - 1, RGB, GIF LZW sub-blocks */

static bool lzw_gif(const u8 *data, size_t n, int mcs, u8 *out, size_t npix)
{
    if (mcs < 1 || mcs > 11) return false;
    u8 *raw = malloc(n + 4);
    if (!raw) return false;
    size_t rn = 0;
    for (size_t p = 0; p < n;) {
        const size_t k = data[p++];
        if (!k) break;
        const size_t m = p + k <= n ? k : n - p;
        memcpy(raw + rn, data + p, m);
        rn += m; p += m;
    }
    memset(raw + rn, 0, 4);
    static u16 prefix[4096];
    static u8 suffix[4096], stk[4097];
    const int clear = 1 << mcs, eoi = clear + 1;
    for (int i = 0; i < clear; i++) suffix[i] = (u8)i;
    int size = mcs + 1, nxt = eoi + 1, prev = -1;
    size_t bit = 0, o = 0;
    const size_t total = rn * 8;
    while (bit + (size_t)size <= total && o < npix) {
        const size_t q = bit >> 3;
        const int code = (int)(((u32)raw[q] | (u32)raw[q + 1] << 8 | (u32)raw[q + 2] << 16) >> (bit & 7) & ((1u << size) - 1));
        bit += (size_t)size;
        if (code == clear) { size = mcs + 1; nxt = eoi + 1; prev = -1; continue; }
        if (code == eoi) break;
        int c = prev >= 0 && code >= nxt ? prev : code, sn = 0;
        while ((c >= clear + 2 || c < clear) && sn < 4096) {   /* expand(c), reversed into stk */
            stk[sn++] = suffix[c];
            if (c < clear) break;
            c = prefix[c];
        }
        if (!sn) break;
        const u8 firstb = stk[sn - 1];
        for (int i = sn - 1; i >= 0 && o < npix; i--) out[o++] = stk[i];
        if (prev < 0) { prev = code; continue; }
        if (code >= nxt && o < npix) out[o++] = firstb;
        if (nxt < 4096) {
            prefix[nxt] = (u16)prev; suffix[nxt] = firstb;
            if (++nxt == 1 << size && size < 12) size++;
        }
        prev = code;
    }
    memset(out + o, 0, npix - o);
    free(raw);
    return true;
}

static bool cvpc_decode(const u8 *d, size_t n, MacPic *p)
{
    if (n < 6) return false;
    const int w = (int)be16(d), h = (int)be16(d + 2), mcs = d[4], nc = d[5] + 1;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || n < 6 + 3 * (size_t)nc) return false;
    u8 *pix = malloc((size_t)w * (size_t)h);
    if (!pix || !lzw_gif(d + 6 + 3 * nc, n - 6 - 3 * (size_t)nc, mcs, pix, (size_t)w * (size_t)h)) { free(pix); return false; }
    memset(p, 0, sizeof *p);
    for (int i = 0; i < nc; i++) {
        const u8 *c = d + 6 + 3 * i;
        p->rgb[i] = (u32)c[0] << 16 | (u32)c[1] << 8 | c[2];
        p->key[i] = p->rgb[i] == 0x00FFFF;
    }
    p->w = w; p->h = h; p->pix = pix;
    return true;
}

/* The CvPc resources of the fork named in names[] (the name without its extension, spaces as '_') -> out[] */
static int rsrc_pics(Buf f, const char *const *names, int count, MacPic *out)
{
    const u32 doff = be32(f.d), moff = be32(f.d + 4), mlen = be32(f.d + 12);
    const u8 *m = f.d + moff;
    if (mlen < 30) return 0;
    const u32 tlo = be16(m + 24), nlo = be16(m + 26);
    if (tlo + 2 > mlen) return 0;
    const u32 nt = be16(m + tlo) + 1;
    int found = 0;
    for (u32 i = 0; i < nt && tlo + 10 + i * 8 <= mlen; i++) {
        const u8 *t = m + tlo + 2 + i * 8;
        if (memcmp(t, "CvPc", 4)) continue;
        const u32 cnt = be16(t + 4) + 1, rlo = be16(t + 6);
        for (u32 j = 0; j < cnt && tlo + rlo + j * 12 + 12 <= mlen; j++) {
            const u8 *r = m + tlo + rlo + j * 12;
            const u32 no = be16(r + 2), ao = (u32)r[5] << 16 | (u32)r[6] << 8 | r[7];
            if (no == 0xFFFF || nlo + no >= mlen) continue;
            char name[256];
            const u8 *q = m + nlo + no;
            int ln = q[0];
            if (nlo + no + 1 + (u32)ln > mlen) continue;
            memcpy(name, q + 1, (size_t)ln);
            name[ln] = 0;
            char *dot = strrchr(name, '.');
            if (dot && dot != name) *dot = 0;
            for (char *s = name; *s; s++) if (*s == ' ') *s = '_';
            const u64 at = (u64)doff + ao;
            if (at + 4 > f.n) continue;
            const u32 len = be32(f.d + at);
            if (at + 4 + len > f.n) continue;
            for (int k = 0; k < count; k++)
                if (!out[k].pix && !strcmp(name, names[k]) && cvpc_decode(f.d + at + 4, len, &out[k])) found++;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ the archive walk */

/* Finds the art fork in the SIT! archive d (nested archives too, this level first): malloc'd, or NULL */
static u8 *find_in_sit(Buf d, size_t *fork_n, int depth)
{
    if (d.n < 22 || memcmp(d.d, "SIT!", 4) || depth > 4) return NULL;
    Buf nested[64];
    int nn = 0;
    u8 *hit = NULL;
    size_t p = 22;
    while (!hit && p + 112 <= d.n) {
        const u8 *h = d.d + p;
        const int rm = h[0], dm = h[1], nl = h[2] < 63 ? h[2] : 63;
        const u32 rul = be32(h + 84), dul = be32(h + 88), rcl = be32(h + 92), dcl = be32(h + 96);
        p += 112;
        if (rm == 32 || dm == 32 || rm == 33 || dm == 33) continue;   /* folder start / end */
        if ((u64)p + rcl + dcl > d.n) break;
        const bool art = (size_t)nl == strlen(ART_NAME) && !memcmp(h + 3, ART_NAME, (size_t)nl);
        u8 *rf = rul ? unpack(rm, d.d + p, rcl, rul) : NULL;
        u8 *df = dul ? unpack(dm, d.d + p + rcl, dcl, dul) : NULL;
        p += (size_t)rcl + dcl;
        if (art && rf && is_rsrc((Buf){ rf, rul })) { hit = rf; *fork_n = rul; rf = NULL; }
        if (df && dul >= 4 && !memcmp(df, "SIT!", 4) && nn < 64) { nested[nn++] = (Buf){ df, dul }; df = NULL; }
        if (rf && rul >= 4 && !memcmp(rf, "SIT!", 4) && nn < 64) { nested[nn++] = (Buf){ rf, rul }; rf = NULL; }
        free(rf); free(df);
    }
    for (int i = 0; i < nn; i++) {
        if (!hit) hit = find_in_sit(nested[i], fork_n, depth + 1);
        free((void *)nested[i].d);
    }
    return hit;
}

int enh_mac_sit_load(const char *path, const char *const *names, int count, MacPic *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = n > 0 && n < (256L << 20) ? malloc((size_t)n) : NULL;
    const bool rd = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!rd) { free(d); return -1; }
    Buf file = { d, (size_t)n }, fork = { NULL, 0 };
    u8 *own = NULL;
    if (file.n >= 4 && !memcmp(d, "SIT!", 4)) {
        size_t fn = 0;
        own = find_in_sit(file, &fn, 0);
        fork = (Buf){ own, fn };
    } else if (is_rsrc(file)) fork = file;
    const int found = fork.d ? rsrc_pics(fork, names, count, out) : 0;
    free(own);
    free(d);
    return found;
}
