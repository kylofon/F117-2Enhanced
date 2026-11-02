/* The hr copy of render3d/r3d_clip.c (VGAME 12EF) plus the projection 11EB:0906 and the edge builder 11EB:0B3F of r3d_object.c
 * (DESIGN.md 2.2, 2.3). Vertices, edge records and the clip scratch live in host memory (int32 coordinates, int64 sums and
 * products); the faithful records in DGROUP are only read (the camera-space vertices C688.., the near-plane point 7D42 / 7D46).
 *
 * Rule R1 here: perspective q = ((v N) >> 8) / zh (small z: ((v N) >> 1) / d); the INT 0 overflow test of 0975 is made on the 1x
 * quotient, and its fallback ((n >> 8) / d) * 256 is taken from the N x numerator; ortho (map) ((x N) >> 16) >> c with the original
 * round-half-up, a 1x 16-bit wrap of the left shift kept as N * the wrapped value; near point ((7D42 N) >> 8) * 2; centre
 * fx + N [85D2]. The fast16 / path32 choice and its overflow tests use the 1x magnitudes (|v| <= 32767 N), so the hr clip takes the
 * same path as the 1x one; every 1x quirk of the paths (bisection out-codes, half_q, a third near-plane point) is kept. */
#include "enhanced/hr.h"

#include <limits.h>
#include <string.h>

#include "enhanced/enhanced.h"
#include "render3d/r3d.h"

typedef struct { s32 x, y; bool behind; } HrVtx;
static HrVtx vtx[256];
static HrEdge edges[256];
static HrEdge closing;                    /* the closing edge, built from the two near-plane points (R3D_CLOSE) */

static struct {
    s32 zclip[2][2];                      /* 8588 / 8590: near-plane points */
    u16 zclip_n, poly_flags, poly_drawn;
    s32 wall_l_lo, wall_l_hi, wall_r_lo, wall_r_hi;
    u8 bisect_last;
    u16 clip_c0, clip_c1;
    s32 clip_dx, clip_dy;
} st;

void hr_clip_regions(void (*add)(void *p, size_t n)) { add(vtx, sizeof vtx); add(edges, sizeof edges); add(&closing, sizeof closing); add(&st, sizeof st); }

#define N (enh_hr.n)

/* the assembler state of a pass starts from the faithful one (R3dWork), scaled */
void hr_clip_begin(void)
{
    R3dWork *w = R3D_W;
    st.zclip_n = w->zclip_n; st.poly_flags = w->poly_flags; st.poly_drawn = w->poly_drawn;
    const s32 ylo = enh_hr.fy, n = N;
    st.wall_l_lo = w->wall_l_lo == 0x7FFF ? INT32_MAX : ylo + n * w->wall_l_lo;
    st.wall_r_lo = w->wall_r_lo == 0x7FFF ? INT32_MAX : ylo + n * w->wall_r_lo;
    st.wall_l_hi = w->wall_l_hi == (s16)0x8000 ? INT32_MIN : ylo + n * w->wall_l_hi;
    st.wall_r_hi = w->wall_r_hi == (s16)0x8000 ? INT32_MIN : ylo + n * w->wall_r_hi;
}

static HrEdge *edge_of(u16 rec)
{
    if (rec == R3D_CLOSE) return &closing;
    u16 k = (u16)((u16)(rec - R3D_EDGES) / 0x18);
    return k < 256 ? &edges[k] : NULL;
}

/* ---------------------------------------------------------------- 11EB:0906 projection */

/* proj_div at N x: the 1x quotient n1 / d decides the INT 0 case (0975), the result comes from the N x numerator nN */
static s64 proj_div_hr(s64 n1, s64 nN, s16 d)
{
    s64 q1 = n1 / d;
    if (q1 >= -32768 && q1 <= 32767) return nN / d;
    s64 f1 = (n1 >> 8) / d;                                  /* 0975: ((n >> 8) / d) * 256, the quotient wrapped to 16 bits */
    if (f1 == (s16)f1) return ((nN >> 8) / d) * 256;
    return (s64)N * (s16)f1 * 256;
}

/* Fine terrain (--fine-terrain): the terrain levels 2..4 round the camera position to their coarse unit (and clamp the height to 2), so at low altitude the coast and the
 * tile edges sit in the wrong place and jump as the aircraft climbs. enh_terr_d is the missing part (Q8 level units, in the argument order x, height, -y); the hr
 * vertices of such an object are moved by it through the camera matrix (the faithful DGROUP vertices stay as they are). */
static bool terr_fix(void) { return enh_terr_d[0] || enh_terr_d[1] || enh_terr_d[2]; }

static void terr_corr(s32 c[3])
{
    c[0] = c[1] = c[2] = 0;
    if (!terr_fix()) return;
    for (int k = 0; k < 3; k++)
        c[k] = (s32)(((s64)enh_terr_d[0] * DSS((u16)(0x7AF8 + 2 * k)) + (s64)enh_terr_d[1] * DSS((u16)(0x7AF8 + 6 + 2 * k)) + (s64)enh_terr_d[2] * DSS((u16)(0x7AF8 + 12 + 2 * k))) >> 8);
}

void hr_project(u16 di, u16 bx)
{
    u16 k = (u16)((u16)(bx - 0xD688) / 8);
    if (k >= 256) return;
    HrVtx *v = &vtx[k];
    s8 m = (s8)DSB(0x7D5E);
    v->behind = false;
    if (m != 0) {                                            /* ortho (the map MFD) */
        s32 out[2];
        for (int i = 0; i < 2; i++) {
            s32 x = DSSL((u16)(di + 4 * i));
            s64 h1 = (s16)(x >> 16), hN = ((s64)x * N) >> 16, r;
            if (m >= 0x10) {
                int c = m & 0x0F;
                r = hN >> c;
                if (c && ((hN >> (c - 1)) & 1)) r++;
            } else {
                int c = (u8)(0x10 - m) & 0x1F;
                if (c >= 16) r = 0;
                else r = hr_keepwrap(h1 << c, hN << c, N);
            }
            out[i] = (s32)r;
        }
        v->x = out[0]; v->y = out[1];
        return;
    }
    s32 cr[3];
    terr_corr(cr);                                           /* ENH: fine terrain */
    const s32 z32 = DSSL((u16)(di + 8)) + cr[2];
    s16 zh = (s16)((u32)z32 >> 16);
    if (zh >= 0x100) {
        s32 a = DSSL(di) + cr[0], b = DSSL((u16)(di + 4)) + cr[1];
        v->x = (s32)proj_div_hr(a >> 8, ((s64)a * N) >> 8, zh);
        v->y = (s32)proj_div_hr(b >> 8, ((s64)b * N) >> 8, zh);
        return;
    }
    if (zh >= 1) {
        s16 d = (s16)(u16)((u16)((u32)z32 >> 8) >> 1);
        s32 a = DSSL(di) + cr[0], b = DSSL((u16)(di + 4)) + cr[1];
        v->x = (s32)proj_div_hr(a >> 1, ((s64)a * N) >> 1, d);
        v->y = (s32)proj_div_hr(b >> 1, ((s64)b * N) >> 1, d);
        return;
    }
    v->behind = true;
}

/* ---------------------------------------------------------------- 12EF clipper */

static u8 outcode16(s64 x, s64 y, bool *ovf)
{
    u8 c = 0;
    if (!hr_fits16(x - enh_hr.wx, N) || !hr_fits16(y - enh_hr.wy, N)) *ovf = true;   /* S3.4: relative to the original window (wx = xmin but in a wide pass) */
    if (x < enh_hr.xmin) c |= 4;
    if (x > enh_hr.xmax) c |= 8;
    if (y < enh_hr.ymin) c |= 1;
    if (y > enh_hr.ymax) c |= 2;
    return c;
}

/* outcode32 of the original is the exact signed compare for a window inside 0..65535 (x < xmin excludes the xmax test) */
static u8 outcode32(s64 x, s64 y)
{
    u8 c = 0;
    if (x < enh_hr.xmin) c |= 4;
    else if (x > enh_hr.xmax) c |= 8;
    if (y < enh_hr.ymin) c |= 1;
    else if (y > enh_hr.ymax) c |= 2;
    return c;
}

static bool on_border(s64 x, s64 y)
{
    if (x < 0 || y < 0) return false;
    return x == enh_hr.xmin || x == enh_hr.xmax || y == enh_hr.ymin || y == enh_hr.ymax;
}

static u8 side_bits(u8 c0, u8 c1)
{
    return (u8)(((c0 & 8) ? 1 : 0) | ((c0 & 4) ? 2 : 0) | ((c1 & 8) ? 4 : 0) | ((c1 & 4) ? 8 : 0));
}

static void flags16(HrEdge *e, u8 c0, u8 c1, s32 y0, s32 y1)
{
    s32 v = y0;
    if (c0 & 1) v = enh_hr.ymin;
    if (c0 & 2) v = enh_hr.ymax;
    e->yc0 = v;
    v = y1;
    if (c1 & 1) v = enh_hr.ymin;
    if (c1 & 2) v = enh_hr.ymax;
    e->yc1 = v;
    e->flags = (u16)(side_bits(c0, c1) * 0x101);
}

static u8 flags32(HrEdge *e, u8 c0, u8 c1)
{
    e->yc0 = (c0 & 1) ? enh_hr.ymin : (c0 & 2) ? enh_hr.ymax : e->y0;
    e->yc1 = (c1 & 1) ? enh_hr.ymin : (c1 & 2) ? enh_hr.ymax : e->y1;
    return side_bits(c0, c1);
}

static int intercept(u8 code, s32 x, s32 y, s32 *ox, s32 *oy, u8 *fail)
{
    *fail = 0;
    if (code == 0) { *ox = x; *oy = y; return 0; }
    s64 dx = st.clip_dx, dy = st.clip_dy, yb;
    if (code & 0xC) {
        s64 xb = (code & 4) ? enh_hr.xmin : enh_hr.xmax;
        s64 yy = (dx ? ((xb - x) * dy) / dx : 0) + y;
        if (yy < enh_hr.ymin) yb = enh_hr.ymin;
        else if (yy > enh_hr.ymax) yb = enh_hr.ymax;
        else { *ox = (s32)xb; *oy = (s32)yy; return 0; }
    } else yb = (code & 1) ? enh_hr.ymin : enh_hr.ymax;
    s64 xx = (dy ? ((yb - y) * dx) / dy : 0) + x;
    *ox = (s32)xx; *oy = (s32)yb;
    if (xx < enh_hr.xmin) { *fail = 0x0A; return 1; }
    if (xx > enh_hr.xmax) { *fail = 0x05; return 1; }
    return 0;
}

typedef struct { s64 x, y; } Pt;

/* 12EF:04EF bisect_in(P0 = *si, P1 = *di) */
static int bisect_in(Pt *si, Pt *di, Pt *m)
{
    Pt a = *di;
    s16 iter = 0x20;
    for (;;) {
        iter--;
        if (iter < 0) return 1;
        Pt b = *si;
        s64 x = (a.x + b.x) >> 1, y = (a.y + b.y) >> 1;
        u8 c = outcode32(x, y);
        st.bisect_last = c;
        if (c == 0) { m->x = x; m->y = y; return 0; }
        if (st.clip_c1 & c) {
            if (st.clip_c0 & c) return 1;
            di->x = x; di->y = y; a = *di;
        } else {
            si->x = x; si->y = y;
            a = *di;
        }
    }
}

/* the half() of 056B: a sum whose lo and hi words are equal is returned unhalved, FFFFFFFFh gives 0 (quirk, kept) */
static s64 half_q(s64 v)
{
    if (v >= INT32_MIN && v <= (s64)UINT32_MAX) {
        u32 u = (u32)v;
        u16 lo = (u16)u, hi = (u16)(u >> 16);
        if (lo == hi) return lo == 0xFFFF ? 0 : v;
    }
    return v >> 1;
}

/* 12EF:056B bisect_out(inside = *si, outside = *di) */
static void bisect_out(Pt *si, Pt *di, Pt *m)
{
    Pt a = *di;
    s16 iter = 0x20;
    for (;;) {
        iter--;
        if (iter < 0) { *m = *si; return; }
        Pt b = *si;
        s64 x = half_q(a.x + b.x), y = half_q(a.y + b.y);
        if (outcode32(x, y)) { di->x = x; di->y = y; a = *di; continue; }
        if (on_border(x, y)) { m->x = x; m->y = y; return; }
        si->x = x; si->y = y;
        a = *di;
    }
}

static bool fast16(HrEdge *e)
{
    bool ovf = false;
    s32 x0 = e->x0, y0 = e->y0, x1 = e->x1, y1 = e->y1;
    u8 c1 = outcode16(x1, y1, &ovf);
    u8 c0 = outcode16(x0, y0, &ovf);
    if (c0 == 0 && c1 == 0) { e->flags = 0; return true; }
    if (c0 & c1) {
        if (c0 & c1 & 3) { e->flags = 0x8080; return true; }
        flags16(e, c0, c1, y0, y1);
        e->flags = (c0 & 8) ? 0x8585 : 0x8A8A;
        return true;
    }
    flags16(e, c0, c1, y0, y1);
    s64 d = (s64)x1 - x0;
    if (!hr_fits16(d, N)) return false;
    st.clip_dx = (s32)d;
    d = (s64)y1 - y0;
    if (!hr_fits16(d, N)) return false;
    st.clip_dy = (s32)d;
    if (ovf) return false;
    s32 x, y;
    u8 f;
    if (intercept(c0, x0, y0, &x, &y, &f)) { e->flags = (u16)((f | 0x80) * 0x101); return true; }
    e->x0 = x; e->y0 = y;
    if (intercept(c1, e->x1, e->y1, &x, &y, &f)) { e->flags = (u16)((f | 0x80) * 0x101); return true; }
    e->x1 = x; e->y1 = y;
    return true;
}

static void path32(HrEdge *e)
{
    s64 x0 = e->x0, y0 = e->y0, x1 = e->x1, y1 = e->y1;
    u8 c1 = outcode32(x1, y1);
    u8 c0 = outcode32(x0, y0);
    if (c0 == 0 && c1 == 0) { e->flags = 0; return; }
    if (c0 & c1) {
        if (c0 & c1 & 3) e->flags = 0x8080;
        else e->flags = (u16)((flags32(e, c0, c1) | 0x80) * 0x101);
        return;
    }
    Pt a0 = { x0, y0 }, a1 = { x1, y1 }, b0 = a0, b1 = a1;      /* 8568 / 8570 and the copy 8578 / 8580 */
    u8 al = flags32(e, c0, c1);
    e->flags = (u16)(al * 0x101);
    Pt p;
    if (c1 == 0) { bisect_out(&a1, &a0, &p); e->x0 = (s32)p.x; e->y0 = (s32)p.y; return; }
    if (c0 == 0) { bisect_out(&a0, &a1, &p); e->x1 = (s32)p.x; e->y1 = (s32)p.y; return; }
    st.clip_c0 = c0; st.clip_c1 = c1;
    Pt m;
    if (bisect_in(&a0, &a1, &m)) { e->flags = (u16)((st.bisect_last & 4 ? 0x8A : 0x85) * 0x101); return; }
    a0 = m;
    bisect_out(&a0, &b0, &p);
    e->x0 = (s32)p.x; a0 = p; e->y0 = (s32)p.y;
    bisect_out(&a0, &b1, &p);
    e->x1 = (s32)p.x; e->y1 = (s32)p.y;
}

/* 12EF:0333 clip_edge */
void hr_clip_edge(HrEdge *e)
{
    if (hr_fits16(e->x0, N) && hr_fits16(e->x1, N) && hr_fits16(e->y0, N) && hr_fits16(e->y1, N) && fast16(e)) return;
    path32(e);
}

/* ---------------------------------------------------------------- 11EB:0B3F make_edge */

/* 11EB:0990 r3d_near_bisect on the corrected camera-space vertices (fine terrain): the near-plane point of the edge from the front vertex p0o to the one behind, p1o */
static void near_bisect_fine(u16 p0o, u16 p1o, const s32 cr[3], s32 *ox, s32 *oy)
{
    s32 p0[3], p1[3], m[3];
    for (int c = 0; c < 3; c++) { p0[c] = DSSL((u16)(p0o + 4 * c)) + cr[c]; p1[c] = DSSL((u16)(p1o + 4 * c)) + cr[c]; }
    for (;;) {
        for (int c = 0; c < 3; c++) m[c] = (s32)((u32)p0[c] + (u32)p1[c] + 1u) >> 1;
        *ox = m[0]; *oy = m[1];
        u16 h = (u16)((u32)m[2] >> 16);
        if (h <= 1) return;
        if ((s16)(h - 1) < 0) memcpy(p1, m, sizeof m);
        else memcpy(p0, m, sizeof m);
    }
}

void hr_make_edge(u16 rec, u16 v0x8, u16 v1x8)
{
    HrEdge *e = edge_of(rec);
    if (!e || v0x8 / 8 >= 256 || v1x8 / 8 >= 256) return;
    const HrVtx *p0 = &vtx[v0x8 / 8], *p1 = &vtx[v1x8 / 8];
    const s32 cx = enh_hr.cx, cy = enh_hr.cy;
    if (!p0->behind && !p1->behind) {
        e->x0 = p0->x + cx; e->y0 = p0->y + cy;
        e->x1 = p1->x + cx; e->y1 = p1->y + cy;
        hr_clip_edge(e);
        return;
    }
    if (p0->behind && p1->behind) { e->flags = 0x8080; return; }
    if (p1->behind) { e->x0 = p0->x + cx; e->y0 = p0->y + cy; }
    else { e->x1 = p1->x + cx; e->y1 = p1->y + cy; }
    s32 b0 = DSSL(0x7D42), b1 = DSSL(0x7D46);                  /* r3d_near_bisect has just left the near-plane point in 7D42 / 7D46 */
    if (terr_fix()) {                                          /* ENH: fine terrain, the point of the corrected vertices */
        s32 cr[3];
        terr_corr(cr);
        const u16 V0 = (u16)(0xC688 + 2 * v0x8), V1 = (u16)(0xC688 + 2 * v1x8);
        if (p1->behind) near_bisect_fine(V0, V1, cr, &b0, &b1);
        else near_bisect_fine(V1, V0, cr, &b0, &b1);
    }
    s32 nx = (s32)((((s64)b0 * N) >> 8) * 2 + cx);
    s32 ny = (s32)((((s64)b1 * N) >> 8) * 2 + cy);
    if (p1->behind) { e->x1 = nx; e->y1 = ny; } else { e->x0 = nx; e->y0 = ny; }
    e->nx = nx; e->ny = ny;
    hr_clip_edge(e);
    e->flags |= 0x4040;
}

void hr_edge_point(u16 rec, s32 *x, s32 *y)
{
    const HrEdge *e = edge_of(rec);
    if (e) { *x = e->x0; *y = e->y0; }
}

/* ---------------------------------------------------------------- 12EF assembler */

static void note(s32 *lo, s32 *hi, s32 a, s32 b)
{
    if (b < a) { s32 t = a; a = b; b = t; }
    if (b >= *hi) *hi = b;
    if (a < *lo) *lo = a;
}
static void note_left(s32 a, s32 b) { note(&st.wall_l_lo, &st.wall_l_hi, a, b); }
static void note_right(s32 a, s32 b) { note(&st.wall_r_lo, &st.wall_r_hi, a, b); }

static void add_edge(HrEdge *e)
{
    u16 f = e->flags;
    st.poly_flags |= f;
    u8 al = (u8)f;
    if (al & 0x40) {                                         /* 12EF:00E0: a third point overwrites the second (quirk) */
        int k = st.zclip_n == 0 ? 0 : 1;
        st.zclip_n++;
        st.zclip[k][0] = e->nx; st.zclip[k][1] = e->ny;
    }
    if (al & 0x80) {
        if (al & 3) ((al & 2) ? note_left : note_right)(e->yc0, e->yc1);
        return;
    }
    st.poly_drawn |= 4;
    hr_edge_dda(e);
    al = (u8)e->flags;
    if (al & 3) ((al & 2) ? note_left : note_right)(e->y0, e->yc0);
    if (al & 0xC) ((al & 8) ? note_left : note_right)(e->y1, e->yc1);
}

void hr_poly_add_edge(u16 rec)
{
    if (rec == R3D_CLOSE) return;                            /* hr_poly_finish adds its own closing edge */
    HrEdge *e = edge_of(rec);
    if (e) add_edge(e);
}

void hr_poly_finish(void)
{
    if (st.zclip_n == 2) {
        closing.x0 = st.zclip[0][0]; closing.y0 = st.zclip[0][1];
        closing.x1 = st.zclip[1][0]; closing.y1 = st.zclip[1][1];
        closing.nx = closing.x0; closing.ny = closing.y0;     /* rec+10h of 8588 is 8598, never flagged 40h: unused */
        hr_clip_edge(&closing);
        add_edge(&closing);
    }
    u16 f = st.poly_flags;
    bool fill = false;
    if (st.poly_drawn) {
        if (f & 5) hr_wall_fill(st.wall_r_hi, st.wall_r_lo, enh_hr.xmax + 1);
        if (f & 0xA) hr_wall_fill(st.wall_l_hi, st.wall_l_lo, enh_hr.xmin - 1);
        fill = true;
    } else if ((f & 5) && (f & 0xA)) {
        hr_wall_fill(st.wall_r_hi, st.wall_r_lo, enh_hr.xmax + 1);
        hr_wall_fill(st.wall_l_hi, st.wall_l_lo, enh_hr.xmin - 1);
        fill = true;
    }
    if (fill) hr_fill(R3D_W->gfx_colour);
    st.wall_r_lo = st.wall_l_lo = INT32_MAX;
    st.wall_r_hi = st.wall_l_hi = INT32_MIN;
    st.poly_drawn = 0; st.poly_flags = 0; st.zclip_n = 0;
}

/* 1359:045D line_M of a clipped, not rejected edge (the face list tests 80h itself) */
void hr_line(u16 rec)
{
    HrEdge *e = edge_of(rec);
    if (!e || (e->flags & 0x80)) return;
    hr_draw_line_shaded(e->x0, e->y0, e->x1, e->y1, R3D_W->gfx_colour);   /* S3.5: with the blend code of a shaded colour */
}
