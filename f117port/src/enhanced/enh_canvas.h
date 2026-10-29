/* The Enhanced canvas and compositor (DESIGN.md 2.1, 2.5, 3.1, 3.2; S2.1).
 *
 * The canvas is the output picture in "layer pixels", Lw x Lh, of any aspect. The original 320x200 frame is placed in it at N times
 * its size (nearest) at (fx, fy). Three N-times planes sit beside the 8-bit page: the 3D layer, the vector layer and the shadow plane
 * (which records, per page pixel, who wrote it last and with what value). Until S2.2 / S3.1 emit into the layers they stay empty and
 * the composed canvas is exactly the 320x200 frame scaled N x N. All of it is off with --classic. */
#pragma once

#include "types.h"

typedef enum {
    ENH_VIEW_MENU,    /* shell, menus, debrief: pillarboxed, centred */
    ENH_VIEW_FRONT,   /* front cockpit: bottom centre, the world around it (Decisions Q4) */
    ENH_VIEW_FULL,    /* full-screen 3D / outside views: bottom centre, the world around it */
    ENH_VIEW_FIT,     /* left, right, rear: the whole picture filling the window height (Decisions 2026-10-21 b: the canvas is the frame, the host scales it) */
    ENH_VIEW_SIDE     /* the second cockpit view (view 0, [294B] = 1): the frame filling the window height, the world extended left and right only (2026-10-21 b) */
} EnhView;

typedef struct {
    int lw, lh;            /* canvas size in layer pixels */
    int n;                 /* scale of the 320x200 frame */
    int fx, fy;            /* layer position of frame pixel (0,0) */
    int vx, vy, vw, vh;    /* the 3D viewport in layer pixels (the whole canvas in the extended views, else the frame) */
} EnhCanvas;

/* The geometry of DESIGN 3.1 / 3.2. win = output size in pixels; aspect43 = pixel aspect 1.2 (the 4:3 option), else 1;
 * fixed_n = --render-scale (0 = the largest whole multiple that fits). Without wide the canvas is the frame: 320n x 200n at (0,0). */
void enh_canvas_geometry(EnhCanvas *c, EnhView view, int win_w, int win_h, bool aspect43, int fixed_n, bool wide);

extern EnhCanvas enh_cv;        /* the current canvas */
extern bool enh_shadow_on;      /* the writers of video/mgraphic.c record into the shadow plane */

typedef enum { ENH_ROUTE_ART, ENH_ROUTE_3D } EnhRoute;
extern int enh_route;           /* set by the 3D pass (S2.2): primitives written now are 3D-layer writes */
extern int enh_stamp_hold;      /* S3.1: > 0 = 3D-route 1x writes are recorded in the shadow but not stamped into the 3D layer */
extern int enh_vec_hold;        /* S3.1: > 0 = the 1x vector writes now are an hr emitter's footprint (hr_obj.c emits the N x look) */

/* Active = VGA mode 13h is set and the canvas is on. */
bool enh_canvas_active(void);
void enh_canvas_set_active(bool on);
void enh_set_view(int view_mode);                       /* view_frame's mode word (DSW C07A) -> EnhView */
EnhView enh_view_class(int view_mode, int style);       /* S3.4: the placement of view C07A with the HUD style [294B] (view 0 with style 1 = the second cockpit view: FIT) */
bool enh_canvas_extended(void);
void enh_canvas_frame_view(int view_mode, int style);   /* S3.4 fix 4: the placement of this frame's view, applied at the frame start (sandbox.c) */
bool enh_present_hold(void);                            /* S3.4 fix 4: the presenter keeps its last picture (no resize, no compose) until the next flip */                         /* S3.4: the current view is extended and the canvas is larger than the frame */
void enh_menu(bool on);                                 /* S3.4: a VGAME menu screen (quit requester, boss screen) is up: pillarboxed; off = the view before it */
void enh_wide_capture(void);                            /* S3.4, sandbox.c: the wide display pass's 3D and vector layers (after its flip) become the pending wide picture */
void enh_wide_capture_to(int slot);                     /* S4.1: ... into slot 0 (pending) or 2 (the display refresh being built) */
void enh_compose_display(u32 *out);                     /* S4.1, sandbox.c: in a display pass after its flip, the pass's picture with the refresh's wide picture */
bool enh_compose_canonical(u32 *out, unsigned bytes);  /* S4.3, sandbox.c: the canonical render (before its flip of `bytes` bytes) composed as a pass at alpha 1 would (not extended) */
void enh_compose_display_self(u32 *out);                /* S4.3, sandbox.c: in a wide display pass after its flip, the pass's picture alone (its 3D / vector layers are the wide picture) */
bool enh_compose_menu(void);                            /* S4.1: a VGAME menu screen is up (enh_menu) */
void enh_wide_clear_outside(void);                      /* S3.4, hr_pass.c: a wide main pass opens: the layers outside the frame are cleared */
EnhView enh_view_get(void);
/* Recomputes the geometry for the output size; true when the canvas size changed (the host then resizes its frame). */
bool enh_canvas_update(int win_w, int win_h, bool aspect43);
void enh_canvas_test_window(int w, int h);              /* S3.4, the render test: the canvas follows this window (1:1 pixels) instead of the host's; 0 = the host's */
/* Composes the live VGA page (CRTC start / pan, DAC) with the layers into the canvas; false when it did not change. */
bool enh_compose_vga(u32 *xrgb);
void enh_canvas_to_frame(int *x, int *y);               /* canvas pixel -> 320x200 frame pixel (clamped), for the mouse */
void enh_frame_to_canvas(int *x, int *y);

/* Shadow records (mgraphic.c hooks). seg:off is the written byte of page 0 or 1; val the byte written. */
void enh_rec_line(u16 seg, u16 off, u8 val);            /* line / pixel: a vector write (or a 3D line inside a 3D pass); call BEFORE the write */
void enh_rec_fill(u16 seg, u16 off, u8 val);            /* polygon / sprite pixel: 3D inside a 3D pass, else plain art; call AFTER the write */
void enh_rec_vecfill(u16 seg, u16 off, u8 old, u8 val);   /* S3.3: a polygon pixel as an hr vector fill's footprint (after the write; old = the replaced page value) */
bool enh_page_tracked(u16 seg);                         /* page 0 or page 1 (video_page_seg(1)): the pages with shadow records */
void enh_page_cleared(u16 seg);                         /* a whole page was overwritten: its records are void */
/* S2.2: a pixel of the faithful 3D raster (1359 spans / lines) that the hr back end mirrors: shadow only (without an open hr pass it is
 * recorded like enh_rec_fill, its block stamped with the 1x value). Call AFTER the write. */
void enh_rec_3d(u16 seg, u16 off, u8 val);
bool enh_rec_remap(u16 seg, u16 off, u8 old, u8 val, u8 (*f)(u8));   /* S3.2: an in-place recolour (FLIR ramp) of a 3D pixel: its layer block follows; false = record it as art */
void enh_rec_span3d(u16 seg, u16 off, int len);         /* the same for len page bytes from off */
bool enh_shadow_vechr(u16 off, u8 *val);                /* S3.1: the work record of off is an hr vector emitter's footprint (its value in *val) */
bool enh_shadow_under(u16 off, u8 *under, bool *under3d);   /* S3.3: what the vector record of off replaced (the page value, or a 3D pixel) */
bool enh_shadow_3d(u16 off, u8 *val);                   /* the work record of off is a 3D write (its value in *val) */
void enh_pass_clear(int x0, int y0, int x1, int y1);    /* a 3D pass opens: shadow, 3D and vector layer cleared inside its window (frame px) */
void enh_blit(u16 sseg, int sx, int sy, u16 dseg, int dx, int dy, int w, int h);   /* S3.2 fix: blit_rect between pages: records and layers follow (page 1 work <-> page 0 shown); from an art page: records voided */
void enh_window_save(int x, int y, int w, int h);       /* S3.2: the map MFD's backup page 2 gets the window's records and layers (one slot) */
void enh_window_restore(int x, int y, int w, int h);    /* S3.2: ... and gives them back where the window is copied back from the backup */
void enh_layers_reset(void);                            /* every record and layer cleared (the render test, per scenario) */
#define ENH_REC_3D(seg, off, val) do { if (enh_shadow_on) enh_rec_3d((seg), (u16)(off), (u8)(val)); } while (0)
#define ENH_REC_SPAN3D(seg, off, len) do { if (enh_shadow_on) enh_rec_span3d((seg), (u16)(off), (len)); } while (0)
void enh_flip(unsigned bytes);                          /* the page flip copied `bytes` of page 1 to page 0 */

#define ENH_REC_LINE(seg, off, val) do { if (enh_shadow_on) enh_rec_line((seg), (u16)(off), (u8)(val)); } while (0)
#define ENH_REC_FILL(seg, off, val) do { if (enh_shadow_on) enh_rec_fill((seg), (u16)(off), (u8)(val)); } while (0)

/* Layers for the emitters (S2.2, S3.x): 16-bit entries, bit 8 = written, low byte = DAC index (bit 9: a stamped 1x value, see
 * enh_rec_fill). Work = being drawn, shown = flipped. */
void enh_vec_put(int lx, int ly, u8 c);                 /* S3.1: a vector-layer pixel of an hr emitter, layer coordinates; marks its page pixel */
/* S4.6, the map MFD between steps (mapmfd.c):
 * enh_cap_shift: run a 1x drawing op (the aircraft marker, a blip) on the rectangle x, y, w, h (1x, at most 8 x 8) of pages 0 / 1 and show what it drew moved by
 * (ox, oy) layer pixels instead: the rectangle's pages, records and layers are left as they were and every pixel the op wrote (found by running it over two
 * sentinel backgrounds) becomes an N x N vector-layer block at the offset, clipped to the 1x rectangle clip = x0, y0, x1, y1 (exclusive).
 * enh_win_shift: the same for any rectangle and ops that also draw hr lines (route, arc): what changed on `seg`'s plane (1x pixels, vector-layer pixels) moves.
 * enh_rect_snap / unsnap: a rectangle of both pages with its records and layers, put back.
 * The map cache: a rectangle as 3D-layer pixels (grab), shown in a window moved by (sxl, syl) layer pixels (put: the window's layer pixel u is the cache's u + s) */
typedef struct EnhRectSnap EnhRectSnap;
EnhRectSnap *enh_rect_snap(int x, int y, int w, int h);
void enh_rect_unsnap(EnhRectSnap *s);
void enh_rect_drop(EnhRectSnap *s);                     /* forget it (nothing put back) */
void enh_cap_shift(int x, int y, int w, int h, int ox, int oy, const int clip[4], void (*op)(void *), void *arg);
void enh_win_shift(u16 seg, int x, int y, int w, int h, int ox, int oy, const int clip[4], void (*op)(void *), void *arg);
void enh_vec_line_f(u16 seg, double x0, double y0, double x1, double y1, u8 c, const int clip[4]);   /* fix 1: a thin line between sub-pixel points */
void enh_vec_clear_rect(u16 seg, const int r[4]);   /* fix 3: a 1x rectangle (exclusive) as plain art: vector layer cleared, records SRC_NONE */
unsigned long long enh_rect_changed_outside(const EnhRectSnap *s, const int in[4]);   /* fix 2 test aid */
typedef struct { int w, h, n; size_t cap, pcap; u8 *pg; u16 *l3; u8 *l3n; } EnhMapCache;
bool enh_map_cache_grab(EnhMapCache *c, u16 seg, int x, int y, int w, int h);
void enh_map_cache_put(const EnhMapCache *c, u16 seg, int x, int y, int w, int h, int sxl, int syl);
void enh_compose_work(u32 *out, const u8 *page, const u32 *pal, bool blend);   /* S3.1 test aid: the frame being drawn composed from `page` (S3.5: blend = pal is the DAC, mix the blend codes) */
/* S3.3: the HUD / MFD vector lines at N x (hr_obj.c). A hook pair around the 1x drawing of line() / hud_tick_strip(): begin decides (true = the 1x footprint is held, SRC_VECHR) and end
 * draws the same line / ticks thin (max(1, N/2) layer pixels, Decisions Q3) in the vector layer, from the 1x endpoints (frame pixels incl. draw_off). */
extern bool enh_hud_hr_off;                             /* test aid (--compose-test): no hr look for the 1x lines */
bool enh_hud_begin(u16 seg, int colour, int x1, int y1, int x2, int y2, u16 draw_off);
void enh_hud_end_line(void);
bool enh_hud_begin_tick(u16 seg, int colour);
void enh_hud_tick(int x, int y, int dir, int npx);      /* npx = 1: one dash of the scale line; 2 / 3: a horizontal tick of that many pixels from x towards dir */
void enh_hud_end_tick(void);
bool enh_adi_begin(u16 seg, int colour);               /* S3.3: the ADI's horizon polygon (cockpit_draw_adi): begin holds its 1x footprint ... */
void enh_adi_end(const s16 *pts, int n);                /* ... end fills the same polygon at N x (window = DS:2293 x 2295 at the draw offset, vertices in window pixels) */
u16 *enh_layer3d(void);
u16 *enh_layer_vec(void);
u8 *enh_layer_night(void);                              /* S3.6: the night plane of the 3D layer (work) */

/* S2.4, the display pass (sandbox.c): the canvas state (geometry, view, route, shadow plane, the four layers) as a value. take = a deep
 * copy of the live state; copy = a deep copy; swap = the live state and *s change places (pointers only, no copy). The buffers of a
 * snapshot are its own and reused; a swap hands them to the live canvas and back. */
typedef struct {
    EnhCanvas cv;
    bool active, shadow_on;
    int view, route;
    void *sh_work, *sh_shown;                 /* PAGE_PIXELS shadow records each */
    u16 *l3d_work, *l3d_shown, *lvec_work, *lvec_shown;
    u8 *l3n_work, *l3n_shown;                 /* S3.6: the night planes */
    size_t layer_px, cap;                     /* pixels in use, pixels the layer buffers hold */
} EnhCanvasSnap;
bool enh_canvas_snap_take(EnhCanvasSnap *d);
bool enh_canvas_snap_copy(EnhCanvasSnap *d, const EnhCanvasSnap *s);
void enh_canvas_snap_swap(EnhCanvasSnap *s);
size_t enh_canvas_snap_bytes(const EnhCanvasSnap *s);
void enh_canvas_snap_peek(EnhCanvasSnap *s);              /* the live state as a snapshot value (its buffers stay the live ones: read only) */

int enh_compose_selftest(void);                         /* --compose-test */
int enh_wide_geometry_test(void);                       /* S3.4: --wide-geometry-test */
