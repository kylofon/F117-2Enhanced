/* The per-frame simulation step 39C4 (spec simflow.md Part 4): player cell and world clamp, the first-call mission initialisation, the moving-map markers, the
 * updaters of the other modules (flight/, S5.7), countdown timers, the nearest-airfield logic, the airfield zone, take-off line, cargo, landing and the failure
 * checks, the beacon phase, the mission clock, the frame-rate estimator, and finally the key commands. The order of the calls is the original's. */
#include <string.h>

#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "sim/sim_stubs.h"
#include "enhanced/mapmfd.h"   /* ENH: S4.6 */

static u16 pilot_w(int off) { return *(u16 *)(far_mp(ds_far(0x98F4)) + off); }
#define SITE(i)  ((u16)(0xB2A2 + 16 * (u16)(i)))
#define UNIT(i)  ((u16)(0xC13E + 0x24 * (u16)(i)))

static s16 abs16(s16 v) { return (s16)(v < 0 ? -v : v); }

/* the first call of the step: the mission initialisation (spec 2.1; [C0AE] 1 -> 2) */
static void mission_init(void)
{
    Comm *comm = comm_get();
    mission_name_targets();
    DSW(0xB75A) = DSW(0xB2A0) = 0;
    DSB(0x294B) = 0;
    DSW(0x98FE) = DSW(0x3D66) = DSW(0x3D64) = DSW(0x3D68) = DSW(0x3D72) = 0xFFFF;
    DSW(0x3634) = 0x2710;
    DSW(0xC5C8) = 2;
    DSW(0x2E7E) = DSW(0x2E7C) = DSW(0x3652) = DSW(0x364E) = DSW(0x3632) = DSW(0x9B04) = DSW(0x43BC) = DSW(0xB76E) = DSW(0x3D62) = 0;
    DSW(0xDE8C) = DSW(0xC600) = DSW(0xC0B6) = DSW(0xDFE0) = DSW(0xE2FE) = 0;
    DSW(0x2646) = 0xC;
    DSW(0x3660) = 4;
    frame_params_update();
    DSW(0x3D6A) = DSW(0xA068) = DSW(0x4082) = DSW(0x4080) = 1;
    DSW(0xC072) = 3;
    u16 dir = DSW((u16)(0x3D7C + 2 * pilot_w(0x38)));
    DSW(0xB16E) = dir;
    if (DSB((u16)(SITE(DSW(0xE2DC)) + 9)) & 2) {
        DSL(0xBA10) = (u32)((s32)DSL(0xBA10) - (s32)(s16)(dir << 7));
        DSB(0x9B04) |= 8;
        hud_lamp_recolor(DSP(DSW(CTX_P0_PTR)), 0x23, 1);
    } else {
        DSL(0xC02A) = (u32)((s32)DSL(0xC02A) - (s32)(s16)(dir * 0x708));
    }
    sim_reset_clock_palette();
    mission_log_event(8, 0);
    mfd_init_geometry();
    hud_lamp_update(3, 0xA);
    mfd_objectives_page();
    DSW(0x3656) = pilot_w(0x3A);
    DSW(0x3654) = pilot_w(0x3E);
    DSW(0x98DE) = comm->adapter_lo;
    view_range_setup();
    comm->mission_state = 1;
    if (pilot_w(0x3C) & 2) DSB(0x9B05) |= 0x10;
    for (int i = 0; i < 4; i++) {
        DSW((u16)(0x363E + 4 * i)) = comm->weapon_id[i];
        DSW((u16)(0x3640 + 4 * i)) = comm->weapon_qty[i];
    }
    DSW(0xC0AE) = 2;
    hud_draw_fuel();
    screen_on();
}

/* the airfield objects at the end of the unit table: the two dressing objects and the two parked aircraft follow the nearest airfield */
static void airfield_objects_rebuild(void)
{
    u16 site = SITE(DSW(0xDFE0));
    u8 fhi = DSB(site + 9);
    u16 flags = DSW(site + 8);
    for (int k = 1; k <= 2; k++) {
        u16 o = UNIT((u16)(DSW(0xDED0) - k));
        DSB(o + 0x18) &= 0xFD;
        if (!(fhi & 4)) DSW(o + 0x16) = 0;
        else DSW(o + 0x16) = k == 1 ? 0x15 : 0x1C;
        if (fhi & 2) DSW(o + 0x16) = 8;
        if (fhi & 1) DSW(o + 0x16) = (u16)(0x18 + ((flags & 0x200) ? 0 : 3));
        DSW(o) = DSW(0xDFE0);
    }
    for (int k = 3; k <= 4; k++) {
        u16 o = UNIT((u16)(DSW(0xDED0) - k));
        DSB(o + 0x18) |= 2;
        DSW(o + 2) = DSW(site + 2);
        DSW(o + 4) = DSW(site + 4);
        if (fhi & 2) {
            DSW(o + 2) = (u16)(DSW(o + 2) + (s16)(DSW(0xB16E) * 5));
            DSW(o + 4) = (u16)(DSW(o + 4) + (u16)((s16)(((k & 1) * (s16)DSW(0xB16E))) << 4));
            DSW(o + 6) = 0x80;
        } else {
            DSW(o + 2) = (u16)(DSW(o + 2) + 10);
            DSW(o + 4) = (u16)(DSW(o + 4) + (((DSB(0xDFE0) + k) & 3) << 4));
            DSW(o + 6) = 0;
        }
        DSL(o + 8) = (u32)DSW(o + 2) << 5;
        DSL(o + 0xC) = (u32)DSW(o + 4) << 5;
        DSW(o + 0x10) = (u16)-rand_n(0x4000);
        DSW(o + 0x16) = (u16)(((flags & 0x400) ? 0 : 1) + 0xA);
        if (fhi & 1) DSW(o + 0x16) = 0xA;
    }
}

/* 0000:39C4 sim_frame_step */
void sim_frame_step(void)
{
    int sx = 0, sy = 0, wsx = 0, wsy = 0;
    DSW(0xC0A4) = (u16)((s32)(DSL(0xBA10) + 0x10) >> 5);
    DSW(0xC0B2) = (u16)(0x8000 - (u16)((s32)(DSL(0xC02A) + 0x10) >> 5));
    if (DSW(0xC0AE) == 1) mission_init();

    /* the world clamp */
    s16 c = (s16)fx_clamp_wrap((s16)DSW(0xC0A4), 0x100, 0x7E00);
    if ((s16)DSW(0xC0A4) != c) {
        DSW(0xC0A4) = (u16)c;
        DSL(0xBA10) = (u32)(u16)c << 5;
    }
    c = (s16)fx_clamp_wrap((s16)DSW(0xC0B2), 0x200, 0x7D00);
    if ((s16)DSW(0xC0B2) != c) {
        DSW(0xC0B2) = (u16)c;
        DSL(0xC02A) = (u32)(u16)(0x8000 - c) << 5;
    }

    /* the markers on the moving map */
    if (DSW(0x365E) != 0 && DSW(0xDEDA) == 0) {
        if (mfd_map_to_screen((s16)DSW(0xC0A4), (s16)DSW(0xC0B2), &sx, &sy)) mfd_map_restore_bg();
        else mfd_map_redraw((s16)DSW(0xC0A4), (s16)DSW(0xC0B2));
        for (int i = 0; i < 2; i++) {
            if (DSW(0x9B04) & (0x4000 >> i)) continue;
            u16 site = SITE(DSW((u16)(0xE2DA + 0x12 * i)));
            if (mfd_map_to_screen((s16)DSW(site + 2), (s16)DSW(site + 4), &wsx, &wsy)) {
                cockpit_keyblit(wsx - 1, wsy - 1, 0x104, 0, 4, 4, 0);
                if (enh_map_log_on) enh_map_log_key(0, DSW(site + 2), DSW(site + 4), wsx - 1, wsy - 1, 0x104, 0, 4, 4, 0);   /* ENH: S4.6 */
            }
        }
        if (DSW(0xDFDC) == 0x14) mfd_map_route();
    }
    hud_threat_update();
    ai_update_units();
    wpn_proj_update_all();
    if (DSW(0x365E) != 0 && DSW(0xDEDA) == 0) {
        cockpit_keyblit(sx - 1, sy - 1, 0x100 | (((s16)(DSW(0x2DEE) + 0x1000) >> 11) & 0x1C), 4, 4, 4, 0);
        if (enh_map_log_on) enh_map_log_key(1, DSW(0xC0A4), DSW(0xC0B2), sx - 1, sy - 1, 0x100 | (((s16)(DSW(0x2DEE) + 0x1000) >> 11) & 0x1C), 4, 4, 4, 0);   /* ENH: S4.6 */
    }
    wpn_cm_tick();
    wpn_gun_update();
    fx_smoke_update();
    fx_fall_update();
    hud_glideslope_update();
    hud_draw_fuel();
    hud_lamp_timers();
    if (DSW(0x3D62)) DSW(0x3D62)--;
    if (DSW(0xDED8)) { if (--DSW(0xDED8) == 0) sim_snd_cmd(0x29, 0, 0, 0); }
    if (DSW(0x9D0A)) { if (--DSW(0x9D0A) == 0) sim_snd_cmd(0x2A, 0, 0, 0); }

    /* every 8th step: the nearest airfield and its objects */
    if ((DSW(0x3D60) & 7) == 0) {
        DSW(0x3D72) = DSW(0xDFE0);
        DSW(0x98F2) = 0x7FFF;
        for (int i = 0; (s16)DSW(0xC0B4) > i; i++) {
            u16 s = SITE(i);
            if ((DSW(s + 8) & 0x201) && (DSB(s + 9) & 5)) {
                s16 d = (s16)fx_dist((s16)(DSW(0xC0A4) - DSW(s + 2)), (s16)(DSW(0xC0B2) - DSW(s + 4)));
                if ((s16)DSW(0x98F2) > d) { DSW(0x98F2) = (u16)d; DSW(0xDFE0) = (u16)i; }
            }
        }
        if (DSW(0xDFE0) != DSW(0x3D72) && !(DSB((u16)(SITE(DSW(0xDFE0)) + 9)) & 8)) airfield_objects_rebuild();
        if ((DSW(0x3D60) & 0x7F) == 0) {
            if (!(DSB((u16)(SITE(DSW(0xDFE0)) + 9)) & 8)) {
                u16 k = (DSW(0x3D60) & 0x80) ? (u16)(DSW(0xDED0) - 1) : (u16)(DSW(0xDED0) - 2);
                u16 o = UNIT(k);
                if (!(DSB(o + 0x18) & 2)) {
                    ai_unit_takeoff((s16)k, (s16)DSW(0xDFE0));
                    DSW(o + 0x18) = 0x207;
                    DSW(o + 6) = 0x3E8;
                    DSW(o + 0x1A) = 0xFA;
                    DSL(o + 0xC) = (u32)((s32)DSL(o + 0xC) + (s32)(s16)(DSW(0xB16E) * 0x3000));
                }
            }
            DSW(0xDE8C) = DSW(0xC600);
            DSW(0xC600) = DSW(0xC0B6);
            DSW(0xC0B6) = 0;
        }
    }

    /* low fuel on final (speech) */
    if (DSW(0x3D94) == 0 && DSW(0xE2EE) == DSW(0xDFE0) && (s16)DSW(0x3634) < 0x1388 && DSW(0x46C2) == 2) {
        sim_snd_cmd(0x2D, 0, 0, 0);
        DSW(0x3D94)++;
    }

    /* the airfield zone, the take-off line, cargo, landing */
    if ((s16)DSW(0x98F2) >= 0x200 && DSW(0xC0A2) != DSW(0x2DF4)) {
        DSW(0x3D74) = 0;
    } else {
        u16 s = SITE(DSW(0xDFE0));
        DSW(0xC0A2) = 0;
        DSW(0x9900) = 0xA0;
        DSW(0x9B06) = 0x800;
        if (DSB(s + 9) & 8) DSW(0x9B06) = 0x400;
        if (DSB(s + 9) & 2) {
            DSW(0xC0A2) = 0x80;
            DSW(0x9900) = 0x100;
            DSW(0x9B06) = 0x3C0;
            if (DSW(0x2DF4) == 0x80 && (s16)DSW(0xB168) > 0x50) {
                u16 prod = (u16)((u16)(DSW(0xC0B2) - DSW(s + 4)) * DSW(0xB16E));
                if (prod >= 0x10 && prod <= 0x14 && abs16((s16)(((u16)(DSW(0xB16E) - 1) << 14) + DSW(0x2DEE))) < 0x2000) {
                    DSW(0x98E0) = 1;
                    snd_cue(0xF, 2);
                }
            }
        }
        if (pilot_w(0x40) == 1) {
            DSW(0x9900) = (u16)(DSW(0x9900) + 0x80);
            DSB(0x9B07)++;
        }
        if (abs16((s16)(DSW(0xC0A4) - DSW(s + 2))) > ((s16)DSW(0x9900) >> 5) || abs16((s16)(DSW(0xC0B2) - DSW(s + 4))) > ((s16)DSW(0x9B06) >> 5)) {
            DSW(0xC0A2) = 0;
            DSW(0x3D74) = 0;
        } else {
            DSW(0x3D74) = 1;
            if (!((s16)DSW(0xB168) > 1 || (DSW(0x3D60) & 0xF))) {
                for (int i = 0; i < 2; i++) {
                    u16 slot = (u16)(0xE2D8 + 0x12 * i);
                    if (DSW(slot + 2) == DSW(0xDFE0) && DSW(slot) == 4 && (s16)DSW(0x98E2) < (s16)DSW(0xDE88)) {
                        mission_target_done(i);
                        msg_post(ds_str(0x2E4E));
                        DSW((u16)(0x3640 + 4 * i)) = 0;
                    }
                }
                u16 flags = DSW(s + 8);
                if ((flags >> 8) & 5 && DSW(0x3D9A) != 0 && !((flags >> 8) & 8)) {
                    u16 old = DSW(0x3D9A);
                    DSW(0x3D9A)++;
                    if (old == 1) {
                        msg_post(ds_str(0x2E5E));
                        sim_snd_cmd(0x2B, 0, 0, 0);
                    }
                    if ((s16)(16 / (s16)DSW(0x3660)) < (s16)DSW(0x3D9A)) mission_end(0);
                }
            }
        }
    }

    /* outside every zone: stranded on the ground, or airborne (arms the landing counter) */
    if (DSW(0x3D74) == 0) {
        if (DSW(0x2DF4) != 0) {
            DSW(0x3D9A) = 1;
        } else if (pilot_w(0x40) != 0 || (s16)DSW(0xC5C8) > 4 || DSW(0x3634) == 0) {
            if (DSW(0xC06E) == 0 && (s16)DSW(0xB168) > 0x32) {
                snd_cue(4, 2);
                gfx_set_fg(0);
                hud_fill_rect(0, 0, 0x13F, 0xC7);
                frame_delay(0x78);
                mission_end(1);
            }
        }
    }

    /* terrain collision */
    if (DSB(0x9F66) != 0 && !(DSW(0xC07A) & 0x80)) {
        if (pilot_w(0x40) != 0 && DSW(0x2DF6) != 0) {
            snd_cue(4, 2);
            screen_off();
            frame_delay(0x78);
            mission_end(2);
        } else {
            DSW(0x2DF6) = (u16)(DSW(0x2DF6) + 0x1F4);
            DSW(0x3652) = 0;
        }
    }

    /* the strip beacon phase */
    if ((DSB((u16)(SITE(DSW(0xDFE0)) + 9)) & 2) && (s16)DSW(0x98F2) < 0x500)
        DSW(0xDFDE) = (u16)(((s16)(((u16)DSB(0xB16E)) << 8) / (s16)DSW(0x3660) + DSW(0xDFDE)) & 0x0FFF);
    else DSW(0xDFDE) = 0;

    /* the mission clock, once per rate steps */
    DSW(0x3D60)++;
    if ((s16)DSW(0x3D60) % (s16)DSW(0x3660) == 0) {
        DSW(0x98E2)++;
        if (DSW(0x98E2) == 1) sim_snd_cmd(0x2C, 0, 0, 0);
        pal_time_of_day_step();
        for (int i = 0; i < 2; i++) {
            u16 slot = (u16)(0xE2D8 + 0x12 * i);
            if (!(DSB(slot + 6) & 0x20)) continue;
            if ((s16)DSW(0x98E2) < (s16)DSW(0xDE88)) continue;
            if (DSW(0xC0A2) == DSW(0x2DF4)) continue;
            u16 s = SITE(DSW(slot + 2));
            if (DSB(s + 8) & 0x80) continue;
            s32 x = (s32)((u32)DSW(s + 2) << 5);
            s32 y = (s32)((u32)((s32)0x8000 - (s32)DSW(s + 4)) << 5);
            u16 cell = terr_find_scenery_at(x, y);
            DSW(0x9F10) = cell;
            if (cell) terr_set_scenery_state(cell, (u16)((s8)DSB(0xDEA4) + 0x100));
            DSB((u16)(SITE(DSW(slot + 2)) + 8)) |= 0x80;
        }
        if ((DSW(0x98E2) & 0x1F) == 0) mission_log_event(9, 0);
    }

    /* the frame-rate estimator, every 4 * rate steps */
    DSW(0x3D78)++;
    if ((u16)(DSW(0x3660) << 2) <= DSW(0x3D78)) {
        DSW(0xE54E) = (u16)(DSW(0x2646) / DSW(0x3660));
        DSW(0x2646) = (u16)(DSW(0x2646) + (u16)((u16)(1 - (DSW(0x43B8) << 1)) * DSW(0x3660) << 1));
        if (DSW(0x2646) < 4) DSW(0x2646) = 4;
        u16 divisor = (u16)(DSW(0x2646) * DSW(0x3D7A));
        u16 q = (u16)((u16)(DSW(0x3660) * 0x3C0) / divisor);
        s16 a = (s16)fx_clamp_wrap((s16)q, 1, 255);
        DSW(0x2646) = 0;
        DSW(0x3D78) = 0;
        if (abs16((s16)((u16)(DSW(0x3660) << 2) - a)) > 3) {
            DSW(0x3660) = (u16)((a + 2) >> 2);
            frame_params_update();
        }
        DSW(0xDED6) = 0;
        for (int i = 3; (s16)DSW(0x98FC) > i; i++) {
            u16 s = SITE(i);
            if ((s16)DSW(s + 10) > 0xC0 && !(DSB(s + 8) & 0x80)) { DSW(0xDED6)++; break; }   /* the original leaves each loop at the first hit (no jump back) */
        }
        for (int k = 0; (s16)DSW(0xDED0) > k; k++) {
            u16 o = UNIT(k);
            if ((s16)DSW(o + 0x22) > 0xC0 && (DSB(o + 0x18) & 2)) { DSW(0xDED6)++; break; }
        }
    }
    sim_key_and_signature();
}
