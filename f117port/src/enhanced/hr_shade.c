/* S3.5 smooth distance shading (DESIGN.md 2.11): the blend codes of the 3D layer.
 *
 * Bands (1442): after the hr bands every layer pixel of the pass window showing a sky (80h + i) or ground (70h + i) band colour gets its position inside the band,
 * from the band geometry of r3d_sky.c band_points (a pixel q has the perpendicular offset t(q) = -(sn dx + cs dy 4/3), the band lies between t_i and t_(i+1)).
 * Objects (1088:0120): the continuous shade of the object being drawn (E7AC, or its DDFE record at DF08), turned into a code for each shaded fill / line.
 * Nothing here writes mem[], the page or the VGA model: only hr state and the layer's bits 10..15. */
#include "enhanced/hr.h"
#include "enhanced/par.h"

#include <math.h>
#include <string.h>

#include "enhanced/enh_canvas.h"
#include "enhanced/enhanced.h"
#include "mem.h"

bool hr_smooth;

/* the object shade: valid, the original shade s (the [4894] it belongs to) and the continuous shade sc in 1/64 of a step (0..1023) */
typedef struct { u8 valid, s; u16 sc; } Shade;
static Shade cur;
static Shade queued[50];                           /* per DDFE record slot (0x49F0 + 0x16 k) */
static struct { u8 valid, lo; } col;               /* the unshaded colour byte of the last set_colour_M */

void hr_shade_regions(void (*add)(void *p, size_t n))
{
    add(&hr_smooth, sizeof hr_smooth); add(&cur, sizeof cur); add(queued, sizeof queued); add(&col, sizeof col);
}

void hr_shade_pass_begin(void)
{
    hr_smooth = enh_smooth_on() || enh_hr.night;                      /* S3.6: true night implies the blend of the colours it darkens */
    cur.valid = 0;
    memset(queued, 0, sizeof queued);
    col.valid = 0;
}

/* 1088:0120 again, continuous: s = clamp((C >> 25) - class, 0, 31) >> 1; sc = clamp(C / 2^25 - class, 0, 32) / 2 (1/64) */
static Shade shade_now(void)
{
    Shade r = { 0, 0, 0 };
    if (!hr_smooth || DSB(0x7D5E) || !DSB(0x43B6)) return r;
    s32 C = (s32)DSSL(0x4960);
    if (C < 0) C = 0;                                                 /* (s8)[4963] < 0 -> 0 */
    const u8 cls = DSB(0x497A);
    u8 h = (u8)((u8)(C >> 25) - cls);
    if (h & 0x80) h = 0;
    if ((s8)h > 0x1F) h = 0x1F;
    const u8 s = (u8)(h >> 1);
    if (cls > 63 || DSB(0x4894) != s) return r;                       /* not the value the original computed (an odd class): no blend */
    s32 hc = (C >> 19) - 64 * (s32)cls;                               /* h in 1/64 */
    if (hc < 0) hc = 0;
    if (hc > 32 * 64 - 1) hc = 32 * 64 - 1;
    s32 sc = hc / 2;
    if (sc / 64 != s) return r;
    r.valid = 1; r.s = s; r.sc = (u16)sc;
    return r;
}

void hr_shade_object(void) { cur = shade_now(); hr_night_object(); /* S3.6 */ }

static int slot_of(u16 e) { int k = (int)(e - 0x49F0) / 0x16; return e >= 0x49F0 && (e - 0x49F0) % 0x16 == 0 && k < 50 ? k : -1; }

void hr_shade_queue(u16 e)
{
    int k = slot_of(e);
    hr_night_queue(k);                                                /* S3.6 */
    if (k < 0) return;
    Shade s = shade_now();
    if (s.valid && DSB((u16)(e + 4)) != s.s) s.valid = 0;
    queued[k] = s;
}

void hr_shade_dequeue(u16 rec)
{
    int k = slot_of(rec);
    hr_night_dequeue(k);                                              /* S3.6 */
    cur = k >= 0 ? queued[k] : (Shade){ 0, 0, 0 };
    if (cur.valid && DSB(0x4894) != cur.s) cur.valid = 0;
}

void hr_shade_colour(u16 colour) { col.valid = 1; col.lo = (u8)colour; }
u8 hr_shade_lo(u16 colour) { return col.valid && (u8)(colour >> 8) == 0xFF && (u8)colour >= 0xA0 ? col.lo : (u8)colour; }   /* S3.6: the unshaded index */

u16 hr_shade_code(u16 colour)
{
    const u8 al = (u8)colour, ah = (u8)(colour >> 8);
    if (!hr_smooth || ah != 0xFF || al < 0xA0 || !cur.valid || !col.valid || DSB(0x7D5E) || DSB(0x4894) != cur.s) return 0;
    const u8 lo = col.lo, base = al & 0xF0;
    if (lo < 0xA0 || (lo & 0xF0) != base) return 0;
    const int s = cur.s;
    if (al != (lo - s > base ? lo - s : base)) return 0;              /* not this object's shade of this colour */
    int t = cur.sc - 32;                                              /* centred: the original colour at the middle of its step */
    if (t < 0) t = 0;
    if (t > 15 * 64) t = 15 * 64;
    int ic = 64 * lo - t;
    if (ic < 64 * base) ic = 64 * base;
    return hr_blend_code(ic - 64 * al);
}

/* ---------------------------------------------------------------- the bands */

/* S4.3: lround (half away from zero) without the library call: v - trunc(v) is exact for |v| < 2^52 */
static inline int round_half_away(double v)
{
    const double t = (double)(long long)v, r = v - t;
    return (int)(r >= 0.5 ? t + 1 : r <= -0.5 ? t - 1 : t);
}

typedef struct { double T[2][17], W[2][16], ax, ay, c0; } BandJob;

static void band_rows(void *jp, int lo, int hi)
{
    const BandJob *j = jp;
    for (s32 y = lo; y < hi; y++) {
        u16 *row = enh_hr.layer + (size_t)y * enh_hr.lw;
        const double ty = j->ay * y + j->c0;
        for (s32 x = enh_hr.xmin < 0 ? 0 : enh_hr.xmin; x <= enh_hr.xmax && x < enh_hr.lw; x++) {
            const u16 l = row[x];
            if ((l & 0x300) != 0x100 || HR_IN_HOLE(x, y)) continue;   /* written by the hr back end, not stamped; S3.4: not the panel hole (other passes' pixels) */
            const u8 c = (u8)l;
            int g;
            if (c >= 0x80 && c <= 0x8F) g = 0;
            else if (c >= 0x70 && c <= 0x7F) g = 1;
            else continue;
            const int i = c & 15;
            const double w = j->W[g][i];
            if (!(fabs(w) >= 0.5)) continue;                          /* a band of no width (the ground at low altitude) */
            double f = (j->ax * x + ty - j->T[g][i]) / w;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            double d = f - 0.5;
            if ((i == 0 && d < 0) || (i == 15 && d > 0)) d = 0;
            row[x] = (u16)((l & 0x3FF) | hr_blend_code(round_half_away(d * 64)));
        }
    }
}

void hr_band_blend(s16 H, const s16 *sky, const s16 *gnd)
{
    if (!hr_smooth || !enh_hr.on || !enh_hr.layer) return;
    BandJob j;
    for (int g = 0; g < 2; g++) {
        for (int i = 0; i <= 16; i++)
            j.T[g][i] = g ? (i < 16 ? (s16)(H - gnd[i]) : (s16)0xA7B8) : (i < 16 ? (s16)(sky[i] + H) : 0x5848);
        for (int i = 0; i < 15; i++) j.W[g][i] = j.T[g][i + 1] - j.T[g][i];
        j.W[g][15] = j.W[g][14];                                      /* band 15 runs to +-22600: the width of band 14 */
    }
    const double N = enh_hr.n;
    const double sn = (double)(s16)(u16)-DSW(0x498A) / 32768.0, cs = (double)(s16)DSW(0x498C) / 32768.0;
    const double w2 = (s16)DSW(0x0D1E), h2 = (s16)DSW(0x0D20);
    const double ox = (double)enh_hr.ox + enh_hr.e2dx, oy = (double)enh_hr.oy + enh_hr.e2dy;
    j.ax = -sn / N; j.ay = -cs * (4.0 / 3.0) / N;                     /* t = ax X + ay Y + c0 (layer px) */
    j.c0 = sn * (ox / N + w2) + cs * (4.0 / 3.0) * (oy / N + h2);
    const s32 y0 = enh_hr.ymin < 0 ? 0 : enh_hr.ymin, y1 = enh_hr.ymax < enh_hr.lh - 1 ? enh_hr.ymax : enh_hr.lh - 1;
    if (y1 >= y0) enh_par_for_from(y0, y1 + 1, band_rows, &j);       /* S4.3: rows on the worker pool (each pixel depends only on itself) */
}
