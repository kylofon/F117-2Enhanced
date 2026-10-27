/* S3.6 true night (DESIGN.md 2.12): the night plane of the 3D layer.
 *
 * In a night mission ([3658] != 0) with --true-night, every layer pixel of the main 3D window gets a brightness factor in the night plane: 1..254 = factor / 256,
 * 255 = a light (shown as the original), 0 = no night pass. The factor is a continuous function of a per-pixel distance:
 *   sky (above the horizon line of the band geometry, 2.11): its elevation;
 *   ground (the backdrop and the AND / OR faces on it): the ground distance solved from the band geometry, per pixel;
 *   objects: min(the ground distance of the pixel, the object's far depth C / 65536 + its class radius) - an object lying on the ground (terrain tiles, roads, the
 *     sea) gets exactly the ground's factor, so neighbouring faces and the backdrop join without a seam; a standing object is never darker than its far side;
 *   anything else the pass left without a factor (sprites: smoke, the far dots; 1x stamps; unwritten 1x pixels) gets the factor of its pixel at the end of the pass.
 * Lights are decided by the drawing source: the faces / lines under a lights condition (var 10 / 11) and the model lines in the light colours 90h..97h; the overlay
 * pass after the scene (explosions, tracers, flares, target boxes) is not a night pass. The compositor multiplies the live-DAC colour by the factor.
 * A night pass also turns on the S3.5 blend codes of the bands and shaded ramps, so the colours under the factor are continuous too.
 * Nothing here writes mem[], the page or the VGA model: only hr state and the night plane. */
#include "enhanced/hr.h"
#include "enhanced/par.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "enhanced/enh_canvas.h"
#include "enhanced/enhanced.h"
#include "mem.h"

/* ---- the tuned constants (DESIGN 2.12) */
#define K_NEAR      0.50       /* ground and objects at depth 0: half their night brightness */
#define K_FAR       0.06       /* far away: near black */
#define D_LIT       3000.0     /* the e-folding depth of the fade (depth units of [4960] >> 16, the units of [4980]) */
#define SKY_HORIZON 0.30       /* the sky at the horizon */
#define SKY_TOP     0.10       /* the sky far above it */
#define SKY_E       0.15       /* the e-folding elevation (tan of the angle above the horizon) */
#define D_HORIZON   131072.0   /* the ground ring of the horizon line (the band formula: (16 - 0) 8192) */

#define LUT_STEP 8.0           /* the depth curve in steps of 8 depth units up to the horizon ring */
#define LUT_N 16385
static u8 lut_d[LUT_N];
#define SKY_LUT_N 2049         /* the sky factor in steps of 1/512 elevation */
static u8 lut_s[SKY_LUT_N];
static bool lut_ready;

/* the hr state of the frame being drawn (in the display-pass snapshot): the geometry of the pass (t = ax X + ay Y + c0 - H, layer px; f the focal, fa = f alt),
 * the far depth of the object being drawn (< 0 = none) and of the queued records, the lights condition */
static struct { bool geo; double ax, ay, c0, f, fa; float obj; float queued[50]; u8 cond, light; } st;
/* the window of the last night pass and its geometry, for the pixels written after its end (fix 2: hr_night_late) */
static struct { bool valid; double ax, ay, c0, f, fa; s32 xmin, ymin, xmax, ymax, lw, lh; bool hole; s32 hole_x0, hole_x1, hole_y; } last;
static long late_px;

void hr_night_regions(void (*add)(void *p, size_t n)) { add(&st, sizeof st); add(&last, sizeof last); add(&late_px, sizeof late_px); }

static u8 to_nd(double k)
{
    int v = (int)lround(k * 256.0);
    return (u8)(v < 1 ? 1 : v > HR_NIGHT_LIT - 1 ? HR_NIGHT_LIT - 1 : v);
}

static double curve(double d) { if (d < 0) d = 0; return K_FAR + (K_NEAR - K_FAR) * exp(-d / D_LIT); }

static void lut_init(void)
{
    if (lut_ready) return;
    for (int i = 0; i < LUT_N; i++) lut_d[i] = to_nd(curve(i * LUT_STEP));
    for (int i = 0; i < SKY_LUT_N; i++) lut_s[i] = to_nd(SKY_TOP + (SKY_HORIZON - SKY_TOP) * exp(-(i / 512.0) / SKY_E));
    lut_ready = true;
}

static inline u8 nd_depth(double d)
{
    if (!(d > 0)) return lut_d[0];
    const double i = d / LUT_STEP + 0.5;
    return i >= LUT_N - 1 ? lut_d[LUT_N - 1] : lut_d[(int)i];
}

static inline double t_of(s32 x, s32 y) { return st.ax * x + st.ay * y + st.c0; }   /* > 0 above the horizon (1x t-units) */
static inline double ground_d(double t) { const double G = t < 0 ? -t : 0; return 1.0 / (G / st.fa + 1.0 / D_HORIZON); }
static inline u8 sky_nd(double t)
{
    const double e = t > 0 ? t / st.f * 512.0 + 0.5 : 0;
    return lut_s[e < SKY_LUT_N - 1 ? (int)e : SKY_LUT_N - 1];
}
static inline u8 pixel_nd(s32 x, s32 y) { const double t = t_of(x, y); return t >= 0 ? sky_nd(t) : nd_depth(ground_d(t)); }

/* S4.3: the rows ymin..ymax (clipped to 0..lh - 1) of a per-pixel loop on the worker pool */
static void par_rows(s32 ymin, s32 ymax, s32 lh, EnhParFn fn, void *ctx)
{
    const s32 y0 = ymin < 0 ? 0 : ymin, y1 = ymax < lh - 1 ? ymax : lh - 1;
    if (y1 >= y0) enh_par_for_from(y0, y1 + 1, fn, ctx);
}
static void backdrop_rows(void *unused, int lo, int hi);

void hr_night_pass_begin(bool main_win)
{
    hr_night_late();                               /* fix 2: what was written after the last night pass ended */
    enh_hr.night = NULL;
    memset(&st, 0, sizeof st);
    st.obj = -1;
    if (!main_win || !enh_true_night_on() || !DSW(0x3658) || DSB(0x7D5E)) return;   /* the main 3D window of a night mission, not the map */
    lut_init();
    enh_hr.night = enh_layer_night();
}

/* ---------------------------------------------------------------- objects */

/* the far depth of the object at 1088:0120's inputs: its view depth C [4960] plus the x radius of its size class (DS:48D6, the cull radius) */
static float obj_now(void)
{
    if (!enh_hr.night) return -1;
    const s32 C = (s32)DSSL(0x4960);
    u8 cls = DSB(0x497A);
    if (cls > 7) cls = 7;
    double d = (double)C / 65536.0 + (s16)DSW((u16)(0x48D6 + 2 * cls));
    return (float)(d < 0 ? 0 : d);
}

void hr_night_object(void) { st.obj = obj_now(); }
void hr_night_queue(int slot) { if (slot >= 0 && slot < 50) st.queued[slot] = obj_now(); }
void hr_night_dequeue(int slot) { st.obj = slot >= 0 && slot < 50 ? st.queued[slot] : -1; }

/* r3d_face_list: a condition on the lights variables (10, 11) holds for the next element; the element begins (or is skipped) */
void hr_night_cond(u16 v) { if (v == 10 || v == 11) st.cond = 1; }
void hr_night_elem(void) { st.light = st.cond; st.cond = 0; }

/* the mode of the object fill (line = false) or line being drawn in colour c (its unshaded index) */
int hr_night_mode(bool line, u8 c)
{
    if (!enh_hr.night) return HR_NM_NONE;
    if (st.light || (line && c >= 0x90 && c <= 0x97)) return HR_NM_LIT;   /* the light faces, the light strings */
    return st.obj >= 0 ? HR_NM_OBJ : HR_NM_NONE;                          /* NONE: filled from the pixel at the end of the pass */
}

/* the factor of an object pixel: its ground distance, not beyond the object's far side (above the horizon: the far side) */
u8 hr_night_obj(s32 x, s32 y)
{
    if (st.obj < 0) return st.geo ? pixel_nd(x, y) : 0;              /* no object depth (an AND / OR face on a 1x pixel): the place's factor */
    if (!st.geo) return nd_depth(st.obj);
    const double t = t_of(x, y);
    double d = t >= 0 ? st.obj : ground_d(t);
    if (d > st.obj) d = st.obj;
    return nd_depth(d);
}

/* ---------------------------------------------------------------- the backdrop */

/* after 1442 (every detail level): the pass geometry from the band geometry of 2.11, t(q) = -(sn (qx - w2) + cs (qy - h2) 4/3) - H in 1x t-units; the focal
 * f = 256 << [433A], halved with [294B]; alt = [4980]. Then every written, not stamped pixel of the pass window gets its sky / ground factor. */
void hr_night_backdrop(s16 H)
{
    if (!enh_hr.night || !enh_hr.on || !enh_hr.layer) return;
    const double N = enh_hr.n;
    const double sn = (double)(s16)(u16)-DSW(0x498A) / 32768.0, cs = (double)(s16)DSW(0x498C) / 32768.0;
    const double w2 = (s16)DSW(0x0D1E), h2 = (s16)DSW(0x0D20);
    const double ox = (double)enh_hr.ox + enh_hr.e2dx, oy = (double)enh_hr.oy + enh_hr.e2dy;
    st.ax = -sn / N; st.ay = -cs * (4.0 / 3.0) / N;
    st.c0 = sn * (ox / N + w2) + cs * (4.0 / 3.0) * (oy / N + h2) - H;
    double f = 256.0;
    const u8 z = DSB(0x433A);
    if (z && (z & 0x1F) < 16) f *= (double)(1 << (z & 0x1F));
    if (DSB(0x294B)) f /= 2;
    const s16 alt = (s16)DSW(0x4980);
    st.f = f; st.fa = f * (alt > 1 ? alt : 1);
    st.geo = true;
    par_rows(enh_hr.ymin, enh_hr.ymax, enh_hr.lh, backdrop_rows, NULL);   /* S4.3: rows on the worker pool */
}

static void backdrop_rows(void *unused, int lo, int hi)
{
    (void)unused;
    for (s32 y = lo; y < hi; y++) {
        const u16 *row = enh_hr.layer + (size_t)y * enh_hr.lw;
        u8 *nrow = enh_hr.night + (size_t)y * enh_hr.lw;
        for (s32 x = enh_hr.xmin < 0 ? 0 : enh_hr.xmin; x <= enh_hr.xmax && x < enh_hr.lw; x++) {
            if ((row[x] & 0x300) != 0x100 || HR_IN_HOLE(x, y)) continue;
            const u8 c = (u8)row[x];
            const double t = t_of(x, y);
            if (c >= 0x80 && c <= 0x8F) nrow[x] = sky_nd(t);                  /* a band pixel the edge rounding put across the horizon line: its own side's */
            else if (c >= 0x70 && c <= 0x7F) nrow[x] = nd_depth(ground_d(t));  /* value at the line (sky_nd / ground_d clamp t at 0) */
            else nrow[x] = pixel_nd(x, y);
        }
    }
}

static void pass_end_rows(void *unused, int lo, int hi)
{
    (void)unused;
    for (s32 y = lo; y < hi; y++) {
        u8 *nrow = enh_hr.night + (size_t)y * enh_hr.lw;
        for (s32 x = enh_hr.xmin < 0 ? 0 : enh_hr.xmin; x <= enh_hr.xmax && x < enh_hr.lw; x++)
            if (!nrow[x] && !HR_IN_HOLE(x, y)) nrow[x] = pixel_nd(x, y);
    }
}

/* the end of the main pass: every pixel of the window still without a factor (sprites, dots, stamps, 1x pixels) gets the factor of its place */
void hr_night_pass_end(void)
{
    if (!enh_hr.night || !st.geo) return;
    par_rows(enh_hr.ymin, enh_hr.ymax, enh_hr.lh, pass_end_rows, NULL);   /* S4.3: rows on the worker pool */
    last.valid = true;
    last.ax = st.ax; last.ay = st.ay; last.c0 = st.c0; last.f = st.f; last.fa = st.fa;
    last.xmin = enh_hr.xmin; last.ymin = enh_hr.ymin; last.xmax = enh_hr.xmax; last.ymax = enh_hr.ymax; last.lw = enh_hr.lw; last.lh = enh_hr.lh;
    last.hole = enh_hr.hole; last.hole_x0 = enh_hr.hole_x0; last.hole_x1 = enh_hr.hole_x1; last.hole_y = enh_hr.hole_y;
}

/* fix 2: the 3D layer pixels of the last night window written after its pass ended (1x writes of the overlay pass and the rest of the frame that the hr back
 * end does not mirror, stamped with factor 0) get the factor of their place too; at the next pass and at the flip. Lights of the overlay pass keep 255. */
typedef struct { u8 *nd; const u16 *lay; } LateJob;
static SDL_AtomicInt late_add;

static void late_rows(void *jp, int lo, int hi)
{
    const LateJob *j = jp;
    int n = 0;
    for (s32 y = lo; y < hi; y++) {
        u8 *nrow = j->nd + (size_t)y * last.lw;
        const u16 *lrow = j->lay + (size_t)y * last.lw;
        for (s32 x = last.xmin < 0 ? 0 : last.xmin; x <= last.xmax && x < last.lw; x++) {
            if (nrow[x] || !(lrow[x] & 0x100)) continue;
            if (last.hole && y >= last.hole_y && x >= last.hole_x0 && x <= last.hole_x1) continue;
            nrow[x] = pixel_nd(x, y);
            n++;
        }
    }
    SDL_AddAtomicInt(&late_add, n);
}

void hr_night_late(void)
{
    if (!last.valid) return;
    last.valid = false;
    u8 *nd = enh_layer_night();
    const u16 *lay = enh_layer3d();
    if (!nd || !lay || last.lw != enh_cv.lw || last.lh != enh_cv.lh) return;
    const double ax = st.ax, ay = st.ay, c0 = st.c0, f = st.f, fa = st.fa;
    st.ax = last.ax; st.ay = last.ay; st.c0 = last.c0; st.f = last.f; st.fa = last.fa;
    LateJob j = { nd, lay };
    SDL_SetAtomicInt(&late_add, 0);
    par_rows(last.ymin, last.ymax, last.lh, late_rows, &j);           /* S4.3: rows on the worker pool */
    late_px += SDL_GetAtomicInt(&late_add);
    st.ax = ax; st.ay = ay; st.c0 = c0; st.f = f; st.fa = fa;
}

long hr_night_late_count(void) { return late_px; }
void hr_night_forget(void) { last.valid = false; }
