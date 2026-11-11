/* The hr pass context (DESIGN.md 2.2, 2.5; S2.2): r3d_set_viewport (DB71) opens a 3D pass (route 3D, the window's shadow, 3D and
 * vector layer cleared, the hr window and centre at N x); r3d_end_scene (DB3E) closes it. A pass draws into the layer only when its
 * page is page 0 or 1 (the pages the shadow plane follows). Also the hr point of B85D and the --hr-check statistics of the render test. */
#include "enhanced/hr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhanced/enh_canvas.h"
#include "enhanced/enhanced.h"
#include "enhanced/sandbox.h"
#include "enhanced/wide.h"
#include "host.h"
#include "render3d/r3d.h"
#include "video/video.h"

HrCtx enh_hr;
HrPoint enh_hr_pt;

/* --hr-dump trace (hr.h) and the scenario's pass windows */
bool hr_tracing;
static s32 *trace;
static size_t trace_n, trace_cap;
static int win_x0, win_y0, win_x1, win_y1;     /* union of the pass windows, layer px (empty when win_x1 < win_x0) */
static u16 last_seg = 0xA000;

/* S2.4: the hr pass context in the display-pass snapshot (sandbox.h) */
void hr_sandbox_regions(EnhRegionAdd add)
{
    add(&enh_hr, sizeof enh_hr); add(&enh_hr_pt, sizeof enh_hr_pt); add(&last_seg, sizeof last_seg);
    hr_clip_regions(add); hr_edge2d_regions(add); hr_raster_regions(add); hr_shade_regions(add);   /* S3.5 */
    hr_night_regions(add);                                                                                             /* S3.6 */
}

static void trace_put(s32 v)
{
    if (trace_n == trace_cap) {
        size_t c = trace_cap ? trace_cap * 2 : 1 << 16;
        s32 *t = realloc(trace, c * sizeof *t);
        if (!t) { hr_tracing = false; return; }
        trace = t; trace_cap = c;
    }
    trace[trace_n++] = v;
}

static bool trace_has_vec;

void hr_trace(int type, int nargs, const s32 *a)
{
    if (type == 9 || type == 11) trace_has_vec = true;
    trace_put(type);
    for (int i = 0; i < nargs; i++) trace_put(a[i]);
}

void hr_trace_poly(s32 ox, s32 oy, u8 colour, int n, const s64 *pts)
{
    trace_put(5); trace_put(ox); trace_put(oy); trace_put(colour); trace_put(n);
    for (int i = 0; i < 2 * n; i++) {
        s64 v = pts[i];
        trace_put((s32)(v < -(1 << 30) ? -(1 << 30) : v > (1 << 30) ? (1 << 30) : v));
    }
}

/* --hr-check statistics of the current scenario */
static struct {
    long checked;     /* 1x pixels of a pass window whose last write was 3D and is still on the page */
    long hr, stamped; /* layer pixels over them written by the hr back end / stamped with the 1x value */
    long agree, differ, empty;   /* layer pixels shown (written, else the 1x value: the compositor) equal / not equal to the 1x pixel; not written */
    long inner, inner_differ;    /* the same in "interior" blocks (the 3x3 1x pixels around are all the same value): away from every edge */
    int passes;
} hs;

static void pass_stats(void)
{
    const int n = enh_hr.n;
    const u8 *pg = seg_ptr(enh_hr.seg);
    for (int y = enh_hr.y0; y <= enh_hr.y1 && y < 200; y++)
        for (int x = enh_hr.x0; x <= enh_hr.x1 && x < 320; x++) {
            u8 val;
            u16 off = (u16)(y * 320 + x);
            if (!enh_shadow_3d(off, &val) || pg[off] != val) continue;
            hs.checked++;
            bool inner = x > enh_hr.x0 && x < enh_hr.x1 && y > enh_hr.y0 && y < enh_hr.y1;
            for (int dy = -1; dy <= 1 && inner; dy++)
                for (int dx = -1; dx <= 1; dx++) if (pg[off + dy * 320 + dx] != val) { inner = false; break; }
            for (int ky = 0; ky < n; ky++)
                for (int kx = 0; kx < n; kx++) {
                    u16 l = enh_hr.layer[(size_t)(enh_hr.fy + y * n + ky) * enh_hr.lw + (size_t)(enh_hr.fx + x * n + kx)];
                    if (l & 0x200) hs.stamped++; else if (l & 0x100) hs.hr++;
                    if (!(l & 0x100)) hs.empty++;
                    bool same = !(l & 0x100) || (u8)l == val;
                    if (same) hs.agree++; else hs.differ++;
                    if (inner) { hs.inner++; if (!same) hs.inner_differ++; }
                }
        }
    hs.passes++;
}

/* S3.2: the next pass continues the previous one in the same window (the TV camera's close view draws the terrain, then the object, as two passes on one page:
 * the second must not clear what the first drew). Set by enh_pass_continue, consumed by the next viewport. */
static bool continue_next;
void enh_pass_continue(void) { continue_next = true; }

/* S3.2: the TV camera draws its 1x backdrop (the sensor window fill, the sky / ground bands of mfd_view_sky_bands) BEFORE the first pass opens. Opening the window early
 * makes those writes scene writes (route 3D, stamped as original pixels into the clear layer) that the pass then keeps, so the hr polygons of the object are not cut
 * to the 1x footprint. ctx = the TV context (DS offset): page word at +0, window rows at +0Eh / +10h, columns at +12h / +14h. */
void enh_tv_backdrop(u16 ctx)
{
    const u16 seg = DSW((u16)(0x85E2 + 2 * DSW(ctx)));
    const int x0 = DSS((u16)(ctx + 0x12)), y0 = DSS((u16)(ctx + 0x0E)), x1 = DSS((u16)(ctx + 0x14)), y1 = DSS((u16)(ctx + 0x10));
    if (!enh_page_tracked(seg) || !enh_layer3d() || x0 < 0 || y0 < 0 || x1 > 319 || y1 > 199 || x1 < x0 || y1 < y0) return;
    enh_pass_clear(x0, y0, x1, y1);
    enh_route = ENH_ROUTE_3D;
    enh_hr.have = true;                                       /* the window of the pass that follows (checked in enh_r3d_viewport) */
    enh_hr.seg = seg; enh_hr.x0 = x0; enh_hr.y0 = y0; enh_hr.x1 = x1; enh_hr.y1 = y1;
    continue_next = true;
}

static bool test_pump(void) { static int v = -1; if (v < 0) { const char *e = getenv("F117_TEST_PUMP"); v = e && *e == '1'; } return v == 1; }

void enh_r3d_viewport(u16 vp)
{
    (void)vp;
    if (enh_hr.on && enh.hr_check) pass_stats();             /* a pass left open (a nested begin without its end) */
    if (enh_wide_on) enh_wide_end();                          /* S3.4: only the main pass is widened */
    enh_route = ENH_ROUTE_3D;
    enh_hr.on = false;
    const u16 seg = DSW(0x85EA);
    const int x0 = DSS(0x85CA), y0 = DSS(0x85CC), x1 = DSS(0x85CE), y1 = DSS(0x85D0);
    const bool main_win = x0 == 0 && y0 == 0 && x1 == 319;               /* S3.4: the main 3D window of view_frame (the TV and map windows are smaller) */
    if (enh.wide_test_w > 0 && main_win && enh_page_tracked(seg)) {        /* S3.4 test aid: the render test's scene on a wide canvas, placed for its view */
        enh_set_view(DSW(0xC07A));
        enh_canvas_test_window(enh.wide_test_w, enh.wide_test_h);
        enh_canvas_update(enh.wide_test_w, enh.wide_test_h, false);
    }
    const bool wide = (enh_wide_pass || enh.wide_test_w > 0) && main_win && y1 <= 199 && enh_canvas_extended();
    const bool keep = continue_next && enh_hr.have && enh_hr.seg == seg && enh_hr.x0 == x0 && enh_hr.y0 == y0 && enh_hr.x1 == x1 && enh_hr.y1 == y1;   /* S3.2 */
    continue_next = false;
    enh_hr.have = false;
    hr_obj_pass_begin();
    hr_night_pass_begin(main_win);                                        /* S3.6 (before S3.5: a night pass turns the blend codes on) */
    hr_shade_pass_begin();                                                /* S3.5 */
    if (!enh_page_tracked(seg)) return;
    if (!keep) enh_pass_clear(x0, y0, x1, y1);
    u16 *layer = enh_layer3d();
    if (!layer || x0 < 0 || y0 < 0 || x1 > 319 || y1 > 199 || x1 < x0 || y1 < y0) return;
    HrCtx *h = &enh_hr;
    const int n = enh_cv.n;
    h->n = n; h->fx = enh_cv.fx; h->fy = enh_cv.fy; h->lw = enh_cv.lw; h->lh = enh_cv.lh;
    h->layer = layer; h->seg = seg;
    h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
    h->xmin = h->fx + n * x0; h->xmax = h->fx + n * (x1 + 1) - 1;
    h->ymin = h->fy + n * y0; h->ymax = h->fy + n * (y1 + 1) - 1;
    h->cx = h->fx + n * DSS(0x85D2); h->cy = h->fy + n * DSS(0x85D4);
    h->ox = h->xmin; h->oy = h->ymin;
    h->exmax = n * (DSS(0x2293) + 1) - 1; h->eymax = n * (DSS(0x2295) + 1) - 1;
    h->wide = h->hole = false;
    h->wx = h->xmin; h->wy = h->ymin; h->e2dx = h->e2dy = 0;
    if (wide) {                                                           /* S3.4: the canvas is the window; the frame below the 3D window (the panel) is a hole */
        h->wide = true;
        h->xmin = 0; h->ymin = 0; h->xmax = h->lw - 1; h->ymax = h->lh - 1;
        h->ox = 0; h->oy = 0; h->exmax = h->lw - 1; h->eymax = h->lh - 1;
        h->e2dx = h->wx; h->e2dy = h->wy;
        if (y1 < 199) { h->hole = true; h->hole_x0 = h->fx; h->hole_x1 = h->fx + 320 * n - 1; h->hole_y = h->fy + n * (y1 + 1); h->hole_y1 = h->lh; }
        if (h->hole && enh_cv.mac) { h->hole_x0 = h->fx + 74 * n; h->hole_x1 = h->fx + 246 * n - 1; h->hole_y = h->fy + 110 * n; h->hole_y1 = h->fy + 200 * n; }   /* S7.4 (DESIGN 7.2): only the MFD
                                                                          * windows' columns from their top row: the rest of the panel lies under the Mac canopy windows */
        enh_wide_clear_outside();
    }
    h->line_w = n / 2 > 1 ? n / 2 : 1;
    h->line_o = (n - h->line_w) / 2;
    h->fine = enh.dither_fine;
    h->on = true;
    h->have = true;
    last_seg = seg;
    if (hr_tracing) {
        s32 w[5] = { h->xmin, h->ymin, h->xmax, h->ymax, keep };   /* the fifth word: the pass keeps the layer of the previous one (S3.2) */
        hr_trace(0, 5, w);
        if (h->hole) { s32 p[3] = { h->hole_x0, h->hole_x1, h->hole_y }; hr_trace(14, 3, p); }   /* S3.4 */
        if (win_x1 < win_x0) { win_x0 = w[0]; win_y0 = w[1]; win_x1 = w[2]; win_y1 = w[3]; }
        else {
            if (w[0] < win_x0) win_x0 = w[0];
            if (w[1] < win_y0) win_y0 = w[1];
            if (w[2] > win_x1) win_x1 = w[2];
            if (w[3] > win_y1) win_y1 = w[3];
        }
    }
    hr_raster_begin();
    hr_clip_begin();
    if (wide) enh_wide_begin();                                           /* S3.4: the widening factors of this canvas */
    if (test_pump() && !enh_sbx && main_win) host_pump();                 /* S3.4 fix, test aid (F117_TEST_PUMP=1): a present inside the open main pass, as the
                                                                           * real-time [262A] wait does (the canvas must not be resized under it) */
}

/* S3.2: the FLIR tint (C6DE) recolours the finished window x..x+w-1, y..y+h-1 (frame px) through the green ramp; the reference does the same to its expected colours */
static int tint_r[4] = { 0, 0, -1, -1 };      /* the last tint, frame px x0 y0 x1 y1 (--hr-check) */
void enh_tint_note(int x, int y, int w, int h)
{
    if (!enh_hr.have) return;
    const int n = enh_cv.n;
    tint_r[0] = x; tint_r[1] = y; tint_r[2] = x + w - 1; tint_r[3] = y + h - 1;
    if (!hr_tracing) return;
    s32 r[4] = { enh_cv.fx + n * x, enh_cv.fy + n * y, enh_cv.fx + n * (x + w) - 1, enh_cv.fy + n * (y + h) - 1 };
    hr_trace(10, 4, r);
}

void enh_r3d_end(void)
{
    if (enh_hr.on) hr_night_pass_end();                                   /* S3.6 */
    if (enh_hr.on && enh.hr_check) pass_stats();
    if (enh_wide_on) enh_wide_end();                                      /* S3.4 */
    enh_hr.on = false;
    enh_route = ENH_ROUTE_ART;
}

/* B85D at N x: lsl32(X, 8) wraps at 1x first (quirk), then N; sy - (sy >> 2) on the N x value; offsets N x. The point is meaningful
 * only when the call left [49E0] != FFFFh (an early return of B85D does not reach this hook). */
void hr_project_point(s32 X, s32 Y, s32 Z, bool visible)
{
    enh_hr_pt.size = 0;
    if (!enh_layer3d() || Z == 0) { enh_hr_pt.valid = false; return; }
    const int n = enh_cv.n;
    s64 xw = (s32)((u32)X << 8), yw = (s32)((u32)Y << 8);
    s64 qx = xw * n / Z, qy = yw * n / Z;
    qy -= qy >> 2;
    s64 oy = DSW(0x365E) != 0 ? (DSB(0x294B) == 0 ? 0x34 : 0x3C) : 0x64;
    enh_hr_pt.x = (s32)(enh_cv.fx + (s64)n * 0xA0 + qx);
    enh_hr_pt.y = (s32)(enh_cv.fy + (s64)n * oy + qy);
    enh_hr_pt.valid = visible;
}

/* ---------------------------------------------------------------- --hr-check (sim/simtest.c, --render-test) */

static bool hr_check_on(void)
{
    enh_env_args();
    return enh.hr_check && enh_canvas_on();
}

void enh_hr_test_begin(void)
{
    if (!hr_check_on()) return;
    int n = enh.render_scale > 0 ? enh.render_scale : 1;
    enh_canvas_set_active(true);
    enh_canvas_test_window(320 * n, 200 * n);                 /* S3.4: not the host's (headless) window */
    enh_canvas_update(320 * n, 200 * n, false);
    enh_layers_reset();
    enh_hr.on = false;
    memset(&hs, 0, sizeof hs);
    { long a, b, c, d, e; hr_obj_stats(&a, &b, &c, &d, &e, true); }
    trace_n = 0;
    trace_has_vec = false;
    hr_tracing = enh.hr_dump[0] != 0;
    win_x0 = win_y0 = 0; win_x1 = win_y1 = -1;
    last_seg = 0xA000;
    tint_r[0] = 0; tint_r[1] = 0; tint_r[2] = -1; tint_r[3] = -1;
}

/* DIR/<id>.hrd (int32 words unless noted): magic 'HRD1', n, fx, fy, lw, lh, fine, win x0 y0 x1 y1 (layer px, the union of the pass windows;
 * x1 < x0 = no pass), trace length, the trace, then u16 layer rows of the window, u8 page[64000] (the last pass's page), u8 dac[768],
 * u8 shadow3d[64000] (1 = the page pixel is a 3D write still on the page). Read by port/notes/e23/r3d_ref.py and dump_scenes.py. */
static void write_dump(const char *id)
{
    if (!hr_tracing) return;
    hr_tracing = false;
    char path[2400];
    snprintf(path, sizeof path, "%s/%s.hrd", enh.hr_dump, id);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const u16 *layer = enh_layer3d();
    bool have = layer && win_x1 >= win_x0 && win_x0 >= 0 && win_y0 >= 0 && win_x1 < enh_cv.lw && win_y1 < enh_cv.lh;
    s32 hd[16] = { 0x31445248, enh_cv.n, enh_cv.fx, enh_cv.fy, enh_cv.lw, enh_cv.lh, enh.dither_fine, have ? win_x0 : 0, have ? win_y0 : 0,
                   have ? win_x1 : -1, have ? win_y1 : -1, (s32)trace_n };
    fwrite(hd, 4, 12, f);
    fwrite(trace, 4, trace_n, f);
    if (have)
        for (int y = win_y0; y <= win_y1; y++) fwrite(layer + (size_t)y * enh_cv.lw + win_x0, 2, (size_t)(win_x1 - win_x0 + 1), f);
    fwrite(seg_ptr(last_seg), 1, 64000, f);
    u8 dac[768];
    dac_get(0, 256, dac);
    fwrite(dac, 1, 768, f);
    static u8 sh[64000];
    const u8 *pg = seg_ptr(last_seg);
    for (int i = 0; i < 64000; i++) { u8 v; sh[i] = enh_shadow_3d((u16)i, &v) && pg[i] == v; }
    fwrite(sh, 1, 64000, f);
    const u16 *vl = enh_layer_vec();                     /* S3.1: the vector layer when something was emitted into it (a flag word, then lh rows of lw u16) */
    s32 hv = vl && win_x1 >= win_x0 && trace_has_vec ? 1 : 0;
    fwrite(&hv, 4, 1, f);
    if (hv) fwrite(vl, 2, (size_t)enh_cv.lw * (size_t)enh_cv.lh, f);
    fclose(f);
}

/* S3.1: the whole frame as the compositor makes it (the work shadow and layers over the page of the last pass) against the 1x page: at N = 1 every
 * pixel must be equal (sprites, dots, tracers, boxes, the keyed art of every view included); at N > 1 the layer pixels of a block equal its 1x pixel except
 * at edges ("interior" blocks: the 3x3 1x pixels around are one value). Also the canvas as RGB for the dumps (DIR/<id>.cmp). */
static struct { long px, differ, inner, inner_differ, vtot, vhit, tpx, tramp, vthin, vstray; } fs;

static void frame_check(const char *id)
{
    const int n = enh_cv.n, lw = enh_cv.lw, lh = enh_cv.lh;
    u32 *img = malloc((size_t)lw * (size_t)lh * sizeof(u32));
    if (!img) return;
    u32 pal[256];
    for (int i = 0; i < 256; i++) pal[i] = (u32)i;
    const u8 *pg = seg_ptr(last_seg);
    enh_compose_work(img, pg, pal, false);
    memset(&fs, 0, sizeof fs);
    const u16 *vl = enh_layer_vec();
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            const u8 v = pg[y * 320 + x];
            u8 sv;
            if (vl && enh_shadow_vechr((u16)(y * 320 + x), &sv) && sv == v) {   /* a 1x vector footprint: the hr line must pass within one original pixel of it */
                bool hit = false;
                for (int by = (y - 1) * n; by < (y + 2) * n && !hit; by++)
                    for (int bx = (x - 1) * n; bx < (x + 2) * n; bx++) {
                        const int lx = enh_cv.fx + bx, ly = enh_cv.fy + by;
                        if (lx < 0 || ly < 0 || lx >= lw || ly >= lh) continue;
                        const u16 l = vl[(size_t)ly * lw + lx];
                        if ((l & 0x100) && (u8)l == v) { hit = true; break; }
                    }
                fs.vtot++;
                if (hit) fs.vhit++;
                u8 under = 0;
                bool under3d = false;
                enh_shadow_under((u16)(y * 320 + x), &under, &under3d);
                bool thin = false, stray = false;           /* S3.3: the block shows less than the full line colour (the line is thin), and only colours of the 5x5 original pixels around it (the footprint itself covered the 3D pixels it replaced) */
                for (int ky = 0; ky < n; ky++)
                    for (int kx = 0; kx < n; kx++) {
                        const u32 c = img[(size_t)(enh_cv.fy + y * n + ky) * lw + (size_t)(enh_cv.fx + x * n + kx)];
                        if (c != v) thin = true;
                        bool near = under3d || c == under;
                        for (int dy = -2; dy <= 2 && !near; dy++)
                            for (int dx = -2; dx <= 2; dx++) {
                                const int qx = x + dx, qy = y + dy;
                                if (qx >= 0 && qx < 320 && qy >= 0 && qy < 200 && pg[qy * 320 + qx] == c) { near = true; break; }
                            }
                        if (!near) stray = true;
                    }
                if (thin) fs.vthin++;
                if (stray) fs.vstray++;
            }
            bool inner = x > 0 && x < 319 && y > 0 && y < 199;
            for (int dy = -1; dy <= 1 && inner; dy++)
                for (int dx = -1; dx <= 1; dx++) if (pg[(y + dy) * 320 + x + dx] != v) { inner = false; break; }
            for (int ky = 0; ky < n; ky++)
                for (int kx = 0; kx < n; kx++) {
                    const bool same = img[(size_t)(enh_cv.fy + y * n + ky) * lw + (size_t)(enh_cv.fx + x * n + kx)] == v;
                    fs.px++;
                    if (!same) fs.differ++;
                    if (inner) { fs.inner++; if (!same) fs.inner_differ++; }
                }
        }
    for (int y = tint_r[1]; y <= tint_r[3]; y++)      /* S3.2: after a FLIR tint every shown pixel of its window is a colour of the green ramp 98h-9Fh, except the labels (original pixels) */
        for (int x = tint_r[0]; x <= tint_r[2] && x >= 0 && y >= 0 && y < 200 && x < 320; x++)
            for (int ky = 0; ky < n; ky++)
                for (int kx = 0; kx < n; kx++) {
                    const u32 c = img[(size_t)(enh_cv.fy + y * n + ky) * lw + (size_t)(enh_cv.fx + x * n + kx)];
                    fs.tpx++;
                    if (c >= 0x98 && c <= 0x9F) fs.tramp++;
                }
    if (enh.hr_dump[0] && id) {                       /* the composed canvas with the DAC, for the dump tools */
        u8 dac[768];
        dac_get(0, 256, dac);
        char path[2400];
        snprintf(path, sizeof path, "%s/%s.cmp", enh.hr_dump, id);
        FILE *f = fopen(path, "wb");
        if (f) {
            s32 hd[2] = { lw, lh };
            fwrite(hd, 4, 2, f);
            fwrite(dac, 1, 768, f);
            u8 *row = malloc((size_t)lw);
            if (row) {
                for (int y = 0; y < lh; y++) {
                    for (int x = 0; x < lw; x++) row[x] = (u8)img[(size_t)y * lw + x];
                    fwrite(row, 1, (size_t)lw, f);
                }
                free(row);
            }
            fclose(f);
        }
        const char *e = getenv("F117_HRD_RGB");                     /* S3.5 test aid: the canvas as presented (the live DAC, blend codes mixed) as DIR/<id>.rgb (s32 lw, lh, lw * lh XRGB) */
        if (e && *e == '1') {
            for (int i = 0; i < 256; i++) {
                u32 r = (u32)dac[3 * i] << 2, g = (u32)dac[3 * i + 1] << 2, b = (u32)dac[3 * i + 2] << 2;
                pal[i] = ((r | r >> 6) << 16) | ((g | g >> 6) << 8) | (b | b >> 6);
            }
            enh_compose_work(img, pg, pal, true);
            snprintf(path, sizeof path, "%s/%s.rgb", enh.hr_dump, id);
            f = fopen(path, "wb");
            if (f) {
                s32 hd[2] = { lw, lh };
                fwrite(hd, 4, 2, f);
                fwrite(img, 4, (size_t)lw * (size_t)lh, f);
                fclose(f);
            }
            const u8 *nd = enh_layer_night();                       /* S3.6 test aid: the night plane as DIR/<id>.nd (s32 lw, lh, lw * lh u8) */
            if (nd && enh_true_night_on()) {
                snprintf(path, sizeof path, "%s/%s.nd", enh.hr_dump, id);
                f = fopen(path, "wb");
                if (f) {
                    s32 hd[2] = { lw, lh };
                    fwrite(hd, 4, 2, f);
                    fwrite(nd, 1, (size_t)lw * (size_t)lh, f);
                    fclose(f);
                }
            }
        }
    }
    free(img);
}

void enh_hr_test_end(const char *dir, const char *id)
{
    static bool first = true;
    if (!hr_check_on()) return;
    if (enh_hr.on) pass_stats();
    enh_hr.on = false;
    enh_route = ENH_ROUTE_ART;
    write_dump(id);
    frame_check(id);
    char path[2300];
    snprintf(path, sizeof path, "%s/hr_check.txt", dir);
    FILE *f = fopen(path, first ? "w" : "a");
    first = false;
    if (!f) return;
    long sp, spx, dots, lines, lpx;
    hr_obj_stats(&sp, &spx, &dots, &lines, &lpx, true);
    long wpx = 0, w3d = 0;                           /* S3.2: the last pass's window at the end of the scenario: pixels, and those still shown from the 3D layer (a 3D record equal to the page) */
    if (enh_hr.have) {
        const u8 *pg = seg_ptr(enh_hr.seg);
        for (int y = enh_hr.y0; y <= enh_hr.y1; y++)
            for (int x = enh_hr.x0; x <= enh_hr.x1; x++) {
                u8 v;
                wpx++;
                if (enh_shadow_3d((u16)(y * 320 + x), &v) && v == pg[y * 320 + x]) w3d++;
            }
    }
    fprintf(f, "%s n=%d passes=%d checked=%ld hr=%ld stamped=%ld agree=%ld differ=%ld empty=%ld inner=%ld inner_differ=%ld "
               "fpx=%ld fdiffer=%ld finner=%ld finner_differ=%ld vtot=%ld vhit=%ld vthin=%ld vstray=%ld sprites=%ld sprite_px=%ld dots=%ld lines=%ld line_px=%ld wpx=%ld w3d=%ld tpx=%ld tramp=%ld\n", id, enh_cv.n, hs.passes, hs.checked, hs.hr, hs.stamped,
            hs.agree, hs.differ, hs.empty, hs.inner, hs.inner_differ, fs.px, fs.differ, fs.inner, fs.inner_differ, fs.vtot, fs.vhit, fs.vthin, fs.vstray, sp, spx, dots, lines, lpx, wpx, w3d, fs.tpx, fs.tramp);
    fclose(f);
}
