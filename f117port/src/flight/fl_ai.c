/* The enemy aircraft (owner ai, spec port/spec/flight.md section 4, simflow.md Part 5 section 2): 5730 ai_update_units (target selection: hunt / RTB and land /
 * patrol and formation, the combat table DS:3A8C, the cannon, the terrain pull-up, the integration, landing and roll-out, endurance, the map blips, the radar
 * schedule, the scramble of parked units), 62D0 ai_unit_radar and 65D4 ai_unit_takeoff. Translated from port/notes/s38/s38_ref.py (0 mismatches). */
#include "flight/fl.h"
#include "enhanced/mapmfd.h"   /* ENH: S4.6 */

#define S(o) ((s16)DSW(o))
#define U(o) DSW(o)
#define SETW(o, v) (DSW(o) = (u16)(v))

/* 0000:62D0 ai_unit_radar(u): the unit's radar on the player, "Visual ID", the air-to-air missile launch, the lock tone */
void ai_unit_radar(int u)
{
    static int brg_i, dist_i;     /* PORT: for radar type 0 / -1 5460 leaves the original's stack locals unwritten (Q-A1); the port keeps the previous call's values */
    u16 un = (u16)(UNIT_BASE + 0x24 * u), a = (u16)(AC_BASE + 32 * S(0x94F0));
    s16 rt = S(a + 0x18);
    s16 sig = (s16)det_radar_signal(U(un + 2), U(un + 4), U(un + 6), rt, &brg_i, &dist_i);
    u16 brg = (u16)brg_i;
    s16 D = (s16)dist_i;
    SETW(0x3D6C, 0x2A);
    s16 sg = S(0x3D5C);
    u16 R = (u16)(0x2E80 + 14 * rt);
    if (sig > 0) {
        SETW(0xC070, (s32)(s16)(sig + sg) * D / 100);
        if (!(DSB(R + 12) & 2)) {
            s16 hh = fl_sx8(U(un + 0x10) >> 8);
            SETW(0xC13A, hh - 0x30);
            SETW(0xC13C, hh + 0x30);
            if (fl_sx8((u16)fl_abs16((s16)(brg - U(un + 0x10))) >> 8) > 0x30) sig = 0;
        }
        if (DSB(un + 0x18) & 8) {
            s16 hb = fl_sx8(brg >> 8);
            SETW(0xC13A, hb - 0x20);
            SETW(0xC13C, hb + 0x20);
        }
        int c = (s16)(sig + sg) > 100 ? 0x0F : 0x0B;
        SETW(0x3D6C, c);
        hud_threat_bar(sig < 99 ? sig : 99, c, 6, 9);
        SETW(0x3D6A, U(0x3660));
        SETW(0xC07E, -1 - u);
        SETW(0x9872, c);
        SETW(0xE54C, DSB(R + 12) & 1);
    }
    if ((s16)(sig + sg) > 100) {
        det_radar_warning();
        int inc = ((S(0x3656) + S(0x3654) + 2) << 4) >> ((DSB(0x9B04) & 0x10) ? 1 : 0);
        SETW(un + 0x22, U(un + 0x22) + inc);
        if (S(un + 0x22) > 0xC0) {
            SETW(0xDED6, U(0xDED6) + 1);
            DSB(un + 0x19) |= 0x40;
            det_alert_network();
            if (!(DSB(un + 0x19) & 8) && (8 >> S(0x3658)) > D) {
                DSB(un + 0x19) |= 8;
                SETW(0xDECE, U(0xDECE) + 1);
                fl_strcpy(0x9876, 0x3E17);
                fl_strcat(0x9876, a);
                msg_post(ds_str(0x9876));
                snd_cue(7, 1);
                mission_log_event(6, S(0x94F0));
            }
            u16 m = (u16)(PROJ_BASE + 0x1C * (u & 7));
            s16 w = S(un + 0x1E);
            u16 wp = (u16)(WPN_BASE + 0x12 * w);
            if (2 * S(0x3654) >= S(0xB054) && U(m + 0xE) == 0 && fl_abs16((s16)(brg - U(un + 0x10))) < 0x1800 && S(wp + 8) > D &&
                (4 - S(0x3654)) * 2 < D && w != 0) {
                SETW(m, U(un + 2)); SETW(m + 2, U(un + 4)); SETW(m + 4, U(un + 6) - 0x19);
                SETW(m + 6, S(wp + 10) >> 6);
                SETW(m + 8, U(un + 0x10)); SETW(m + 0xA, U(un + 0x12)); SETW(m + 0xC, U(un + 0x14));
                SETW(m + 0xE, ((s32)(S(wp + 8) * S(0x3660)) << 5) / S(m + 6));
                SETW(m + 0x10, w); SETW(m + 0x16, -u);
                fl_strcpy(0x9876, wp);
                fl_strcat(0x9876, 0x3E24);
                fl_strcat(0x9876, a);
                msg_post(ds_str(0x9876));
                fl_comm_add(0x2C, 1);
            }
        }
        if (!(DSB(un + 0x18) & 8)) snd_cue(7, 1);
        DSB(un + 0x18) |= 8;
    } else {
        DSB(un + 0x18) &= (u8)~8;
        SETW(un + 0x22, U(un + 0x22) - 0x20);
    }
    SETW(un + 0x22, fx_clamp_wrap(S(un + 0x22), 0, 0xFF));
}

/* 0000:65D4 ai_unit_takeoff(u, site): the unit on the runway (or the carrier deck) of `site`, rolling, with its endurance */
void ai_unit_takeoff(int u, int site)
{
    u16 un = (u16)(UNIT_BASE + 0x24 * u), a = (u16)(AC_BASE + 32 * S(un + 0x16)), r = (u16)(SITE_BASE + 16 * site);
    s16 d = S(0xB16E);
    SETW(un + 0x10, d == 1 ? 0 : 0x8000);
    if (DSB(r + 9) & 2) {
        SETW(un + 2, U(r + 2) + 3 * d); SETW(un + 4, -(12 * d - U(r + 4))); SETW(un + 6, 0x8C); SETW(un + 0x1A, 0x64);
        SETW(un + 0x10, U(un + 0x10) - 0x400);
    } else {
        SETW(un + 2, U(r + 2)); SETW(un + 4, U(r + 4) + 30 * d); SETW(un + 6, 0x0C); SETW(un + 0x1A, 0x0A);
    }
    DSL(un + 8) = (u32)U(un + 2) << 5;
    DSL(un + 0xC) = (u32)U(un + 4) << 5;
    SETW(un + 0x12, 0); SETW(un + 0x14, 0);
    SETW(un + 0x18, U(un + 0x18) | 0x403);
    SETW(un, site);
    SETW(un + 0x1C, ((s32)(S(a + 0x14) * S(0x3660)) << 11) / S(a + 0x12));
    if (S(0x3D6E) == -1) DSB(un + 0x19) &= (u8)~1;
    msg_site_label(site);
    fl_strcat(0x9876, 0x3E2F);
    fl_strcat(0x9876, a);
    fl_strcat(0x9876, 0x3E33);
    if (u < S(0xDED0) - 4) msg_post(ds_str(0x9876));
}

/* the scramble of a parked unit (5730, flight.md 4.3) */
static void scramble(int u, u16 un, u16 a)
{
    int cl = (S(0x3656) + 4) & 0x1F;
    if ((((S(0x98E2) >> cl) ^ u) & 7) != 0) return;
    if (!(S(0xDED0) - 4 > u) || u == 0) return;
    int r = fl_rand_n(U(0xE540));
    if (!(S(0x3D62) > S(0x9F64) || (DSB(un + 0x18) & 0x80))) return;
    u16 sr = (u16)(SITE_BASE + 16 * r);
    if ((U(sr + 8) & 0x181) != 1) return;
    if (U(un + 0x16) != U(sr + 10)) return;
    s16 D = (s16)((s16)fx_dist((s16)(U(0xB2A0) - U(sr + 2)), (s16)(U(0xB75A) - U(sr + 4))) >> 6);
    if (U(a + 0x14) > (u16)D) ai_unit_takeoff(u, r);
}

/* steering, guns, integration of one unit towards (tx, ty, tz) in `mode` (flight.md 4.2) */
static void steer(int u, u16 un, u16 a, int mode, u16 tx, u16 ty, u16 tz)
{
    u16 st = U(0x3D60);
    s16 rate = fl_rate(), sk = S(0x3654);
    if (mode == 3 && (DSB(un + 0x18) & 8)) { tx = U(0xC0A4); ty = U(0xC0B2); tz = U(un + 6); }
    s16 dx = (s16)(tx - U(un + 2)), dy = (s16)(ty - U(un + 4));
    u16 brg = fx_heading(dx, (s16)-dy);
    s16 D = (s16)fx_dist(dx, dy);
    s16 pt = (s16)fx_clamp_wrap((s16)fx_heading((s16)(tz - U(un + 6)) >> 5, D), (s16)0xE000, 0x1000);
    s16 bt;
    if (mode == 1 && D < 0x400) {                                         /* dogfight: the cannon and the combat table */
        SETW(0x94F2, U(0x94F2) + 1);
        if (D < 0x200 && !(st & 3) && S(0x3D62) > S(0x9F64) && fl_abs16((s16)(U(un + 0x10) - brg)) < 0x400 && fl_abs16((s16)(U(un + 0x12) - pt)) < 0x400) {
            u16 t = (u16)(TRACER_BASE + 12 * (s16)(((st & 0xC) >> 2) + U(0xB16C)));
            int sp = 0xE0 / rate;
            SETW(t + 10, (s16)fx_sin_mul((u16)-S(un + 0x12), sp) << 5);   /* Q-A3: the vertical velocity of -pitch */
            s16 h = (s16)fx_cos_mul(U(un + 0x12), sp);
            SETW(t + 6, fx_sin_mul(U(un + 0x10), h));
            SETW(t + 8, -fx_cos_mul(U(un + 0x10), h));
            SETW(t, U(un + 2)); SETW(t + 2, U(un + 4)); SETW(t + 4, U(un + 6));
        }
        int row = fx_clamp_wrap((u & 3) + sk, 0, 2);
        if (u == 0) row = 1;
        u16 h = U(un + 0x10);
        if (fl_abs16(S(un + 0x14)) < 0x4000) h = (u16)(h + (S(un + 0x14) >> 2));
        int b = ((s16)(brg - h) >> 13) & 7;
        u16 h2 = U(0x2DEE);
        if (fl_abs16(S(0x2DF2)) < 0x4000) h2 = (u16)(h2 + (S(0x2DF2) >> 1));
        int ai = (((s16)(U(un + 0x10) - h2) >> 13) + 4) & 7;
        s16 cmd = S((u16)(0x3A8C + 2 * ((row * 8 + ai) * 8 + b)));
        bt = (s16)(cmd << 12);
        int hi3d61 = DSB(0x3D61) & 8;
        if (cmd == 0x100) { pt = 0x6000; bt = hi3d61 ? 0x4000 : (s16)0xC000; }
        if (cmd == 0x200) { pt = (s16)0xA000; bt = hi3d61 ? (s16)0xC000 : 0x4000; }
        if (pt == (s16)0xA000) {
            s16 p = S(un + 0x12);
            if ((s16)(3000 - (p >> 3)) > S(un + 6)) pt = (s16)(p + 0x1000);
        }
        if (fl_abs16(S(un + 0x14)) > 0x4000) { bt = 0; pt = 0; }
    } else {
        bt = (s16)(fx_clamp_wrap((s16)(brg - U(un + 0x10)), (s16)0xE000, 0x2000) << 1);
        if (mode == 1 && 2 * sk < S(0xB054)) bt = 0x3000;
    }
    if (mode == 1 && (DSB((u16)(SITE_BASE + 16 * S(0xDFE0) + 9)) & 4) && S(0x98F2) < 0x780) bt = 0x3000;
    s16 ag = S(a + 0x16);
    s16 lim = (s16)(ag << 12);
    bt = (s16)fx_clamp_wrap(bt, (s16)-lim, lim);
    s16 lim2 = (s16)((ag & 0xFF) << 8);
    s16 rb = (s16)fx_clamp_wrap((s16)(bt - U(un + 0x14)), (s16)-lim2, lim2);
    if (DSB(un + 0x19) & 4) {                                             /* taking off */
        if (S(un + 0x1A) < 0x96) SETW(un + 0x12, 0);
        else DSB(un + 0x13) += 1;
        rb = 0;
        if (S(un + 0x1A) < S(a + 0x12)) SETW(un + 0x1A, U(un + 0x1A) + 0x28 / rate);
        else if (S(un + 6) > 0x12C) DSB(un + 0x19) &= (u8)~4;
    }
    if (DSB(un + 0x18) & 0x30) rb = 0x400;
    if (((u ^ st) & 3) == 0) {                                            /* terrain probe every 4th step */
        terr_probe_cell(S(un + 2), S(un + 4), S(un + 6));
        if (DSB(0xC686)) DSB(un + 0x19) |= 0x20;
        else DSB(un + 0x19) &= (u8)~0x20;
    }
    if (DSB(un + 0x19) & 0x20) pt = 0x3000;
    SETW(un + 0x14, U(un + 0x14) + (s16)(rb << 2) / rate);
    SETW(un + 0x10, U(un + 0x10) + (S(un + 0x14) >> 3) / rate);
    s16 dp = (s16)(pt - U(un + 0x12));
    if (DSB(un + 0x18) & 0x20) {                                          /* shot down: falling, smoke */
        dp = (s16)0xFE00;
        if (!(st & 1)) {
            int j = (st & 0x1E) >> 1;
            u16 pp = (u16)(0x39DA + 8 * j);
            SETW(pp, U(un + 2)); SETW(pp + 2, U(un + 4)); SETW(pp + 4, U(un + 6));
            SETW(0x3A5A, j);
            SETW(pp + 6, 0);
            for (int n = 0; n < 16; n++)
                if (n != j && U(0x39DA + 8 * n) != 0) SETW(0x39DA + 8 * n + 6, U(0x39DA + 8 * n + 6) + 1);
        }
    }
    if (S(un + 0x12) < 0 && S(un + 6) < (s16)(200 - fx_sin_mul(U(un + 0x12), 2000)) && !(U(un + 0x18) & 0x220)) dp = 0x400;
    dp = (s16)fx_clamp_wrap(dp, (s16)0xFC00, 0x400);
    SETW(un + 0x12, U(un + 0x12) + (s16)(dp << 2) / rate);
    if (fl_abs16(S(un + 0x12)) > 0x4000) {
        DSB(un + 0x11) += 0x80;
        DSB(un + 0x15) += 0x80;
        SETW(un + 0x12, 0x8000 - U(un + 0x12));
    }
    DSB(un + 0x18) &= (u8)~0x10;
    s16 v = (s16)(((s32)S(un + 0x1A) * (u16)(0x8000 - U(un + 0x12))) >> 15);
    s16 w = (s16)(v - (fl_abs16((s16)fx_sin_mul(U(un + 0x14), v)) >> 1));
    if (mode == 1 && sk >= S(0xB054)) w = (s16)(w + (w >> 1));
    w = (s16)(((s16)(w << 2) / rate) >> 2);
    s16 h = (s16)fx_cos_mul(U(un + 0x12), w);
    DSL(un + 8) = (u32)((s32)DSL(un + 8) + fx_sin_mul(U(un + 0x10), h));
    DSL(un + 0xC) = (u32)((s32)DSL(un + 0xC) - fx_cos_mul(U(un + 0x10), h));
    SETW(un + 6, U(un + 6) + fx_sin_mul(U(un + 0x12), w));
    SETW(un + 2, (s32)DSL(un + 8) >> 5);
    SETW(un + 4, (s32)DSL(un + 0xC) >> 5);
    if (S(un + 6) > 30000) SETW(un + 0x12, 0);
    if (S(un + 6) < 0) {                                                  /* hit the ground */
        SETW(un + 0x18, U(un + 0x18) & (u >= 1 ? 0x1C1 : 0));
        SETW(0xC0A0, U(un + 2)); SETW(0xC0AA, U(un + 4)); SETW(0xC0B0, U(un + 6)); SETW(0x9D0C, 0xFFF0);
        if (S(0x3D64) == u) SETW(0x3D64, 0xFFFF);
    }
    if (D < 0x10 && mode == 2) {                                          /* over the base: final approach, then touch-down */
        msg_site_label(U(un));
        fl_strcat(0x9876, 0x3E00);
        fl_strcat(0x9876, a);
        if (DSB(un + 0x19) & 2) { DSB(un + 0x19) |= 0x10; fl_strcat(0x9876, 0x3E04); }
        else { DSB(un + 0x19) |= 2; fl_strcat(0x9876, 0x3E0D); }
        if (S(0xDED0) - 4 > u) msg_post(ds_str(0x9876));
    }
    if (DSB(un + 0x19) & 0x10) {                                          /* rolling out (Q-A5: the deck height of the player's airfield) */
        SETW(un + 0x12, 0); SETW(un + 0x14, 0);
        SETW(un + 0x10, S(0xB16E) == 1 ? 0 : 0x8000);
        SETW(un + 6, (DSB((u16)(SITE_BASE + 16 * S(0xDFE0) + 9)) & 2) ? 0x8C : 0x0C);
        if (S(un + 0x1A) > 0) SETW(un + 0x1A, U(un + 0x1A) - 0x3C / rate);
        else {
            SETW(un + 0x18, U(un + 0x18) & 0x1C1);
            if (u == 0 && S(0xE2D8) >= 5) SETW(un + 0x18, 0);
        }
        if (!(S(0xDED0) - 4 > u) && S(un + 0x1A) < 0x64) SETW(un + 0x18, (U(un + 0x18) & 0x1C1) | 0x406);
    }
    SETW(un + 0x1C, U(un + 0x1C) - 1);
    if (U(un + 0x1C) == 0) {                                              /* endurance over: home to the nearest airbase */
        DSB(un + 0x18) |= 4;
        s16 best = 0x7FFF;
        for (int i = 3; i < S(0xE540); i++) {
            u16 sr = (u16)(SITE_BASE + 16 * i);
            if ((U(sr + 8) & 0x101) == 1) {
                s16 d = (s16)fx_dist((s16)(U(un + 2) - U(sr + 2)), (s16)(U(un + 4) - U(sr + 4)));
                if (d < best) { SETW(un, i); best = d; }
            }
        }
    }
    if (DSB(un + 0x18) & 2) {
        int c = 0x29;
        if (u == 0 && S(0xE2D8) >= 5 && (st & 1)) c = 0;
        if (U(0x98DE) != 0 || u == 0) {
            if (enh_map_log_on) enh_map_tag(ENH_OBJ_UNIT, u);   /* ENH: S4.6, the blip is unit u's */
            mfd_map_plot(U(un + 2), U(un + 4), c, 1);
        }
        if ((s16)st % (rate << 4) == (s16)((((u & 8) >> 3) + 2 * (u & 7)) * rate))
            if (!(DSB(un + 0x18) & 0x20)) ai_unit_radar(u);
    }
}

/* 0000:5730 ai_update_units */
void ai_update_units(void)
{
    u16 st = U(0x3D60);
    if (!(st & 1) && S(0x3D68) == -1) SETW(0x39DA + 4 * (st & 0x1E), 0);
    SETW(TRACER_BASE + 12 * (s16)(((st & 0xC) >> 2) + U(0xB16C)), 0);
    SETW(0xB054, U(0x94F2));
    SETW(0x94F2, 0);
    s16 sk = S(0x3654);
    for (int u = 0; u < S(0xDED0); u++) {
        u16 un = (u16)(UNIT_BASE + 0x24 * u);
        u16 fl = U(un + 0x18);
        if (!(fl & 1)) continue;
        SETW(0x94F0, U(un + 0x16));
        u16 a = (u16)(AC_BASE + 32 * S(0x94F0));
        if (!(fl & 2) || U(un + 0x1A) == 0) { scramble(u, un, a); continue; }
        int mode;
        u16 tx, ty, tz;
        if (fl & 4) {                                                     /* return to base and land */
            mode = 2;
            u16 si = (u16)(SITE_BASE + 16 * S(un));
            tx = U(si + 2);
            if (DSB(un + 0x19) & 2) {
                ty = U(si + 4);
                s16 dxu = (s16)-(s16)(tx - U(un + 2));
                tx = (u16)(tx - (s16)(dxu << 1));
                tz = ((u16)(fl_abs16(dxu) + U(si + 8)) & 0x200) == 0 ? 0x0C : 0x8C;
            } else {
                ty = (u16)(S(0xB16E) * 0x280 + U(si + 4));
                tz = (u16)(fx_dist((s16)(U(un + 2) - tx), (s16)(U(un + 4) - ty)) + 0x7D0);
            }
        } else if (U(0x3D62) == 0 || ((fl & 0x140) && S(0x3D62) <= S(0x9F64))) {   /* patrol, formation */
            mode = 3;
            s16 L = S(0x3D6E);
            if ((DSB(un + 0x19) & 1) && L != -1) {
                u16 lu = (u16)(UNIT_BASE + 0x24 * L);
                int k = u & 7;
                u16 ang = (u16)((k << 11) + U(lu + 0x10) - 0x1800);
                tx = (u16)(fx_sin_mul(ang, S(lu + 0x1A)) + U(lu + 2));
                ty = (u16)-(s16)(fx_cos_mul(ang, S(lu + 0x1A)) - U(lu + 4));
                tz = (u16)((k << 6) + U(lu + 6));
            } else {
                if (((((u << 3) + U(0x98E2)) & 0xFF) & 0xBF) == 0 && !(DSB(un + 0x18) & 0x40)) {
                    s16 best = 0x7FFF;
                    u16 bp = fx_heading((s16)(U(0xC0A4) - U(un + 2)), (s16)(U(un + 4) - U(0xC0B2)));
                    for (int n = 0; n < 8; n++) {
                        int c = fl_rand_n(U(0xC0B4)) + 1;                 /* Q-A4: may be one past the table */
                        u16 sr = (u16)(SITE_BASE + 16 * c);
                        if (DSB(sr + 9) & 4) continue;
                        s16 d = (s16)-(s16)(fx_heading((s16)(U(sr + 2) - U(un + 2)), (s16)(U(un + 4) - U(sr + 4))) - bp);
                        if (fl_abs16(d) >= best) continue;
                        best = fl_abs16(d);
                        SETW(un, c);
                        if ((s16)((4 - sk) << 12) > best) break;
                    }
                    s16 dd = (s16)fx_dist((s16)(U(0xC0A4) - U(un + 2)), (s16)(U(0xC0B2) - U(un + 4)));
                    if (((u16)dd & 0xFFC0) > 0x5780 && u != 0) { SETW(un + 0x18, U(un + 0x18) & 0x1C1); SETW(un + 0x1C, 0); }
                }
                u16 si = (u16)(SITE_BASE + 16 * S(un));
                tx = U(si + 2);
                ty = U(si + 4);
                tz = (u16)fx_clamp_wrap((s16)(U(0x2DF4) + 1000), 5000, 20000);
            }
        } else {                                                          /* hunt: the last known position, or the decoy */
            mode = 1;
            tx = U(0xB2A0); ty = U(0xB75A); tz = U(0xB76E);
            if (U(0x39B2) != 0) { tx = U(0x39AA); ty = U(0x39AC); tz = (u16)fx_clamp_wrap(U(0x2DF4), 1000, 30000); }
        }
        steer(u, un, a, mode, tx, ty, tz);
    }
}
