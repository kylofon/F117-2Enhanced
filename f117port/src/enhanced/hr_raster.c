/* The hr copy of render3d/r3d_raster.c (VGAME 1359, mode M; DESIGN.md 2.2, 2.4): the same integer edge DDA, walls, span fill and
 * Bresenham line on layer coordinates (int32), into the 3D layer. Row tables have Lh rows; the sentinels 7FFFh / 8001h become
 * INT32_MAX / INT32_MIN + 1. The colour word is the faithful one (DS:85D6 after r3d_setcolor_M, read only).
 * Dither: --dither grid (default) evaluates the checkerboard on the original-pixel phase ((X - fx) / N ^ (Y - fy) / N) & 1, so a
 * dithered face keeps the original look at any N; --dither fine uses the layer pixel ((X - fx) ^ (Y - fy)) & 1. Both are the
 * original's (x ^ y) & 1 at N = 1. Lines are max(1, N/2) layer pixels wide (Q3), the brush centred in the original pixel. */
#include "enhanced/hr.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "enhanced/enh_canvas.h"
#include "render3d/r3d.h"

#define IDLE_L INT32_MAX
#define IDLE_R (INT32_MIN + 1)

static s32 *tab_l, *tab_r;
static int tab_rows;
s32 hr_miny = IDLE_L;
void hr_raster_regions(void (*add)(void *p, size_t n)) { add(&hr_miny, sizeof hr_miny); }

s32 *hr_tab_l(void) { return tab_l; }
s32 *hr_tab_r(void) { return tab_r; }

static s32 fdiv(s32 a, s32 n) { return a >= 0 ? a / n : -((-a + n - 1) / n); }

static bool tables_alloc(void)
{
    if (tab_rows == enh_hr.lh && tab_l) return true;
    free(tab_l); free(tab_r);
    tab_l = malloc(sizeof(s32) * (size_t)enh_hr.lh);
    tab_r = malloc(sizeof(s32) * (size_t)enh_hr.lh);
    if (!tab_l || !tab_r) { free(tab_l); free(tab_r); tab_l = tab_r = NULL; tab_rows = 0; return false; }
    tab_rows = enh_hr.lh;
    for (int i = 0; i < tab_rows; i++) { tab_l[i] = IDLE_L; tab_r[i] = IDLE_R; }
    return true;
}

/* The faithful row tables carry state from one polygon to the next (fill_M stops at the first idle row and leaves the rows below it:
 * the "gap row" behaviour), so a pass starts from the faithful tables, scaled: row r -> rows fy + N r .. + N - 1, L -> fx + N L,
 * R -> fx + N R + N - 1. At N = 1 this is a copy. */
void hr_raster_begin(void)
{
    if (!tables_alloc()) { enh_hr.on = false; return; }
    const int n = enh_hr.n;
    for (int i = 0; i < tab_rows; i++) { tab_l[i] = IDLE_L; tab_r[i] = IDLE_R; }
    for (int r = 0; r < 480; r++) {
        s16 L = R3D_L(2 * r), R = R3D_R(2 * r);
        if (L == 0x7FFF && R == (s16)0x8001) continue;
        for (int k = 0; k < n; k++) {
            int row = enh_hr.fy + n * r + k;
            if (row < 0 || row >= tab_rows) continue;
            tab_l[row] = L == 0x7FFF ? IDLE_L : enh_hr.fx + n * L;
            tab_r[row] = R == (s16)0x8001 ? IDLE_R : enh_hr.fx + n * R + n - 1;
        }
    }
    s16 my = R3D_W->edge_miny;
    hr_miny = my == 0x7FFF ? IDLE_L : enh_hr.fy + n * my;
}

/* ---------------------------------------------------------------- layer pixels */

static u16 put_code;                               /* S3.5: the blend code of the shaded fill / line being drawn (0 otherwise) */
static int put_night;                              /* S3.6: how the pixels of the fill / line being drawn get their night factor (HR_NM_*) */
static u8 put_keep;                                /* S3.6: the factor for HR_NM_KEEP */
void hr_put(s32 x, s32 y, u8 c)
{
    if (HR_IN_HOLE(x, y)) return;                  /* S3.4 */
    const size_t at = (size_t)y * enh_hr.lw + x;
    enh_hr.layer[at] = (u16)(0x100 | c | put_code);
    if (enh_hr.night)                              /* S3.6 */
        enh_hr.night[at] = put_night == HR_NM_OBJ ? hr_night_obj(x, y) : put_night == HR_NM_LIT ? HR_NIGHT_LIT : put_night == HR_NM_KEEP ? put_keep : 0;
}

u8 hr_get(s32 x, s32 y)
{
    u16 l = enh_hr.layer[(size_t)y * enh_hr.lw + x];
    if (l & 0x100) return (u8)l;
    s32 px = fdiv(x - enh_hr.fx, enh_hr.n), py = fdiv(y - enh_hr.fy, enh_hr.n);
    if (px < 0 || px > 319 || py < 0 || py > 199) return 0;
    return seg_ptr(enh_hr.seg)[py * 320 + px];
}

static inline int phase(s32 x, s32 y)
{
    s32 dx = x - enh_hr.fx, dy = y - enh_hr.fy;
    if (enh_hr.fine) return (int)((dx ^ dy) & 1);
    return (int)((fdiv(dx, enh_hr.n) ^ fdiv(dy, enh_hr.n)) & 1);
}

/* S4.3: a run of hr_put on row y, x = l..r (clipped by the caller), colour `ah` where phase(x, y) is 1 and `al` elsewhere (dith: false = al everywhere;
 * only_ah: the pixels of phase 0 are left as they are); the same layer and night values as hr_put pixel by pixel, written a row at a time */
static void span_run(s32 y, s32 l, s32 r, u8 al, u8 ah, bool dith, bool only_ah)
{
    if (l > r) return;
    if (enh_hr.hole && y >= enh_hr.hole_y && r >= enh_hr.hole_x0 && l <= enh_hr.hole_x1) {   /* S3.4: the panel hole is never written */
        span_run(y, l, enh_hr.hole_x0 - 1, al, ah, dith, only_ah);
        span_run(y, enh_hr.hole_x1 + 1, r, al, ah, dith, only_ah);
        return;
    }
    const size_t at = (size_t)y * enh_hr.lw;
    u16 *row = enh_hr.layer + at;
    u8 *nrow = enh_hr.night ? enh_hr.night + at : NULL;
    const u16 base = (u16)(0x100 | put_code);
    const u16 vl = (u16)(base | al), vh = (u16)(base | ah);
    if (!dith || (al == ah && !only_ah)) {
        for (s32 x = l; x <= r; x++) row[x] = vl;
    } else if (enh_hr.fine) {
        const s32 dy = y - enh_hr.fy;
        for (s32 x = l; x <= r; x++) {
            const int ph = (int)(((x - enh_hr.fx) ^ dy) & 1);
            if (ph) row[x] = vh; else if (!only_ah) row[x] = vl;
        }
    } else {
        const int n = enh_hr.n, py = (int)(fdiv(y - enh_hr.fy, n) & 1);
        s32 x = l;
        while (x <= r) {
            const s32 bx = fdiv(x - enh_hr.fx, n);               /* the original pixel column of x and where it ends */
            s32 e = enh_hr.fx + (bx + 1) * n - 1;
            if (e > r) e = r;
            const int ph = (int)((bx ^ py) & 1);
            if (ph) for (s32 k = x; k <= e; k++) row[k] = vh;
            else if (!only_ah) for (s32 k = x; k <= e; k++) row[k] = vl;
            x = e + 1;
        }
    }
    if (!nrow) return;                                     /* S3.6: the night factor of every pixel written */
    if (put_night == HR_NM_OBJ || only_ah) {
        for (s32 x = l; x <= r; x++)
            if (!only_ah || phase(x, y)) nrow[x] = put_night == HR_NM_OBJ ? hr_night_obj(x, y) : put_night == HR_NM_LIT ? HR_NIGHT_LIT : put_night == HR_NM_KEEP ? put_keep : 0;
    } else {
        const u8 v = put_night == HR_NM_LIT ? HR_NIGHT_LIT : put_night == HR_NM_KEEP ? put_keep : 0;
        memset(nrow + l, v, (size_t)(r - l + 1));
    }
}

/* S4.3: the AND (FEh) / OR (FDh) fill of row y, x = l..r: per pixel v = hr_get(x, y), nv = v & al / v | al, the S3.5 blend code and the S3.6 factor of the
 * written pixel under it kept, then hr_put(x, y, nv) (nothing in the panel hole) */
static void span_andor(s32 y, s32 l, s32 r, u8 al, bool is_and)
{
    const size_t at = (size_t)y * enh_hr.lw;
    u16 *row = enh_hr.layer + at;
    u8 *nrow = enh_hr.night ? enh_hr.night + at : NULL;
    const int n = enh_hr.n;
    const s32 py = fdiv(y - enh_hr.fy, n);
    const u8 *page = py >= 0 && py <= 199 ? seg_ptr(enh_hr.seg) + py * 320 : NULL;
    const bool hole_row = enh_hr.hole && y >= enh_hr.hole_y;
    s32 px = fdiv(l - enh_hr.fx, n), k = (l - enh_hr.fx) - px * n;   /* the original pixel of x and x's place in it (0..n-1) */
    for (s32 x = l; x <= r; x++) {
        if (!(hole_row && x >= enh_hr.hole_x0 && x <= enh_hr.hole_x1)) {
            const u16 old = row[x];
            u8 v;
            if (old & 0x100) v = (u8)old;
            else v = page && px >= 0 && px <= 319 ? page[px] : 0;
            const u8 nv = is_and ? (u8)(v & al) : (u8)(v | al);
            const u16 code = (old & 0x100) && nv >= 0x60 ? (u16)(old & HR_BLEND_MASK) : 0;
            row[x] = (u16)(0x100 | nv | code);
            if (nrow) {
                const u8 keep = (old & 0x100) ? nrow[x] : 0;
                nrow[x] = keep && keep != HR_NIGHT_LIT ? keep : hr_night_obj(x, y);
            }
        }
        if (++k == n) { k = 0; px++; }
    }
}

void hr_put_span(s32 y, s32 l, s32 r, u8 c) { span_run(y, l, r, c, c, false, false); }

/* ---------------------------------------------------------------- 1359:0719 edge_dda */

static inline bool row_ok(s32 r) { return r >= 0 && r < tab_rows; }
#define TL(r) tab_l[r]
#define TR(r) tab_r[r]

void hr_edge_dda(const HrEdge *e)
{
    if (hr_tracing) { s32 a[4] = { e->x0, e->y0, e->x1, e->y1 }; hr_trace(1, 4, a); }
    s32 ax = e->x0, dx = e->x1, bx = e->y0, sv = e->y1, t;
    if (ax > dx) { t = ax; ax = dx; dx = t; t = sv; sv = bx; bx = t; }
    s32 di = bx;
    dx -= ax;
    s32 bp = 1;
    if (sv < bx) { bp = -1; t = bx; bx = sv; sv = t; }
    if (bx < enh_hr.ymin || bx > enh_hr.ymax || sv < enh_hr.ymin || sv > enh_hr.ymax) return;
    if (!row_ok(bx) || !row_ok(sv)) return;
    if (bx < hr_miny) hr_miny = bx;
    sv -= bx;
    if (sv <= dx) {                                         /* x-major */
        s32 cx = dx + 1, err = -(cx >> 1);
        if (ax < TL(di)) TL(di) = ax;
        for (;;) {
            err += sv;
            if (err >= 0) {
                if (ax > TR(di)) TR(di) = ax;
                err -= dx;
                cx--;
                if (cx == 0) return;
                ax++;
                di += bp;
                if (ax < TL(di)) TL(di) = ax;
            } else {
                ax++;
                cx--;
                if (cx == 0) {
                    ax--;
                    if (ax > TR(di)) TR(di) = ax;
                    return;
                }
            }
        }
    } else {                                                /* y-major */
        s32 cx = sv + 1, err = -(cx >> 1);
        for (;;) {
            if (ax < TL(di)) TL(di) = ax;
            if (ax > TR(di)) TR(di) = ax;
            err += dx;
            if (err >= 0) {
                cx--;
                if (cx == 0) return;
                err -= sv;
                ax++;
                di += bp;
            } else {
                di += bp;
                cx--;
                if (cx == 0) return;
            }
        }
    }
}

/* ---------------------------------------------------------------- 1359:07D6 wall_fill */

void hr_wall_fill(s32 a, s32 b, s32 x)
{
    if (hr_tracing) { s32 t[3] = { a, b, x }; hr_trace(2, 3, t); }
    s32 bx = a, cx = b;
    if (cx < bx) { s32 t = bx; bx = cx; cx = t; }
    if (cx <= enh_hr.ymin || bx >= enh_hr.ymax) return;
    if (bx < enh_hr.ymin) bx = enh_hr.ymin;
    if (cx > enh_hr.ymax) cx = enh_hr.ymax;
    if (cx == bx) return;
    if (bx < hr_miny) hr_miny = bx;
    for (s32 row = bx; row <= cx; row++) {
        if (!row_ok(row)) continue;
        if (x < TL(row)) TL(row) = x;
        if (x > TR(row)) TR(row) = x;
    }
}

/* ---------------------------------------------------------------- 1359:0AF7 fill_M */

typedef struct { s32 row; } Spans;
static bool spans_begin(Spans *s, bool unsigned_check)
{
    s32 r = hr_miny;
    if (r == IDLE_L || (unsigned_check && r < 0)) { hr_miny = IDLE_L; return false; }
    s->row = r;
    return true;
}
static bool spans_next(Spans *s, s32 *row, s32 *l, s32 *r)
{
    for (;;) {
        if (!row_ok(s->row)) { hr_miny = IDLE_L; return false; }
        s32 L = TL(s->row), R = TR(s->row);
        if (L == IDLE_L) { hr_miny = IDLE_L; return false; }
        *row = s->row;
        TL(s->row) = IDLE_L;
        TR(s->row) = IDLE_R;
        s->row++;
        if (!(L > enh_hr.xmax || R < enh_hr.xmin)) {
            *l = L > enh_hr.xmin ? L : enh_hr.xmin;
            *r = R < enh_hr.xmax ? R : enh_hr.xmax;
            return true;
        }
    }
}

void hr_fill(u16 colour)
{
    if (hr_tracing) { s32 t = colour; hr_trace(3, 1, &t); }
    u8 al = (u8)colour, ah = (u8)(colour >> 8);
    Spans sp;
    s32 y, l, r;
    if (ah < 0xFB || ah == 0xFF) {
        if (ah == 0xFF) { put_code = hr_shade_code(colour); ah = al; }   /* S3.5 */
        put_night = hr_night_mode(false, al);                            /* S3.6 */
        if (!spans_begin(&sp, false)) { put_code = 0; put_night = 0; return; }
        while (spans_next(&sp, &y, &l, &r))
            span_run(y, l, r, al, ah, true, false);                       /* S4.3: = hr_put(x, y, phase(x, y) ? ah : al) for x = l..r */
        put_code = 0; put_night = 0;
    } else if (ah == 0xFE || ah == 0xFD) {
        if (!spans_begin(&sp, false)) return;
        while (spans_next(&sp, &y, &l, &r)) span_andor(y, l, r, al, ah == 0xFE);   /* S4.3: a row at a time (the same pixels as hr_get / hr_put per x) */
        put_code = 0; put_night = 0;
    } else if (ah == 0xFC) {
        if (!spans_begin(&sp, true)) return;
        put_night = hr_night_mode(false, al);                            /* S3.6 */
        while (spans_next(&sp, &y, &l, &r))
            span_run(y, l, r, 0, al, true, true);                         /* S4.3: = if (phase(x, y)) hr_put(x, y, al) for x = l..r */
        put_night = 0;
    } else {                                                /* FBh: clear only */
        if (!spans_begin(&sp, false)) return;
        while (spans_next(&sp, &y, &l, &r)) {}
    }
}

/* ---------------------------------------------------------------- 1359:045D line_M */

static void brush(s32 x, s32 y, u8 c)
{
    const int w = enh_hr.line_w, o = enh_hr.line_o;
    for (int ky = 0; ky < w; ky++) {
        s32 py = y + o + ky;
        if (py < enh_hr.ymin || py > enh_hr.ymax) continue;
        for (int kx = 0; kx < w; kx++) {
            s32 px = x + o + kx;
            if (px >= enh_hr.xmin && px <= enh_hr.xmax) hr_put(px, py, c);
        }
    }
}

void hr_draw_line(s32 bx, s32 di, s32 bp, s32 cx, u8 c)
{
    if (hr_tracing) { s32 t[7] = { bx, di, bp, cx, c, enh_hr.line_w, enh_hr.line_o }; hr_trace(4, 7, t); }
    s32 ady = cx - di, adx = bp - bx, t;
    if (ady < 0) ady = -ady;
    if (adx < 0) adx = -adx;
    s32 x, y, n, err;
    if (ady <= adx) {                                       /* x-major */
        if (bp < bx) { t = bx; bx = bp; bp = t; t = di; di = cx; cx = t; }
        s32 step = cx < di ? -1 : 1;
        x = bx; y = di;
        n = bp - bx + 1;
        err = -(n >> 1);
        for (;;) {
            brush(x, y, c);
            x++;
            err += ady;
            if (err >= 0) { y += step; err -= adx; }
            if (--n == 0) return;
        }
    } else {                                                /* y-major */
        if (cx < di) { t = bx; bx = bp; bp = t; t = di; di = cx; cx = t; }
        s32 step = bp < bx ? -1 : 1;
        n = cx - di + 1;
        x = bx; y = di;
        err = -(n >> 1);
        for (;;) {
            brush(x, y, c);
            y++;
            err += adx;
            if (err >= 0) { x += step; err -= ady; }
            if (--n == 0) return;
        }
    }
}

/* S3.5: a 3D line (line_M) in the colour word: a shaded colour carries the object's blend code */
void hr_draw_line_shaded(s32 x0, s32 y0, s32 x1, s32 y1, u16 colour)
{
    put_code = hr_shade_code(colour);
    put_night = hr_night_mode(true, hr_shade_lo(colour));   /* S3.6: a line in its unshaded colour */
    hr_draw_line(x0, y0, x1, y1, (u8)colour);
    put_code = 0; put_night = 0;
}
