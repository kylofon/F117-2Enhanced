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
#include "gen/module_simflow.h"
#include "mem.h"
#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "video/video.h"

bool enh_map_log_on;

enum { OP_PLOT, OP_KEY, OP_OWN, OP_ROUTE, OP_ARC };
enum { MAX_OPS = 256, JUMP_CELLS_OWN = 32 };   /* the own aircraft's jump limit of interp.c (1024 world units) in cells */

typedef struct { u8 kind; s8 cls; s16 slot; int a[7]; } Op;
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
static unsigned long long st_ns, st_passes;   /* the replay's cost (without the test-aid check) */
static unsigned long long st_frames, st_active, st_own, st_own_held, st_blips, st_blips_lerp, st_tag_bad, st_over, st_chk, st_chk_bad;

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

void enh_map_log_key(int own, int dx, int dy, int sx, int sy, int w, int h, int clip)
{
    if (!logging()) return;
    Op *o = op_new(own ? OP_OWN : OP_KEY);
    if (!o) return;
    const int a[7] = { dx, dy, sx, sy, w, h, clip };
    memcpy(o->a, a, sizeof a);
    if (!own) return;
    Own *m = &bown;
    m->valid = true;
    m->x = (s32)DSL(0xBA10); m->y = (s32)DSL(0xC02A);
    m->cx = DSW(0xC0A4); m->cy = DSW(0xC0B2); m->hdg = DSW(0x2DEE);
    /* the cells as 39C4 derives them, with the fraction the rounding dropped (both kept inside the cell, so the whole pixel is the step's) */
    const bool kx = (u16)((m->x + 16) >> 5) == m->cx, ky = (u16)(0x8000 - (u16)((m->y + 16) >> 5)) == m->cy;
    m->tx = m->cx + (kx ? ((m->x + 16) & 31) / 32.0 : 0.0);
    m->ty = m->cy + (ky ? (31 - ((m->y + 16) & 31)) / 32.0 : 0.0);
}

/* ---------------------------------------------------------------- the map's projection (mfd_map_sx / sy, 8514 / 852B), unrounded */

static int map_shift(void) { return (10 - (s8)DSB(0x4080)) & 31; }
static double wrap16(double d) { return d - 65536.0 * floor((d + 32768.0) / 65536.0); }
static double map_px(double wx) { return 0x70 + wrap16(wx - DSW(0x4084)) / (double)(1u << map_shift()); }
static double map_py(double wy) { return 0x8B + 0.75 * wrap16(wy - DSW(0x4086)) / (double)(1u << map_shift()); }
static bool inside_strict(int x, int y)   /* mfd_map_to_screen (85ED) */
{
    return x > (s16)DSW(0xDE94) && x < (s16)DSW(0xE302) - 1 && y > (s16)DSW(0xDE96) && y < (s16)DSW(0xE444) - 1;
}
static bool inside_plot(int x, int y)     /* mfd_map_plot (8548) */
{
    return !(x < (s16)DSW(0xDE94) || x >= (s16)DSW(0xE302) - 1 || y < (s16)DSW(0xDE96) || y >= (s16)DSW(0xE444) - 1);
}

static double own_x(double t) { return own_prev.tx + wrap16(own_cur.tx - own_prev.tx) * t; }
static double own_y(double t) { return own_prev.ty + wrap16(own_cur.ty - own_prev.ty) * t; }
static u16 own_hdg(double t) { return (u16)(own_prev.hdg + (int)lround((s16)(u16)(own_cur.hdg - own_prev.hdg) * t)); }

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
    active = own_ok = false;
    if (!enh_map_log_on) return;
    if (shown) st_frames++;
    if (!fvalid || !shown || !enh_interp_lerp()) return;
    active = true;
    st_active++;
    for (int i = 0; i < fn; i++) {                            /* a blip is the object's when the step plotted it at F_k's cells */
        Op *o = &fl[i];
        if (o->kind != OP_PLOT || o->cls < 0) continue;
        double x, y;
        u16 kx = (u16)~o->a[0], ky = (u16)~o->a[1];
        enh_interp_cells(o->cls, o->slot, 1.0, &x, &y, &kx, &ky);
        if (kx != (u16)o->a[0] || ky != (u16)o->a[1]) { o->cls = -1; st_tag_bad++; }
    }
    if (own_cur.valid) {
        own_ok = own_prev.valid && own_prev.seq + 1 == own_cur.seq && fabs(wrap16(own_cur.tx - own_prev.tx)) <= JUMP_CELLS_OWN
              && fabs(wrap16(own_cur.ty - own_prev.ty)) <= JUMP_CELLS_OWN;
        if (own_ok)                                           /* both ends inside the window of this frame's map (a recentred map keeps the last step inside) */
            for (int e = 0; e < 2; e++) {
                const double t = e;
                if (!inside_strict((int)floor(map_px(own_x(t))), (int)floor(map_py(own_y(t))))) own_ok = false;
            }
        if (own_ok) st_own++; else st_own_held++;
    }
    static int chk = -1;
    if (chk < 0) { const char *e = getenv("F117_MAP_CHECK"); chk = (e && *e == '1') || enh.fps_trace[0] ? 1 : 0; }
    want_check = chk == 1;
}

bool enh_map_active(void) { return active; }

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

/* the trace of this pass: per moved symbol the continuous position (1/64 pixel) and the drawn layer pixel, at t, 0 and 1 */
typedef struct { s8 cls; s16 slot; int v[4], a[4], b[4]; } TraceRow;
static TraceRow trow[MAX_OPS];
static int tn;

static void pos_row(double px, double py, int out[4])
{
    const int n = enh_cv.n, fx = (int)floor(px), fy = (int)floor(py);
    out[0] = (int)lround(px * 64); out[1] = (int)lround(py * 64);
    out[2] = fx * n + sub(px, fx); out[3] = fy * n + sub(py, fy);
}

static void draw_own(double t, const Op *o)
{
    const double px = map_px(own_x(t)), py = map_py(own_y(t));
    const int fx = (int)floor(px), fy = (int)floor(py);
    const u16 h = own_hdg(t);
    KeyArg k;
    memcpy(k.a, o->a, sizeof k.a);
    k.a[0] = fx - 1; k.a[1] = fy - 1;
    k.a[2] = (o->a[2] & ~0x1C) | (((s16)(u16)(h + 0x1000) >> 11) & 0x1C);   /* the heading's sprite (sim_frame_step) */
    if (enh_cv.n <= 1) op_key(&k);
    else enh_cap_shift(fx - 1, fy - 1, o->a[4], o->a[5], sub(px, fx), sub(py, fy), clip_r, op_key, &k);
    if (tn < MAX_OPS) {
        TraceRow *r = &trow[tn++];
        r->cls = 0; r->slot = 0;
        pos_row(px, py, r->v);
        pos_row(map_px(own_x(0)), map_py(own_y(0)), r->a);
        pos_row(map_px(own_x(1)), map_py(own_y(1)), r->b);
    }
}

static bool draw_blip(double t, const Op *o)
{
    double x, y, x0, y0, x1, y1;
    u16 kx, ky;
    if (!enh_interp_cells(o->cls, o->slot, t, &x, &y, &kx, &ky)) return false;
    enh_interp_cells(o->cls, o->slot, 0.0, &x0, &y0, &kx, &ky);
    enh_interp_cells(o->cls, o->slot, 1.0, &x1, &y1, &kx, &ky);
    const double px = map_px(x), py = map_py(y);
    const int fx = (int)floor(px), fy = (int)floor(py);
    if (DSW(0xDEDA) || DSW(0x365E) == 0 || !inside_plot(fx, fy) || o->a[2] == -1) return true;   /* as mfd_map_plot: outside the window nothing */
    PixArg q = { fx, fy, o->a[2], o->a[3] };
    if (enh_cv.n <= 1) op_pix(&q);
    else enh_cap_shift(fx, fy, 2, 2, sub(px, fx), sub(py, fy), clip_r, op_pix, &q);
    if (tn < MAX_OPS) {
        TraceRow *r = &trow[tn++];
        r->cls = (s8)(o->cls == ENH_OBJ_UNIT ? 1 : 2); r->slot = o->slot;
        pos_row(px, py, r->v);
        pos_row(map_px(x0), map_py(y0), r->a);
        pos_row(map_px(x1), map_py(y1), r->b);
    }
    return true;
}

static void replay(double t, bool exact)
{
    const u16 sx = DSW(0xC0A4), sy = DSW(0xC0B2);
    mfd_map_restore_bg();
    for (int i = 0; i < fn; i++) {
        const Op *o = &fl[i];
        switch (o->kind) {
        case OP_KEY: cockpit_keyblit(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]); break;
        case OP_ARC: mfd_map_arc(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]); break;
        case OP_ROUTE:                                        /* the first leg starts at the aircraft: the marker's whole cell (the step's at t = 1) */
            if (own_cur.valid) {
                DSW(0xC0A4) = exact || !own_ok ? own_cur.cx : (u16)floor(own_x(t));
                DSW(0xC0B2) = exact || !own_ok ? own_cur.cy : (u16)floor(own_y(t));
            }
            mfd_map_route();
            DSW(0xC0A4) = sx; DSW(0xC0B2) = sy;
            break;
        case OP_PLOT:
            if (!exact && o->cls >= 0) { st_blips++; if (draw_blip(t, o)) { st_blips_lerp++; break; } }
            mfd_map_plot(o->a[0], o->a[1], o->a[2], o->a[3]);
            break;
        case OP_OWN:
            if (!exact && own_ok) draw_own(t, o);
            else cockpit_keyblit(o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5], o->a[6]);
            break;
        }
    }
}

static unsigned long long chk_frame;

void enh_map_apply(double alpha)
{
    tn = 0;
    if (!active || !enh_sbx) return;
    const double t = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    clip_r[0] = (s16)DSW(0xDE94); clip_r[1] = (s16)DSW(0xDE96);   /* the map window (74, 110) 77 x 60 */
    clip_r[2] = (s16)DSW(0xDE94) + 0x4D; clip_r[3] = (s16)DSW(0xDE96) + 0x3C;
    if (want_check && chk_frame != seq) {                     /* test aid: the log replayed as logged must give the window A_k shows (nothing missed) */
        chk_frame = seq;
        const u16 seg = video_page_seg(*(s16 *)DSP(DSW(DSB(0x4088) ? CTX_P1_PTR : CTX_P0_PTR)));   /* the draw page (mfd_map_save_bg) */
        static u8 win[0x3C][0x4D];
        u8 *pg = seg_ptr(seg);
        for (int j = 0; j < 0x3C; j++) memcpy(win[j], pg + (0x6E + j) * 320 + 0x4A, 0x4D);
        replay(1.0, true);
        bool same = true;
        for (int j = 0; j < 0x3C && same; j++) same = !memcmp(win[j], pg + (0x6E + j) * 320 + 0x4A, 0x4D);
        st_chk++;
        if (!same) st_chk_bad++;
    }
    const Uint64 t0 = SDL_GetTicksNS();
    replay(t, false);
    st_ns += SDL_GetTicksNS() - t0;
    st_passes++;
}

void enh_map_trace(FILE *f, unsigned long long frame, int pass)
{
    static const char *const name[3] = { "mapown", "mapunit", "mapproj" };
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
            "tags not at F_k %llu; log overflows %llu; replay check %llu frames, %llu differ; %.3f ms a pass (%llu passes)\n", st_frames, st_active, st_own,
            st_own_held, st_blips_lerp, st_blips, st_tag_bad, st_over, st_chk, st_chk_bad, st_passes ? (double)st_ns / (double)st_passes / 1e6 : 0.0, st_passes);
}
