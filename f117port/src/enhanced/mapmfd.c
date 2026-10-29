/* The map MFD between sim steps (mapmfd.h; DESIGN.md 4.11; S4.6). */
#include "enhanced/mapmfd.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "enhanced/enh_canvas.h"
#include "enhanced/enhanced.h"
#include "enhanced/interp.h"
#include "enhanced/sandbox.h"
#include "gen/module_render3d.h"
#include "gen/module_simflow.h"
#include "mem.h"
#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "video/video.h"

bool enh_map_log_on;

enum { OP_PLOT, OP_KEY, OP_OWN, OP_ROUTE, OP_ARC };
enum { MAX_OPS = 256, JUMP_CELLS_OWN = 32 };   /* the own aircraft's jump limit of interp.c (1024 world units) in cells */
/* the map window (74, 110) 77 x 60 (mfd_init_geometry), its centre (112, 139) (mfd_map_sx / sy); the cache's margin and the statics' clip margin (pixels) */
enum { WX0 = 0x4A, WY0 = 0x6E, WW = 0x4D, WH = 0x3C, CX = 0x70, CY = 0x8B, MARGIN = 24, SMARGIN = 4, CEDGE = 8 };   /* CEDGE: the cache's rim not shown (its polygons are clipped there) */

typedef struct { u8 kind; s8 cls; s16 slot; int a[9]; } Op;   /* a: plot wx, wy, c, big; key dx, dy, sx, sy, w, h, clip, wx, wy; arc cx, cy, r, c, solid, a0, a1 */
typedef struct { bool valid; unsigned seq; s32 x, y; u16 cx, cy, hdg; double tx, ty; } Own;   /* the aircraft marker's step: BA10 / C02A, C0A4 / C0B2, 2DEE, the true cells */

static Op bl[MAX_OPS], fl[MAX_OPS];   /* the step being logged, this frame's log (the step k-1 that A_k shows) */
static int bn, fn;
static bool bstarted, bover, fvalid, active, own_ok;
static int arc_depth;
static s8 tag_cls = -1;
static s16 tag_slot;
static Own bown, own_cur, own_prev;
static unsigned seq;
static bool want_check;
static u64 bg_hash;                   /* the backup page's map window (the original's terrain + site dots): a change = the original redrew it */

/* the terrain cache: the map window grown by MARGIN on every side, rendered (map_render_terrain + the site dots, as 8387 draws them) at the cells (ca, cb) */
static EnhMapCache cache;
static bool cache_ok;
static int cache_z, cache_n;
static u16 cache_a, cache_b;
static u64 cache_hash, cache_site;
static u64 site_hash;                 /* the part of bg_hash the site dots depend on */
static u64 prev_site;
static EnhMapCache prev_cache;        /* test aid: the cache before, for the overlap check */
static bool prev_ok;
static int prev_z;
static u16 prev_a, prev_b;

static unsigned long long st_ns, st_passes, st_build_ns, st_builds;   /* the per-pass cost, the cache renders */
static unsigned long long st_frames, st_active, st_own, st_own_held, st_blips, st_blips_lerp, st_tag_bad, st_over, st_chk, st_chk_bad;
static unsigned long long st_recentre;
static unsigned long long st_cov, st_uncov, st_outside, st_scope;   /* fix 2: passes checked, window pixels beyond the world, pixels changed outside the window */
static bool pass_disp, scope_on;   /* fix 1 */
static double pass_alpha = 1.0;   /* the original map moved (39C4 / 8387 recentred it) while the scrolled map was shown */
static unsigned long long st_tchk, st_tchk_px, st_tchk_edge, st_tchk_max, st_ovl, st_ovl_px, st_ovl_max;

/* ---------------------------------------------------------------- the log (live, in the step) */

static bool logging(void) { return enh_map_log_on && !enh_sbx; }

static Op *op_new(int kind)
{
    bstarted = true;
    if (bn >= MAX_OPS) { bover = true; return NULL; }
    Op *o = &bl[bn++];
    memset(o, 0, sizeof *o);
    o->kind = (u8)kind;
    o->cls = -1;
    return o;
}

void enh_map_log_reset(void)
{
    if (!logging()) return;
    bn = 0; bover = false; bstarted = true; bown.valid = false; arc_depth = 0; tag_cls = -1;
}

void enh_map_tag(int cls, int slot)
{
    if (!logging()) return;
    tag_cls = (s8)cls; tag_slot = (s16)slot;
}

void enh_map_log_plot(int wx, int wy, int c, int big)
{
    if (!logging()) return;
    const s8 cls = tag_cls;
    tag_cls = -1;
    if (arc_depth > 0) return;
    Op *o = op_new(OP_PLOT);
    if (!o) return;
    o->a[0] = (u16)wx; o->a[1] = (u16)wy; o->a[2] = c; o->a[3] = big;
    o->cls = cls; o->slot = tag_slot;
}

void enh_map_log_route(void)
{
    if (!logging() || arc_depth > 0) return;
    op_new(OP_ROUTE);
}

void enh_map_log_arc(int cx, int cy, int r, int c, int solid, int a0, int a1)
{
    if (!logging()) return;
    if (arc_depth++ > 0) return;
    Op *o = op_new(OP_ARC);
    if (!o) return;
    const int a[7] = { cx, cy, r, c, solid, a0, a1 };
    memcpy(o->a, a, sizeof a);
}

void enh_map_log_arc_end(void)
{
    if (!logging()) return;
    if (arc_depth > 0) arc_depth--;
}

void enh_map_log_key(int own, int wx, int wy, int dx, int dy, int sx, int sy, int w, int h, int clip)
{
    if (!logging()) return;
    Op *o = op_new(own ? OP_OWN : OP_KEY);
    if (!o) return;
    const int a[9] = { dx, dy, sx, sy, w, h, clip, (u16)wx, (u16)wy };
    memcpy(o->a, a, sizeof a);
    if (!own) return;
    Own *m = &bown;
    m->valid = true;
    m->x = (s32)DSL(0xBA10); m->y = (s32)DSL(0xC02A);
    m->cx = DSW(0xC0A4); m->cy = DSW(0xC0B2); m->hdg = DSW(0x2DEE);
    /* the cells as 39C4 derives them, with the fraction the rounding dropped (both kept inside the cell, so the whole cell is the step's) */
    const bool kx = (u16)((m->x + 16) >> 5) == m->cx, ky = (u16)(0x8000 - (u16)((m->y + 16) >> 5)) == m->cy;
    m->tx = m->cx + (kx ? ((m->x + 16) & 31) / 32.0 : 0.0);
    m->ty = m->cy + (ky ? (31 - ((m->y + 16) & 31)) / 32.0 : 0.0);
}

/* ---------------------------------------------------------------- the map's projection (mfd_map_sx / sy, 8514 / 852B), unrounded, around an origin */

static int map_shift(void) { return (10 - (s8)DSB(0x4080)) & 31; }
static double unit(void) { return (double)(1u << map_shift()); }   /* cells per original pixel */
static double wrap16(double d) { return d - 65536.0 * floor((d + 32768.0) / 65536.0); }
static bool inside_plot(int x, int y)     /* mfd_map_plot (8548) */
{
    return !(x < (s16)DSW(0xDE94) || x >= (s16)DSW(0xE302) - 1 || y < (s16)DSW(0xDE96) || y >= (s16)DSW(0xE444) - 1);
}

static double own_x(double t) { return own_prev.tx + wrap16(own_cur.tx - own_prev.tx) * t; }
static double own_y(double t) { return own_prev.ty + wrap16(own_cur.ty - own_prev.ty) * t; }
static u16 own_hdg(double t) { return (u16)(own_prev.hdg + (int)lround((s16)(u16)(own_cur.hdg - own_prev.hdg) * t)); }
static double pos_x(double t) { return own_ok ? own_x(t) : own_cur.tx; }   /* the aircraft marker's position at t */
static double pos_y(double t) { return own_ok ? own_y(t) : own_cur.ty; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the map's centre: the marker's position, kept as far from the world's edge as 8387 keeps its centre (the window never shows beyond the world:
 * x within 4Dh << (9 - z) cells of the edge, y within ((3Ch << (9 - z)) / 3) << 2); away from the edges the marker is at the centre, near them it moves off it */
static double lim_x(void) { return (double)(0x4D << ((9 - (s8)DSB(0x4080)) & 31)); }
static double lim_y(void) { return (double)((((s16)(0x3C << ((9 - (s8)DSB(0x4080)) & 31))) / 3) << 2); }
static double org_x(double t) { return clampd(pos_x(t), lim_x(), 0x7FFF - lim_x()); }
static double org_y(double t) { return clampd(pos_y(t), lim_y(), 0x7FFF - lim_y()); }
static u16 org_hdg(double t) { return own_ok ? own_hdg(t) : own_cur.hdg; }

static u16 draw_seg(void) { return video_page_seg(*(s16 *)DSP(DSW(DSB(0x4088) ? CTX_P1_PTR : CTX_P0_PTR))); }   /* the draw page (mfd_map_save_bg) */

static u64 fnv(const u8 *p, size_t n, u64 h) { for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull; return h; }

/* ---------------------------------------------------------------- the frame */

void enh_map_frame(void)
{
    enh_map_log_on = enh_fps_on();
    seq++;
    own_prev = own_cur;
    own_cur = bown;
    own_cur.valid = bstarted && bown.valid;
    own_cur.seq = seq;
    fvalid = bstarted && !bover;
    if (bstarted && bover) st_over++;
    fn = fvalid ? bn : 0;
    memcpy(fl, bl, (size_t)fn * sizeof(Op));
    bn = 0; bstarted = bover = false; bown.valid = false; arc_depth = 0; tag_cls = -1;
    const bool shown = DSW(0x365E) != 0 && DSW(0xDEDA) == 0;
    active = own_ok = scope_on = false;
    if (!enh_map_log_on) return;
    { static int off = -1; if (off < 0) { const char *e = getenv("F117_TEST_MAP_OFF"); off = e && *e == '1'; }
      scope_on = !off && DSW(0x365E) != 0 && DSW(0xDEDA) == 1; }   /* fix 1: the scope is redrawn unrounded by every pass */
    if (shown) st_frames++;
    if (!fvalid || !shown || !own_cur.valid) return;
    static int off = -1;
    if (off < 0) { const char *e = getenv("F117_TEST_MAP_OFF"); off = e && *e == '1'; }   /* test aid: the passes show the step's map as the original drew it */
    if (off) return;
    active = true;
    st_active++;
    {
        static u16 l84, l86, lz;
        static unsigned lseq;
        if (lseq + 1 == seq && lz == DSW(0x4080) && (l84 != DSW(0x4084) || l86 != DSW(0x4086))) st_recentre++;
        l84 = DSW(0x4084); l86 = DSW(0x4086); lz = DSW(0x4080); lseq = seq;
    }
    for (int i = 0; i < fn; i++) {                            /* a blip is the object's when the step plotted it at F_k's cells */
        Op *o = &fl[i];
        if (o->kind != OP_PLOT || o->cls < 0) continue;
        double x, y;
        u16 kx = (u16)~o->a[0], ky = (u16)~o->a[1];
        enh_interp_cells(o->cls, o->slot, 1.0, &x, &y, &kx, &ky);
        if (kx != (u16)o->a[0] || ky != (u16)o->a[1]) { o->cls = -1; st_tag_bad++; }
    }
    own_ok = enh_interp_lerp() && own_prev.valid && own_prev.seq + 1 == own_cur.seq && fabs(wrap16(own_cur.tx - own_prev.tx)) <= JUMP_CELLS_OWN
          && fabs(wrap16(own_cur.ty - own_prev.ty)) <= JUMP_CELLS_OWN;
    if (own_ok) st_own++; else st_own_held++;
    const u8 *bk = seg_ptr(video_page_seg(*(s16 *)DSP(DSW(CTX_P2_PTR))));
    u64 h = 0xCBF29CE484222325ull, hs = h;
    for (int j = 0; j < WH; j++) h = fnv(bk + (WY0 + j) * 320 + WX0, WW, h);
    for (int i = 1; i < (s16)DSW(0xC0B4) && i < 256; i++) {  /* what 8387's site dots depend on: position, +6 != 0, the flags 84h / 2 */
        const u16 s = (u16)(0xB2A2 + 16 * i);
        const u16 k[3] = { DSW(s + 2), DSW(s + 4), (u16)((DSW(s + 6) != 0) | (DSW(s + 8) & 0x86) << 1) };
        hs = fnv((const u8 *)k, sizeof k, hs);
    }
    bg_hash = h ^ hs;
    site_hash = hs;
    static int chk = -1;
    if (chk < 0) { const char *e = getenv("F117_MAP_CHECK"); chk = (e && *e == '1') || enh.fps_trace[0] ? 1 : 0; }
    want_check = chk == 1;
}

bool enh_map_active(void) { return active || scope_on; }

/* ---------------------------------------------------------------- the terrain cache (in a display pass) */

/* the map window grown by MARGIN, cleared and drawn as mfd_map_redraw (8387) draws the window, with the map at the cells (a, b); then A_k again */
static bool cache_render(EnhMapCache *c, u16 a, u16 b)
{
    const int x0 = WX0 - MARGIN, y0 = WY0 - MARGIN, w = WW + 2 * MARGIN, h = WH + 2 * MARGIN;
    EnhRectSnap *sn = enh_rect_snap(x0, y0, w, h);
    if (!sn) return false;
    TextCtx *mc = (TextCtx *)DSP(DSW(0x402A));
    mc->xmin = (s16)x0; mc->ymin = (s16)y0; mc->xmax = (s16)(x0 + w - 1); mc->ymax = (s16)(y0 + h - 1);
    DSW(0xDE94) = (u16)x0; DSW(0xE302) = (u16)(x0 + w - 1); DSW(0xDE96) = (u16)y0; DSW(0xE444) = (u16)(y0 + h - 1);
    DSW(0x4084) = a; DSW(0x4086) = b;
    gfx_set_fg(6);                                            /* mfd_clear(1, 6) */
    fill_rows_far(DSP(DSW(DSB(0x4088) ? CTX_P1_PTR : CTX_P0_PTR)), x0, y0, x0 + w - 1, y0 + h - 1);
    map_render_terrain(DSP(DSW(0x402A)), (s16)a / 2, 0x4000 - (s16)b / 2, 9 - (s16)DSW(0x4080));
    for (int i = 1; i < (s16)DSW(0xC0B4); i++) {             /* the site dots of 8387 */
        const u16 s = (u16)(0xB2A2 + 16 * i);
        if (DSW(s + 6) != 0 && (DSW(s + 8) & 0x84) == 4) mfd_map_plot((s16)DSW(s + 2), (s16)DSW(s + 4), (DSW(s + 8) & 2) ? 0 : 8, 1);
    }
    const bool ok = enh_map_cache_grab(c, draw_seg(), x0, y0, w, h);
    enh_rect_unsnap(sn);
    enh_sandbox_reload();
    return ok;
}

/* test aid: of two 1x renders, the pixels that differ where they overlap (b's pixel (i, j) is a's (i + dx, j + dy)) */
static unsigned long long cache_diff(const EnhMapCache *a, const EnhMapCache *b, int dx, int dy, int inset)
{
    unsigned long long d = 0;
    for (int j = inset; j < b->h - inset; j++)
        for (int i = inset; i < b->w - inset; i++) {
            const int ai = i + dx, aj = j + dy;
            if (ai < inset || aj < inset || ai >= a->w - inset || aj >= a->h - inset) continue;
            d += a->pg[aj * a->w + ai] != b->pg[j * b->w + i];
        }
    return d;
}

static void scroll_of(double t, double *sx, double *sy)        /* the cache pixel under the window's pixel 0 (sub-pixel), relative to the cache's own window */
{
    *sx = wrap16(org_x(t) - cache_a) / unit();
    *sy = 0.75 * wrap16(org_y(t) - cache_b) / unit();
}

void enh_map_prepare(double alpha)
{
    pass_disp = alpha >= 0 && enh_sbx;                        /* fix 1: the scope redraw needs this pass's alpha */
    pass_alpha = alpha < 0 ? 1.0 : alpha > 1 ? 1.0 : alpha;
    if (!active || !enh_sbx || alpha < 0) return;
    const int z = (s8)DSB(0x4080), n = enh_cv.n;
    bool due = !cache_ok || cache_z != z || cache_n != n || cache_hash != bg_hash;
    for (int e = 0; e < 2 && !due; e++) {                     /* both ends of this frame inside the cache's margin */
        double sx, sy;
        scroll_of(own_ok ? e : 1.0, &sx, &sy);
        if (fabs(sx) > MARGIN - CEDGE || fabs(sy) > MARGIN - CEDGE) due = true;
    }
    if (!due) return;
    const Uint64 t0 = SDL_GetTicksNS();
    const double u = unit();
    /* the cells of the new cache: on the grid of whole map pixels (x) and of whole 3/4-row groups (y: 4 cell rows -> 3 pixel rows), so the terrain's pixels lie
     * the same in every cache and the scroll is a translation by whole layer pixels */
    const double ox = org_x(1.0), oy = org_y(1.0);
    const u16 a = (u16)(u * floor(ox / u)), b = (u16)(4 * u * floor(oy / (4 * u)));
    if (want_check && cache_ok) { prev_cache = cache; memset(&cache, 0, sizeof cache); prev_ok = true; prev_site = cache_site; prev_z = cache_z; prev_a = cache_a; prev_b = cache_b; }
    cache_ok = cache_render(&cache, a, b);
    cache_a = a; cache_b = b; cache_z = z; cache_n = n; cache_hash = bg_hash; cache_site = site_hash;
    st_build_ns += SDL_GetTicksNS() - t0;
    st_builds++;
    if (!cache_ok || !want_check) return;
    if (prev_ok && prev_z == z && prev_site == site_hash) {   /* (a site dot that came or went is new content) */                             /* the overlap of the two caches: the same terrain pixels (no jump when the cache moves) */
        const double dx = wrap16((double)a - prev_a) / u, dy = 0.75 * wrap16((double)b - prev_b) / u;
        if (dx == floor(dx) && dy == floor(dy)) {
            const unsigned long long d = cache_diff(&prev_cache, &cache, (int)dx, (int)dy, CEDGE - 1);
            st_ovl++; st_ovl_px += d; if (d > st_ovl_max) st_ovl_max = d;
        }
    }
    if (prev_ok) { free(prev_cache.pg); free(prev_cache.l3); free(prev_cache.l3n); memset(&prev_cache, 0, sizeof prev_cache); prev_ok = false; }
    /* the original's own window: a cache rendered at the step's map cells (4084 / 4086) shows, in its inner 77 x 60, the backup page's window. Within EDGE pixels
     * of the window's border they may differ: there the original clips the terrain polygons to the window (rounded vertices), the larger render does not */
    enum { EDGE = 7 };
    EnhMapCache oc = { 0 };
    if (cache_render(&oc, DSW(0x4084), DSW(0x4086))) {
        const u8 *bk = seg_ptr(video_page_seg(*(s16 *)DSP(DSW(CTX_P2_PTR))));
        unsigned long long d = 0, e = 0;
        for (int j = 0; j < WH; j++)
            for (int i = 0; i < WW; i++) {
                if (oc.pg[(j + MARGIN) * oc.w + i + MARGIN] == bk[(WY0 + j) * 320 + WX0 + i]) continue;
                if (i < EDGE || j < EDGE || i >= WW - EDGE || j >= WH - EDGE) e++; else d++;
            }
        st_tchk++; st_tchk_px += d; st_tchk_edge += e; if (d > st_tchk_max) st_tchk_max = d;
    }
    free(oc.pg); free(oc.l3); free(oc.l3n);
}

/* ---------------------------------------------------------------- the replay (in a display pass) */

typedef struct { int a[7]; } KeyArg;
static void op_key(void *p) { const int *a = ((KeyArg *)p)->a; cockpit_keyblit(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); }
typedef struct { int x, y, c, big; } PixArg;
static void op_pix(void *p)
{
    const PixArg *q = p;
    hud_pixel(q->x, q->y, q->c);
    if (q->big) { hud_pixel(q->x + 1, q->y, q->c); hud_pixel(q->x, q->y + 1, q->c); hud_pixel(q->x + 1, q->y + 1, q->c); }
}

static int clip_r[4];
static int sub(double v, int whole) { const int n = enh_cv.n; int o = (int)floor((v - whole) * n); return o < 0 ? 0 : o >= n ? n - 1 : o; }

/* the trace of this pass: per moved symbol the continuous position (1/64 pixel) and the drawn layer pixel, at t, 0 and 1; the map's scroll */
typedef struct { s8 cls; s16 slot; int v[4], a[4], b[4]; } TraceRow;
static TraceRow trow[MAX_OPS + 1];
static int tn;
static TraceRow *trace_row(int cls, int slot) { if (tn >= MAX_OPS + 1) return NULL; TraceRow *r = &trow[tn++]; r->cls = (s8)cls; r->slot = (s16)slot; return r; }

static void pos_row(double px, double py, int out[4])
{
    const int n = enh_cv.n, fx = (int)floor(px), fy = (int)floor(py);
    out[0] = (int)lround(px * 64); out[1] = (int)lround(py * 64);
    out[2] = fx * n + sub(px, fx); out[3] = fy * n + sub(py, fy);
}

/* a blip at its cells relative to the map's centre at t */
static void blip_pos(double x, double y, double t, double *px, double *py)
{
    *px = CX + wrap16(x - org_x(t)) / unit();
    *py = CY + 0.75 * wrap16(y - org_y(t)) / unit();
}

static bool draw_blip(double t, const Op *o)
{
    double x, y, x0, y0, x1, y1, px, py;
    u16 kx, ky;
    const bool lerped = enh_interp_cells(o->cls, o->slot, t, &x, &y, &kx, &ky);
    if (!lerped) { x = (u16)o->a[0]; y = (u16)o->a[1]; }      /* not interpolated: where the step plotted it (F_k), still on the scrolled map */
    blip_pos(x, y, t, &px, &py);
    const int fx = (int)floor(px), fy = (int)floor(py);
    if (!inside_plot(fx, fy) || o->a[2] == -1) return lerped;   /* as mfd_map_plot: outside the window nothing */
    PixArg q = { fx, fy, o->a[2], o->a[3] };
    if (enh_cv.n <= 1) op_pix(&q);
    else enh_cap_shift(fx, fy, 2, 2, sub(px, fx), sub(py, fy), clip_r, op_pix, &q);
    TraceRow *r = trace_row(o->cls == ENH_OBJ_UNIT ? 1 : 2, o->slot);
    if (r && own_ok && lerped) {
        enh_interp_cells(o->cls, o->slot, 0.0, &x0, &y0, &kx, &ky);
        enh_interp_cells(o->cls, o->slot, 1.0, &x1, &y1, &kx, &ky);
        double ax, ay, bx, by;
        blip_pos(x0, y0, 0.0, &ax, &ay); blip_pos(x1, y1, 1.0, &bx, &by);
        pos_row(px, py, r->v); pos_row(ax, ay, r->a); pos_row(bx, by, r->b);
    } else if (r) tn--;
    return lerped;
}

/* the statics (target markers, route, detection plot and arc) on the map pixel grid of the cells (sa, sb), clip grown by SMARGIN */
typedef struct { u16 sa, sb, cx, cy; } StatArg;
static void statics_op(void *p)
{
    const StatArg *q = p;
    DSW(0x4084) = q->sa; DSW(0x4086) = q->sb;
    DSW(0xDE94) = WX0 - SMARGIN; DSW(0xE302) = WX0 + WW - 1 + SMARGIN; DSW(0xDE96) = WY0 - SMARGIN; DSW(0xE444) = WY0 + WH - 1 + SMARGIN;
    for (int i = 0; i < fn; i++) {
        const Op *o = &fl[i];
        int sx, sy;
        switch (o->kind) {
        case OP_KEY:                                          /* a target marker: where its site is on this map */
            if (mfd_map_to_screen((s16)o->a[7], (s16)o->a[8], &sx, &sy)) cockpit_keyblit(sx - 1, sy - 1, o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]);
            break;
        case OP_ARC: mfd_map_arc(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]); break;
        case OP_ROUTE:                                        /* the first leg from the aircraft (the map's centre) */
            DSW(0xC0A4) = q->cx; DSW(0xC0B2) = q->cy;
            mfd_map_route();
            break;
        case OP_PLOT: if (o->cls < 0) mfd_map_plot(o->a[0], o->a[1], o->a[2], o->a[3]); break;
        default: break;
        }
    }
}

/* test aid: the log replayed as the step drew it (the original's placement) must give the window A_k shows */
static void replay_exact(void)
{
    mfd_map_restore_bg();
    const u16 sx = DSW(0xC0A4), sy = DSW(0xC0B2);
    for (int i = 0; i < fn; i++) {
        const Op *o = &fl[i];
        switch (o->kind) {
        case OP_KEY: case OP_OWN: cockpit_keyblit(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]); break;
        case OP_ARC: mfd_map_arc(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]); break;
        case OP_ROUTE: DSW(0xC0A4) = own_cur.cx; DSW(0xC0B2) = own_cur.cy; mfd_map_route(); DSW(0xC0A4) = sx; DSW(0xC0B2) = sy; break;
        case OP_PLOT: mfd_map_plot(o->a[0], o->a[1], o->a[2], o->a[3]); break;
        }
    }
}

static unsigned long long chk_frame;

void enh_map_apply(double alpha)
{
    tn = 0;
    if (!active || !enh_sbx) return;
    const double t = own_ok ? (alpha < 0 ? 0 : alpha > 1 ? 1 : alpha) : 1.0;
    clip_r[0] = WX0; clip_r[1] = WY0; clip_r[2] = WX0 + WW; clip_r[3] = WY0 + WH;
    const u16 seg = draw_seg();
    if (want_check && chk_frame != seq) {
        chk_frame = seq;
        static u8 win[WH][WW];
        u8 *pg = seg_ptr(seg);
        for (int j = 0; j < WH; j++) memcpy(win[j], pg + (WY0 + j) * 320 + WX0, WW);
        replay_exact();
        bool same = true;
        for (int j = 0; j < WH && same; j++) same = !memcmp(win[j], pg + (WY0 + j) * 320 + WX0, WW);
        st_chk++;
        if (!same) st_chk_bad++;
    }
    if (!cache_ok) return;
    const Uint64 t0 = SDL_GetTicksNS();
    const int n = enh_cv.n;
    const double u = unit(), ox = org_x(t), oy = org_y(t);
    EnhRectSnap *ring = want_check ? enh_rect_snap(WX0 - 8, WY0 - 8, WW + 16, WH + 16) : NULL;   /* fix 2 test aid: nothing drawn outside the window */
    if (want_check) {                                         /* fix 2 test aid: every window pixel shows the world (no band beyond its edge) */
        unsigned long long unc = 0;
        for (int j = 0; j < WH; j++)
            for (int i = 0; i < WW; i++) {
                const double wx = ox + (WX0 + i + 0.5 - CX) * u, wy = oy + (WY0 + j + 0.5 - CY) / 0.75 * u;
                unc += wx < 0 || wx >= 0x8000 || wy < 0 || wy >= 0x8000;
            }
        st_cov++; st_uncov += unc;
    }
    /* the terrain: the cache moved by the scroll in whole layer pixels */
    double sx, sy;
    scroll_of(t, &sx, &sy);
    const long dxl = lround(sx * n), dyl = lround(sy * n);
    enh_map_cache_put(&cache, seg, WX0, WY0, WW, WH, (int)(MARGIN * n + dxl), (int)(MARGIN * n + dyl));
    /* the statics on the pixel grid of the cells sa / sb, moved by the rest of the scroll */
    const u16 sa = (u16)(u * floor(ox / u)), sb = (u16)(4 * u * floor(oy / (4 * u)));
    const double fx = wrap16(ox - sa) / u, fy = 0.75 * wrap16(oy - sb) / u;
    const StatArg q = { sa, sb, (u16)floor(pos_x(t)), (u16)floor(pos_y(t)) };   /* the route starts at the aircraft */
    const u16 s94 = DSW(0xDE94), s302 = DSW(0xE302), s96 = DSW(0xDE96), s444 = DSW(0xE444), s84 = DSW(0x4084), s86 = DSW(0x4086), sca = DSW(0xC0A4), scb = DSW(0xC0B2);
    bool any_static = false;
    for (int i = 0; i < fn && !any_static; i++) any_static = fl[i].kind == OP_KEY || fl[i].kind == OP_ROUTE || fl[i].kind == OP_ARC || (fl[i].kind == OP_PLOT && fl[i].cls < 0);
    if (any_static) enh_win_shift(seg, WX0 - SMARGIN, WY0 - SMARGIN, WW + 2 * SMARGIN, WH + 2 * SMARGIN, (int)-lround(fx * n), (int)-lround(fy * n), clip_r, statics_op, (void *)&q);
    DSW(0xDE94) = s94; DSW(0xE302) = s302; DSW(0xDE96) = s96; DSW(0xE444) = s444; DSW(0x4084) = s84; DSW(0x4086) = s86; DSW(0xC0A4) = sca; DSW(0xC0B2) = scb;
    /* the moving symbols: blips at their cells, the aircraft marker at the centre with the heading's sprite (sim_frame_step) */
    for (int i = 0; i < fn; i++) {
        const Op *o = &fl[i];
        if (o->kind == OP_PLOT && o->cls >= 0) {
            st_blips++;
            if (draw_blip(t, o)) st_blips_lerp++;
        } else if (o->kind == OP_OWN) {
            const double px = CX + wrap16(pos_x(t) - ox) / u, py = CY + 0.75 * wrap16(pos_y(t) - oy) / u;   /* the centre unless the map is held at an edge */
            const int ix = (int)floor(px), iy = (int)floor(py);
            if (!(ix > WX0 && ix < WX0 + WW - 2 && iy > WY0 && iy < WY0 + WH - 2)) continue;   /* mfd_map_to_screen */
            KeyArg k;
            memcpy(k.a, o->a, sizeof k.a);
            k.a[0] = ix - 1; k.a[1] = iy - 1;
            k.a[2] = (o->a[2] & ~0x1C) | (((s16)(u16)(org_hdg(t) + 0x1000) >> 11) & 0x1C);
            if (n <= 1) op_key(&k);
            else enh_cap_shift(ix - 1, iy - 1, o->a[4], o->a[5], sub(px, ix), sub(py, iy), clip_r, op_key, &k);
            TraceRow *r = own_ok ? trace_row(0, 0) : NULL;
            if (r) {
                pos_row(px, py, r->v);
                pos_row(CX + wrap16(pos_x(0) - org_x(0)) / u, CY + 0.75 * wrap16(pos_y(0) - org_y(0)) / u, r->a);
                pos_row(CX + wrap16(pos_x(1) - org_x(1)) / u, CY + 0.75 * wrap16(pos_y(1) - org_y(1)) / u, r->b);
            }
        }
    }
    if (own_ok) {                                             /* the scroll: continuous (1/64 pixel) and drawn (the world's layer pixel under the window) */
        TraceRow *r = trace_row(3, (s8)DSB(0x4080));
        if (r) {
            const double ex[3] = { t, 0.0, 1.0 };
            int *dst[3] = { r->v, r->a, r->b };
            for (int e = 0; e < 3; e++) {
                const double px = wrap16(org_x(ex[e]) - 0x4000) / u, py = 0.75 * wrap16(org_y(ex[e]) - 0x4000) / u;   /* from a fixed world point */
                dst[e][0] = (int)lround(px * 64); dst[e][1] = (int)lround(py * 64);
                double cx_, cy_;
                scroll_of(ex[e], &cx_, &cy_);
                dst[e][2] = (int)(lround(wrap16((double)cache_a - 0x4000) / u) * n + lround(cx_ * n));
                dst[e][3] = (int)(lround(0.75 * wrap16((double)cache_b - 0x4000) / u) * n + lround(cy_ * n));
            }
        }
    }
    if (ring) { const int in[4] = { WX0, WY0, WX0 + WW, WY0 + WH }; st_outside += enh_rect_changed_outside(ring, in); enh_rect_drop(ring); }
    st_ns += SDL_GetTicksNS() - t0;
    st_passes++;
}

/* ---------------------------------------------------------------- the scope between steps (fix 1, U1 feedback "grid radar view") */

/* mfd_scope_draw (8C10) in a display pass, unrounded. The original computes every point with mfd_w2s (90B9): the integer cells' offset from C0A4 / C0B2 shifted
 * right by 7 - [4082], rotated by the heading in Q15 and rounded to a whole pixel: drawn from the lerped pose that moves in whole pixels, and as the lerped
 * heading turns, the rounding of each rotated grid point flips (the grid's shimmer). Here the same points come from the unrounded position (BA10 / C02A of
 * the pass), the lerped heading and the objects' unrounded cells, and are drawn at their sub-pixel places: the grid, target boxes and projectile tracks as
 * thin vector-layer lines, the icons as the original 7 x 7 icons moved by the fraction (enh_cap_shift). The fill, the order, the colours, what is shown
 * and the visibility test (on the whole pixel) are the original's. */
typedef struct { double x, y; bool vis; } ScP;
static double sc_ox, sc_oy, sc_c, sc_s, sc_k;
static int sc_clip[4], sc_iclip[4];

static void sc_pose(double t)                                 /* the aircraft at t: the scope's origin and rotation */
{
    double x, y;
    u16 h;
    enh_interp_own(t, &x, &y, &h);
    sc_ox = (x + 16) / 32.0;                                  /* the cells as 39C4 derives them, unrounded */
    sc_oy = 0x8000 - (y + 16) / 32.0;
    const double a = h * (6.283185307179586 / 65536.0);
    sc_c = cos(a); sc_s = sin(a);
}

static ScP sc_w2s(double wx, double wy)
{
    const double a = wrap16(wx - sc_ox) * sc_k, b = wrap16(sc_oy - wy) * sc_k;
    ScP p;
    const double x = sc_c * a - sc_s * b, y = sc_s * a + sc_c * b;
    p.x = 0x70 + x; p.y = 0x98 - y;
    const int ix = (int)floor(p.x + 0.5), iy = (int)floor(p.y + 0.5);
    p.vis = !(ix < 0x4E || ix > 0x93 || iy < 0x71 || iy > 0xA6);
    return p;
}

typedef struct { int cx, cy, x, y; } IconArg;
static void op_icon(void *p) { const IconArg *q = p; mfd_icon(q->cx, q->cy, q->x, q->y); }
static void sc_icon(u16 seg, int cx, int cy, ScP p)
{
    (void)seg;
    const int ix = (int)floor(p.x), iy = (int)floor(p.y);
    IconArg q = { cx, cy, ix, iy };
    if (enh_cv.n <= 1) { q.x = (int)floor(p.x + 0.5); q.y = (int)floor(p.y + 0.5); op_icon(&q); return; }
    enh_cap_shift(ix - 3, iy - 3, 7, 7, sub(p.x, ix), sub(p.y, iy), sc_iclip, op_icon, &q);
}
static void sc_box(u16 seg, ScP p, int col)                   /* mfd_target_box (903F): a 9 x 7 frame */
{
    const u8 c = (u8)col;
    enh_vec_line_f(seg, p.x - 4, p.y - 3, p.x + 4, p.y - 3, c, sc_clip);
    enh_vec_line_f(seg, p.x + 4, p.y - 3, p.x + 4, p.y + 3, c, sc_clip);
    enh_vec_line_f(seg, p.x + 4, p.y + 3, p.x - 4, p.y + 3, c, sc_clip);
    enh_vec_line_f(seg, p.x - 4, p.y + 3, p.x - 4, p.y - 3, c, sc_clip);
}

static void obj_cells(int cls, int slot, u16 rec, double *x, double *y)
{
    u16 kx, ky;
    if (!enh_interp_cells(cls, slot, pass_alpha, x, y, &kx, &ky)) { *x = DSW(rec); *y = DSW(rec + 2); }
}

bool enh_scope_pass(int flip)
{
    if (!enh_sbx || !pass_disp || !scope_on) return false;
    st_scope++;
    const u16 seg = draw_seg();
    const int r = (s16)DSW(0x4082) + 1;
    gfx_set_fg(0);
    fill_rows_far(DSP(DSW((u8)flip ? CTX_P1_PTR : CTX_P0_PTR)), 0x49, 0x6D, 0x97, 0xAA);
    sc_clip[0] = 0x4A; sc_clip[1] = 0x6E; sc_clip[2] = 0x97; sc_clip[3] = 0xAA;   /* hud_line_clip's window, inclusive -> exclusive */
    sc_iclip[0] = 0x4A; sc_iclip[1] = 0x6E; sc_iclip[2] = 0x97; sc_iclip[3] = 0xAA;
    { const int fr[4] = { 0x49, 0x6D, 0x98, 0xAB }; enh_vec_clear_rect(seg, fr); }   /* fix 3: the last pass's lines go with the fill */
    sc_pose(pass_alpha);
    sc_k = 1.0 / (double)(1u << ((7 - DSB(0x4082)) & 31));
    int n = 1;
    if ((s16)DSW(0x4082) < 2 && DSW(0x98DE) != 0) n = (1 << ((2 - DSB(0x4082)) & 31)) + 1;
    const u16 gx = DSW(0xC0A4) & 0xFC00, gy = DSW(0xC0B2) & 0xFC00;
    for (int i = 1 - n; i <= n; i++) {                        /* the grid: vertical lines, then horizontal ones (1024 cells apart) */
        const ScP a = sc_w2s((s16)((i << 10) + gx), (s16)(gy + 0x1400)), b = sc_w2s((s16)((i << 10) + gx), (s16)(gy - 0x1000));
        enh_vec_line_f(seg, a.x, a.y, b.x, b.y, 8, sc_clip);
    }
    for (int i = 1 - n; i <= n; i++) {
        const ScP a = sc_w2s((s16)(gx + 0x1400), (s16)((i << 10) + gy)), b = sc_w2s((s16)(gx - 0x1000), (s16)((i << 10) + gy));
        enh_vec_line_f(seg, a.x, a.y, b.x, b.y, 8, sc_clip);
    }
    for (int i = 0; (s16)DSW(0xDED0) > i; i++) {              /* the air units */
        const u16 o = (u16)(0xC13E + 0x24 * i);
        if (!(DSB(o + 0x18) & 2) || DSW(o + 0x1A) == 0) continue;
        double x, y;
        obj_cells(ENH_OBJ_UNIT, i, (u16)(o + 2), &x, &y);
        const ScP p = sc_w2s(x, y);
        if (!p.vis) continue;
        if (DSW(0xE2FE) == 1 && (s16)DSW(0x3D64) == i) sc_box(seg, p, 7);
        if ((s16)DSW(0x3D6A) > 0 && (u16)(0xFFFF - i) == DSW(0xC07E)) sc_box(seg, p, (s16)DSW(0x9872));
        s16 a = (s16)(DSW(o + 0x10) - DSW(0x2DEE));
        a = (s16)(a + 0x800);
        const s16 dz = (s16)(DSW(o + 6) - DSW(0x2DF4));
        const int cls = dz < -1000 ? 1 : dz > 1000 ? 2 : 0;
        sc_icon(seg, (a >> 12) & 0xF, cls, p);
    }
    for (int i = 0; i < 12; i++) {                            /* the missile and projectile tracks */
        const u16 t = (u16)(0x3C0C + 0x1C * i);
        if (DSW(t + 0xE) == 0) continue;
        double x, y;
        obj_cells(ENH_OBJ_PROJ, i, t, &x, &y);
        const ScP p = sc_w2s(x, y);
        if (!p.vis) continue;
        u8 c = (s16)DSW((u16)(0x336E + 0x12 * DSW(t + 0x10))) > 0 ? 0x11 : 0x29;
        if (i >= 8) c = 0xF;
        const double a = (u16)(DSW(t + 8) - DSW(0x2DEE)) * (6.283185307179586 / 65536.0);
        enh_vec_line_f(seg, p.x, p.y, p.x - sin(a) * r, p.y + cos(a) * r, c, sc_clip);
    }
    for (int i = 0; (s16)DSW(0xC0B4) > i; i++) {              /* the sites */
        const u16 s = (u16)(0xB2A2 + 16 * i), fl = DSW(s + 8);
        if ((fl & 0x80) && !(fl & 0x200)) continue;
        const ScP p = sc_w2s((s16)DSW(s + 2), (s16)DSW(s + 4));
        if (!p.vis) continue;
        if (DSW(0xE2FE) == 2 && (s16)DSW(0x3D66) == i) sc_box(seg, p, 7);
        if ((s16)DSW(0x3D6A) > 0 && DSW(0xC07E) == i) sc_box(seg, p, (s16)DSW(0x9872));
        int c = 5;
        if (DSW(s + 8) & 0x201) c = ((((u16)(0x1000 - DSW(0x2DEE))) & 0x6000) >> 13) + 8;
        if (DSW(s + 6) != 0) c = 1;
        if (DSB(s + 8) & 8) c = 7;
        sc_icon(seg, c, 3, p);
    }
    mfd_icon(0, 3, 0x70, 0x98);                               /* the ownship (w2s of the aircraft: the scope's origin) */
    for (int i = 0; i < 4; i++) {                             /* the countermeasure marks */
        const u16 m = (u16)(0x39AA + 12 * i);
        if (DSW(m + 8) == 0) continue;
        const ScP p = sc_w2s((s16)DSW(m), (s16)DSW(m + 2));
        if (!p.vis) continue;
        switch (DSW(m + 6)) {
        case 1: sc_icon(seg, 2, 3, p); break;
        case 2: sc_icon(seg, 3, 3, p); break;
        case 3: sc_icon(seg, 6, 3, p); break;
        }
    }
    /* the trace (o-line "scope"): a grid crossing (a fixed world point of this frame: the aircraft's 1024-cell corner at F_k) where this pass drew it, and
     * at t = 0 / 1, in 1/64 pixel; the third component is the heading */
    TraceRow *tr = trace_row(4, (s8)DSB(0x4082));
    if (tr) {
        double fx_, fy_;
        u16 fh;
        enh_interp_own(1.0, &fx_, &fy_, &fh);
        const double px = (double)((u16)floor((fx_ + 16) / 32.0) & 0xFC00), py = (double)((u16)floor(0x8000 - (fy_ + 16) / 32.0) & 0xFC00);
        const double ts[3] = { pass_alpha, 0.0, 1.0 };
        int *dst[3] = { tr->v, tr->a, tr->b };
        for (int e = 0; e < 3; e++) {
            sc_pose(ts[e]);
            const ScP g = sc_w2s(px, py);
            double hx, hy;
            u16 hh;
            enh_interp_own(ts[e], &hx, &hy, &hh);
            dst[e][0] = (int)lround(g.x * 64); dst[e][1] = (int)lround(g.y * 64); dst[e][2] = hh; dst[e][3] = 0;
        }
    }
    return true;
}

void enh_map_trace(FILE *f, unsigned long long frame, int pass)
{
    static const char *const name[5] = { "mapown", "mapunit", "mapproj", "mapscroll", "scope" };
    if (!f) return;
    for (int i = 0; i < tn; i++) {
        const TraceRow *r = &trow[i];
        fprintf(f, "o %llu %d %s %d v=%d,%d,%d,%d a=%d,%d,%d,%d b=%d,%d,%d,%d\n", frame, pass, name[r->cls], r->slot, r->v[0], r->v[1], r->v[2], r->v[3],
                r->a[0], r->a[1], r->a[2], r->a[3], r->b[0], r->b[1], r->b[2], r->b[3]);
    }
    tn = 0;
}

void enh_map_stats(FILE *f)
{
    if (!st_frames) return;
    fprintf(f, "display-map: %llu frames with the map shown, %llu redrawn per refresh; aircraft marker moved %llu, held %llu; blips moved %llu of %llu; "
            "tags not at F_k %llu; log overflows %llu; replay check %llu frames, %llu differ; %.3f ms a pass (%llu passes); terrain cache renders %llu, %.2f ms each; "
            "terrain check %llu renders, %llu pixels differ inside (max %llu a render), %llu near the edge; cache overlap %llu, %llu pixels differ (max %llu); original recentres %llu; window check %llu passes, %llu pixels beyond the world, %llu changed outside; scope %llu passes\n", st_frames, st_active, st_own,
            st_own_held, st_blips_lerp, st_blips, st_tag_bad, st_over, st_chk, st_chk_bad, st_passes ? (double)st_ns / (double)st_passes / 1e6 : 0.0, st_passes,
            st_builds, st_builds ? (double)st_build_ns / (double)st_builds / 1e6 : 0.0, st_tchk, st_tchk_px, st_tchk_max, st_tchk_edge, st_ovl, st_ovl_px, st_ovl_max, st_recentre, st_cov, st_uncov, st_outside, st_scope);
}
