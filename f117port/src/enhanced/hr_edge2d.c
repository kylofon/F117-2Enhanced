/* The hr copy of render3d/edge2d.c (VGAME 1001: clip core, edge tables, polygon fill) and of the sky / ground backdrop 1442
 * (render3d/r3d_sky.c, hz_draw), for the backdrop of a 3D pass (DESIGN.md 2.2, 2.3). Coordinates are window-relative as in 1001
 * (layer = ox + x, oy + y); XMAX / YMAX are N W - 1 / N H - 1; tables have YMAX + 1 rows (int32, L idle INT32_MAX, R idle 0).
 *
 * R1 here: the band and horizon points use fx_mul15 / hz_rnd with N inside the rounding ((a b N + 4000h) >> 15), w2 / h2 -> N w2 / N h2.
 * A point whose 1x value wraps at 16 bits (the band quads reach +-45000 at steep roll) is used as N * its wrapped 1x value, so the
 * picture is the 1x one scaled there. The divide overflow of the intercept (INT 0 -> +-7F00h) and the halving of an overflowing
 * delta are tested at the 1x magnitude (|v| > 32767 N) and saturate to N * 7F00h. A single-pixel span at column 0 or 13Fh is skipped
 * (slot 37 quirk), 13Fh read as the last layer column of frame column 319, N 320 - 1. */
#include "enhanced/hr.h"

#include <limits.h>
#include <stdlib.h>

#include "render3d/r3d.h"
#include "sim/sim.h"
#include "gen/module_video.h"

#define N (enh_hr.n)
#define XMAX ((s64)enh_hr.exmax)
#define YMAX ((s64)enh_hr.eymax)
#define IDLE_LO INT32_MAX

static s32 *tl, *tr;
static int trows;
static s32 dlo = IDLE_LO, dhi;                      /* the dirty rows 2615 / 2617 */
static struct { s64 x1, y1, x2, y2; } sg;           /* 2299 / 229D / 229B / 229F */
void hr_edge2d_regions(void (*add)(void *p, size_t n)) { add(&dlo, sizeof dlo); add(&dhi, sizeof dhi); add(&sg, sizeof sg); }

static s64 wrapn(s64 v) { return hr_fits16(v, N) ? v : (s64)N * (s16)(v / N); }

static bool tabs_ready(void)
{
    int rows = enh_hr.eymax + 1;
    if (rows < 1) return false;
    if (rows != trows || !tl) {
        free(tl); free(tr);
        tl = malloc(sizeof(s32) * (size_t)rows); tr = malloc(sizeof(s32) * (size_t)rows);
        if (!tl || !tr) { free(tl); free(tr); tl = tr = NULL; trows = 0; return false; }
        trows = rows;
        for (int i = 0; i < rows; i++) { tl[i] = IDLE_LO; tr[i] = 0; }
        dlo = IDLE_LO; dhi = 0;
    }
    return true;
}

static u8 outcode(s64 x, s64 y)
{
    u8 al = 0x0F;
    if (x >= 0) al &= (u8)~8;
    if (x <= XMAX) al &= (u8)~1;
    if (y >= 0) al &= (u8)~4;
    if (y <= YMAX) al &= (u8)~2;
    return al;
}

static s64 rdiv(s64 num, s64 D, s64 h)
{
    bool neg = (num < 0) != (D < 0);
    s64 q, r;
    if (D == 0 || !hr_fits16(num / D, N)) { q = neg ? -0x7F00LL * N : 0x7F00LL * N; r = 0; }
    else { q = num / D; r = num % D; }
    if (neg) { r = -r; q = wrapn(q - 1); }
    r = wrapn(r - h);
    if ((r < 0) == (num < 0)) q = wrapn(q + 1);
    return q;
}

static void e2_tables_clear(void)
{
    if (dlo == IDLE_LO) return;
    for (s32 y = dlo; y <= dhi && y < trows; y++) if (y >= 0) { tl[y] = IDLE_LO; tr[y] = 0; }
    dlo = IDLE_LO; dhi = 0;
}

static void e2_edge_column(s64 x, s64 ya, s64 yb)
{
    s32 *tbl;
    if (x == 0) tbl = tl;
    else if (x == XMAX) tbl = tr;
    else return;
    s64 hi = ya, lo = yb;
    if (hi < lo) { s64 t = hi; hi = lo; lo = t; }
    if (lo < 0) lo = 0;
    if (hi > YMAX) hi = YMAX;
    if (hi < lo) return;                            /* both rows beyond one border: never in the game (the original would run wild) */
    if (hi > dhi) dhi = (s32)hi;
    if (lo < dlo) dlo = (s32)lo;
    for (s64 y = lo; y <= hi; y++) tbl[y] = (s32)x;
}

static void e2_edge_column_x(s64 x, s64 ya, s64 yb)
{
    if (x < 0) e2_edge_column(0, ya, yb);
    else if (x > XMAX) e2_edge_column(XMAX, ya, yb);
}

typedef struct { s64 cx, bp, di, x; u8 al, oc; } ClipOut;

static int e2_clip_core(bool edge, ClipOut *o)
{
    s64 cx = sg.x1, dx = sg.y1, si = sg.x2, di = sg.y2, bp, bx;
    u8 oc = outcode(cx, dx);
    u8 al = outcode(si, di);
    if (al == 0) {
        if (oc == 0) return 0;
        s64 t;
        t = si; si = cx; cx = t;
        t = di; di = dx; dx = t;
        u8 tmp = oc; oc = al; al = tmp;
        sg.x1 = cx; sg.y1 = dx;
    }
    bp = dx;
    if (oc & al) { o->cx = cx; o->di = di; o->bp = bp; o->al = al; o->oc = oc; return 1; }
    s64 Dx, Dy, hx, hy;
    {
        s64 tdx = si - cx, tdy = di - bp;
        bool oxf = !hr_fits16(tdx, N), oyf = !hr_fits16(tdy, N);
        if (oxf) { Dx = tdx >> 1; hx = Dx >> 1; Dy = tdy >> 1; }
        else {
            Dx = tdx; hx = Dx >> 1;
            if (oyf) { Dy = tdy >> 1; Dx >>= 1; hx >>= 1; }
            else Dy = tdy;
        }
        hy = Dy >> 1;
    }
    for (;;) {
        s64 rx, ry, x;
        if (al & 9) {
            bx = si < 0 ? 0 : XMAX;
            s64 y = wrapn(rdiv(wrapn(bx - cx) * Dy, Dx, hx) + bp);
            if (y >= 0 && y <= YMAX) { rx = bx; ry = y; goto got; }
            if (y < 0) bx = 0;
            else bx = di > YMAX ? YMAX : 0;
        } else {
            bx = di > YMAX ? YMAX : 0;
        }
        x = wrapn(rdiv(wrapn(bx - bp) * Dx, Dy, hy) + cx);
        if (x < 0 || x > XMAX) { o->cx = cx; o->di = di; o->bp = bp; o->x = x; o->al = al; return 2; }
        rx = x; ry = bx;
    got:
        if (edge) e2_edge_column(rx, ry, di);
        if (oc == 0) { sg.y2 = ry; sg.x2 = rx; return 0; }
        sg.y1 = ry;
        sg.x1 = rx;
        { s64 t = si; si = cx; cx = t; t = di; di = bp; bp = t; }
        al = oc;
        oc = 0;
    }
}

static void e2_edge_walk(void)
{
    if (sg.x2 < sg.x1) {
        s64 t = sg.x2; sg.x2 = sg.x1; sg.x1 = t;
        t = sg.y2; sg.y2 = sg.y1; sg.y1 = t;
    }
    s64 DX = sg.x2 - sg.x1, dyv = sg.y2 - sg.y1, yhi = sg.y2, ylo = sg.y1;
    int step = 1;
    if (dyv < 0) { s64 t = yhi; yhi = ylo; ylo = t; step = -1; dyv = -dyv; }
    s64 dy = dyv;
    if (yhi > dhi) dhi = (s32)yhi;
    if (ylo < dlo) dlo = (s32)ylo;
    s64 o = sg.y1, x = sg.x1;
    if (o < 0 || o >= trows || o + step * dy < 0 || o + step * dy >= trows) return;   /* clipped: never */
    if (dy < DX) {
        s64 err = -((DX + 1) >> 1), n = DX;
        if (x < tl[o]) tl[o] = (s32)x;
        for (;;) {
            n--;
            if (n < 0) { if (x > tr[o]) tr[o] = (s32)x; return; }
            err += dy;
            if (err >= 0) {
                if (x > tr[o]) tr[o] = (s32)x;
                x++;
                err -= DX;
                o += step;
                if (x < tl[o]) tl[o] = (s32)x;
            } else x++;
        }
    } else {
        s64 err = -((dy + 1) >> 1), n = dy;
        for (;;) {
            if (x < tl[o]) tl[o] = (s32)x;
            if (x > tr[o]) tr[o] = (s32)x;
            n--;
            if (n < 0) return;
            o += step;
            err += DX;
            if (err < 0) continue;
            err -= dy;
            x++;
        }
    }
}

static void e2_edge_to_tables(void)
{
    ClipOut o;
    int r = e2_clip_core(true, &o);
    if (r == 0) { e2_edge_walk(); return; }
    if (r == 1) {
        if ((o.al & o.oc) & 6) return;
        e2_edge_column_x(o.cx, o.di, o.bp);
        return;
    }
    e2_edge_column_x(o.x, o.di, o.bp);
}

/* slot 37 mode 0 on the tables, rows ymax down to ymin */
static void fill(u8 c)
{
    if (dlo == IDLE_LO || dlo < 0) return;
    const s64 last = enh_hr.wide ? XMAX : (s64)N * 0x140 - 1;          /* S3.4: in a wide pass the quirk's column 13Fh is the canvas edge */
    s32 y = dhi;
    do {
        if (y >= 0 && y < trows) {
            s64 L = tl[y], R = tr[y];
            if (!(R < L || (R == L && (R == 0 || R == last)))) {
                s32 py = enh_hr.oy + y;
                if (py >= 0 && py < enh_hr.lh) {                          /* S4.3: one span (= hr_put of each x with 0 <= ox + x < lw) */
                    s64 a = enh_hr.ox + L, b = enh_hr.ox + R;
                    if (a < 0) a = 0;
                    if (b > enh_hr.lw - 1) b = enh_hr.lw - 1;
                    if (a <= b) hr_put_span(py, (s32)a, (s32)b, c);
                }
            }
        }
        y--;
    } while (y >= dlo);
}

static void poly_fill_hr(int n, const s64 *pts, u8 colour)
{
    if (hr_tracing) hr_trace_poly(enh_hr.ox, enh_hr.oy, colour, n, pts);
    e2_tables_clear();
    sg.x1 = pts[0]; sg.y1 = pts[1];
    for (int k = 1; k < n; k++) {
        sg.x2 = pts[2 * k]; sg.y2 = pts[2 * k + 1];
        e2_edge_to_tables();
        sg.x1 = pts[2 * k]; sg.y1 = pts[2 * k + 1];
    }
    sg.x2 = pts[0]; sg.y2 = pts[1];
    e2_edge_to_tables();
    fill(colour);
}

/* ---------------------------------------------------------------- 1442 bands */

static s64 m15(s64 a, s64 b, int n) { return (a * b * n + 0x4000) >> 15; }

/* band_points of r3d_sky.c at 1x (the original s16 code), exact at 1x, and at N x; per coordinate the R1 wrap rule */
static void band_points_hr(s16 t, s64 out[4])
{
    s16 nsC = (s16)DSW(0x98C8), cC = (s16)DSW(0x498C), w2 = (s16)DSW(0x0D1E), h2 = (s16)DSW(0x0D20);
    const s16 A = (s16)0xA7B8;
    s16 si = (s16)fx_mul15(cC, A), di = (s16)fx_mul15(nsC, t);
    s16 v[4];
    v[0] = (s16)(w2 - di + si);
    v[2] = (s16)(w2 - di - si);
    s16 si2 = (s16)fx_mul15(cC, t), di2 = (s16)fx_mul15(nsC, A);
    s16 s = (s16)(di2 + si2);
    v[1] = (s16)((s >> 2) - di2 - si2 + h2);
    s16 u = (s16)(si2 - di2);
    v[3] = (s16)((u >> 2) - si2 + h2 + di2);
    for (int pass = 0; pass < 2; pass++) {               /* 0: exact at 1x, 1: at N x */
        int n = pass ? N : 1;
        s64 Si = m15(cC, A, n), Di = m15(nsC, t, n), Si2 = m15(cC, t, n), Di2 = m15(nsC, A, n);
        s64 e[4];
        e[0] = (s64)n * w2 - Di + Si;
        e[2] = (s64)n * w2 - Di - Si;
        e[1] = ((Di2 + Si2) >> 2) - Di2 - Si2 + (s64)n * h2;
        e[3] = ((Si2 - Di2) >> 2) - Si2 + (s64)n * h2 + Di2;
        for (int k = 0; k < 4; k++) {
            if (pass == 0) out[k] = e[k] == v[k] ? 0 : 1;        /* 1 = the 1x value wrapped somewhere */
            else out[k] = (out[k] ? (s64)N * v[k] : e[k]) + ((k & 1) ? enh_hr.e2dy : enh_hr.e2dx);   /* S3.4: into the wide window (0 otherwise) */
        }
    }
}

static void bands_hr(s16 H, const s16 *off, bool ground, bool flag)
{
    s64 P[17][4];
    for (int i = 0; i <= 16; i++) {
        s16 t;
        if (ground) t = i < 16 ? (s16)(H - off[i]) : (s16)0xA7B8;
        else t = i < 16 ? (s16)(off[i] + H) : 0x5848;
        band_points_hr(t, P[i]);
    }
    for (int i = 0; i < 16; i++) {
        s64 pts[8] = { P[i][0], P[i][1], P[i][2], P[i][3], P[i + 1][2], P[i + 1][3], P[i + 1][0], P[i + 1][1] };
        int col = flag ? (ground ? 0x7C : 0x88) : (ground ? 0x70 : 0x80) + i;
        poly_fill_hr(4, pts, (u8)col);
    }
}

/* ---------------------------------------------------------------- 1001:0018 hz_draw */

static s64 rnd16(s64 v, int n) { v *= n; return (v >> 16) + ((v >> 15) & 1); }
static u16 hz_rnd(u32 v) { return (u16)((v >> 16) + ((v >> 15) & 1)); }

static void hz_draw_hr(void)
{
    if (hr_tracing) hr_trace(6, 0, NULL);
    u16 nsC = (u16)-DSW(0x498A);
    s16 sB = (s16)DSW(0x4986), cB = (s16)DSW(0x4988), cC = (s16)DSW(0x498C);
    u16 H;
    if (cB > 0x1F0B) H = (u16)(s16)(((s32)sB * 256) / cB);
    else H = 0x3FF;
    u8 z = DSB(0x433A);
    if (z) H = (z & 0x1F) < 16 ? (u16)(H << (z & 0x1F)) : 0;
    if (DSB(0x294B)) H = (u16)((s16)H >> 1);
    u16 w2 = DSW(0x0D1E), h2 = DSW(0x0D20);
    s16 half = (s16)(w2 << 1);                           /* the half length of the horizon line: 2 w2 */
    if (enh_hr.wide) {                                   /* S3.4: at least the canvas half diagonal (1x px) around the centre, so the line spans the canvas */
        s32 ax = enh_hr.cx > enh_hr.lw - enh_hr.cx ? enh_hr.cx : enh_hr.lw - enh_hr.cx, ay = enh_hr.cy > enh_hr.lh - enh_hr.cy ? enh_hr.cy : enh_hr.lh - enh_hr.cy;
        s32 need = (ax + ay) / N + 2;
        if (need > half) half = (s16)(need < 0x3000 ? need : 0x3000);
    }
    /* the 1x points exactly as edge2d.c hz_draw */
    u32 P1 = (u32)((s32)(s16)H * (s16)nsC) << 1, P2 = (u32)((s32)half * cC) << 1;
    u32 Q1 = (u32)((s32)(s16)H * cC) << 1, Q2 = (u32)((s32)half * (s16)nsC) << 1;
    s16 v[4];
    v[0] = (s16)(u16)(hz_rnd(P2 - P1) + w2);
    v[2] = (s16)(u16)(w2 - hz_rnd(P1 + P2));
    u16 r = hz_rnd(Q2 - Q1);
    v[3] = (s16)(u16)(r - (u16)((s16)r >> 2) + h2);
    u16 s = hz_rnd(Q1 + Q2);
    v[1] = (s16)(u16)(h2 - (u16)(s - (u16)((s16)s >> 2)));
    /* exact at 1x and at N x */
    s64 p1 = (s64)(s16)H * (s16)nsC * 2, p2 = (s64)half * cC * 2;
    s64 q1 = (s64)(s16)H * cC * 2, q2 = (s64)half * (s16)nsC * 2;
    s64 pt[4];
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? N : 1;
        s64 e[4], rr = rnd16(q2 - q1, n), ss = rnd16(q1 + q2, n);
        e[0] = rnd16(p2 - p1, n) + (s64)n * (s16)w2;
        e[2] = (s64)n * (s16)w2 - rnd16(p1 + p2, n);
        e[3] = rr - (rr >> 2) + (s64)n * (s16)h2;
        e[1] = (s64)n * (s16)h2 - (ss - (ss >> 2));
        for (int k = 0; k < 4; k++) {
            if (pass == 0) pt[k] = e[k] == v[k] ? 0 : 1;
            else pt[k] = (pt[k] ? (s64)N * v[k] : e[k]) + ((k & 1) ? enh_hr.e2dy : enh_hr.e2dx);   /* S3.4 */
        }
    }
    u8 colour = DSB(0x0D46);
    e2_tables_clear();
    sg.x1 = pt[0]; sg.x2 = pt[2]; sg.y2 = pt[3]; sg.y1 = pt[1];
    ClipOut o;
    bool rejected = e2_clip_core(false, &o) != 0;
    if (!rejected) {                                     /* the horizon line itself: 1 px (the fills cover it; a wide brush would poke past them) */
        int w = enh_hr.line_w, wo = enh_hr.line_o;
        enh_hr.line_w = 1; enh_hr.line_o = 0;
        hr_draw_line((s32)(enh_hr.ox + sg.x1), (s32)(enh_hr.oy + sg.y1), (s32)(enh_hr.ox + sg.x2), (s32)(enh_hr.oy + sg.y2), colour);
        enh_hr.line_w = w; enh_hr.line_o = wo;
    }
    s64 y1 = sg.y1, y2 = sg.y2;
    if (rejected || (y1 == y2 && (y1 == 0 || y1 == YMAX))) {
        if (!(DSW(0x4986) & 0x8000)) colour = 0x78;
        e2_edge_column(0, 0, YMAX);
        e2_edge_column(XMAX, 0, YMAX);
        fill(colour);
        return;
    }
    for (int pass = 0;; pass++) {
        s64 sv[4] = { sg.x1, sg.y1, sg.x2, sg.y2 };
        e2_edge_to_tables();
        sg.x1 = sv[0]; sg.y1 = sv[1]; sg.x2 = sv[2]; sg.y2 = sv[3];
        u16 d48 = pass ? 0xFFFF : 0;
        s64 corner = ((d48 ^ nsC) & 0x8000) ? XMAX : 0;
        s64 bx = 0, bp = YMAX;
        if ((d48 ^ DSW(0x498C)) & 0x8000) { s64 t = bx; bx = bp; bp = t; }
        s64 pxy[2][2] = { { sg.x1, sg.y1 }, { sg.x2, sg.y2 } };
        for (int k = 0; k < 2; k++) {
            s64 px = pxy[k][0], py = pxy[k][1];
            if (py == bx) continue;
            if (py == bp) px = corner;
            e2_edge_column(px, py, bx);
        }
        fill(colour);
        if (pass) return;
        colour = 0x78;
        e2_tables_clear();
    }
}

/* ---------------------------------------------------------------- 1442 r3d_draw_sky_ground, after the faithful one */

void hr_sky_ground(void *ctx)
{
    (void)ctx;
    if ((u16)seg_get() != enh_hr.seg) return;           /* 1001 draws on MGRAPHIC's current page (the TV camera's may be another): stamped */
    if (!tabs_ready()) return;
    s16 cB = (s16)DSW(0x4988);
    if (cB < 0x200) cB = 0x200;
    s16 H = (s16)(((s32)(s16)DSW(0x4986) * 256) / cB);
    u8 z = DSB(0x433A);
    if (z) H = (z & 0x1F) < 16 ? (s16)(u16)((u16)H << (z & 0x1F)) : 0;
    if (DSB(0x294B)) H >>= 1;
    if ((s8)DSB(0x98DE) < 3) { hz_draw_hr(); hr_night_backdrop(H); /* S3.6 */ return; }
    bool flag = DSW(0x4360) && DSW(0x43B2);
    s16 sky[16], gnd[16];
    for (int i = 0; i < 16; i++) sky[i] = (s16)DSW(0x0D22 + 2 * i);
    bands_hr(H, sky, false, flag);
    s16 alt = (s16)DSW(0x4980);
    u16 c0 = (u16)(alt / 0x200);
    gnd[0] = 0;
    for (int i = 1; i < 16; i++) gnd[i] = (s16)(u16)((u16)(alt / ((16 - i) << 5)) - c0);
    bands_hr(H, gnd, true, flag);
    if (!flag) hr_band_blend(H, sky, gnd);               /* S3.5: the position of each band pixel inside its band (--smooth-shading) */
    if (!flag) hr_night_backdrop(H);                     /* S3.6: the sky / ground night factors (--true-night) */
}
