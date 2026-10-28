/* The Enhanced canvas, layers, shadow plane and compositor (see enh_canvas.h; DESIGN.md 2.1, 2.5, 3.1, 3.2). */
#include "enhanced/enh_canvas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhanced/enhanced.h"
#include "enhanced/hr.h"
#include "enhanced/sandbox.h"
#include "enhanced/wide.h"
#include "enhanced/par.h"
#include "mem.h"
#include "render3d/r3d.h"
#include "platform/vga.h"
#include "video/video.h"

#define PAGE_PIXELS 64000
#define MAX_SIDE 8192

enum { SRC_NONE, SRC_3D, SRC_VEC, SRC_VECHR };   /* SRC_VECHR (S3.1): a 1x vector write whose N x look is emitted by an hr emitter (hr_obj.c): no hr pixel -> what was under it */

typedef struct {
    u8 src;       /* SRC_*: who wrote the pixel last */
    u8 val;       /* the byte written */
    u8 under;     /* vector write: the page value it replaced */
    u8 under3d;   /* vector write: the pixel under it was a (fresh) 3D pixel */
} Shadow;

EnhCanvas enh_cv = { 320, 200, 1, 0, 0, 0, 0, 320, 200 };
bool enh_shadow_on;
int enh_route;
int enh_stamp_hold;   /* S3.1: > 0 = the 1x writes of a 3D pass are recorded but not stamped (an hr emitter draws the object itself) */
int enh_vec_hold;     /* S3.1: > 0 = the 1x line()/put_pixel writes are an hr emitter's (SRC_VECHR) */

static bool active;
static EnhView view = ENH_VIEW_MENU;
static bool menu_on;                     /* S3.4: a VGAME menu screen is up (enh_menu) */
static int last_win_w, last_win_h;          /* S3.4 fix 4: the host's window, for a placement change at the start of a frame */
static bool last_a43, present_hold;
static EnhView menu_saved;

static Shadow sh_buf[2][PAGE_PIXELS];
static Shadow *sh_work = sh_buf[0], *sh_shown = sh_buf[1];  /* pointers, so a display pass can swap them (S2.4, enh_canvas_snap_swap) */
static u16 *l3d_work, *l3d_shown, *lvec_work, *lvec_shown;   /* lw * lh each; bit 8 = written */
static u8 *l3n_work, *l3n_shown;                             /* S3.6 (DESIGN 2.12): the night plane of the 3D layer, 0 = as the original, else the factor / 256 */
static size_t layer_px, layer_cap;                           /* layer_cap: the pixels the four buffers hold (>= layer_px) */

/* ---------------------------------------------------------------- geometry (3.1, 3.2) */

void enh_canvas_geometry(EnhCanvas *c, EnhView v, int win_w, int win_h, bool aspect43, int fixed_n, bool wide)
{
    int lw = win_w < 1 ? 1 : win_w;
    int lh = aspect43 ? win_h * 5 / 6 : win_h;                 /* floor(Ho / a), a = 1.2 */
    if (lh < 1) lh = 1;
    int fit = lw / 320 < lh / 200 ? lw / 320 : lh / 200;
    if (fit < 1) fit = 1;
    if (!wide) {                                               /* the canvas is the frame; the host letterboxes it */
        int n = fixed_n > 0 ? fixed_n : fit;
        if (n > MAX_SIDE / 320) n = MAX_SIDE / 320;
        c->lw = 320 * n; c->lh = 200 * n; c->n = n;
        c->fx = c->fy = 0;
        c->vx = c->vy = 0; c->vw = c->lw; c->vh = c->lh;
        return;
    }
    if (v == ENH_VIEW_FIT || v == ENH_VIEW_SIDE) {             /* S3.4 fix (Decisions 2026-10-21 b): the picture fills the window height at the scale height / 200 */
        int nf = fixed_n > 0 ? fixed_n : (lh + 199) / 200;     /* rendered at the next whole N at or above it; the host scales the canvas to the window (letterbox) */
        if (nf < 1) nf = 1;
        if (nf > MAX_SIDE / 320) nf = MAX_SIDE / 320;
        int cw = 320 * nf;
        if (v == ENH_VIEW_SIDE) {                              /* as wide as the window's aspect at that height: the world beside the frame */
            long long w = (long long)lw * (200 * nf) / lh;
            if (w > cw) cw = (int)(w > MAX_SIDE ? MAX_SIDE : w);
        }
        c->lw = cw; c->lh = 200 * nf; c->n = nf;
        c->fx = (cw - 320 * nf) / 2; c->fy = 0;
        if (v == ENH_VIEW_SIDE) { c->vx = 0; c->vy = 0; c->vw = cw; c->vh = 200 * nf; }
        else { c->vx = c->fx; c->vy = 0; c->vw = 320 * nf; c->vh = 200 * nf; }
        return;
    }
    int n = fixed_n > 0 ? fixed_n : fit;                       /* S3.4: a fixed scale is kept, as without wide (the canvas then holds at least the frame) */
    if (n > MAX_SIDE / 320) n = MAX_SIDE / 320;
    if (lw < 320 * n || lh < 200 * n) {                         /* the frame is larger than the window: the smallest canvas of the window's aspect that holds it (the host
                                                                 * scales it down), so a fixed scale keeps the wider view */
        long long w = ((long long)200 * n * lw + lh - 1) / lh;
        if (w >= 320 * n) { lw = (int)w; lh = 200 * n; }
        else { lh = (int)(((long long)320 * n * lh + lw - 1) / lw); lw = 320 * n; }
    }
    if (lw > MAX_SIDE) lw = MAX_SIDE;
    if (lh > MAX_SIDE) lh = MAX_SIDE;
    c->lw = lw; c->lh = lh; c->n = n;
    c->fx = (lw - 320 * n) / 2;
    bool extended = v == ENH_VIEW_FRONT || v == ENH_VIEW_FULL;
    c->fy = extended ? lh - 200 * n : (lh - 200 * n) / 2;      /* bottom centre, or centred */
    if (extended) { c->vx = c->vy = 0; c->vw = lw; c->vh = lh; }
    else { c->vx = c->fx; c->vy = c->fy; c->vw = 320 * n; c->vh = 200 * n; }
}

bool enh_canvas_active(void) { return active && enh_canvas_on(); }

void enh_canvas_set_active(bool on)
{
    active = on;
    enh_shadow_on = on && enh_canvas_on();
    view = ENH_VIEW_MENU;                        /* a mode set starts a new program: the menu placement */
    menu_on = false;
    present_hold = false;
    enh_route = ENH_ROUTE_ART;
}

/* S3.4 (Decisions 2026-10-21): the first cockpit view (view 0, [294B] = 0) and the outside views are extended; the second cockpit view (view 0 with [294B] = 1: the
 * key C small view, whose window-frame struts hud_side_strips keys over rows 0..90 from the top of 256PIT.PIC) and the looking-out views 0x41..0x43 are the whole
 * picture fitted and centred (DESIGN 3.2). */
EnhView enh_view_class(int m, int style)
{
    if (m == 0) return style ? ENH_VIEW_SIDE : ENH_VIEW_FRONT;
    if (m >= 0x41 && m <= 0x43) return ENH_VIEW_FIT;
    return ENH_VIEW_FULL;
}

/* S3.4 fix 4 (the black flash of a view switch): sandbox.c enh_frame_begin, before A_k: the placement of the view this frame will draw (view C07A, [294B]) is applied
 * now, so the frame and its wide display pass are drawn for it; until that frame is flipped the presenter keeps the last picture (enh_compose_vga, host canvas_sync). */
void enh_canvas_frame_view(int m, int style)
{
    if (!enh_canvas_active() || !last_win_w) return;
    const EnhView v = enh_view_class(m, m == 0 ? style : 0);
    if (v == view || menu_on) return;
    const EnhCanvas old = enh_cv;
    view = v;
    enh_canvas_update(last_win_w, last_win_h, last_a43);
    if (memcmp(&old, &enh_cv, sizeof old)) present_hold = true;
}

bool enh_present_hold(void) { return present_hold && enh_canvas_active(); }

void enh_set_view(int m)
{
    view = enh_view_class(m, m == 0 ? DSB(0x294B) : 0);
}

/* S3.4: VGAME's own full-screen screens (the quit requester, the boss screen) are menus: pillarboxed, no extended world; the view comes back after them */
void enh_menu(bool on)
{
    if (on && !menu_on) { menu_saved = view; view = ENH_VIEW_MENU; menu_on = true; }
    else if (!on && menu_on) { view = menu_saved; menu_on = false; }
}

bool enh_canvas_extended(void)
{
    return (view == ENH_VIEW_FRONT || view == ENH_VIEW_FULL || view == ENH_VIEW_SIDE) && (enh_cv.vw > 320 * enh_cv.n || enh_cv.vh > 200 * enh_cv.n);
}

EnhView enh_view_get(void) { return view; }

/* ---------------------------------------------------------------- layers */

static void layers_free(void)
{
    free(l3d_work); free(l3d_shown); free(lvec_work); free(lvec_shown);
    l3d_work = l3d_shown = lvec_work = lvec_shown = NULL;
    free(l3n_work); free(l3n_shown); l3n_work = l3n_shown = NULL;   /* S3.6 */
    layer_px = layer_cap = 0;
}

static void layers_alloc(void)
{
    if (enh_hr.on) fprintf(stderr, "[enh] the layers were reallocated inside a 3D pass\n");   /* S3.4 fix: must never happen (the test greps for it) */
    enh_hr.night = NULL;                               /* S3.6 */
    hr_night_forget();
    enh_hr.on = enh_hr.have = false;                   /* S3.4 fix: the hr pass context points into the old layers: void it (A17E, the next hooks see no pass) */
    size_t px = (size_t)enh_cv.lw * (size_t)enh_cv.lh;
    layers_free();
    l3d_work = calloc(px, sizeof(u16)); l3d_shown = calloc(px, sizeof(u16));
    lvec_work = calloc(px, sizeof(u16)); lvec_shown = calloc(px, sizeof(u16));
    l3n_work = calloc(px, 1); l3n_shown = calloc(px, 1);                                  /* S3.6 */
    if (l3d_work && l3d_shown && lvec_work && lvec_shown && l3n_work && l3n_shown) layer_px = layer_cap = px;
    else layers_free();
}

u16 *enh_layer3d(void) { return l3d_work; }
u16 *enh_layer_vec(void) { return lvec_work; }
u8 *enh_layer_night(void) { return l3n_work; }

/* S3.4: the render test fixes the window (320N x 200N, or the --wide-test size), so the host's window (a headless run's nominal one) cannot move the canvas mid-scene */
static int test_win_w, test_win_h;
void enh_canvas_test_window(int w, int h) { test_win_w = w; test_win_h = h; }

bool enh_canvas_update(int win_w, int win_h, bool aspect43)
{
    EnhCanvas c;
    last_win_w = win_w; last_win_h = win_h; last_a43 = aspect43;
    if (test_win_w > 0) { win_w = test_win_w; win_h = test_win_h; aspect43 = false; }
    if (enh_hr.on || enh_sbx) return false;           /* S3.4 fix: never inside a 3D pass or a display pass (host_pump runs in the [262A] wait of r3d_begin_scene: a
                                                       * view change, e.g. key c, resized the layers under the open pass -> writes into freed memory, the crash) */
    enh_canvas_geometry(&c, view, win_w, win_h, aspect43, enh.render_scale, enh.wide);
    if (!memcmp(&c, &enh_cv, sizeof c) && layer_px) return false;
    bool resized = c.lw != enh_cv.lw || c.lh != enh_cv.lh;
    enh_cv = c;
    layers_alloc();                                   /* a new placement: the old layer contents no longer line up */
    return resized;
}

/* ---------------------------------------------------------------- shadow records (2.5) */

/* the pages the shadow follows: page 0 (A000) and page 1, the back page (S2.2: in the game page 1 is a DOS block, gfx_alloc(1), not A400) */
bool enh_page_tracked(u16 seg) { return seg == 0xA000 || (seg != 0 && seg == video_page_seg(1)); }
static bool rec_ok(u16 seg, u16 off) { return off < PAGE_PIXELS && enh_page_tracked(seg); }

/* a 1x write inside a 3D pass that the hr back end does not mirror (sprites, dots, the 1001 fills before the hr backdrop) shows as its
 * original pixel: its block of the 3D layer gets the value (bit 9 marks it, for --hr-check); a later hr write replaces it */
static void stamp3d(u16 seg, u16 off, u8 val)
{
    if (enh_stamp_hold > 0 || !l3d_work || (enh_hr.on && seg != enh_hr.seg)) return;   /* the other page during an hr pass (the TV camera's backdrop): not this layer */
    int x = off % 320, y = off / 320, n = enh_cv.n;
    const size_t at = (size_t)(enh_cv.fy + y * n) * enh_cv.lw + (size_t)(enh_cv.fx + x * n);
    u16 *p = l3d_work + at;
    for (int ky = 0; ky < n; ky++, p += enh_cv.lw)
        for (int kx = 0; kx < n; kx++) p[kx] = (u16)(0x300 | val);
    for (int ky = 0; ky < n; ky++) memset(l3n_work + at + (size_t)ky * enh_cv.lw, 0, (size_t)n);   /* S3.6: shown as the original */
}

/* S3.3: a vector write on page 0 (the displayed page: the second copy of the map's lines, the route legs the step draws after the flip) belongs to the shown plane and layer, one
 * on page 1 (the page being drawn) to the work ones. Page 1 aliasing page 0 (a single-page mode) is all work. */
static inline bool plane_shown(u16 seg) { return seg == 0xA000 && video_page_seg(1) != 0xA000; }

static void vec_clear_block(u16 off, u16 seg)
{
    u16 *lv = plane_shown(seg) ? lvec_shown : lvec_work;
    if (!lv) return;
    int x = off % 320, y = off / 320, n = enh_cv.n;
    int bx = enh_cv.fx + x * n, by = enh_cv.fy + y * n;
    bool any = false;
    for (int ky = 0; ky < n && !any; ky++)
        for (int kx = 0; kx < n; kx++) if (lv[(size_t)(by + ky) * enh_cv.lw + bx + kx] & 0x100) { any = true; break; }
    if (!any) return;
    if (hr_tracing && lv == lvec_work) { s32 t[4] = { bx, by, bx + n - 1, by + n - 1 }; hr_trace(15, 4, t); }   /* S3.4 fix: 15 = this block of the vector layer was cleared */
    for (int ky = 0; ky < n; ky++) memset(lv + (size_t)(by + ky) * enh_cv.lw + bx, 0, (size_t)n * sizeof(u16));
}

void enh_rec_line(u16 seg, u16 off, u8 val)
{
    if (!rec_ok(seg, off)) return;
    Shadow *r = &sh_work[off];
    if (enh_route == ENH_ROUTE_3D) { r->src = SRC_3D; r->val = val; stamp3d(seg, off, val); return; }
    if (plane_shown(seg)) r = &sh_shown[off];                         /* ENH: S3.3 */
    u8 old = seg_ptr(seg)[off];
    bool fresh = r->src != SRC_NONE && old == r->val;
    if (!fresh) vec_clear_block(off, seg);
    if ((r->src == SRC_VEC || r->src == SRC_VECHR) && fresh) {      /* a line over a line keeps what was below both */
        if (enh_vec_hold <= 0) {                                    /* a classic line over an hr one: shown as its original pixel */
            if (val != r->val || r->src == SRC_VECHR) vec_clear_block(off, seg);   /* S3.4 fix: the hr line under it goes; before, the compositor kept showing it
                                                                                    * over the 1x line (e.g. a colour-0 bar of 7898 drawn over an hr HUD line) */
            r->src = SRC_VEC;
        }
        r->val = val;
        return;
    }
    r->under3d = (u8)(r->src == SRC_3D && fresh);
    r->under = old;
    r->src = enh_vec_hold > 0 ? SRC_VECHR : SRC_VEC;
    r->val = val;
}

/* S3.3: a polygon pixel written (call AFTER the write; `old` = the page value it replaced) as the footprint of an hr vector fill: the same record as a held line pixel */
void enh_rec_vecfill(u16 seg, u16 off, u8 old, u8 val)
{
    if (!rec_ok(seg, off)) return;
    Shadow *r = plane_shown(seg) ? &sh_shown[off] : &sh_work[off];
    bool fresh = r->src != SRC_NONE && old == r->val;
    if (!fresh) vec_clear_block(off, seg);
    if ((r->src == SRC_VEC || r->src == SRC_VECHR) && fresh) { r->val = val; return; }
    r->under3d = (u8)(r->src == SRC_3D && fresh);
    r->under = old;
    r->src = SRC_VECHR;
    r->val = val;
}

void enh_rec_fill(u16 seg, u16 off, u8 val)
{
    if (!rec_ok(seg, off)) return;
    Shadow *r = &sh_work[off];
    if (enh_route == ENH_ROUTE_3D) { r->src = SRC_3D; r->val = val; stamp3d(seg, off, val); }
    else r->src = SRC_NONE;                                          /* plain art over what was there */
}

/* S3.2: the 1x page pixel at off was recoloured in place, new = f(old) (the FLIR ramp 98h-9Fh of the TV window, poly_fill mode 3). A 3D record that still holds
 * `old` follows: the colours of its N x N layer block go through the same f (written and stamped pixels alike) and the record takes the new value, so the tinted
 * window stays at N x. Anything else is plain art over what was there (false: the caller records it as art). */
bool enh_rec_remap(u16 seg, u16 off, u8 old, u8 val, u8 (*f)(u8))
{
    if (!rec_ok(seg, off)) return false;
    Shadow *r = &sh_work[off];
    if (r->src != SRC_3D || r->val != old || !l3d_work || (enh_hr.on && seg != enh_hr.seg)) return false;
    const int n = enh_cv.n;
    u16 *p = l3d_work + (size_t)(enh_cv.fy + (off / 320) * n) * enh_cv.lw + (size_t)(enh_cv.fx + (off % 320) * n);
    for (int ky = 0; ky < n; ky++, p += enh_cv.lw)
        for (int kx = 0; kx < n; kx++)
            if (p[kx] & 0x100) p[kx] = (u16)((p[kx] & 0x0300) | f((u8)p[kx]));   /* S3.5: the tint drops the blend code (another ramp) */
    const size_t at = (size_t)(enh_cv.fy + (off / 320) * n) * enh_cv.lw + (size_t)(enh_cv.fx + (off % 320) * n);
    for (int ky = 0; ky < n; ky++) memset(l3n_work + at + (size_t)ky * enh_cv.lw, 0, (size_t)n);   /* S3.6: and the night factor */
    r->val = val;
    return true;
}

void enh_rec_3d(u16 seg, u16 off, u8 val)
{
    if (!enh_hr.on) { enh_rec_fill(seg, off, val); return; }
    if (!rec_ok(seg, off)) return;
    sh_work[off].src = SRC_3D;
    sh_work[off].val = val;
}

void enh_rec_span3d(u16 seg, u16 off, int len)
{
    if (!enh_page_tracked(seg)) return;
    const u8 *pg = seg_ptr(seg);
    for (int i = 0; i < len; i++) enh_rec_3d(seg, (u16)(off + i), pg[(u16)(off + i)]);
}

bool enh_shadow_vechr(u16 off, u8 *val)
{
    if (off >= PAGE_PIXELS || sh_work[off].src != SRC_VECHR || sh_work[off].under == sh_work[off].val) return false;   /* a footprint: the page changed under it */
    *val = sh_work[off].val;
    return true;
}

bool enh_shadow_under(u16 off, u8 *under, bool *under3d)      /* S3.3: what a vector record replaced */
{
    if (off >= PAGE_PIXELS || (sh_work[off].src != SRC_VECHR && sh_work[off].src != SRC_VEC)) return false;
    *under = sh_work[off].under; *under3d = sh_work[off].under3d != 0;
    return true;
}

bool enh_shadow_3d(u16 off, u8 *val)
{
    if (off >= PAGE_PIXELS || sh_work[off].src != SRC_3D) return false;
    *val = sh_work[off].val;
    return true;
}

void enh_pass_clear(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 319) x1 = 319;
    if (y1 > 199) y1 = 199;
    if (x1 < x0 || y1 < y0) return;
    for (int y = y0; y <= y1; y++) memset(&sh_work[y * 320 + x0], 0, (size_t)(x1 - x0 + 1) * sizeof(Shadow));
    if (!layer_px) return;
    const int n = enh_cv.n, lw = enh_cv.lw;
    for (int y = enh_cv.fy + n * y0; y < enh_cv.fy + n * (y1 + 1); y++) {
        size_t at = (size_t)y * lw + (size_t)(enh_cv.fx + n * x0), cnt = (size_t)n * (size_t)(x1 - x0 + 1);
        memset(l3d_work + at, 0, cnt * sizeof(u16));
        memset(lvec_work + at, 0, cnt * sizeof(u16));
        memset(l3n_work + at, 0, cnt);                                /* S3.6 */
    }
}

void enh_layers_reset(void)
{
    memset(sh_work, 0, PAGE_PIXELS * sizeof(Shadow));
    memset(sh_shown, 0, PAGE_PIXELS * sizeof(Shadow));
    if (!layer_px) return;
    memset(l3d_work, 0, layer_px * sizeof(u16)); memset(l3d_shown, 0, layer_px * sizeof(u16));
    memset(lvec_work, 0, layer_px * sizeof(u16)); memset(lvec_shown, 0, layer_px * sizeof(u16));
    memset(l3n_work, 0, layer_px); memset(l3n_shown, 0, layer_px);    /* S3.6 */
}

void enh_page_cleared(u16 seg)
{
    if (!enh_page_tracked(seg)) return;
    memset(sh_work, 0, PAGE_PIXELS * sizeof(Shadow));
    if (seg == 0xA000) memset(sh_shown, 0, PAGE_PIXELS * sizeof(Shadow));
}

static void probe_flip(void);

/* ---------------------------------------------------------------- the wide picture (S3.4) */

/* The 3D layer (whole canvas) and the vector layer (used outside the frame) of the last wide display pass, as its flip left them. [0] = pending (the pass of the
 * frame being drawn), [1] = current (the frame on screen): the live flip of the canonical frame makes the pending one current. [2] (S4.1): the wide pass of the
 * display refresh being built, composed with that refresh's main pass (enh_compose_display) and then void. */
typedef struct { bool valid; EnhCanvas cv; int x0, y0, x1, y1; u16 *l3, *lv; u8 *n3; size_t cap; } Held;   /* n3: S3.6, the night plane of l3 */
static Held held[3];

void enh_wide_capture(void) { enh_wide_capture_to(0); }

void enh_wide_capture_to(int slot)
{
    Held *h = &held[slot];
    h->valid = false;
    if (!layer_px || !enh_wide_drawn) return;
    if (h->cap < layer_px) {
        free(h->l3); free(h->lv); free(h->n3);
        h->l3 = malloc(layer_px * sizeof(u16)); h->lv = malloc(layer_px * sizeof(u16)); h->n3 = malloc(layer_px);
        h->cap = h->l3 && h->lv && h->n3 ? layer_px : 0;
        if (!h->cap) { free(h->l3); free(h->lv); free(h->n3); h->l3 = h->lv = NULL; h->n3 = NULL; return; }
    }
    memcpy(h->l3, l3d_shown, layer_px * sizeof(u16));
    memcpy(h->lv, lvec_shown, layer_px * sizeof(u16));
    memcpy(h->n3, l3n_shown, layer_px);                               /* S3.6 */
    h->cv = enh_cv;
    h->x0 = enh_wide_win[0]; h->y0 = enh_wide_win[1]; h->x1 = enh_wide_win[2]; h->y1 = enh_wide_win[3];
    h->valid = true;
}

static void held_promote(void)
{
    if (held[0].valid) { Held t = held[1]; held[1] = held[0]; held[0] = t; held[0].valid = false; }
    else held[1].valid = false;
}

/* the extended area outside the frame is cleared when a wide main pass opens (the frame's own rectangles are cleared by enh_pass_clear) */
void enh_wide_clear_outside(void)
{
    if (!layer_px) return;
    const int n = enh_cv.n, lw = enh_cv.lw, fx = enh_cv.fx, fy = enh_cv.fy;
    for (int y = 0; y < enh_cv.lh; y++) {
        u16 *a = l3d_work + (size_t)y * lw, *b = lvec_work + (size_t)y * lw;
        u8 *c = l3n_work + (size_t)y * lw;                            /* S3.6 */
        if (y < fy || y >= fy + 200 * n) { memset(a, 0, (size_t)lw * sizeof(u16)); memset(b, 0, (size_t)lw * sizeof(u16)); memset(c, 0, (size_t)lw); continue; }
        memset(a, 0, (size_t)fx * sizeof(u16)); memset(b, 0, (size_t)fx * sizeof(u16)); memset(c, 0, (size_t)fx);
        const int r = fx + 320 * n;
        memset(a + r, 0, (size_t)(lw - r) * sizeof(u16)); memset(b + r, 0, (size_t)(lw - r) * sizeof(u16)); memset(c + r, 0, (size_t)(lw - r));
    }
}

void enh_flip(unsigned bytes)
{
    size_t n = bytes < PAGE_PIXELS ? bytes : PAGE_PIXELS;
    hr_night_late();                                                  /* S3.6 fix 2: the night window's pixels written after its pass */
    if (!enh_sbx) { held_promote(); present_hold = false; }          /* S3.4: the wide picture of this frame goes on screen with it (fix 4: the held picture is released) */
    memcpy(sh_shown, sh_work, n * sizeof(Shadow));
    if (layer_px && enh_sbx) {                                        /* S4.3: in a display pass the work layers are not drawn on after its flip (the pass ends and its
                                                                       * canvas is dropped): the shown layers become the work ones without a copy */
        u16 *t = l3d_shown; l3d_shown = l3d_work; l3d_work = t;
        t = lvec_shown; lvec_shown = lvec_work; lvec_work = t;
        u8 *t8 = l3n_shown; l3n_shown = l3n_work; l3n_work = t8;
    } else if (layer_px) {
        memcpy(l3d_shown, l3d_work, layer_px * sizeof(u16));
        memcpy(lvec_shown, lvec_work, layer_px * sizeof(u16));
        memcpy(l3n_shown, l3n_work, layer_px);                        /* S3.6 */
    }
    if (enh.mfd_probe[0] && !enh_sbx) probe_flip();
}

/* ---------------------------------------------------------------- compose (2.5) */

/* idx/sidx: the shown 320x200 page and the page offset of each pixel (0xFFFF = none); pal: the live DAC as XRGB. */
/* the shadow plane and the 3D / vector layers being composed; S3.4: l3h / lvh = the wide picture (held): its 3D layer inside the frame rectangle hx0..hx1, hy0..hy1
 * (frame px: the 3D window of its main pass) and everywhere outside the frame, its vector layer outside the frame; cls (test aid): per canvas pixel what it shows */
typedef struct { const Shadow *sh; const u16 *l3, *lv; const u16 *l3h, *lvh; int hx0, hy0, hx1, hy1; u8 *cls; bool blend;
                 const u8 *n3, *n3h; u8 *ndo; } ComposeSrc;   /* ndo (test aid): per canvas pixel the night value the 3D pixel had (0 = none / not 3D) */   /* blend: S3.5, pal is a real palette; n3 / n3h: S3.6, the night planes of l3 / l3h (NULL = none) */

/* S3.5 (DESIGN 2.11): a 3D layer pixel; with a blend code (bits 10..15, --smooth-shading only) the mix of its colour and the neighbour entry of the same 16-entry ramp */
static inline u32 l3_rgb(const u32 *pal, u16 l, bool blend)
{
    const u8 i = (u8)l;
    if (!blend || !(l & HR_BLEND_MASK)) return pal[i];
    const int d = (int)(l >> HR_BLEND_SHIFT) - 32, w = d < 0 ? -d : d;
    const u8 j = (u8)(d > 0 ? i + 1 : i - 1);
    if ((i ^ j) & 0xF0) return pal[i];
    const u32 a = pal[i], b = pal[j];
    u32 o = 0;
    for (int sh = 0; sh <= 16; sh += 8) o |= ((((a >> sh) & 255) * (u32)(64 - w) + ((b >> sh) & 255) * (u32)w + 32) >> 6) << sh;
    return o;
}
/* S3.6 (DESIGN 2.12): a 3D layer pixel of a true-night pass, its colour times nd / 256 (0 = as the original) */
static inline u32 l3_night(u32 c, u8 nd)
{
    if (!nd || nd == HR_NIGHT_LIT) return c;
    return ((((c >> 16) & 255) * nd + 128) >> 8) << 16 | ((((c >> 8) & 255) * nd + 128) >> 8) << 8 | (((c & 255) * nd + 128) >> 8);
}
enum { CLS_BLACK, CLS_ART, CLS_WORLD, CLS_VEC, CLS_WORLD1 };   /* nothing / a page pixel (art, text, panel) / a 3D layer pixel / a vector layer pixel / a 3D record shown as its 1x pixel */

/* S4.3: the canvas rows ylo..yhi - 1 of a composition (compose_canvas runs bands of rows on the worker pool, par.h) */
typedef struct { u32 *out; const u8 *idx; const u16 *sidx; const u32 *pal; const ComposeSrc *cs; } ComposeJob;

static void compose_rows(void *jp, int ylo, int yhi)
{
    const ComposeJob *job = jp;
    u32 *const out = job->out;
    const u8 *const idx = job->idx;
    const u16 *const sidx = job->sidx;
    const u32 *const pal = job->pal;
    const ComposeSrc *const cs = job->cs;
    const Shadow *sh_shown = cs->sh;
    const u16 *l3d_shown = cs->l3, *lvec_shown = cs->lv;
    const int lw = enh_cv.lw, n = enh_cv.n, fx = enh_cv.fx, fy = enh_cv.fy;
    u8 *const cls = cs->cls;
    memset(out + (size_t)ylo * lw, 0, (size_t)lw * (size_t)(yhi - ylo) * sizeof(u32));
    if (cls) memset(cls + (size_t)ylo * lw, CLS_BLACK, (size_t)lw * (size_t)(yhi - ylo));
    const bool layers = layer_px != 0;
    if (layers && (enh_cv.vw > 320 * n || enh_cv.vh > 200 * n)) {    /* the extended area: the 3D layer around the frame (S3.4: the wide picture's, vector layer on top) */
        const u16 *ext3 = cs->l3h ? cs->l3h : l3d_shown;
        const u8 *extn = cs->l3h ? cs->n3h : cs->n3;                   /* S3.6 */
        for (int y = ylo; y < yhi; y++) {
            u32 *o = out + (size_t)y * lw;
            const u16 *l = ext3 + (size_t)y * lw, *v = cs->lvh ? cs->lvh + (size_t)y * lw : NULL;
            const u8 *nl = extn ? extn + (size_t)y * lw : NULL;
            u8 *c = cls ? cls + (size_t)y * lw : NULL;
            bool in_rows = y >= fy && y < fy + 200 * n;
            for (int x = 0; x < lw; x++) {
                if (in_rows && x >= fx && x < fx + 320 * n) { x = fx + 320 * n - 1; continue; }
                if (v && (v[x] & 0x100)) { o[x] = pal[v[x] & 255]; if (c) c[x] = CLS_VEC; }
                else if (l[x] & 0x100) { o[x] = l3_rgb(pal, l[x], cs->blend); if (nl) o[x] = l3_night(o[x], nl[x]); if (c) c[x] = CLS_WORLD; if (cs->ndo && nl) cs->ndo[(size_t)y * lw + x] = nl[x]; }
            }
        }
    }
    for (int y = 0; y < 200; y++) {
        const bool hrow = cs->l3h && y >= cs->hy0 && y <= cs->hy1;
        for (int ky = 0; ky < n; ky++) {
            if (fy + y * n + ky < ylo || fy + y * n + ky >= yhi) continue;   /* S4.3: another band's row */
            u32 *o = out + (size_t)(fy + y * n + ky) * lw + fx;
            u8 *co = cls ? cls + (size_t)(fy + y * n + ky) * lw + fx : NULL;
            for (int x = 0; x < 320; x++, o += n) {
                const u8 v = idx[y * 320 + x];
                const u16 si = sidx[y * 320 + x];
                const Shadow *r = si < PAGE_PIXELS ? &sh_shown[si] : NULL;
                if (!layers || !r || r->src == SRC_NONE || r->val != v) {
                    for (int kx = 0; kx < n; kx++) o[kx] = pal[v];
                    if (co) memset(co + x * n, CLS_ART, (size_t)n);
                    if (layers && v >= 0x70 && v <= 0x8F) {                  /* S3.6 fix 2: a sky / ground band colour (only the 3D backdrop draws them) shown as its 1x
                                                                              * page pixel inside a night window (the record lost, e.g. a 1x write after the pass): the factor
                                                                              * of its place, like the 3D layer around it */
                        const size_t at = (size_t)(fy + y * n + ky) * lw + (size_t)(fx + x * n);
                        const u8 *N = hrow && x >= cs->hx0 && x <= cs->hx1 ? cs->n3h : cs->n3;
                        if (N) for (int kx = 0; kx < n; kx++) {
                            o[kx] = l3_night(o[kx], N[at + kx]);
                            if (cs->ndo && N[at + kx] && N[at + kx] != HR_NIGHT_LIT) cs->ndo[at + kx] = 3;   /* test aid: a band pixel shown from the page */
                        }
                    }
                    if (cs->ndo && r && r->src == SRC_3D)                    /* S3.6 test aid: a 3D record the page no longer matches (shown as the 1x pixel) */
                        for (int kx = 0; kx < n; kx++) if (!cs->ndo[(size_t)(fy + y * n + ky) * lw + (size_t)(fx + x * n + kx)]) cs->ndo[(size_t)(fy + y * n + ky) * lw + (size_t)(fx + x * n + kx)] = 1;
                    continue;
                }
                const size_t at = (size_t)(fy + y * n + ky) * lw + (size_t)(fx + x * n);
                const bool hw = hrow && x >= cs->hx0 && x <= cs->hx1;
                const u16 *L3 = hw ? cs->l3h : l3d_shown;
                const u8 *N3 = hw ? cs->n3h : cs->n3;                  /* S3.6 */
                if (r->src == SRC_3D) {
                    for (int kx = 0; kx < n; kx++) {
                        u16 l = L3[at + kx];
                        o[kx] = (l & 0x100) ? l3_rgb(pal, l, cs->blend) : pal[v];
                        if (N3) o[kx] = l3_night(o[kx], N3[at + kx]);     /* also a 1x pixel of the scene (the pass end gave it a factor) */
                        if (cs->ndo && N3) cs->ndo[at + kx] = N3[at + kx];
                        if (co) co[x * n + kx] = (l & 0x100) ? CLS_WORLD : CLS_WORLD1;
                    }
                    continue;
                }
                bool any = false;                                      /* vector pixel: its layer pixels over what was below it */
                for (int by = 0; by < n && !any; by++)
                    for (int kx = 0; kx < n; kx++)
                        if (lvec_shown[(size_t)(fy + y * n + by) * lw + (size_t)(fx + x * n + kx)] & 0x100) { any = true; break; }
                if (!any && r->src != SRC_VECHR) {                     /* nothing emitted: the original pixel */
                    for (int kx = 0; kx < n; kx++) o[kx] = pal[v];
                    if (co) memset(co + x * n, CLS_VEC, (size_t)n);
                    continue;
                }
                for (int kx = 0; kx < n; kx++) {
                    u16 lv = lvec_shown[at + kx];
                    u16 l3 = L3[at + kx];
                    o[kx] = (lv & 0x100) ? pal[lv & 255] : (r->under3d && (l3 & 0x100)) ? l3_rgb(pal, l3, cs->blend) : pal[r->under];
                    if (!(lv & 0x100) && r->under3d && N3) {                /* S3.6: the 3D pixel under a vector record (layer or 1x) */
                        o[kx] = l3_night(o[kx], N3[at + kx]);
                        if (cs->ndo) cs->ndo[at + kx] = N3[at + kx];
                    }
                    else if (!(lv & 0x100) && N3 && N3[at + kx] && cs->ndo) cs->ndo[at + kx] = 2;   /* test aid: the page value under a vector record in the night window */
                    if (co) co[x * n + kx] = (lv & 0x100) ? CLS_VEC : (r->under3d && (l3 & 0x100)) ? CLS_WORLD : r->under3d ? CLS_WORLD1 : CLS_ART;
                }
            }
        }
    }
}

static void compose_canvas(u32 *out, const u8 *idx, const u16 *sidx, const u32 *pal, bool screen, const ComposeSrc *cs)
{
    const int lw = enh_cv.lw, lh = enh_cv.lh;
    if (!screen) {
        memset(out, 0, (size_t)lw * (size_t)lh * sizeof(u32));
        if (cs->cls) memset(cs->cls, CLS_BLACK, (size_t)lw * (size_t)lh);
        return;
    }
    ComposeJob job = { out, idx, sidx, pal, cs };
    enh_par_for(lh, compose_rows, &job);                              /* S4.3: bands of rows on the worker pool (the same pixels as one band) */
}

/* S3.4: the sources of the present path: the shown plane and layers, and the wide picture h when it belongs to this placement (S4.1: inside a display pass the
 * shown plane and layers are the pass's) */
static void compose_src(ComposeSrc *cs, const Held *h)
{
    memset(cs, 0, sizeof *cs);
    cs->sh = sh_shown; cs->l3 = l3d_shown; cs->lv = lvec_shown;
    cs->blend = true;                                                 /* S3.5: the live DAC */
    cs->n3 = l3n_shown;                                               /* S3.6 */
    if (h->valid && !menu_on && enh_canvas_extended() && h->cap >= layer_px && !memcmp(&h->cv, &enh_cv, sizeof enh_cv)) {
        cs->l3h = h->l3; cs->lvh = h->lv; cs->n3h = h->n3;
        cs->hx0 = h->x0; cs->hy0 = h->y0; cs->hx1 = h->x1; cs->hy1 = h->y1;
    }
}

static void compose_src_live(ComposeSrc *cs) { compose_src(cs, &held[1]); }

/* S4.1: in a display pass after its flip (sandbox.c): the pass's shown page, plane and layers with the refresh's wide picture (held[2]) into out (the pass's canvas
 * size); the DAC / CRTC are the live ones (sandbox.c puts them in before). */
void enh_compose_display(u32 *out)
{
    static u8 idx[PAGE_PIXELS];
    static u16 sidx[PAGE_PIXELS];
    u32 pal[256];
    const bool screen = vga_shown_frame(idx, sidx, pal);
    ComposeSrc cs;
    compose_src(&cs, &held[2]);
    compose_canvas(out, idx, sidx, pal, screen, &cs);
    held[2].valid = false;
}

/* S4.3: the one pass of a display refresh (sandbox.c, after its flip): a wide pass's own shown layers are the wide picture (S4.1 took them from a second pass) */
void enh_compose_display_self(u32 *out)
{
    static u8 idx[PAGE_PIXELS];
    static u16 sidx[PAGE_PIXELS];
    u32 pal[256];
    const bool screen = vga_shown_frame(idx, sidx, pal);
    ComposeSrc cs;
    held[2].valid = false;
    compose_src(&cs, &held[2]);
    if (enh_wide_drawn && layer_px && !menu_on && enh_canvas_extended()) {
        cs.l3h = l3d_shown; cs.lvh = lvec_shown; cs.n3h = l3n_shown;
        cs.hx0 = enh_wide_win[0]; cs.hy0 = enh_wide_win[1]; cs.hx1 = enh_wide_win[2]; cs.hy1 = enh_wide_win[3];
    }
    compose_canvas(out, idx, sidx, pal, screen, &cs);
}

/* S4.3: the canonical frame's own render as its flip (frame_loop_body: flip(DSW(2E34) * 160)) will show it, composed now with the live look, without a display pass:
 * the picture a pass at alpha 1 would compose (it renders the same A_k with the same fields). For a view that is not extended (no wide picture). The flip is
 * previewed: the shown page's first `bytes` bytes from the back page (put back afterwards), the shown shadow records likewise, the work layers; the night pixels
 * written after the last night pass get their factor now (hr_night_late: the flip would do the same, then has nothing left to do). false = not possible here. */
bool enh_compose_canonical(u32 *out, unsigned bytes)
{
    static u8 idx[PAGE_PIXELS], save[PAGE_PIXELS];
    static u16 sidx[PAGE_PIXELS];
    static Shadow sh[PAGE_PIXELS];
    if (!layer_px || menu_on || enh_canvas_extended() || video_page_seg(0) != 0xA000 || !video_page_seg(1) || video_page_seg(1) == 0xA000) return false;
    const size_t n = bytes < PAGE_PIXELS ? bytes : PAGE_PIXELS;
    u8 *vr = vga_vram();
    hr_night_late();
    memcpy(save, vr, n);
    memcpy(vr, seg_ptr(video_page_seg(1)), n);
    u32 pal[256];
    const bool screen = vga_shown_frame(idx, sidx, pal);
    memcpy(vr, save, n);
    memcpy(sh, sh_shown, sizeof sh);
    memcpy(sh, sh_work, n * sizeof(Shadow));
    ComposeSrc cs;
    memset(&cs, 0, sizeof cs);
    cs.sh = sh; cs.l3 = l3d_work; cs.lv = lvec_work; cs.n3 = l3n_work;
    cs.blend = true;
    compose_canvas(out, idx, sidx, pal, screen, &cs);
    return true;
}

bool enh_compose_menu(void) { return menu_on; }

bool enh_compose_vga(u32 *xrgb)
{
    static u8 idx[PAGE_PIXELS];
    static u16 sidx[PAGE_PIXELS];
    static u32 *last;
    static size_t last_px;
    u32 pal[256];
    const size_t px = (size_t)enh_cv.lw * (size_t)enh_cv.lh;
    if (present_hold) return false;                                   /* S3.4 fix 4: the last picture stays until the new placement's first frame is flipped */
    if (!menu_on) {                                                   /* S4.1: in flight the display passes' pictures are presented (sandbox.c), not the live page */
        const int d = enh_display_take(xrgb);
        if (d >= 0) { last_px = (size_t)-1; return d > 0; }
    }
    bool screen = vga_shown_frame(idx, sidx, pal);
    ComposeSrc cs;
    compose_src_live(&cs);                                            /* ENH: S3.4, with the wide picture */
    compose_canvas(xrgb, idx, sidx, pal, screen, &cs);
    if (last_px == (size_t)-1) { free(last); last = NULL; last_px = 0; }   /* S4.1: a display picture was shown since */
    if (last_px == px && last && !memcmp(last, xrgb, px * sizeof(u32))) return false;
    if (last_px != px) { free(last); last = malloc(px * sizeof(u32)); last_px = last ? px : 0; }
    if (last) memcpy(last, xrgb, px * sizeof(u32));
    return true;
}

/* S3.1 test aid (the hr check and the dumps of the render test): the frame being drawn (the work shadow and layers) composed from `page` (the
 * 320x200 page of its pass) with `pal` (256 entries). Same rule as the present path. */
void enh_compose_work(u32 *out, const u8 *page, const u32 *pal, bool blend)
{
    static u16 ident[PAGE_PIXELS];
    if (ident[1] == 0) for (int i = 0; i < PAGE_PIXELS; i++) ident[i] = (u16)i;
    const ComposeSrc cs = { sh_work, l3d_work, lvec_work, NULL, NULL, 0, 0, 0, 0, NULL, blend, blend ? l3n_work : NULL, NULL, NULL };   /* S3.5: blend = pal is the DAC; S3.6 */
    compose_canvas(out, page, ident, pal, true, &cs);
}

/* ---------------------------------------------------------------- --mfd-probe (S3.2 test aid) */

/* After every page flip: the pixels of the TV window (x 169..245, y 110..169) and of the map window (x 74..150, y 110..169) of the shown page that come from a 3D
 * record equal to the page (so the compositor shows the 3D layer there) against the window size; F117_PROBE_FRAMES=a,b writes the composed shown canvas of those frames
 * to PREFIX_<frame>.raw (s32 lw, lh, then lw * lh XRGB u32 as composed, then the same frame as the classic 1x page scaled N x N). */
/* S3.4: the canvas as presented after this flip, per pixel class (compose_canvas cls): the 3D world (hr layer / 1x 3D pixel), art (page pixels), vector, black; the same
 * outside the frame (the extended area); and the seams: of the pixel pairs across the frame border (left, right, top) where both sides show the 3D world, how many
 * differ, against the same count across block boundaries inside the frame (columns N 80, N 160, N 240 of the frame) */
static void probe_wide(FILE *f, const u8 *pg)
{
    if (!layer_px) return;
    const int lw = enh_cv.lw, lh = enh_cv.lh, n = enh_cv.n, fx = enh_cv.fx, fy = enh_cv.fy;
    u32 *img = malloc(layer_px * sizeof(u32));
    u8 *cl = malloc(layer_px);
    if (!img || !cl) { free(img); free(cl); return; }
    static u16 ident[PAGE_PIXELS];
    for (int i = 0; i < PAGE_PIXELS; i++) ident[i] = (u16)i;
    u32 pal[256];
    u8 dac[768];
    dac_get(0, 256, dac);
    for (int i = 0; i < 256; i++) pal[i] = (u32)dac[3 * i] << 16 | (u32)dac[3 * i + 1] << 8 | dac[3 * i + 2];
    ComposeSrc cs;
    compose_src_live(&cs);
    cs.cls = cl;
    compose_canvas(img, pg, ident, pal, true, &cs);
    long all[5] = { 0 }, ext[5] = { 0 };
    for (int y = 0; y < lh; y++)
        for (int x = 0; x < lw; x++) {
            const u8 c = cl[(size_t)y * lw + x];
            all[c]++;
            if (y < fy || y >= fy + 200 * n || x < fx || x >= fx + 320 * n) ext[c]++;
        }
    long sp[4] = { 0 }, sd[4] = { 0 };      /* left, right, top, inside reference */
#define PAIR(k, xa, ya, xb, yb) do { const size_t a_ = (size_t)(ya) * lw + (size_t)(xa), b_ = (size_t)(yb) * lw + (size_t)(xb); \
        if (cl[a_] == CLS_WORLD && cl[b_] == CLS_WORLD) { sp[k]++; if (img[a_] != img[b_]) sd[k]++; } } while (0)
    for (int y = fy; y < fy + 200 * n; y++) {
        if (fx > 0) PAIR(0, fx - 1, y, fx, y);
        if (fx + 320 * n < lw) PAIR(1, fx + 320 * n - 1, y, fx + 320 * n, y);
        for (int k = 1; k <= 3; k++) PAIR(3, fx + n * 80 * k - 1, y, fx + n * 80 * k, y);
    }
    if (fy > 0) for (int x = fx; x < fx + 320 * n; x++) PAIR(2, x, fy - 1, x, fy);
#undef PAIR
    long barmis = 0, barpx = 0;                /* S3.4 fix: the 7898 bars beside the tapes (x 72 stall margin, x 247 VSI; colour 0x2A at night): rows shown in the bar colour vs the 1x rows */
    for (int k = 0; k < 2; k++) {
        const int bx = k ? 247 : 72;
        for (int y = 0; y < 91; y++) {
            const bool p1 = pg[y * 320 + bx] == 0x2A;
            bool sh = false;
            for (int kx = 0; kx < n && !sh; kx++) sh = img[(size_t)(fy + y * n + n / 2) * lw + (size_t)(fx + bx * n + kx)] == pal[0x2A];
            if (p1 || sh) barpx++;
            if (p1 != sh) barmis++;
        }
    }
    fprintf(f, " bars=%ld/%ld", barmis, barpx);
    fprintf(f, " lw=%d lh=%d n=%d fx=%d fy=%d vclass=%d held=%d style=%x world=%ld world1=%ld art=%ld vec=%ld black=%ld eworld=%ld eblack=%ld evec=%ld epx=%ld"
               " seaml=%ld/%ld seamr=%ld/%ld seamt=%ld/%ld seamref=%ld/%ld", lw, lh, n, fx, fy, (int)view, cs.l3h != NULL, DSB(0x294B), all[CLS_WORLD], all[CLS_WORLD1],
            all[CLS_ART], all[CLS_VEC], all[CLS_BLACK], ext[CLS_WORLD], ext[CLS_BLACK], ext[CLS_VEC], ext[0] + ext[1] + ext[2] + ext[3] + ext[4],
            sd[0], sp[0], sd[1], sp[1], sd[2], sp[2], sd[3], sp[3]);
    free(img); free(cl);
}

static void probe_flip(void)
{
    static int frame;
    static FILE *f;
    if (!f) { char p[1100]; snprintf(p, sizeof p, "%s.txt", enh.mfd_probe); f = fopen(p, "w"); if (!f) return; }
    const u8 *pg = seg_ptr(0xA000);
    const struct { int x0, y0, x1, y1; } w[2] = { { 169, 110, 245, 169 }, { 74, 110, 150, 169 } };
    long c3[2] = { 0, 0 }, cn[2] = { 0, 0 };
    for (int k = 0; k < 2; k++)
        for (int y = w[k].y0; y <= w[k].y1; y++)
            for (int x = w[k].x0; x <= w[k].x1; x++) {
                const Shadow *r = &sh_shown[y * 320 + x];
                cn[k]++;
                if (r->src == SRC_3D && r->val == pg[y * 320 + x]) c3[k]++;
            }
    /* S3.3: the vector records of the shown page (a fresh line record) and how many of them show an hr pixel: the HUD window, the map window, the whole frame, and the elements of the
     * HUD: the airspeed tape (left), the altitude tape (right), the glass frame (left / right edge), the centre with the pitch ladder and the reticle */
    enum { VR_HUD, VR_MAP, VR_ALL, VR_TAPEL, VR_TAPER, VR_FRAMEL, VR_FRAMER, VR_CENTRE, VR_N };
    static const char *const vname[VR_N] = { "hudv", "mapv", "allv", "tapel", "taper", "framel", "framer", "centre" };
    long vh[VR_N] = { 0 }, vt[VR_N] = { 0 };
    const struct { int x0, y0, x1, y1; } vw[VR_N] = { { 48, 2, 271, 90 }, { 74, 110, 150, 169 }, { 0, 0, 319, 199 }, { 62, 20, 82, 84 }, { 240, 20, 260, 84 }, { 44, 12, 47, 90 }, { 273, 12, 276, 90 },
                                                       { 100, 20, 220, 80 } };
    for (int k = 0; k < VR_N; k++)
        for (int y = vw[k].y0; y <= vw[k].y1; y++)
            for (int x = vw[k].x0; x <= vw[k].x1; x++) {
                const Shadow *r = &sh_shown[y * 320 + x];
                if (!(r->src == SRC_VEC || r->src == SRC_VECHR) || r->val != pg[y * 320 + x] || r->val == 0) continue;   /* (colour 0 lines stay 1x pixels: they erase) */
                vt[k]++;
                if (r->src != SRC_VECHR || !lvec_shown) continue;
                const int nn = enh_cv.n;
                bool any = false;
                for (int ky = 0; ky < nn && !any; ky++)
                    for (int kx = 0; kx < nn; kx++)
                        if (lvec_shown[(size_t)(enh_cv.fy + y * nn + ky) * enh_cv.lw + (size_t)(enh_cv.fx + x * nn + kx)] & 0x100) { any = true; break; }
                if (any) vh[k]++;
            }
    fprintf(f, "frame %d tv %ld/%ld map %ld/%ld rmfd=%x lmfdoff=%x cockpit=%x view=%x p4088=%x draw=%x master=%x site=%x unit=%x hudcol=%x/%x hudlvl=%x", frame, c3[0], cn[0], c3[1], cn[1],
            DSW(0xDFDC), DSW(0xDEDA), DSW(0x365E), DSW(0xC07A), DSB(0x4088), video_draw_seg(), DSW(0xE2FE), DSW(0x3D66), DSW(0x3D64), DSB(0x2CA0), DSB(0x2CA1), DSB(0x294A));
    for (int k = 0; k < VR_N; k++) fprintf(f, " %s %ld/%ld", vname[k], vh[k], vt[k]);
    probe_wide(f, pg);                                                /* S3.4 */
    {   /* S3.4 fix: hr vector pixels left in a block whose last write was a classic 1x line (vstale; the compositor would show the stale hr line there) */
        long vmis = 0;
        int bx0 = 999, by0 = 999, bx1 = -1, by1 = -1, mcol = -1, mv = -1;
        const int nn = enh_cv.n;
        for (int y = 0; y < 107 && lvec_shown; y++)                  /* the HUD (the 3D window rows; the map's 2x2 blips legitimately sit on route-line blocks) */
            for (int x = 0; x < 320; x++) {
                const Shadow *r = &sh_shown[y * 320 + x];
                const u8 v = pg[y * 320 + x];
                if (r->src != SRC_VEC || r->val != v) continue;          /* the last write was a classic (1x) line: its block must show no hr line pixel */
                for (int ky = 0; ky < nn; ky++)
                    for (int kx = 0; kx < nn; kx++) {
                        const u16 l = lvec_shown[(size_t)(enh_cv.fy + y * nn + ky) * enh_cv.lw + (size_t)(enh_cv.fx + x * nn + kx)];
                        if (l & 0x100) {
                            vmis++; mcol = (u8)l; mv = v;
                            if (x < bx0) { bx0 = x; } if (x > bx1) { bx1 = x; } if (y < by0) { by0 = y; } if (y > by1) { by1 = y; }
                        }
                    }
            }
        fprintf(f, " spd=%d alt=%d vs=%d night=%d nlate=%ld", (s16)DSW(0xB168), (s16)DSW(0x2DF4), (s16)DSW(0xE55A), DSW(0x3658), hr_night_late_count());
        fprintf(f, " vstale=%ld vstalebox=%d,%d,%d,%d vstalecol=%d/%d", vmis, bx0, by0, bx1, by1, mcol, mv);
    }
    fprintf(f, "\n");
    fflush(f);
    const char *e = getenv("F117_PROBE_FRAMES");
    for (const char *t = e; t && *t; ) {
        char *end;
        long v = strtol(t, &end, 10);
        if (end == t) break;
        if (v == frame && layer_px) {
            u32 *img = malloc(layer_px * sizeof(u32));
            u32 pal[256];
            u8 dac[768];
            dac_get(0, 256, dac);
            for (int i = 0; i < 256; i++) {
                u32 r = (u32)dac[3 * i] << 2, g = (u32)dac[3 * i + 1] << 2, b = (u32)dac[3 * i + 2] << 2;
                pal[i] = ((r | r >> 6) << 16) | ((g | g >> 6) << 8) | (b | b >> 6);
            }
            static u16 ident[PAGE_PIXELS];
            for (int i = 0; i < PAGE_PIXELS; i++) ident[i] = (u16)i;
            u8 *cls = calloc(layer_px, 1), *ndo = calloc(layer_px, 1);     /* S3.6 test aid: the pixel classes and night values follow the two pictures */
            if (img) {
                ComposeSrc cs;
                compose_src_live(&cs);                                /* S3.4: as presented, with the wide picture */
                cs.cls = cls; cs.ndo = ndo;
                compose_canvas(img, pg, ident, pal, true, &cs);
                char p[1100];
                snprintf(p, sizeof p, "%s_%04d.raw", enh.mfd_probe, frame);
                FILE *o = fopen(p, "wb");
                if (o) {
                    s32 hd[2] = { enh_cv.lw, enh_cv.lh };
                    fwrite(hd, 4, 2, o);
                    fwrite(img, 4, layer_px, o);
                    static Shadow none[PAGE_PIXELS];                       /* no records: the 1x page scaled N x N, the classic picture of the same frame */
                    const ComposeSrc c1 = { none, l3d_shown, lvec_shown, NULL, NULL, 0, 0, 0, 0, NULL, false, NULL, NULL, NULL };
                    compose_canvas(img, pg, ident, pal, true, &c1);
                    fwrite(img, 4, layer_px, o);
                    if (cls && ndo) { fwrite(cls, 1, layer_px, o); fwrite(ndo, 1, layer_px, o); }
                    fclose(o);
                }
                free(img);
            }
            free(cls); free(ndo);
        }
        t = *end == ',' ? end + 1 : end;
    }
    frame++;
}

/* ---------------------------------------------------------------- hr vector emitters (S3.1) */

/* A vector layer pixel (layer coordinates) of an hr emitter. The page pixel whose block it lies in becomes a vector record whose "nothing emitted" look is what
 * was under it (SRC_VECHR) unless it is one already, so an hr line shows where the 1x line did not reach and the 1x footprint shows nothing where it did. */
static void vec_put_seg(u16 seg, int lx, int ly, u8 c);

void enh_vec_put(int lx, int ly, u8 c) { vec_put_seg(video_draw_seg(), lx, ly, c); }

static void vec_put_seg(u16 seg, int lx, int ly, u8 c)
{
    const int n = enh_cv.n;
    if (!lvec_work || lx < 0 || ly < 0 || lx >= enh_cv.lw || ly >= enh_cv.lh) return;
    if (lx < enh_cv.fx || ly < enh_cv.fy || lx >= enh_cv.fx + 320 * n || ly >= enh_cv.fy + 200 * n) {   /* outside the frame: only a wide pass draws there (S3.4) */
        if (enh_wide_pt) lvec_work[(size_t)ly * enh_cv.lw + lx] = (u16)(0x100 | c);
        return;
    }
    int px = (lx - enh_cv.fx) / n, py = (ly - enh_cv.fy) / n;
    if (px > 319 || py > 199) return;
    const u16 off = (u16)(py * 320 + px);
    u16 *lv = plane_shown(seg) ? lvec_shown : lvec_work;
    if (enh_page_tracked(seg)) {
        Shadow *r = plane_shown(seg) ? &sh_shown[off] : &sh_work[off];
        const u8 pv = seg_ptr(seg)[off];
        const bool fresh = r->src != SRC_NONE && r->val == pv;
        if (!(fresh && (r->src == SRC_VEC || r->src == SRC_VECHR))) {
            if (!fresh) vec_clear_block(off, seg);
            r->under3d = (u8)(r->src == SRC_3D && fresh);
            r->under = pv;
            r->src = SRC_VECHR;
            r->val = pv;
        }
    }
    lv[(size_t)ly * enh_cv.lw + lx] = (u16)(0x100 | c);
}

/* ---------------------------------------------------------------- moved 1x symbols (S4.6) */

enum { CAP_MAX = 8, CAP_LMAX = CAP_MAX * MAX_SIDE / 320 + 8 };   /* a rectangle of at most 8 x 8 page pixels, N <= 25 */

typedef struct {
    u8 pg[2][CAP_MAX * CAP_MAX];
    Shadow rec[2][CAP_MAX * CAP_MAX];
    u16 l3[2][CAP_LMAX * CAP_LMAX], lv[2][CAP_LMAX * CAP_LMAX];
    u8 l3n[2][CAP_LMAX * CAP_LMAX];
} CapSave;

static void cap_io(CapSave *s, bool save, const u16 seg[2], int np, int x, int y, int w, int h)
{
    const int n = enh_cv.n, ln = w * n;
    for (int p = 0; p < np; p++) {
        u8 *pg = seg_ptr(seg[p]);
        Shadow *sh = p == 0 ? sh_shown : sh_work;   /* page 0 = the shown plane, page 1 = the work plane (plane_shown) */
        u16 *l3 = p == 0 ? l3d_shown : l3d_work, *lv = p == 0 ? lvec_shown : lvec_work;
        u8 *l3n = p == 0 ? l3n_shown : l3n_work;
        if (np == 1) { sh = sh_work; l3 = l3d_work; lv = lvec_work; l3n = l3n_work; }   /* page 1 aliases page 0: all work */
        for (int j = 0; j < h; j++) {
            const int o = (y + j) * 320 + x;
            if (save) { memcpy(s->pg[p] + j * w, pg + o, (size_t)w); memcpy(s->rec[p] + j * w, sh + o, (size_t)w * sizeof(Shadow)); }
            else { memcpy(pg + o, s->pg[p] + j * w, (size_t)w); memcpy(sh + o, s->rec[p] + j * w, (size_t)w * sizeof(Shadow)); }
        }
        if (!l3 || !lv || !l3n) continue;
        for (int j = 0; j < h * n; j++) {
            const size_t at = (size_t)(enh_cv.fy + y * n + j) * enh_cv.lw + (size_t)(enh_cv.fx + x * n);
            if (save) { memcpy(s->l3[p] + j * ln, l3 + at, (size_t)ln * sizeof(u16)); memcpy(s->lv[p] + j * ln, lv + at, (size_t)ln * sizeof(u16)); memcpy(s->l3n[p] + j * ln, l3n + at, (size_t)ln); }
            else { memcpy(l3 + at, s->l3[p] + j * ln, (size_t)ln * sizeof(u16)); memcpy(lv + at, s->lv[p] + j * ln, (size_t)ln * sizeof(u16)); memcpy(l3n + at, s->l3n[p] + j * ln, (size_t)ln); }
        }
    }
}

static void cap_fill(const u16 seg[2], int np, int x, int y, int w, int h, u8 v)
{
    for (int p = 0; p < np; p++)
        for (int j = 0; j < h; j++) memset(seg_ptr(seg[p]) + (y + j) * 320 + x, v, (size_t)w);
}

void enh_cap_shift(int x, int y, int w, int h, int ox, int oy, const int clip[4], void (*op)(void *), void *arg)
{
    static CapSave sv;
    static u8 v0[2][CAP_MAX * CAP_MAX], wr[2][CAP_MAX * CAP_MAX];
    const int n = enh_cv.n;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 200) h = 200 - y;
    if (w <= 0 || h <= 0 || w > CAP_MAX || h > CAP_MAX || n > 25 || !lvec_work || !l3d_work || !l3n_work) { op(arg); return; }
    const u16 seg[2] = { 0xA000, video_page_seg(1) };
    const int np = seg[1] == 0 || seg[1] == 0xA000 ? 1 : 2;
    cap_io(&sv, true, seg, np, x, y, w, h);
    cap_fill(seg, np, x, y, w, h, 0x00);                      /* the op over background 0 ... */
    op(arg);
    for (int p = 0; p < np; p++)
        for (int j = 0; j < h; j++) memcpy(v0[p] + j * w, seg_ptr(seg[p]) + (y + j) * 320 + x, (size_t)w);
    cap_io(&sv, false, seg, np, x, y, w, h);
    cap_fill(seg, np, x, y, w, h, 0xFF);                      /* ... and over FFh: a pixel it wrote reads the same both times */
    op(arg);
    for (int p = 0; p < np; p++)
        for (int j = 0; j < h; j++) {
            const u8 *pg = seg_ptr(seg[p]) + (y + j) * 320 + x;
            for (int i = 0; i < w; i++) wr[p][j * w + i] = (u8)(v0[p][j * w + i] == pg[i]);
        }
    cap_io(&sv, false, seg, np, x, y, w, h);                  /* the rectangle as it was */
    for (int p = 0; p < np; p++)
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                if (!wr[p][j * w + i]) continue;
                const u8 c = v0[p][j * w + i];
                const int lx0 = enh_cv.fx + (x + i) * n + ox, ly0 = enh_cv.fy + (y + j) * n + oy;
                for (int ky = 0; ky < n; ky++)
                    for (int kx = 0; kx < n; kx++) {
                        const int lx = lx0 + kx, ly = ly0 + ky;
                        if (lx < enh_cv.fx || ly < enh_cv.fy) continue;
                        const int qx = (lx - enh_cv.fx) / n, qy = (ly - enh_cv.fy) / n;   /* the 1x pixel the layer pixel lies in */
                        if (qx < clip[0] || qy < clip[1] || qx >= clip[2] || qy >= clip[3]) continue;
                        vec_put_seg(seg[p], lx, ly, c);
                    }
            }
}

/* ---------------------------------------------------------------- page-to-page blits (S3.2 fix) */

/* The shadow has two planes: work (the page being drawn, page 1) and shown (page 0, the displayed one; enh_flip copies work to shown for the bytes the flip copies).
 * The game also moves rectangles between the pages with blit_rect: every frame view_frame copies the two MFD windows (map x 74.., TV x 169.., 77x60) from the draw page to the
 * other page, and the flip does not cover those rows, so without this the displayed page's records for the MFDs were never refreshed and the windows showed as 1x pixels.
 * enh_blit follows such a blit: records and the three layers go along between two tracked pages (page 0 = shown, page 1 = work); a blit from any other page (the
 * picture page 2: panels, fonts, saved backgrounds) is art, so the records under its destination rectangle are voided. */
void enh_blit(u16 sseg, int sx, int sy, u16 dseg, int dx, int dy, int w, int h)
{
    const bool dt = enh_page_tracked(dseg);
    if (!dt || !sh_work || w <= 0 || h <= 0 || sx < 0 || sy < 0 || dx < 0 || dy < 0 || sx + w > 320 || dx + w > 320 || sy + h > 200 || dy + h > 200) return;
    const bool d_shown = dseg == 0xA000, s_shown = sseg == 0xA000, st = enh_page_tracked(sseg);
    Shadow *dsh = d_shown ? sh_shown : sh_work;
    if (!st) {                                                   /* from an art page: void the destination records */
        for (int j = 0; j < h; j++) memset(&dsh[(dy + j) * 320 + dx], 0, (size_t)w * sizeof(Shadow));
        return;
    }
    if (s_shown == d_shown) return;                              /* the same plane */
    const Shadow *ssh = s_shown ? sh_shown : sh_work;
    const int n = enh_cv.n, lw = enh_cv.lw;
    u16 *dl3 = d_shown ? l3d_shown : l3d_work, *dlv = d_shown ? lvec_shown : lvec_work;
    const u16 *sl3 = s_shown ? l3d_shown : l3d_work, *slv = s_shown ? lvec_shown : lvec_work;
    u8 *dn3 = d_shown ? l3n_shown : l3n_work;                       /* S3.6 */
    const u8 *sn3 = s_shown ? l3n_shown : l3n_work;
    const bool lay = layer_px != 0;
    for (int j = 0; j < h; j++) {
        memcpy(&dsh[(dy + j) * 320 + dx], &ssh[(sy + j) * 320 + sx], (size_t)w * sizeof(Shadow));
        if (!lay) continue;
        for (int k = 0; k < n; k++) {
            const size_t so = (size_t)(enh_cv.fy + (sy + j) * n + k) * lw + (size_t)(enh_cv.fx + sx * n);
            const size_t d = (size_t)(enh_cv.fy + (dy + j) * n + k) * lw + (size_t)(enh_cv.fx + dx * n);
            memcpy(dl3 + d, sl3 + so, (size_t)w * n * sizeof(u16));
            memcpy(dlv + d, slv + so, (size_t)w * n * sizeof(u16));
            memcpy(dn3 + d, sn3 + so, (size_t)w * n);
        }
    }
}

/* ---------------------------------------------------------------- window store (S3.2) */

/* The map MFD keeps its finished window in the backup page 2 (mfd_map_save_bg, 973C) and copies it back every frame to erase the aircraft marker (mfd_map_restore_bg,
 * 9770) instead of drawing the terrain again. A blit between pages is a plain page write, so without help the restored pixels would lose their records and show as 1x
 * pixels. The store keeps the work records and the three layers of the window beside the backup page (one slot, the map window). Not touched in a display pass. */
static struct { bool valid; int x, y, w, h, n; Shadow *rec; u16 *l3, *lv; u8 *n3; } store;   /* n3: S3.6 */

void enh_window_save(int x, int y, int w, int h)
{
    if (enh_sbx || !l3d_work || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 320 || y + h > 200) return;
    const int n = enh_cv.n;
    free(store.rec); free(store.l3); free(store.lv); free(store.n3);
    store.n3 = NULL;
    store.valid = false;
    if (hr_tracing) { s32 t[4] = { enh_cv.fx + n * x, enh_cv.fy + n * y, enh_cv.fx + n * (x + w) - 1, enh_cv.fy + n * (y + h) - 1 }; hr_trace(13, 4, t); }   /* S3.3: 13 = the window's vector layer is saved (the reference keeps its lines) */
    store.rec = malloc((size_t)w * h * sizeof(Shadow));
    store.l3 = malloc((size_t)w * h * n * n * sizeof(u16));
    store.lv = malloc((size_t)w * h * n * n * sizeof(u16));
    store.n3 = malloc((size_t)w * h * n * n);
    if (!store.rec || !store.l3 || !store.lv || !store.n3) { free(store.rec); free(store.l3); free(store.lv); free(store.n3); store.rec = NULL; store.l3 = store.lv = NULL; store.n3 = NULL; return; }
    for (int j = 0; j < h; j++) memcpy(store.rec + (size_t)j * w, sh_work + (y + j) * 320 + x, (size_t)w * sizeof(Shadow));
    for (int j = 0; j < h * n; j++) {
        const size_t at = (size_t)(enh_cv.fy + y * n + j) * enh_cv.lw + (size_t)(enh_cv.fx + x * n);
        memcpy(store.l3 + (size_t)j * w * n, l3d_work + at, (size_t)w * n * sizeof(u16));
        memcpy(store.lv + (size_t)j * w * n, lvec_work + at, (size_t)w * n * sizeof(u16));
        memcpy(store.n3 + (size_t)j * w * n, l3n_work + at, (size_t)w * n);
    }
    store.x = x; store.y = y; store.w = w; store.h = h; store.n = n;
    store.valid = true;
}

void enh_window_restore(int x, int y, int w, int h)
{
    if (!store.valid || !l3d_work || store.x != x || store.y != y || store.w != w || store.h != h || store.n != enh_cv.n) return;
    const int n = enh_cv.n;
    if (hr_tracing) { s32 t[4] = { enh_cv.fx + n * x, enh_cv.fy + n * y, enh_cv.fx + n * (x + w) - 1, enh_cv.fy + n * (y + h) - 1 }; hr_trace(12, 4, t); }   /* S3.3: 12 = the vector layer of this rectangle is replaced by the saved one (the reference clears its lines) */
    for (int j = 0; j < h; j++) memcpy(sh_work + (y + j) * 320 + x, store.rec + (size_t)j * w, (size_t)w * sizeof(Shadow));
    for (int j = 0; j < h * n; j++) {
        const size_t at = (size_t)(enh_cv.fy + y * n + j) * enh_cv.lw + (size_t)(enh_cv.fx + x * n);
        memcpy(l3d_work + at, store.l3 + (size_t)j * w * n, (size_t)w * n * sizeof(u16));
        memcpy(lvec_work + at, store.lv + (size_t)j * w * n, (size_t)w * n * sizeof(u16));
        memcpy(l3n_work + at, store.n3 + (size_t)j * w * n, (size_t)w * n);
    }
}

/* ---------------------------------------------------------------- display-pass snapshots (S2.4, sandbox.c) */

static void snap_get_live(EnhCanvasSnap *s)
{
    s->cv = enh_cv; s->active = active; s->shadow_on = enh_shadow_on; s->view = (int)view; s->route = enh_route;
    s->sh_work = sh_work; s->sh_shown = sh_shown;
    s->l3d_work = l3d_work; s->l3d_shown = l3d_shown; s->lvec_work = lvec_work; s->lvec_shown = lvec_shown;
    s->l3n_work = l3n_work; s->l3n_shown = l3n_shown;
    s->layer_px = layer_px; s->cap = layer_cap;
}

static void snap_set_live(const EnhCanvasSnap *s)
{
    enh_cv = s->cv; active = s->active; enh_shadow_on = s->shadow_on; view = (EnhView)s->view; enh_route = s->route;
    sh_work = s->sh_work; sh_shown = s->sh_shown;
    l3d_work = s->l3d_work; l3d_shown = s->l3d_shown; lvec_work = s->lvec_work; lvec_shown = s->lvec_shown;
    l3n_work = s->l3n_work; l3n_shown = s->l3n_shown;
    layer_px = s->layer_px; layer_cap = s->cap;
}

bool enh_canvas_snap_copy(EnhCanvasSnap *d, const EnhCanvasSnap *s)
{
    if (!d->sh_work) d->sh_work = malloc(PAGE_PIXELS * sizeof(Shadow));
    if (!d->sh_shown) d->sh_shown = malloc(PAGE_PIXELS * sizeof(Shadow));
    if (!d->sh_work || !d->sh_shown) return false;
    if (d->cap < s->layer_px) {
        free(d->l3d_work); free(d->l3d_shown); free(d->lvec_work); free(d->lvec_shown); free(d->l3n_work); free(d->l3n_shown);
        d->l3d_work = malloc(s->layer_px * sizeof(u16)); d->l3d_shown = malloc(s->layer_px * sizeof(u16));
        d->lvec_work = malloc(s->layer_px * sizeof(u16)); d->lvec_shown = malloc(s->layer_px * sizeof(u16));
        d->l3n_work = malloc(s->layer_px); d->l3n_shown = malloc(s->layer_px);                     /* S3.6 */
        d->cap = d->l3d_work && d->l3d_shown && d->lvec_work && d->lvec_shown && d->l3n_work && d->l3n_shown ? s->layer_px : 0;
        if (!d->cap) {
            free(d->l3d_work); free(d->l3d_shown); free(d->lvec_work); free(d->lvec_shown); free(d->l3n_work); free(d->l3n_shown);
            d->l3d_work = d->l3d_shown = d->lvec_work = d->lvec_shown = NULL;
            d->l3n_work = d->l3n_shown = NULL;
            return false;
        }
    }
    memcpy(d->sh_work, s->sh_work, PAGE_PIXELS * sizeof(Shadow));
    memcpy(d->sh_shown, s->sh_shown, PAGE_PIXELS * sizeof(Shadow));
    const size_t nb = s->layer_px * sizeof(u16);
    if (nb) {
        memcpy(d->l3d_work, s->l3d_work, nb); memcpy(d->l3d_shown, s->l3d_shown, nb);
        memcpy(d->lvec_work, s->lvec_work, nb); memcpy(d->lvec_shown, s->lvec_shown, nb);
        memcpy(d->l3n_work, s->l3n_work, s->layer_px); memcpy(d->l3n_shown, s->l3n_shown, s->layer_px);   /* S3.6 */
    }
    d->cv = s->cv; d->active = s->active; d->shadow_on = s->shadow_on; d->view = s->view; d->route = s->route;
    d->layer_px = s->layer_px;
    return true;
}

bool enh_canvas_snap_take(EnhCanvasSnap *d)
{
    EnhCanvasSnap live;
    snap_get_live(&live);
    return enh_canvas_snap_copy(d, &live);
}

void enh_canvas_snap_swap(EnhCanvasSnap *s)
{
    EnhCanvasSnap live;
    snap_get_live(&live);
    snap_set_live(s);
    *s = live;
}

void enh_canvas_snap_peek(EnhCanvasSnap *s) { snap_get_live(s); }

size_t enh_canvas_snap_bytes(const EnhCanvasSnap *s) { return 2 * PAGE_PIXELS * sizeof(Shadow) + 4 * s->layer_px * sizeof(u16) + 2 * s->layer_px; }

/* ---------------------------------------------------------------- mouse */

void enh_canvas_to_frame(int *x, int *y)
{
    int n = enh_cv.n;
    int fxp = (*x - enh_cv.fx) / n, fyp = (*y - enh_cv.fy) / n;
    if (*x < enh_cv.fx) fxp = 0;
    if (*y < enh_cv.fy) fyp = 0;
    *x = fxp > 319 ? 319 : fxp;
    *y = fyp > 199 ? 199 : fyp;
}

void enh_frame_to_canvas(int *x, int *y)
{
    *x = enh_cv.fx + *x * enh_cv.n + enh_cv.n / 2;
    *y = enh_cv.fy + *y * enh_cv.n + enh_cv.n / 2;
}
