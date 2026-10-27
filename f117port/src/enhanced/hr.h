/* The high-resolution 3D back end (DESIGN.md 2.2-2.5; S2.2).
 *
 * The faithful render3d runs unchanged and writes the 320x200 page. At each geometry point an ENH: hook hands the same primitive to
 * this back end, which recomputes it at N x (rule R1: N enters inside the innermost rounding of each formula, so at N = 1 every formula
 * is the original one) and rasterises it into the 3D layer of the canvas (enh_canvas.h) as 8-bit DAC indices.
 *
 *   hr_pass.c    the pass context: viewport hook, route, layer / shadow clear, the B85D point, the --hr-check statistics
 *   hr_clip.c    copy of render3d/r3d_clip.c + the 0906 projection and 0B3F edges of r3d_object.c (int32 / int64)
 *   hr_raster.c  copy of render3d/r3d_raster.c (row tables of Lh rows, dither modes, wide lines)
 *   hr_edge2d.c  copy of render3d/edge2d.c (1001) + the sky / ground bands of r3d_sky.c (1442)
 *
 * The back end never writes mem[], the VGA model or the driver statics: the 1x page and the sim cannot see it. */
#pragma once

#include "types.h"

typedef struct HrCtx {
    bool on;                      /* a 3D pass that draws into the layer is open */
    bool have;                    /* S3.1: the window / layer fields below describe the last pass on a tracked page (stays true after r3d_end_scene, for the overlay pass A17E) */
    int n, fx, fy;                /* scale and frame origin in layer pixels */
    int lw, lh;                   /* layer size */
    u16 *layer;                   /* the 3D layer being drawn (enh_layer3d) */
    u16 seg;                      /* the page segment of the pass (DS:85EA) */
    int x0, y0, x1, y1;           /* the pass window in frame pixels (DB71) */
    s32 xmin, ymin, xmax, ymax;   /* the 12EF / 1359 window in layer pixels: [fx + N x0, fx + N (x1 + 1) - 1] */
    s32 cx, cy;                   /* the projection centre in layer pixels: fx + N [85D2], fy + N [85D4] */
    s32 ox, oy;                   /* the 1001 window origin (layer pixel of window pixel 0,0) */
    s32 exmax, eymax;             /* the 1001 XMAX / YMAX: N W - 1, N H - 1 */
    int line_w, line_o;           /* line brush: max(1, N/2) pixels, offset (N - w) / 2 (DESIGN 2.4, Q3) */
    bool fine;                    /* --dither fine */
    /* S3.4, a wide main pass (wide.h): the clip window xmin..ymax and the 1001 window (ox, oy, exmax, eymax) are the canvas; wx, wy = the layer pixel of the
     * original window's frame pixel (x0, y0) (the origin the R1 "fits s16" tests and the sprite placement use; = xmin, ymin otherwise); e2dx, e2dy = what moves the
     * 1001 window-relative points of the original window into the canvas window; the hole (the frame below the 3D window, where the panel is) is never written */
    bool wide, hole;
    s32 wx, wy, e2dx, e2dy;
    s32 hole_x0, hole_x1, hole_y;
    u8 *night;                    /* S3.6: the night plane of a true-night pass (hr_night.c), NULL otherwise */
} HrCtx;

/* S3.4: the panel hole of a wide main pass */
#define HR_IN_HOLE(x, y) (enh_hr.hole && (y) >= enh_hr.hole_y && (x) >= enh_hr.hole_x0 && (x) <= enh_hr.hole_x1)

extern HrCtx enh_hr;

/* hr position of the last r3d_project_point (B85D) call, for the emitters of the same object (S3.1) */
typedef struct { bool valid; s32 x, y; s32 size; } HrPoint;   /* size: the hr size (layer px) of the sprite of this point when a caller computed it, else 0 */
extern HrPoint enh_hr_pt;

#define ENH_HR(call) do { if (enh_hr.on) { call; } } while (0)

/* ---- hooks in the faithful render3d (each behind enh_shadow_on / enh_hr.on) */
void enh_r3d_viewport(u16 vp);                     /* end of r3d_set_viewport (DB71): opens a pass, route 3D */
void enh_r3d_end(void);                            /* end of r3d_end_scene (DB3E): closes it, route art */
void enh_pass_continue(void);                      /* S3.2: the next pass continues the previous one (same window and page): its layers are kept */
void enh_tv_backdrop(u16 ctx);                    /* S3.2: the TV window's 1x backdrop follows: route 3D, the pass that comes keeps it */
void enh_tint_note(int x, int y, int w, int h);    /* S3.2: the FLIR tint recoloured this frame rectangle (--hr-dump trace type 10) */
void hr_edge_point(u16 rec, s32 *x, s32 *y);       /* S3.2: the clipped first end of the hr edge record `rec` (layer px) */
void enh_tv_sprite_pending(u16 rec, int size);     /* S3.2: r3d_draw_queued, before the TV sprite of the object whose last edge is `rec` */
void hr_project(u16 di, u16 bx);                   /* r3d_project (11EB:0906): the hr vertex of the same slot */
void hr_make_edge(u16 rec, u16 v0x8, u16 v1x8);    /* r3d_make_edge (11EB:0B3F): the hr edge record, clipped */
void hr_poly_add_edge(u16 rec);                    /* r3d_poly_add_edge (12EF:003E) */
void hr_poly_finish(void);                         /* end of r3d_poly_finish (12EF:010A): closing edge, walls, fill */
void hr_line(u16 rec);                             /* r3d_line_M (1359:045D) */
void hr_sky_ground(void *ctx);                     /* end of r3d_draw_sky_ground (1442) */
void hr_project_point(s32 X, s32 Y, s32 Z, bool visible);   /* end of r3d_project_point (B85D) */

/* ---- S3.1 (hr_obj.c): the objects the world pass 9ACA and the overlay pass A17E draw at 1x on top of the scene: sprites (smoke billboards 1AD6, the
 * explosion 1A85), the far dots, the tracers and the target boxes. Each is emitted at N x from the hr point of B85D; the 1x footprint stays on the page
 * (and in the shadow) but is not stamped / shown. All hooks are behind enh_shadow_on. */
void hr_obj_pass_begin(void);                      /* a pass opens: the billboard table is cleared */
void enh_bill_size(int mode, int d);               /* 9ACA (B): the hr size of the puff just projected (lib_scale_eighths(mode, |0x100 N / d|)) */
void enh_bill_note(void);                          /* r3d_queue_billboard, before the insert: the hr point and size of the record the insert will fill */
void enh_bill_pending(u16 rec);                    /* r3d_draw_queued, before the billboard sprite of `rec` */
void enh_expl_pending(int dep);                    /* A17E S3, before the explosion sprite: B85D's point, size |0x100 N / dep| */
bool enh_sprite_begin(u16 ctx);                    /* sprite(), before sprite_scaled: true = an hr sprite follows (1x footprint recorded, not stamped) */
void enh_sprite_end(u16 off, u16 seg, int size);   /* sprite(), after: draws the hr sprite (the source bitmap at off in seg, 1x size `size`) */
void enh_dot_begin(void);                          /* 9ACA (D)/(E): around the one-pixel far dot (hud_line_view of one point) */
void enh_dot_end(int colour);
void enh_tracer_p0(void);                          /* A17E S2: B85D's point after the first projection (the tail of the tracer) */
bool enh_tracer_begin(void);                       /* before the hud_line_view of the tracer: true = an hr line follows */
void enh_tracer_end(int colour);
bool enh_box_begin(void);                          /* hud_target_box / hud_draw_diamond: true = an hr outline follows (the 1x lines are a footprint) */
void enh_box_end(int x, int y, int sz, int hex, int colour);   /* hud_target_box after the lines: sz = the half size after the 294B halving */
void enh_diamond_end(int sx, int sy, int colour);

/* ---- S2.4: the fixed statics of each file for the display-pass snapshot (the row tables tl/tr, tab_l/tab_r are scratch rebuilt from the
 * faithful R3dWork tables when a pass opens, and are not in it) */
#include <stddef.h>
void hr_clip_regions(void (*add)(void *p, size_t n));
void hr_edge2d_regions(void (*add)(void *p, size_t n));
void hr_raster_regions(void (*add)(void *p, size_t n));

/* ---- S3.5 smooth distance shading (hr_shade.c, DESIGN 2.11). A 3D layer word: bits 0..7 the index, bit 8 written, bit 9 stamped, bits 10..15 the blend code
 * (0 = none, else d = code - 32 in -31..31: the continuous colour position minus the index in 1/64 of a ramp entry; the compositor mixes DAC[idx] with DAC[idx +- 1]
 * of the same 16-entry ramp by |d| / 64). Only written with --smooth-shading. */
#define HR_BLEND_SHIFT 10
#define HR_BLEND_MASK 0xFC00
static inline u16 hr_blend_code(int d) { if (d < -31) d = -31; if (d > 31) d = 31; return d ? (u16)((32 + d) << HR_BLEND_SHIFT) : 0; }
extern bool hr_smooth;                             /* the option, latched when a pass opens */
void hr_shade_pass_begin(void);                    /* a pass opens: the object shade is forgotten */
void hr_shade_object(void);                        /* r3d_place_object (E7AC) after [4894] = 1088:0120: the continuous shade of the object being drawn */
void hr_shade_queue(u16 e);                        /* r3d_queue_insert (DDFE) after the record's +4 shade: the continuous shade of record e */
void hr_shade_dequeue(u16 rec);                    /* r3d_draw_queued (DF08) after [4894] = record +4 */
void hr_shade_colour(u16 colour);                  /* r3d_setcolor_M (1359:01D4): the colour word before the shade (its unshaded low byte) */
u8   hr_shade_lo(u16 colour);                       /* S3.6: the unshaded low byte of the colour word being drawn */
u16  hr_shade_code(u16 colour);                    /* the blend code of a fill / line in the colour word (DS:85D6 after set_colour_M); 0 = not a shaded colour */
void hr_band_blend(s16 H, const s16 *sky, const s16 *gnd);   /* hr_sky_ground after the bands: the per-pixel code of the band pixels */
void hr_shade_regions(void (*add)(void *p, size_t n));

/* ---- S3.6 true night (hr_night.c, DESIGN 2.12): the night plane (u8 per layer pixel: 0 = no night pass, 1..254 = the brightness factor / 256, HR_NIGHT_LIT = a
 * light, shown as the original), only in the main 3D window of a night mission with --true-night */
#define HR_NIGHT_LIT 255
enum { HR_NM_NONE, HR_NM_OBJ, HR_NM_LIT, HR_NM_KEEP };   /* how hr_put sets the factor: none (filled at the pass end), the object's, a light, a given value */
void hr_night_pass_begin(bool main_win);           /* a pass opens: enh_hr.night = the plane or NULL */
void hr_night_pass_end(void);                      /* the main pass ends: the pixels left without a factor get the factor of their place */
void hr_night_late(void);                          /* fix 2: ... and those written after it ended (at the next pass, at the flip) */
long hr_night_late_count(void);                    /* test aid: the pixels hr_night_late gave a factor */
void hr_night_forget(void);                        /* the layers were reallocated */
void hr_night_object(void);                        /* hr_shade_object: the factor of the object being drawn, from its depth */
void hr_night_queue(int slot);                     /* hr_shade_queue: ... of DDFE record slot */
void hr_night_dequeue(int slot);                   /* hr_shade_dequeue */
void hr_night_cond(u16 v);                         /* r3d_face_list: a condition on variable v holds (10 / 11 = the lights) */
void hr_night_elem(void);                          /* r3d_face_list: an element (polygon / lines) begins */
int  hr_night_mode(bool line, u8 c);               /* the mode of the object fill / line being drawn in colour c */
u8   hr_night_obj(s32 x, s32 y);                   /* the factor of an object pixel */
void hr_night_backdrop(s16 H);                     /* hr_sky_ground after the backdrop: the sky / ground factors of its pixels */
void hr_night_regions(void (*add)(void *p, size_t n));

/* ---- inside the back end */
typedef struct HrEdge {
    s32 x0, y0, x1, y1;           /* before the clip: projected + centre; after it: the clipped ends */
    u16 flags;                    /* the rec+2 flag word of the original */
    s32 yc0, yc1;                 /* rec+6 / rec+0E: y clamped to the window */
    s32 nx, ny;                   /* rec+10h: the near-plane point */
} HrEdge;

void hr_clip_edge(HrEdge *e);
void hr_clip_begin(void);                          /* the assembler state (walls, flags, near-plane points) from the faithful one */
void hr_raster_begin(void);                       /* the row tables and assembler state from the faithful ones (R3dWork) */
void hr_edge_dda(const HrEdge *e);
void hr_wall_fill(s32 a, s32 b, s32 x);
void hr_fill(u16 colour);                          /* fill_M on the row tables */
void hr_draw_line(s32 x0, s32 y0, s32 x1, s32 y1, u8 c);   /* line_M with the wide brush */
void hr_draw_line_shaded(s32 x0, s32 y0, s32 x1, s32 y1, u16 colour);   /* S3.5: line_M with the blend code of a shaded colour word */
void hr_put(s32 x, s32 y, u8 c);                   /* one layer pixel (no clip) */
void hr_put_span(s32 y, s32 l, s32 r, u8 c);       /* S4.3: hr_put(x, y, c) for x = l..r (no clip) */
u8   hr_get(s32 x, s32 y);                         /* the layer pixel, or the page pixel under it when not written */
s32 *hr_tab_l(void);
s32 *hr_tab_r(void);
extern s32 hr_miny;

/* 16-bit wrap rule of R1: a value that wraps at the 1x magnitude is used wrapped, then scaled */
static inline s64 hr_keepwrap(s64 exact1, s64 hr, int n) { return exact1 == (s16)exact1 ? hr : (s64)n * (s16)exact1; }
static inline bool hr_fits16(s64 v, int n) { return v >= -32768LL * n && v <= 32767LL * n; }

/* ---- --hr-dump (S2.3): the primitives the hr back end rasterised, as int32 words, for the independent reference (port/notes/e23/r3d_ref.py).
 * 0 B xmin ymin xmax ymax keep (a pass opens; layer px; keep = 1: it continues the previous pass, S3.2)   1 E x0 y0 x1 y1 (a clipped edge entering the DDA)   2 W a b x (a wall)
 * 3 F colour (the polygon of the E / W since the last F is filled)   4 L x0 y0 x1 y1 colour w o (a line, brush w at offset o)
 * 5 Q ox oy colour n x y ... (a 1001 polygon, points relative to the window origin)   6 H (hz_draw fills a half plane: not modelled)
 * 10 T x0 y0 x1 y1 (S3.2: the FLIR tint recoloured this layer rectangle through the green ramp 98h-9Fh)
 * 14 P x0 x1 y (S3.4: the pass just opened is a wide main pass whose layer pixels x0..x1, rows y.. (the frame below the 3D window) are never written) */
extern bool hr_tracing;
void hr_trace(int type, int nargs, const s32 *a);
void hr_trace_poly(s32 ox, s32 oy, u8 colour, int n, const s64 *pts);

/* ---- --hr-check (the render test): per scenario statistics of the hr layer against the 1x page */
void enh_hr_test_begin(void);                      /* simtest "scn": canvas on at --render-scale, layers and shadow cleared */
void enh_hr_test_end(const char *dir, const char *id);
void hr_obj_stats(long *sprites, long *sprite_px, long *dots, long *lines, long *line_px, bool reset);   /* S3.1: what the emitters drew since the last reset */   /* simtest "end": one line into DIR/hr_check.txt */
