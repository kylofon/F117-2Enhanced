/* Weapons and countermeasures (owner weapons, spec port/spec/flight.md section 5, simflow.md Part 4 section 4 and Part 6 section 3): the store release 753F,
 * the 12 projectile slots 671A with the seeker test 711F, the dropped countermeasures 4512 / 461C, the player gun 465B, the suitability table 8BAB, the world
 * object lookup BAD1, the range / bearing helpers BBB4 / BB86 / BB9E and the class test BCAC. Translated from port/notes/s38/s38_ref.py (0 mismatches against
 * the original). */
#include <string.h>

#include "flight/fl.h"
#include "enhanced/mapmfd.h"   /* ENH: S4.6 */

#define S(o) ((s16)DSW(o))
#define U(o) DSW(o)
#define SETW(o, v) (DSW(o) = (u16)(v))

/* ------------------------------------------------------------------------------------------------------- small helpers */

/* 0000:BBB4 obj_dist_bearing: [9502] = bearing player -> object (only with want_bearing), [94FE] = distance */
int obj_dist_bearing(int x_u, int y_u, int want_bearing)
{
    s16 dx = (s16)(U(0xC0A4) - (u16)x_u), dy = (s16)(U(0xC0B2) - (u16)y_u);
    if (want_bearing) SETW(0x9502, fx_heading((s16)-dx, dy));
    SETW(0x94FE, fx_dist(dx, dy));
    return S(0x94FE);
}

/* 0000:BB86 wpn_site_dist / 0000:BB9E wpn_unit_dist */
int wpn_site_dist(int site) { u16 s = (u16)(0xB2A4 + 16 * site); return obj_dist_bearing(S(s), S(s + 2), 1); }
int wpn_unit_dist(int unit) { u16 u = (u16)(0xC140 + 0x24 * unit); return obj_dist_bearing(S(u), S(u + 2), 0); }

static int obj_class(int o) { return DSB((u16)(0xC604 + (DSB((u16)(SITE_BASE + 16 * o + 0xE)) & 0x7F))) & 0xF; }

/* 0000:8BAB wpn_suit: lethality of store `wpn` against the class of object `site` (table DS:386A, signed bytes) */
int wpn_suit(int wpn, int site) { return fl_sx8(DSB((u16)(0x386A + (u16)(wpn << 4) + obj_class(site)))); }

/* 0000:BCAC obj_cls_ship */
int obj_cls_ship(int o) { int c = obj_class(o); return c == 0xC || c == 0xD; }

/* 0000:BAD1 obj_find_at: the world object at a point -> registered site index (>= 1), 0 = the scratch record 0 filled in, -1 = none */
int obj_find_at(int x, int y)
{
    SETW(0x9F10, terr_find_scenery_at((s32)(s16)x << 5, (0x8000 - (s32)(s16)y) << 5));     /* both sign-extended */
    u16 w = U(0x9F10);
    if (w == 0) return -1;
    u16 ox = (u16)((s32)DSL(w + 4) >> 5), oy = (u16)(0x8000 - (u16)((s32)DSL(w + 8) >> 5));
    for (int i = 1; i < S(0xC0B4); i++)
        if (U(0xB2A4 + 16 * i) == ox && U(0xB2A6 + 16 * i) == oy) return i;
    SETW(0xB2A4, ox);
    SETW(0xB2A6, oy);
    SETW(0xB2B0, U(w) + 0x100);
    if (S(0x3D68) == 0) SETW(0x3D68, 0xFFFF);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- countermeasures */

/* 0000:4512 wpn_release_countermeasure(a): 1 flare, 2 chaff, 3 decoy */
void wpn_release_countermeasure(int a)
{
    u16 o = (u16)(0x3636 + 2 * a);
    s16 old = S(o);
    SETW(o, old - 1);
    if (old <= 0) { SETW(o, 0); msg_post(ds_str(0x3DA7)); return; }
    int slot = -1;
    if (a == 3) {
        if (U(DECOY_BASE + 8) == 0 && !(DSB(0x3632) & 0x40)) slot = 0;
    } else {
        for (int k = 1; k < 4; k++)
            if (U(DECOY_BASE + 12 * k + 8) == 0) slot = k;
    }
    if (slot != -1) {
        u16 r = (u16)(DECOY_BASE + 12 * slot);
        SETW(r, U(0xC0A4));
        SETW(r + 2, U(0xC0B2));
        SETW(r + 6, a);
        SETW(r + 8, (9 - 2 * S(0x3654)) * S(0x3660));
        if (a == 1) { fl_strcpy(0x9876, 0x3DB8); SETW(0x3D90, 3 * S(0x3660)); }
        else if (a == 2) { fl_strcpy(0x9876, 0x3DBE); SETW(0x3D8E, 3 * S(0x3660)); }
        else if (a == 3) { fl_strcpy(0x9876, 0x3DC4); SETW(r + 8, U(r + 8) << 3); }
        fl_strcat(0x9876, 0x3DCA);
        msg_post(ds_str(0x9876));
    }
    hud_draw_store_count(a);
    snd_cue(0xF, 2);
}

/* 0000:461C wpn_cm_tick: only the FIRST busy record ages per step (Q-W1) */
void wpn_cm_tick(void)
{
    for (int k = 0; k < 4; k++) {
        u16 r = (u16)(DECOY_BASE + 12 * k);
        if (U(r + 8) == 0) continue;
        SETW(r + 8, U(r + 8) - 1);
        if (U(r + 6) == 3) hud_lamp_update(7, U(r + 8) >= 1 ? 0x29 : 0);
        return;
    }
}

/* 0000:465B wpn_gun_update: the rounds move; one round every 2 steps into slot (st >> 1) % [B16C] while button 0 is held (model 0 only) */
void wpn_gun_update(void)
{
    s16 n = S(0xB16C);
    if (n == 0) return;
    for (int k = 0; k < n + 4; k++) {
        u16 t = (u16)(TRACER_BASE + 12 * k);
        if (U(t) != 0) { SETW(t, U(t) + U(t + 6)); SETW(t + 2, U(t + 2) + U(t + 8)); SETW(t + 4, U(t + 4) + U(t + 10)); }
    }
    s16 st = S(0x3D60);
    if (!(st & 1) || fl_pilot_w(0x42) != 0) return;
    s16 q = (s16)(st >> 1);
    u16 t = (u16)(TRACER_BASE + 12 * (q % n));
    if (input_button(0) != 0 && S(0x3650) > 0 && U(0xC06E) == 0) {
        SETW(0x3650, fx_clamp_wrap(-40 / fl_rate() + S(0x3650), 0, 1000));
        snd_cue(6, 2);
        int sp = 0x70 / fl_rate();
        SETW(t + 10, (s16)fx_sin_mul(U(0x2DF0), sp) << 5);
        s16 h = (s16)fx_cos_mul(U(0x2DF0), sp);
        SETW(t + 6, fx_sin_mul(U(0x2DEE), h));
        SETW(t + 8, -fx_cos_mul(U(0x2DEE), h));
        SETW(t, U(t + 6) + U(0xC0A4));
        SETW(t + 2, U(t + 8) + U(0xC0B2));
        SETW(t + 4, U(t + 10) + U(0x2DF4) - 2);
        SETW(0x98EA, 1);
    } else {
        SETW(t, 0);
        SETW(0x98EA, 0);
    }
}

/* ------------------------------------------------------------------------------------------------------------ projectiles */

/* 0000:711F wpn_seeker_check: the 22.5 degree cone (rear aspect for class 0) of slot `slot` against a point; [94F6] bearing, [94F4] distance (tz unused) */
int wpn_seeker_check(int slot, int tx, int ty, int tz, int cls)
{
    (void)tz;
    u16 p = (u16)(PROJ_BASE + 0x1C * slot);
    s16 dx = (s16)((u16)tx - U(p)), dy = (s16)((u16)ty - U(p + 2));
    s16 dist = (s16)fx_dist(dx, dy);
    u16 brg = fx_heading(dx, (s16)-dy);
    SETW(0x94F6, brg);
    s16 a = fl_abs16((s16)(brg - U(p + 8)));
    if (a > 0x1000 && cls != 3) {
        if (a > 0x6000 && slot < 8 && S(p + 6) < dist) SETW(p + 0xE, 0);
        return 0;
    }
    if (cls == 0 && fl_abs16((s16)(U(p + 8) - U(0x2DEE))) > 0x2000) return 0;
    SETW(0x94F4, dist);
    return 1;
}

/* 671A keeps tz and the target of the player slots in stack locals that are not reset between slots (Q-W2); they are only read after a lock in the same slot */
static u16 proj_tz, proj_tgt;

static void msg_cat_post(u16 a, u16 b)
{
    fl_strcat(0x9876, a);
    fl_strcat(0x9876, b);
    msg_post(ds_str(0x9876));
}

/* 0000:671A wpn_proj_update_all */
void wpn_proj_update_all(void)
{
    memmove(DSP(0xB290), DSP(0xB2A2), 16);                                /* the scratch site record 0 is saved, BAD1 may overwrite it */
    hud_lamp_update(0, 0);
    hud_lamp_update(1, 0);
    hud_lamp_update(8, (U(0xDED6) != 0 && (DSB(0x3D60) & 1)) ? 0x11 : 3);
    u16 ax, ay;
    if (U(0x39B2) != 0) { ax = U(0x39AA); ay = U(0x39AC); }
    else { ax = U(0xC0A4); ay = U(0xC0B2); }
    s16 sk = S(0x3654);
    for (int i = 0; i < 12; i++) {
        u16 p = (u16)(PROJ_BASE + 0x1C * i);
        if (U(p + 0xE) == 0) continue;
        int lock = 0;
        u16 brg = 0, dist, tz;
        s16 typ = S(p + 0x10);
        u16 wp = (u16)(WPN_BASE + 0x12 * typ);
        s16 cls = S(wp + 0xC);
        s16 st = S(0x3D60), rate = fl_rate();
        if (i < 8) {                                                      /* enemy missiles: the player, decoys, flares, chaff, jammers */
            tz = U(0x2DF4);
            proj_tz = tz;
            lock = wpn_seeker_check(i, ax, ay, tz, cls);
            dist = U(0x94F4);
            brg = U(0x94F6);
            bool cm = ((DSB(0x9B04) & 0x10) && cls > 0) || ((DSB(0x9B04) & 0x20) && cls <= 0) || (S(0x9F64) > S(0x3D62) && S(p + 0x10) != 0x21);
            if (cm && ((u16)((sk << 8) + 0x180) < dist || cls == 1 || cls == -1)) { brg = U(p + 8); lock = 0; }
            for (int j = 1; j < 4; j++) {
                u16 dd = (u16)(DECOY_BASE + 12 * j);
                s16 t = S(dd + 6);
                bool cand = false;
                if (t == 1 && cls <= 0) cand = true;
                else if (t == 2 && (cls == 1 || cls == 2 ||
                                    (cls == 3 && fl_abs16((s16)(fl_abs16(fl_sx8((u16)(brg - U(0x2DEE)) >> 8)) - 0x40)) < (s16)(0x40 - sk * 12))))
                    cand = true;
                if (cand) {
                    int r = wpn_seeker_check(i, U(dd), U(dd + 2), U(0x2DF4), cls);
                    if (r != 0) { brg = (u16)r; lock = 0; }
                }
            }
            if (dist > 0x200) {
                s16 aux = S(p + 0x16);
                if (aux > 2 && !(DSB((u16)(SITE_BASE + 16 * aux + 8)) & 0x10)) lock = 0;
                if (aux <= 0 && !(DSB((u16)(UNIT_BASE + 0x24 * -aux + 0x18)) & 8)) lock = 0;
            }
            if ((S(wp + 10) >> 6) > S(p + 6) && (st & 1)) SETW(p + 6, U(p + 6) + 1);
        } else {                                                          /* player stores: AAMs on units, guided weapons on the designated target */
            dist = 0x7FFF;
            tz = proj_tz;
            u16 tgt = proj_tgt;
            if (cls == 7) {
                for (int j = 0; j < S(0xDED0); j++) {
                    u16 uu = (u16)(UNIT_BASE + 0x24 * j);
                    if (!(DSB(uu + 0x18) & 2) || U(uu + 0x1A) == 0) continue;
                    int r = wpn_seeker_check(i, U(uu + 2), U(uu + 4), U(uu + 6), cls);
                    if (dist > U(0x94F4) && r != 0) {
                        brg = U(0x94F6); dist = U(0x94F4); tgt = (u16)j; tz = U(uu + 6); lock = 1;
                        if (dist < 0x180) DSB(uu + 0x18) |= 0x10;
                    }
                }
            }
            if ((S(wp + 10) >> 6) > S(p + 6) && (st & 1)) { SETW(p + 6, U(p + 6) + 1); brg = U(p + 8); }
            if ((cls == 4 || cls == 5 || cls == 6) && S(p + 0x14) != -1) {
                s16 t = S(p + 0x14);
                if (t == 0 && (U(p + 0x18) != U(0xB2A4) || U(p + 0x1A) != U(0xB2A6))) { obj_find_at(U(p + 0x18), U(p + 0x1A)); SETW(0xB2AA, 0); }
                if (wpn_seeker_check(i, U(SITE_BASE + 16 * t + 2), U(SITE_BASE + 16 * t + 4), 0, cls)) {
                    brg = U(0x94F6); dist = U(0x94F4); tgt = (u16)t; tz = 0; lock = 1;
                }
            }
            if (typ == 0x1C && S(0x3D66) != -1) {
                s16 t = S(0x3D66);
                if (wpn_seeker_check(i, U(SITE_BASE + 16 * t + 2), U(SITE_BASE + 16 * t + 4), 0, cls)) {
                    brg = U(0x94F6); dist = U(0x94F4); tgt = (u16)t; tz = 0; lock = 1;
                }
            }
            proj_tz = tz;
            proj_tgt = tgt;
        }
        /* ---- warnings, steering */
        if (lock && i < 8 && fl_abs16((s16)(U(0x94F6) - U(p + 8))) < 0x1000 && !(DSB(0x3632) & 1) && U(0x39B2) == 0) {
            if (cls <= 0 && (st & 2)) hud_lamp_update(1, 0x29);
            if (cls != 0 && !(st & 2)) hud_lamp_update(0, 0x11);
            if (!(st & 3) && (u16)(U(p + 6) << 4) > dist) snd_cue(9, 1);
        }
        s16 adist = fl_abs16((s16)dist);
        if (brg != 0 && lock != 0) {
            s16 dh = (s16)(brg - U(p + 8));
            if (i < 8) dh = (s16)fx_clamp_wrap(dh, (s16)-((sk + 1) << 8), (sk + 1) << 8);
            s16 tl = (s16)(S(wp + 0xE) << 7);
            dh = (s16)fx_clamp_wrap(dh, (s16)-tl, tl);
            SETW(p + 8, U(p + 8) + (s16)(dh << 2) / rate);
            SETW(p + 0xC, dh << 1);
            u16 pa;
            if (i < 8 && dist < 0x400) pa = fx_heading((s16)(tz - U(p + 4)) >> 4, adist);
            else {
                s16 lob = adist > 0x140 ? (s16)(adist >> 3) : 0;
                pa = fx_heading((s16)(((s16)(tz - U(p + 4)) >> 5) + lob), adist);
            }
            s16 pe = (s16)(pa - U(p + 0xA));
            if (i < 8) pe = (s16)fx_clamp_wrap(pe, (s16)(u16)((0xFFFF - sk) << 9), (s16)((sk + 1) << 9));
            s16 pl = (s16)((U(wp + 0xE) & 0xFF) << 8);
            pe = (s16)fx_clamp_wrap(pe, (s16)-pl, pl);
            SETW(p + 0xA, U(p + 0xA) + (s16)(pe << 2) / rate);
        } else if (S(p + 0xA) > 0 && typ != 0x1E) {
            SETW(p + 0xA, U(p + 0xA) - 0x800 / rate);
        }
        if (typ == 0x1C && S(p + 0xA) > (s16)0xF800) SETW(p + 0xA, 0xF800);
        if (typ == 0x1E || S(p + 4) == 1) {
            SETW(p + 0xA, U(p + 0xA) - 0x800 / rate);
            if (S(p + 0x16) > S(p + 0xA)) SETW(p + 0xA, U(p + 0x16));
        }
        /* ---- move */
        s16 h = (s16)((s16)(fx_cos_mul(U(p + 0xA), S(p + 6)) << 2) / rate);
        SETW(p, U(p) + fx_sin_mul(U(p + 8), h));
        SETW(p + 2, U(p + 2) - fx_cos_mul(U(p + 8), h));
        SETW(p + 4, U(p + 4) + fx_sin_mul(U(p + 0xA), (s16)(U(p + 6) << 7) / rate));
        SETW(p + 0xE, U(p + 0xE) - 1);
        DSB(0xC686) = 0;
        if (((i ^ st) & 3) == 0) terr_probe_cell(S(p), S(p + 2), S(p + 4));
        /* ---- impact */
        if (S(p + 4) < 0 || DSB(0xC686)) {
            SETW(0xC0A0, U(p)); SETW(0xC0AA, U(p + 2)); SETW(0xC0B0, U(p + 4)); SETW(0x9D0C, 0xFFFA);
            SETW(p + 0xE, 0);
            mfd_tv_label();
            u16 sn = (u16)(0x366C + 0x1A * S(p + 0x12));
            fl_strcpy(0x9876, sn);
            if (typ == 0x1E || typ == 0x1D || typ == 0x1C) {               /* bombs: the object hit, the blast near the player */
                snd_cue(5, 2);
                fl_strcat(0x9876, 0x3E40);
                int o = obj_find_at(U(0xC0A0), U(0xC0AA));
                if (o != -1 && !(DSB((u16)(SITE_BASE + 16 * o + 8)) & 0x80)) {
                    u16 wo = U(0x9F10);
                    s16 ox = (s16)((s32)DSL(wo + 4) >> 5);
                    s16 oy = (s16)(0x8000 - (u16)((s32)DSL(wo + 8) >> 5));
                    u16 miss = (u16)fx_dist((s16)(U(0xC0A0) - ox), (s16)(U(0xC0AA) - oy));
                    if ((u16)((s16)(wpn_suit(S(p + 0x12), o) << 6) / (sk + 2)) > miss) {
                        dmg_object_destroy(o);
                        fl_strcat(0x9876, 0x3E49);
                        fl_strcat(0x9876, sn);
                        SETW(0x9D0C, 0x10);
                        SETW(0xC0B0, 0);
                    }
                }
                msg_post(ds_str(0x9876));
                u16 d = (u16)(fx_dist((s16)(U(0xC0A4) - U(p)), (s16)(U(0xC0B2) - U(p + 2))) + (U(0x2DF4) >> 5));
                if ((u16)(sk << 5) > d) { dmg_player_blast(); fl_strcpy(0x9876, 0x3E58); }
            } else if (i >= 8) {
                fl_strcat(0x9876, 0x3E82);
                msg_post(ds_str(0x9876));
            }
        }
        /* ---- proximity fuse */
        u16 slant = (u16)((fl_abs16((s16)(tz - U(p + 4))) >> 5) + dist);
        if ((u16)((s16)(U(p + 6) << 3) / rate) > slant && lock) {
            if (i < 8) {
                if (S(p + 0xE) > 4 * rate) {
                    SETW(0xC0A0, U(p)); SETW(0xC0AA, U(p + 2)); SETW(0xC0B0, U(p + 4)); SETW(0x9D0C, 0x10);
                    SETW(p + 0xE, 2 * rate);
                    if (U(0x39B2) == 0) {
                        fl_strcpy(0x9876, 0x3E91);
                        fl_strcat(0x9876, wp);
                        msg_post(ds_str(0x9876));
                        dmg_player_blast();
                        if (!(DSB(0x9B05) & 0x10)) mission_log_event(5, typ);
                    }
                }
            } else if (cls == 7) {
                u16 uu = (u16)(UNIT_BASE + 0x24 * proj_tgt);
                SETW(p, U(uu + 2)); SETW(0xC0A0, U(uu + 2));
                SETW(p + 2, U(uu + 4)); SETW(0xC0AA, U(uu + 4));
                SETW(p + 4, U(uu + 6)); SETW(0xC0B0, U(uu + 6));
                SETW(0x9D0C, 0x10);
                SETW(p + 0xE, 2 * rate);
                dmg_unit_kill((s16)proj_tgt);
                msg_cat_post(0x3EA5, wp);
            } else {
                mfd_tv_label();
                s16 t = (s16)proj_tgt;
                u16 ss = (u16)(SITE_BASE + 16 * t);
                SETW(0xC0A0, U(ss + 2)); SETW(0xC0AA, U(ss + 4)); SETW(p + 0xE, 0); SETW(0xC0B0, 0); SETW(0x9D0C, 0x10);
                int r = fl_rand_n(4);
                if (wpn_suit(S(p + 0x12), t) > r) dmg_object_destroy(t);
                else fl_strcpy(0x9876, 0x3E99);
                SETW(0x3D62, U(0xB286)); SETW(0xB2A0, U(0xC0A0)); SETW(0xB75A, U(0xC0AA)); SETW(0xB76E, 0xBB8);
                msg_cat_post(0x3EA5, wp);
            }
        }
        if (i < 8 && U(p + 0xE) != 0 && (DSB(0x3D60) & 1)) {
            if (enh_map_log_on) enh_map_tag(ENH_OBJ_PROJ, i);   /* ENH: S4.6, the blip is projectile i's */
            mfd_map_plot(U(p), U(p + 2), 0x11, 1);   /* PORT: the 4th argument is not pushed: 8548 reads 671A's saved SI (the caller's SI, not 0 in the game) */
        }
    }
    memmove(DSP(0xB2A2), DSP(0xB290), 16);
}

/* 0000:753F wpn_release: the fire key: a store of the selected bay into the last free player slot 8..11, or fuel from a tank */
void wpn_release(void)
{
    if (fl_abs16(S(0x2DF2)) > 0x3000) return;
    s16 b = S(0x364E);
    u16 bb = (u16)(0x363E + 4 * b);
    if (U(bb + 2) == 0) return;
    s16 store = S(bb);
    s16 typ = S(0x3678 + 0x1A * store);
    if (typ == 0 || typ == -1) return;
    if (!(DSB(0x9B04) & 4) && typ != -2) { sim_snd_cmd(0x20, 0, 0, 0); msg_post(ds_str(0x3ECC)); return; }
    if (U(0x2DF6) == 0 || ((DSB((u16)(SITE_BASE + 16 * S(0xDFE0) + 9)) & 2) && U(0x2DF6) == 0x80)) { sim_snd_cmd(0x20, 0, 0, 0); msg_post(ds_str(0x3ED7)); return; }
    SETW(bb + 2, U(bb + 2) - 1);
    if (typ == -2) {                                                      /* external fuel tank */
        SETW(0x3634, U(0x3634) + 0x76C);
        msg_post(ds_str(0x3EED));
        if (U(0xDFDC) == 0x14) mfd_nav_page();
        return;
    }
    mission_log_event(4, b);
    frame_rate_halve();
    int sl = -1;
    for (int k = 8; k < 12; k++)
        if (U(PROJ_BASE + 0x1C * k + 0xE) == 0) sl = k;
    if (sl != -1) {
        u16 p = (u16)(PROJ_BASE + 0x1C * sl), wp = (u16)(WPN_BASE + 0x12 * typ);
        SETW(p, U(0xC0A4)); SETW(p + 2, U(0xC0B2)); SETW(p + 4, U(0x2DF4) - 0x14); SETW(p + 6, U(0xB052) >> 10);
        SETW(p + 8, U(0x2DEE)); SETW(p + 0xA, U(0x2DF0)); SETW(p + 0xC, U(0x2DF2));
        s32 num = (s32)(S(wp + 8) * S(0x3660)) << 5;
        s16 life = (s16)(num / ((S(wp + 10) >> 6) + 1) + 6);
        SETW(p + 0xE, life > 6 ? life : 0x3E7);
        SETW(p + 0x10, typ); SETW(p + 0x12, store); SETW(p + 0x1A, 0xFFFF); SETW(p + 0x18, 0xFFFF); SETW(p + 0x14, 0xFFFF);
        if (typ == 0x1B) SETW(0x43B4, 0);
        if (typ == 0x1E || typ == 0x1D || typ == 0x1C || typ == 0x26) SETW(p + 6, S(p + 6) >> 1);
        if (typ == 0x1E) SETW(p + 0x16, fm_bomb_dive_angle() - 0x400);
        s16 t = S(0x3D66);
        if (t >= 0) {
            if (U(0xE55C) != 0) {
                s16 c = S(wp + 0xC);
                if (c == 6 || (c == 4 && U(SITE_BASE + 16 * t + 6) != 0) || (c == 5 && obj_cls_ship(t))) {
                    SETW(p + 0x14, t); SETW(p + 0x18, U(SITE_BASE + 16 * t + 2)); SETW(p + 0x1A, U(SITE_BASE + 16 * t + 4));
                }
            } else {
                SETW(p + 0x14, 0xFFFF);
            }
        }
        if (typ == 0x1D) { SETW(p + 0xA, 0xC000); SETW(p + 6, 1); }
        SETW(0xB04C, sl);
        fl_strcpy(0x9876, (u16)(0x366C + 0x1A * store));
        fl_strcat(0x9876, 0x3EFB);
        msg_post(ds_str(0x9876));
        snd_cue(U(wp + 8) == 0 ? 0x10 : 0x0D, 2);
    }
    if (U(0xDFDC) == 0x15) mfd_refresh(0x15);
}
