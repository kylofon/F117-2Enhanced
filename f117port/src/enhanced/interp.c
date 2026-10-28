/* The interpolated fields of a display pass (interp.h; DESIGN.md 4.3, 4.8, 4.10; S4.1, S4.2). */
#include "enhanced/interp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "sim/sim_priv.h"

enum { OBJ_UNITS = 48, OBJ_PROJ = 12, OBJ_TRACERS = 20, OBJ_SMOKE = 16 };

typedef struct {
    bool valid;
    u16 view; u8 style;           /* C07A, [294B] */
    s32 x, y;                     /* BA10, C02A (world units) */
    u16 cx, cy;                   /* C0A4, C0B2 (cells) */
    u16 alt;                      /* 2DF4 */
    u16 hdg, pit, rol;            /* 2DEE, 2DF0, 2DF2 */
    s16 m[9];                     /* 2D94 */
    u16 eject, end;               /* C06E, E552 */
    /* S4.2: the objects (DESIGN 4.3) */
    u16 fpm;                      /* 98C6, the flight-path marker's offset (the HUD velocity vector) */
    u16 spin[2];                  /* C04C, C04E (radar dishes, vars 15 / 16) */
    u16 wreck[3];                 /* C0A6, C0B8, C602 */
    u16 ring_i, ring_n, ring_ctr; /* the pose-ring slot view 0x84 reads ((3D60 - 3660) & 15), 3660, 3D60 */
    u8 ring[16];                  /* that slot (B056 + 16 i): x32, y32, alt, hdg, pitch, roll */
    u16 n_units, n_tr;            /* [DED0] (clamped), [B16C] + 4 (clamped) */
    u8 unit[OBJ_UNITS][0x24];     /* C140 */
    u8 proj[OBJ_PROJ][0x1C];      /* 3C0C */
    u8 tr[OBJ_TRACERS][12];       /* E44E */
    u8 smoke[OBJ_SMOKE][8];       /* 39DA */
} Pose;

static Pose fp, fc;               /* F_{k-1}, F_k */
static bool lerp_ok;
static const char *hold_why = "start";

enum { JUMP_POS = 1024, JUMP_ALT = 0x800, JUMP_ANG = 0x4000 };
/* S4.2 object jump limits: a unit (world units, x32 / y32), a projectile and the wreck (cells), a projectile's / the wreck's height, a smoke puff's height, the
 * flight-path marker's offset. Measured moves per step (Game speed 7, enh_fps_jerk / the sea chain): see DESIGN 4.10. */
enum { JUMP_UNIT = 2048, JUMP_CELLS = 512, JUMP_Z = 0x2000, JUMP_SMOKE_Z = 0x400, JUMP_FPM = 0x1000 };

/* S4.2: per object whether its slot interpolates in this frame (the same object in F_{k-1} and F_k: slot + type, no jump) */
static bool m_unit[OBJ_UNITS], m_proj[OBJ_PROJ], m_tr[OBJ_TRACERS], m_smoke[OBJ_SMOKE], m_wreck, m_spin, m_ring, m_fpm;
static unsigned long long st_lerp[ENH_OBJ_CLASSES], st_held[ENH_OBJ_CLASSES];
static int max_move[ENH_OBJ_CLASSES];   /* the largest move per step of a matched object (the jump limits' margin, the exit statistics) */

static inline u16 rw(const u8 *b, int o) { return (u16)(b[o] | b[o + 1] << 8); }
static inline s32 rl(const u8 *b, int o) { return (s32)((u32)rw(b, o) | (u32)rw(b, o + 2) << 16); }
static inline int d16(u16 a, u16 b) { return (s16)(u16)(b - a); }   /* a 16-bit field's move a -> b, positions and angles alike (the shortest arc) */

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
    p->fpm = DSW(0x98C6);
    p->spin[0] = DSW(0xC04C); p->spin[1] = DSW(0xC04E);
    p->wreck[0] = DSW(0xC0A6); p->wreck[1] = DSW(0xC0B8); p->wreck[2] = DSW(0xC602);
    p->ring_ctr = DSW(0x3D60); p->ring_n = DSW(0x3660);
    p->ring_i = (u16)((DSB(0x3D60) - DSB(0x3660)) & 15);          /* as view_frame's case 0x84 picks it */
    memcpy(p->ring, mp(cur_dgroup, (u16)(0xB056 + 16 * p->ring_i)), 16);
    const s16 nu = (s16)DSW(0xDED0);
    p->n_units = (u16)(nu < 0 ? 0 : nu > OBJ_UNITS ? OBJ_UNITS : nu);
    memcpy(p->unit, mp(cur_dgroup, 0xC140), (size_t)p->n_units * 0x24);
    memcpy(p->proj, mp(cur_dgroup, 0x3C0C), sizeof p->proj);
    const s16 nt = (s16)(DSW(0xB16C) + 4);
    p->n_tr = (u16)(nt < 0 ? 0 : nt > OBJ_TRACERS ? OBJ_TRACERS : nt);
    memcpy(p->tr, mp(cur_dgroup, 0xE44E), (size_t)p->n_tr * 12);
    memcpy(p->smoke, mp(cur_dgroup, 0x39DA), sizeof p->smoke);
}

static int ang_d(u16 a, u16 b) { return (s16)(u16)(b - a); }   /* the shortest arc a -> b */

static bool angs_ok(const u8 *a, const u8 *b, int o)
{
    for (int i = 0; i < 3; i++)
        if (abs(d16(rw(a, o + 2 * i), rw(b, o + 2 * i))) > JUMP_ANG) return false;
    return true;
}

static void count(int cls, bool m, int move)
{
    if (m) { st_lerp[cls]++; if (move > max_move[cls]) max_move[cls] = move; }
    else st_held[cls]++;
}

static int imax(int a, int b) { return a > b ? a : b; }

/* S4.2 (DESIGN 4.3): which objects interpolate between F_{k-1} and F_k. An object is the same one in both frames when its slot is in use in both with the same
 * type; it then interpolates unless it jumped (a new object in the slot, a teleport). A mismatch or a jump shows that object at F_k (A_k's own value). */
static void objects_match(void)
{
    memset(m_unit, 0, sizeof m_unit); memset(m_proj, 0, sizeof m_proj); memset(m_tr, 0, sizeof m_tr); memset(m_smoke, 0, sizeof m_smoke);
    m_wreck = m_spin = m_ring = m_fpm = false;
    if (!lerp_ok) return;
    const u16 nu = fp.n_units < fc.n_units ? fp.n_units : fc.n_units;
    for (int i = 0; i < nu; i++) {                                   /* units: +16 bit 1 active, +14 type */
        const u8 *a = fp.unit[i], *b = fc.unit[i];
        if (!(a[0x16] & 2) || !(b[0x16] & 2)) continue;
        const long dx = labs((long)rl(b, 6) - rl(a, 6)), dy = labs((long)rl(b, 0xA) - rl(a, 0xA));
        m_unit[i] = rw(a, 0x14) == rw(b, 0x14) && dx <= JUMP_UNIT && dy <= JUMP_UNIT && abs(d16(rw(a, 4), rw(b, 4))) <= JUMP_ALT && angs_ok(a, b, 0xE);
        count(ENH_OBJ_UNIT, m_unit[i], (int)(dx > dy ? dx : dy));
    }
    for (int i = 0; i < OBJ_PROJ; i++) {                              /* projectiles: +E active, +10 type */
        const u8 *a = fp.proj[i], *b = fc.proj[i];
        if (!rw(a, 0xE) || !rw(b, 0xE)) continue;
        const int dx = abs(d16(rw(a, 0), rw(b, 0))), dy = abs(d16(rw(a, 2), rw(b, 2)));
        m_proj[i] = rw(a, 0x10) == rw(b, 0x10) && dx <= JUMP_CELLS && dy <= JUMP_CELLS && abs(d16(rw(a, 4), rw(b, 4))) <= JUMP_Z && angs_ok(a, b, 8);
        count(ENH_OBJ_PROJ, m_proj[i], imax(dx, dy));
    }
    if (fp.n_tr == fc.n_tr)
        for (int i = 0; i < fc.n_tr; i++) {                          /* cannon tracers: x != 0; the same round = moved by its own velocity (465B), the velocity kept */
            const u8 *a = fp.tr[i], *b = fc.tr[i];
            if (!rw(a, 0) || !rw(b, 0)) continue;
            bool same = true;
            for (int c = 0; c < 3; c++)
                if (rw(a, 6 + 2 * c) != rw(b, 6 + 2 * c) || (u16)(rw(a, 2 * c) + rw(a, 6 + 2 * c)) != rw(b, 2 * c)) same = false;
            m_tr[i] = same;
            count(ENH_OBJ_TRACER, same, imax(abs(d16(rw(a, 0), rw(b, 0))), abs(d16(rw(a, 2), rw(b, 2)))));
        }
    for (int i = 0; i < OBJ_SMOKE; i++) {                             /* smoke puffs: the same puff = the same x / y (!= 0); its height (+4) moves */
        const u8 *a = fp.smoke[i], *b = fc.smoke[i];
        if (!rw(a, 0) || !rw(b, 0)) continue;
        const int dz = abs(d16(rw(a, 4), rw(b, 4)));
        m_smoke[i] = rw(a, 0) == rw(b, 0) && rw(a, 2) == rw(b, 2) && dz <= JUMP_SMOKE_Z;
        count(ENH_OBJ_SMOKE, m_smoke[i], dz);
    }
    if ((s16)fp.wreck[2] > 0 && (s16)fc.wreck[2] > 0) {             /* the falling wreck, while C602 > 0 in both */
        const int dx = abs(d16(fp.wreck[0], fc.wreck[0])), dy = abs(d16(fp.wreck[1], fc.wreck[1]));
        m_wreck = dx <= JUMP_CELLS && dy <= JUMP_CELLS && abs(d16(fp.wreck[2], fc.wreck[2])) <= JUMP_Z;
        count(ENH_OBJ_WRECK, m_wreck, imax(dx, dy));
    }
    m_spin = true;                                                    /* the spinners: always forward (r3d_end_scene adds to them, never back) */
    count(ENH_OBJ_SPIN, true, (u16)(fc.spin[0] - fp.spin[0]));
    if (fc.view == 0x84) {                                            /* the delayed view's pose: the slot it reads moved on by one step, the delay kept */
        const long dx = labs((long)rl(fc.ring, 0) - rl(fp.ring, 0)), dy = labs((long)rl(fc.ring, 4) - rl(fp.ring, 4));
        m_ring = fp.ring_n == fc.ring_n && (u16)(fc.ring_ctr - fp.ring_ctr) == 1 && dx <= JUMP_POS && dy <= JUMP_POS
              && abs(d16(rw(fp.ring, 8), rw(fc.ring, 8))) <= JUMP_ALT && angs_ok(fp.ring, fc.ring, 10);
        count(ENH_OBJ_RING, m_ring, (int)(dx > dy ? dx : dy));
    }
    const int df = abs(d16(fp.fpm, fc.fpm));
    m_fpm = df <= JUMP_FPM;
    count(ENH_OBJ_FPM, m_fpm, df);
}

void enh_interp_reset(void) { fp.valid = fc.valid = false; lerp_ok = false; hold_why = "start"; objects_match(); }

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
    objects_match();
}

bool enh_interp_lerp(void) { return lerp_ok; }
/* S4.6: a unit's / projectile's map cells (+0 / +2 of its record) at t, unrounded (the map MFD moves its blip by sub-pixels); false = not interpolated
 * (then *x / *y = F_k's). kx / ky: F_k's cells (the step plotted the blip there) */
bool enh_interp_cells(int cls, int slot, double t, double *x, double *y, u16 *kx, u16 *ky)
{
    const u8 *a, *b;
    bool m;
    if (cls == ENH_OBJ_UNIT && slot >= 0 && slot < OBJ_UNITS && slot < fc.n_units) { a = fp.unit[slot]; b = fc.unit[slot]; m = m_unit[slot]; }
    else if (cls == ENH_OBJ_PROJ && slot >= 0 && slot < OBJ_PROJ) { a = fp.proj[slot]; b = fc.proj[slot]; m = m_proj[slot]; }
    else return false;
    *kx = rw(b, 0); *ky = rw(b, 2);
    if (!m || t >= 1.0) { *x = *kx; *y = *ky; return m; }
    if (t < 0) t = 0;
    *x = rw(a, 0) + d16(rw(a, 0), rw(b, 0)) * t;
    *y = rw(a, 2) + d16(rw(a, 2), rw(b, 2)) * t;
    return true;
}

const char *enh_interp_hold_reason(void) { return hold_why; }

static s32 lerp_l(s32 a, s32 b, double t) { return a + (s32)lround((double)((long long)b - a) * t); }
static u16 lerp_a(u16 a, u16 b, double t) { return (u16)(a + (int)lround(ang_d(a, b) * t)); }   /* also every 16-bit position: the signed move */
static u16 lerp_fwd(u16 a, u16 b, double t) { return (u16)(a + (u16)lround((double)(u16)(b - a) * t)); }   /* a counter that only goes up */

/* the matrix sim_matrix_from_angles builds for the angles, as fm_mat_from_euler calls it (fl_flight.c) */
static void mat_of(s16 *m, u16 h, u16 p, u16 r)
{
    sim_matrix_from_angles(0x2D94, h, (s16)p, r);
    for (int i = 0; i < 9; i++) m[i] = (s16)DSW((u16)(0x2D94 + 2 * i));
}

/* S4.2: a 16-bit field (offset o of the records a / b) at t, written to DS:dst */
static void put_w(u16 dst, const u8 *a, const u8 *b, int o, double t) { DSW(dst) = lerp_a(rw(a, o), rw(b, o), t); }
static void put_l(u16 dst, const u8 *a, const u8 *b, int o, double t) { DSL(dst) = (u32)lerp_l(rl(a, o), rl(b, o), t); }

static void objects_apply(double t)
{
    for (int i = 0; i < OBJ_UNITS; i++) {
        if (!m_unit[i]) continue;
        const u8 *a = fp.unit[i], *b = fc.unit[i];
        const u16 r = (u16)(0xC140 + 0x24 * i);
        put_w(r, a, b, 0, t); put_w((u16)(r + 2), a, b, 2, t); put_w((u16)(r + 4), a, b, 4, t);   /* the cells (projection, culling) and the height */
        put_l((u16)(r + 6), a, b, 6, t); put_l((u16)(r + 0xA), a, b, 0xA, t);                     /* the world position the model is drawn at */
        for (int c = 0; c < 3; c++) put_w((u16)(r + 0xE + 2 * c), a, b, 0xE + 2 * c, t);          /* heading, pitch, roll */
    }
    for (int i = 0; i < OBJ_PROJ; i++) {
        if (!m_proj[i]) continue;
        const u8 *a = fp.proj[i], *b = fc.proj[i];
        const u16 r = (u16)(0x3C0C + 0x1C * i);
        for (int c = 0; c < 3; c++) put_w((u16)(r + 2 * c), a, b, 2 * c, t);
        for (int c = 0; c < 3; c++) put_w((u16)(r + 8 + 2 * c), a, b, 8 + 2 * c, t);
    }
    for (int i = 0; i < OBJ_TRACERS; i++) {
        if (!m_tr[i]) continue;
        const u8 *a = fp.tr[i];
        const u16 r = (u16)(0xE44E + 12 * i);
        for (int c = 0; c < 3; c++) DSW((u16)(r + 2 * c)) = (u16)(rw(a, 2 * c) + (int)lround((s16)rw(a, 6 + 2 * c) * t));   /* along its own velocity */
    }
    for (int i = 0; i < OBJ_SMOKE; i++)
        if (m_smoke[i]) put_w((u16)(0x39DA + 8 * i + 4), fp.smoke[i], fc.smoke[i], 4, t);
    if (m_wreck) {
        DSW(0xC0A6) = lerp_a(fp.wreck[0], fc.wreck[0], t);
        DSW(0xC0B8) = lerp_a(fp.wreck[1], fc.wreck[1], t);
        DSW(0xC602) = lerp_a(fp.wreck[2], fc.wreck[2], t);
    }
    if (m_spin) { DSW(0xC04C) = lerp_fwd(fp.spin[0], fc.spin[0], t); DSW(0xC04E) = lerp_fwd(fp.spin[1], fc.spin[1], t); }
    if (m_ring) {                                                     /* the slot view_frame reads: the pose between the two it read in frames k-1 and k */
        const u16 r = (u16)(0xB056 + 16 * fc.ring_i);
        put_l(r, fp.ring, fc.ring, 0, t); put_l((u16)(r + 4), fp.ring, fc.ring, 4, t);
        for (int c = 0; c < 4; c++) put_w((u16)(r + 8 + 2 * c), fp.ring, fc.ring, 8 + 2 * c, t);
    }
    if (m_fpm) DSW(0x98C6) = lerp_a(fp.fpm, fc.fpm, t);
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
    objects_apply(t);                                                 /* S4.2 */
}

void enh_interp_trace(FILE *f, unsigned long long frame, int pass, double alpha, double ms)
{
    if (!f) return;
    fprintf(f, "%llu %d %.4f %.3f %d %s view=%x cam=%d,%d,%d ang=%u,%u,%u own=%d,%d,%u hdg=%u\n", frame, pass, alpha, ms, lerp_ok ? 1 : 0,
            lerp_ok ? "-" : hold_why, DSW(0xC07A), (s32)DSL(0xB288), (s32)DSL(0xB752), DSW(0xB75C), DSW(0xE44C), DSW(0xC07C), DSW(0xB770),
            (s32)DSL(0xBA10), (s32)DSL(0xC02A), DSW(0x2DF4), DSW(0x2DEE));
    /* S4.2: per interpolating object the values this refresh shows (computed as objects_apply writes them; alpha 1 = F_k) and both ends:
     * "o <frame> <pass> <class> <slot> v=x,y,z,a a=... b=..." with x / y / z the position and a the heading (or the one value of a 1-component class) */
    if (!lerp_ok) return;
    const double t = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    static const char *const name[ENH_OBJ_CLASSES] = { "unit", "proj", "tracer", "smoke", "wreck", "spin", "ring", "fpm" };
#define ROW(cls, slot, A, B, V) fprintf(f, "o %llu %d %s %d v=%d,%d,%d,%d a=%d,%d,%d,%d b=%d,%d,%d,%d\n", frame, pass, name[cls], slot, \
                                        V[0], V[1], V[2], V[3], A[0], A[1], A[2], A[3], B[0], B[1], B[2], B[3])
    for (int i = 0; i < OBJ_UNITS; i++) {
        if (!m_unit[i]) continue;
        const u8 *a = fp.unit[i], *b = fc.unit[i];
        const int A[4] = { rl(a, 6), rl(a, 0xA), (s16)rw(a, 4), rw(a, 0xE) }, B[4] = { rl(b, 6), rl(b, 0xA), (s16)rw(b, 4), rw(b, 0xE) };
        const int V[4] = { lerp_l(A[0], B[0], t), lerp_l(A[1], B[1], t), (s16)lerp_a((u16)A[2], (u16)B[2], t), lerp_a((u16)A[3], (u16)B[3], t) };
        ROW(ENH_OBJ_UNIT, i, A, B, V);
    }
    for (int i = 0; i < OBJ_PROJ; i++) {
        if (!m_proj[i]) continue;
        const u8 *a = fp.proj[i], *b = fc.proj[i];
        const int A[4] = { rw(a, 0), rw(a, 2), rw(a, 4), rw(a, 8) }, B[4] = { rw(b, 0), rw(b, 2), rw(b, 4), rw(b, 8) };
        const int V[4] = { lerp_a((u16)A[0], (u16)B[0], t), lerp_a((u16)A[1], (u16)B[1], t), lerp_a((u16)A[2], (u16)B[2], t), lerp_a((u16)A[3], (u16)B[3], t) };
        ROW(ENH_OBJ_PROJ, i, A, B, V);
    }
    for (int i = 0; i < OBJ_TRACERS; i++) {
        if (!m_tr[i]) continue;
        const u8 *a = fp.tr[i], *b = fc.tr[i];
        const int A[4] = { rw(a, 0), rw(a, 2), rw(a, 4), 0 }, B[4] = { rw(b, 0), rw(b, 2), rw(b, 4), 0 };
        int V[4] = { 0, 0, 0, 0 };
        for (int c = 0; c < 3; c++) V[c] = (u16)(rw(a, 2 * c) + (int)lround((s16)rw(a, 6 + 2 * c) * t));
        ROW(ENH_OBJ_TRACER, i, A, B, V);
    }
    for (int i = 0; i < OBJ_SMOKE; i++) {
        if (!m_smoke[i]) continue;
        const int A[4] = { rw(fp.smoke[i], 4), 0, 0, 0 }, B[4] = { rw(fc.smoke[i], 4), 0, 0, 0 };
        const int V[4] = { lerp_a((u16)A[0], (u16)B[0], t), 0, 0, 0 };
        ROW(ENH_OBJ_SMOKE, i, A, B, V);
    }
    if (m_wreck) {
        const int A[4] = { fp.wreck[0], fp.wreck[1], fp.wreck[2], 0 }, B[4] = { fc.wreck[0], fc.wreck[1], fc.wreck[2], 0 };
        const int V[4] = { lerp_a(fp.wreck[0], fc.wreck[0], t), lerp_a(fp.wreck[1], fc.wreck[1], t), lerp_a(fp.wreck[2], fc.wreck[2], t), 0 };
        ROW(ENH_OBJ_WRECK, 0, A, B, V);
    }
    if (m_spin) {   /* the counters unwrapped: b = a + the forward move */
        const int A[4] = { fp.spin[0], fp.spin[1], 0, 0 }, B[4] = { fp.spin[0] + (u16)(fc.spin[0] - fp.spin[0]), fp.spin[1] + (u16)(fc.spin[1] - fp.spin[1]), 0, 0 };
        const int V[4] = { fp.spin[0] + (u16)(lerp_fwd(fp.spin[0], fc.spin[0], t) - fp.spin[0]), fp.spin[1] + (u16)(lerp_fwd(fp.spin[1], fc.spin[1], t) - fp.spin[1]), 0, 0 };
        ROW(ENH_OBJ_SPIN, 0, A, B, V);
    }
    if (m_ring) {
        const int A[4] = { rl(fp.ring, 0), rl(fp.ring, 4), rw(fp.ring, 8), rw(fp.ring, 10) }, B[4] = { rl(fc.ring, 0), rl(fc.ring, 4), rw(fc.ring, 8), rw(fc.ring, 10) };
        const int V[4] = { lerp_l(A[0], B[0], t), lerp_l(A[1], B[1], t), lerp_a((u16)A[2], (u16)B[2], t), lerp_a((u16)A[3], (u16)B[3], t) };
        ROW(ENH_OBJ_RING, fc.ring_i, A, B, V);
    }
    if (m_fpm) {
        const int A[4] = { (s16)fp.fpm, 0, 0, 0 }, B[4] = { (s16)fc.fpm, 0, 0, 0 }, V[4] = { (s16)lerp_a(fp.fpm, fc.fpm, t), 0, 0, 0 };
        ROW(ENH_OBJ_FPM, 0, A, B, V);
    }
#undef ROW
}

void enh_interp_stats(FILE *f)
{
    static const char *const name[ENH_OBJ_CLASSES] = { "units", "projectiles", "tracers", "smoke", "wreck", "spinners", "pose-ring", "fpm" };
    fprintf(f, "display-objects: per frame, interpolated / held (largest move a step):");
    for (int c = 0; c < ENH_OBJ_CLASSES; c++) fprintf(f, " %s %llu/%llu (%d)", name[c], st_lerp[c], st_held[c], max_move[c]);
    fprintf(f, "\n");
}

/* --interp-test (S4.2): the object rules on synthetic records in DGROUP (no game files): matched objects step evenly from F_{k-1} to F_k and land on F_k,
 * a type change, a freed slot, a jump or a new tracer round is not interpolated, spinners go forward over the wrap, the delayed view's slot moves. */
int enh_interp_selftest(void)
{
    int bad = 0;
#define CHECK(c, ...) do { if (!(c)) { if (++bad < 20) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)
    const u16 dg = cur_dgroup;
    cur_dgroup = 0x1000;
    memset(mp(cur_dgroup, 0), 0, 0x10000);
    enh_interp_reset();
    /* frame k-1 */
    DSW(0xDED0) = 4; DSW(0xB16C) = 3;
    for (int i = 0; i < 4; i++) {
        const u16 r = (u16)(0xC140 + 0x24 * i);
        DSB((u16)(r + 0x16)) = 2; DSW((u16)(r + 0x14)) = 1;
        DSL((u16)(r + 6)) = 100000; DSL((u16)(r + 0xA)) = 200000; DSW((u16)(r + 4)) = 0x300; DSW((u16)(r + 0xE)) = 0xFF00; DSW(r) = 3125; DSW((u16)(r + 2)) = 6250;
    }
    DSW(0x3C0C + 0xE) = 1; DSW(0x3C0C + 0x10) = 7; DSW(0x3C0C) = 1000; DSW(0x3C0C + 2) = 2000; DSW(0x3C0C + 4) = 500; DSW(0x3C0C + 8) = 0x7F00;
    DSW(0xE44E) = 900; DSW(0xE44E + 2) = 900; DSW(0xE44E + 4) = 100; DSW(0xE44E + 6) = 40; DSW(0xE44E + 8) = (u16)-20; DSW(0xE44E + 10) = 8;   /* round 0 */
    DSW(0xE45A) = 500; DSW(0xE45A + 6) = 10;                                                                                                    /* round 1 */
    DSW(0x39DA) = 77; DSW(0x39DA + 2) = 88; DSW(0x39DA + 4) = 0x100;
    DSW(0xC602) = 0x400; DSW(0xC0A6) = 50; DSW(0xC0B8) = 60;
    DSW(0xC04C) = 0xF000; DSW(0xC04E) = 0x1000;
    DSW(0xC07A) = 0x84; DSW(0x3660) = 3; DSW(0x3D60) = 20;
    DSL(0xB056 + 16 * 1) = 5000; DSW(0xB056 + 16 * 1 + 10) = 0xFFF0;       /* slot (20 - 3) & 15 = 1 */
    DSL(0xB056 + 16 * 2) = 5300; DSW(0xB056 + 16 * 2 + 10) = 0x0030;       /* the slot read in frame k */
    DSW(0x98C6) = (u16)-100;
    enh_interp_frame();
    /* frame k */
    for (int i = 0; i < 4; i++) {
        const u16 r = (u16)(0xC140 + 0x24 * i);
        DSL((u16)(r + 6)) = 100400; DSL((u16)(r + 0xA)) = 199600; DSW((u16)(r + 4)) = 0x340; DSW((u16)(r + 0xE)) = 0x0100; DSW(r) = 3137; DSW((u16)(r + 2)) = 6237;
    }
    DSW(0xC140 + 0x24 * 1 + 0x14) = 2;                                     /* unit 1: another type in the slot */
    DSB(0xC140 + 0x24 * 2 + 0x16) = 0;                                     /* unit 2: freed */
    DSL(0xC140 + 0x24 * 3 + 6) = 150000;                                   /* unit 3: a jump */
    DSW(0x3C0C) = 1060; DSW(0x3C0C + 2) = 1940; DSW(0x3C0C + 4) = 520; DSW(0x3C0C + 8) = 0x8100;
    DSW(0xE44E) = 940; DSW(0xE44E + 2) = 880; DSW(0xE44E + 4) = 108;       /* round 0 moved by its velocity */
    DSW(0xE45A) = 1200;                                                    /* round 1: a new round in the slot */
    DSW(0x39DA + 4) = 0x140;
    DSW(0xC602) = 0x380; DSW(0xC0A6) = 54; DSW(0xC0B8) = 58;
    DSW(0xC04C) = 0x0E00; DSW(0xC04E) = 0x2800;                            /* C04C wraps */
    DSW(0x3D60) = 21;
    DSW(0x98C6) = (u16)-60;
    enh_interp_frame();
    CHECK(enh_interp_lerp(), "frame k holds (%s)", enh_interp_hold_reason());
    CHECK(m_unit[0] && !m_unit[1] && !m_unit[2] && !m_unit[3], "unit match %d %d %d %d", m_unit[0], m_unit[1], m_unit[2], m_unit[3]);
    CHECK(m_proj[0] && !m_proj[1], "projectile match");
    CHECK(m_tr[0] && !m_tr[1], "tracer match %d %d", m_tr[0], m_tr[1]);
    CHECK(m_smoke[0] && m_wreck && m_spin && m_ring && m_fpm, "smoke / wreck / spin / ring / fpm match %d %d %d %d %d", m_smoke[0], m_wreck, m_spin, m_ring, m_fpm);
    /* the steps over K = 4 refreshes from F_{k-1}: even and forward; alpha 1 = F_k */
    static u8 snap[0x10000];
    memcpy(snap, mp(cur_dgroup, 0), sizeof snap);
    s32 last_x = 100000, last_hdg = -0x100, last_spin = 0xF000, last_ring = 5000;
    int last_px = 1000, last_ang = 0x7F00, last_tr = 900, last_z = 0x100, last_wz = 0x400, last_fpm = -100;
    for (int j = 1; j <= 4; j++) {
        const double t = j / 4.0;
        memcpy(mp(cur_dgroup, 0), snap, 0x10000);                         /* A_k, then the fields at t (as a pass does on its copy of A_k) */
        if (j < 4) enh_interp_apply(t);
        const s32 x = (s32)DSL(0xC140 + 6);
        const s32 hd = (s16)DSW(0xC140 + 0xE);
        CHECK(x - last_x == 100, "unit x step %d at %.2f", x - last_x, t);
        CHECK(hd - last_hdg == 0x80, "unit heading step %d at %.2f (shortest arc)", hd - last_hdg, t);
        CHECK(DSL(0xC140 + 0x24 + 6) == 100400 && DSL(0xC140 + 0x24 * 3 + 6) == 150000, "a held unit moved");
        const int px = DSW(0x3C0C), ang = DSW(0x3C0C + 8);
        CHECK(px - last_px == 15 && ang - last_ang == 0x80, "projectile steps %d %d", px - last_px, ang - last_ang);
        CHECK(DSW(0xE44E) - last_tr == 10 && DSW(0xE45A) == 1200, "tracer step %d, new round %u", DSW(0xE44E) - last_tr, DSW(0xE45A));
        CHECK(DSW(0x39DA + 4) - last_z == 0x10, "smoke step");
        CHECK(last_wz - (s16)DSW(0xC602) == 0x20, "wreck step");
        const s32 sp = (s32)(u16)(DSW(0xC04C) - 0xF000) + 0xF000;
        CHECK(sp - last_spin == 0x780, "spinner step %d (forward over the wrap)", sp - last_spin);
        CHECK((s32)DSL(0xB056 + 32) - last_ring == 75, "pose ring step %d", (s32)DSL(0xB056 + 32) - last_ring);
        CHECK((s16)DSW(0x98C6) - last_fpm == 10, "fpm step");
        last_x = x; last_hdg = hd; last_px = px; last_ang = ang; last_tr = DSW(0xE44E); last_z = DSW(0x39DA + 4); last_wz = (s16)DSW(0xC602); last_spin = sp;
        last_ring = (s32)DSL(0xB056 + 32); last_fpm = (s16)DSW(0x98C6);
    }
    /* a view change holds everything */
    DSW(0xC07A) = 0; DSW(0x3D60) = 22;
    enh_interp_frame();
    CHECK(!enh_interp_lerp() && !m_unit[0] && !m_proj[0] && !m_spin, "a view change does not hold the objects");
    memset(mp(cur_dgroup, 0), 0, 0x10000);
    cur_dgroup = dg;
    enh_interp_reset();
    memset(st_lerp, 0, sizeof st_lerp); memset(st_held, 0, sizeof st_held); memset(max_move, 0, sizeof max_move);
    printf(bad ? "interp-test FAILED (%d)\n" : "interp-test OK: units, projectiles, tracers, smoke, wreck, spinners, pose ring, fpm: matched / held / even steps\n", bad);
#undef CHECK
    return bad ? 1 : 0;
}
