/* The interpolated fields of a display pass (interp.h; DESIGN.md 4.3, 4.8; S4.1). */
#include "enhanced/interp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "sim/sim_priv.h"

typedef struct {
    bool valid;
    u16 view; u8 style;           /* C07A, [294B] */
    s32 x, y;                     /* BA10, C02A (world units) */
    u16 cx, cy;                   /* C0A4, C0B2 (cells) */
    u16 alt;                      /* 2DF4 */
    u16 hdg, pit, rol;            /* 2DEE, 2DF0, 2DF2 */
    s16 m[9];                     /* 2D94 */
    u16 eject, end;               /* C06E, E552 */
} Pose;

static Pose fp, fc;               /* F_{k-1}, F_k */
static bool lerp_ok;
static const char *hold_why = "start";

enum { JUMP_POS = 1024, JUMP_ALT = 0x800, JUMP_ANG = 0x4000 };

static void pose_read(Pose *p)
{
    p->valid = true;
    p->view = DSW(0xC07A); p->style = DSB(0x294B);
    p->x = (s32)DSL(0xBA10); p->y = (s32)DSL(0xC02A);
    p->cx = DSW(0xC0A4); p->cy = DSW(0xC0B2);
    p->alt = DSW(0x2DF4);
    p->hdg = DSW(0x2DEE); p->pit = DSW(0x2DF0); p->rol = DSW(0x2DF2);
    for (int i = 0; i < 9; i++) p->m[i] = (s16)DSW((u16)(0x2D94 + 2 * i));
    p->eject = DSW(0xC06E); p->end = DSB(0xE552);
}

static int ang_d(u16 a, u16 b) { return (s16)(u16)(b - a); }   /* the shortest arc a -> b */

void enh_interp_reset(void) { fp.valid = fc.valid = false; lerp_ok = false; hold_why = "start"; }

void enh_interp_frame(void)
{
    fp = fc;
    pose_read(&fc);
    lerp_ok = false;
    if (!fp.valid) hold_why = "start";
    else if (fp.view != fc.view || fp.style != fc.style) hold_why = "view";
    else if (fp.eject || fc.eject) hold_why = "eject";
    else if (fp.end || fc.end) hold_why = "end";
    else if (labs((long)fc.x - fp.x) > JUMP_POS || labs((long)fc.y - fp.y) > JUMP_POS || abs((int)fc.alt - (int)fp.alt) > JUMP_ALT) hold_why = "jump";
    else if (abs(ang_d(fp.hdg, fc.hdg)) > JUMP_ANG || abs(ang_d(fp.pit, fc.pit)) > JUMP_ANG || abs(ang_d(fp.rol, fc.rol)) > JUMP_ANG) hold_why = "turn";
    else { lerp_ok = true; hold_why = ""; }
}

bool enh_interp_lerp(void) { return lerp_ok; }
const char *enh_interp_hold_reason(void) { return hold_why; }

static s32 lerp_l(s32 a, s32 b, double t) { return a + (s32)lround((double)((long long)b - a) * t); }
static u16 lerp_a(u16 a, u16 b, double t) { return (u16)(a + (int)lround(ang_d(a, b) * t)); }

/* the matrix sim_matrix_from_angles builds for the angles, as fm_mat_from_euler calls it (fl_flight.c) */
static void mat_of(s16 *m, u16 h, u16 p, u16 r)
{
    sim_matrix_from_angles(0x2D94, h, (s16)p, r);
    for (int i = 0; i < 9; i++) m[i] = (s16)DSW((u16)(0x2D94 + 2 * i));
}

void enh_interp_apply(double t)
{
    if (!lerp_ok || t >= 1.0) return;
    if (t < 0) t = 0;
    const s32 x = lerp_l(fp.x, fc.x, t), y = lerp_l(fp.y, fc.y, t);
    DSL(0xBA10) = (u32)x;
    DSL(0xC02A) = (u32)y;
    /* the cells as 39C4 derives them ((x + 16) >> 5, 0x8000 - ((y + 16) >> 5), clamped): F_k's value moved by the cells the lerped position is off */
    DSW(0xC0A4) = (u16)(fc.cx + ((x + 16) >> 5) - ((fc.x + 16) >> 5));
    DSW(0xC0B2) = (u16)(fc.cy - (((y + 16) >> 5) - ((fc.y + 16) >> 5)));
    DSW(0x2DF4) = (u16)lerp_l(fp.alt, fc.alt, t);
    const u16 h = lerp_a(fp.hdg, fc.hdg, t), p = lerp_a(fp.pit, fc.pit, t), r = lerp_a(fp.rol, fc.rol, t);
    s16 ra[9], rp[9], rc[9];
    mat_of(rp, fp.hdg, fp.pit, fp.rol);
    mat_of(rc, fc.hdg, fc.pit, fc.rol);
    mat_of(ra, h, p, r);
    for (int i = 0; i < 9; i++) {
        const double e = (1.0 - t) * (fp.m[i] - rp[i]) + t * (fc.m[i] - rc[i]);   /* the integrated matrix's residual against its angles */
        long v = ra[i] + lround(e);
        DSW((u16)(0x2D94 + 2 * i)) = (u16)(s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    DSW(0x2DEE) = h; DSW(0x2DF0) = p; DSW(0x2DF2) = r;
}

void enh_interp_trace(FILE *f, unsigned long long frame, int pass, double alpha, double ms)
{
    if (!f) return;
    fprintf(f, "%llu %d %.4f %.3f %d %s view=%x cam=%d,%d,%d ang=%u,%u,%u own=%d,%d,%u hdg=%u\n", frame, pass, alpha, ms, lerp_ok ? 1 : 0,
            lerp_ok ? "-" : hold_why, DSW(0xC07A), (s32)DSL(0xB288), (s32)DSL(0xB752), DSW(0xB75C), DSW(0xE44C), DSW(0xC07C), DSW(0xB770),
            (s32)DSL(0xBA10), (s32)DSL(0xC02A), DSW(0x2DF4), DSW(0x2DEE));
}
