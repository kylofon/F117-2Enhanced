/* The world-object pass of render3d (spec port/spec/render3d.md Part 2): 9ACA (own aircraft, the two designator blocks of the weapons logic, smoke billboards,
 * units with their shadows, projectiles, the falling wreck), the depth helper BA67 and the TV-camera effects C73C / C834. B85D / BA13 (the point projection) are
 * in sim/sim_objovl.c (written by S5.5 for the overlay). The range / bearing helpers bbb4 / bb86 / bb9e are the weapons module's (flight/fl_weapons.c). */
#include "render3d/r3d.h"
#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "enhanced/interp.h"   /* ENH: smooth clips, projectiles between whole cells */

#define LONG(o) ((s32)DSL(o))

static inline s16 abs16(s16 v) { return (s16)(v < 0 ? (u16)-(u16)v : (u16)v); }
static inline s16 mq(s16 a, s16 b) { return (s16)fx_mul15(a, b); }


/* 0000:BA67 r3d_view_depth: [DE92] = the depth of the point against the third row of the cull matrix (the z-term sign quirk is kept) */
void r3d_view_depth(s16 px, s16 py, s16 pz, s16 cx, s16 cy, s16 cz)
{
    s16 a = (s16)(py - cy), b = (s16)((s16)(pz - cz) >> 5), c = (s16)(cx - px);
    s32 s = (s32)mq(DSS(0x4992), c) + (s32)mq(DSS(0x4998), b) + (s32)mq(DSS(0x499E), a);
    DSW(0xDE92) = (u16)(s16)(s >> 3);
}

/* 0000:C73C fx_fire_puffs: six fire puffs around a burning air target (TV camera); consumes the game RNG in the original order */
void fx_fire_puffs(s16 hdg, s16 pitch, s16 roll, s16 X, s16 Y, s16 Z)
{
    for (int i = 0; i < 6; i++) {
        u16 R = (u16)(0x3A5C + 8 * i);
        if (DSW((u16)(R + 6)) == 0 || DSS((u16)(R + 4)) >= 6) {
            DSW(R) = DSW((u16)(0x436A + 2 * rand_n(7)));
            DSW((u16)(R + 2)) = DSW((u16)(0x436A + 2 * rand_n(7)));
            DSW((u16)(R + 4)) = (u16)(rand_n(2) + 1);
            DSW((u16)(R + 6)) = 1;
            continue;
        }
        s16 h = (s16)(hdg + DSS(R)), p = (s16)(pitch + DSS((u16)(R + 2)));
        if (abs16(p) > 0x4000 || p == (s16)0x8000) { p = (s16)(0x8000 - (u16)p); h = (s16)(h + 0x8000); }
        u16 f = DSW((u16)(R + 4));
        DSW((u16)(R + 4)) = (u16)(f + 1);
        if (i < 4) r3d_place_object((u16)(0x25 + f), h, p, roll, X, Y, Z);
        else { DSW(0xC040) = f; r3d_place_object(0x24, h, p, roll, X, Y, Z); }
    }
}

/* 0000:C834 fx_smoke_draw: the smoke column of the burning site as sorted 25h objects for the TV camera */
void fx_smoke_draw(s16 cx, s16 cy, s16 cz)
{
    if (DSW(0x3D68) == 0xFFFF) return;
    for (int i = 0; i < 16; i++) {
        u16 P = (u16)(0x39DA + 8 * i);
        if (DSW(P) == 0) continue;
        r3d_view_depth(DSS(P), DSS((u16)(P + 2)), DSS((u16)(P + 4)), cx, cy, cz);
        s16 d = DSS(0xDE92);
        if (!(d < 0 && d > -0x100)) continue;
        s16 size = (s16)fx_clamp_wrap(lib_scale_eighths(DSS((u16)(P + 6)), abs16((s16)(0x200 / d))), 4, 0xFF);
        DSW(0x49C6) = (u16)(((size & 0xFF) << 8) | ((DSW((u16)(P + 6)) & 0x0E) >> 1));
        r3d_queue_object(0x25, (s32)((u32)DSW(P) << 5), (s32)((u32)DSW((u16)(P + 2)) << 5), DSS((u16)(P + 4)), 0, 0, 0);
    }
}

/* TILE(): the 16x16 terrain-class grid of the .WLD */
static u16 tile_class(u16 xu, u16 yu) { return DSB((u16)(0xB174 + (yu >> 11) * 16 + (xu >> 11))) & 3; }
static s16 cone(void) { return abs16((s16)(DSW(0xE556) - DSW(0x9502) + DSW(0x2DEE))); }
static s32 own_y32(void) { return (s32)(0x01000000u - (u32)LONG(0xC02A)); }

/* 0000:9ACA r3d_draw_world_objects */
void r3d_draw_world_objects(void)
{
    s16 lim, cur_d, best, hi;
    /* (1) own aircraft, view 0x8B only */
    if (DSW(0xC07A) == 0x8B) {
        DSW(0x49C6) = tile_class(DSW(0xC0A4), DSW(0xC0B2));
        r3d_draw_model(0x18, LONG(0xBA10), own_y32(), (s16)(DSW(0x2DF4) + 0x10), DSS(0x2DEE), DSS(0x2DF0), DSS(0x2DF2), 4);
    }
    if (DSW(0xB28E) != 0) DSW(0xB28E)--;

    /* (A) ground-target designator (weapons logic) */
    if ((DSB(0x3D66) & 0x80) && (DSW(0xDFDC) == 0x13 || DSW(0xDFDC) == 0x19) && DSW(0xB28E) == 0 && DSW(0xE2FE) != 1 && !(DSB(0xC07A) & 0x80)) {
        best = -1;
        lim = (s16)(u16)(100u << ((6 - DSB(0x3658)) & 0x1F));
        cur_d = 0;
        if (DSW(0x3D66) != 0xFFFF) {
            cur_d = (s16)(wpn_site_dist((s16)(DSW(0x3D66) - 0x80)) - 1);
            if (cone() > 0x2000) cur_d = 0;
        }
        for (s16 i = 0; i < DSS(0xC0B4); i++) {
            wpn_site_dist(i);
            if (!(cone() < 0x2000 || (DSW(0xE2FE) == 0 && cur_d > 0))) continue;
            if ((s16)(i - DSW(0x3D66)) == -0x80) continue;
            u16 fl = DSW((u16)(0xB2A4 + 16 * i + 6));
            if (fl & 0x80) continue;
            if (DSS(0x94FE) >= lim) continue;
            if (DSS(0x94FE) <= cur_d) continue;
            if (DSW(0xE2FE) != 2 && (s16)(fl & 0x501) <= 1) continue;
            if (i == 0 && (DSW(0xB2A4) == 0 || DSW(0xB2A6) == 0 || DSW(0xB2B0) == 0)) continue;
            best = i;
            lim = DSS(0x94FE);
        }
        if ((u8)best & 0x80) {
            if (DSW(0x3D66) == 0xFFFF) DSW(0xB28E) = 4;
            else DSW(0x3D66) = 0xFFFF;
        } else DSW(0x3D66) = (u16)best;
    }

    /* (B) smoke puffs as billboards ([49C6] stale) */
    for (int i = 0; i < 16; i++) {
        u16 P = (u16)(0x39DA + 8 * i);
        if (DSW(P) == 0) continue;
        r3d_project_point(DSS(P), DSS((u16)(P + 2)), DSS((u16)(P + 4)));
        s16 d = DSS(0xDE92);
        if (DSW(0x49E0) == 0xFFFF || !(d < 0 && d > -0x100)) continue;
        s16 size = (s16)lib_scale_eighths(DSS((u16)(P + 6)), abs16((s16)(0x100 / d)));
        if (enh_shadow_on) enh_bill_size(DSS((u16)(P + 6)), d);   /* ENH: S3.1 */
        r3d_queue_billboard((u16)(0x400 + ((DSW((u16)(P + 6)) & 0x0E) >> 1)), (s32)((u32)DSW(P) << 5), (s32)((u32)DSW((u16)(P + 2)) << 5), DSS((u16)(P + 4)),
                            DSS(0x49E0), DSS(0x49E8), size);
    }
    lim = (s16)(u16)(0x4Bu << ((6 - DSB(0x3658)) & 0x1F));
    hi = (DSW(0x365E) != 0 && (u16)(DSW(0x2DF4) + DSW(0x98F2)) > 0x5DC) ? 1 : 0;

    /* (C1) air-target designator set-up (weapons logic) */
    if ((DSB(0x3D64) & 0x80) && DSW(0x3D64) != 0xFFFF) {
        u16 k = (u16)(DSW(0x3D64) - 0x80), U = (u16)(0xC140 + 0x24 * k);
        cur_d = (s16)obj_dist_bearing(DSS(U), DSS((u16)(U + 2)), 1);
        if (cone() > 0x2000) cur_d = 0;
    } else cur_d = 0;
    best = -1;

    /* (D) units */
    for (s16 i = 0; i < DSS(0xDED0); i++) {
        u16 U = (u16)(0xC140 + 0x24 * i);
        DSW(0x49C6) = 0;
        if (!(DSB((u16)(U + 0x16)) & 2)) continue;
        if ((s16)wpn_unit_dist(i) >= 0x12C0) continue;
        s16 dd = DSS(0x94FE);
        if (((DSB(0x3D64) & 0x80) || dd < 0x100) && dd < lim && dd > cur_d && !(DSB(0xC07A) & 0x80) && !(DSB((u16)(U + 0x16)) & 0x20) && DSW((u16)(U + 0x18)) != 0) {
            obj_dist_bearing(DSS(U), DSS((u16)(U + 2)), 1);
            if (cone() < 0x2000) { lim = DSS(0x94FE); best = i; }
        }
        r3d_project_point(DSS(U), DSS((u16)(U + 2)), DSS((u16)(U + 4)));
        if (DSS(0xDE92) >= 0 || DSW(0x49E0) == 0xFFFF) continue;
        DSW(0xDE92) = (u16)(DSS(0xDE92) >> hi);
        if (DSS(0xDE92) <= -0x20) {
            gfx_set_fg(0x0F);
            if (enh_shadow_on) enh_dot_begin();   /* ENH: S3.1, the dot at N x */
            hud_line_view(DSS(0x49E0), DSS(0x49E8), DSS(0x49E0), DSS(0x49E8));
            if (enh_shadow_on) enh_dot_end(0x0F);   /* ENH: S3.1 */
            continue;
        }
        if (DSS((u16)(U + 4)) < 0x3E7 && DSW(0x3658) == 0) {
            s16 shadow_alt = 0;
            u16 s = (u16)(0xB2A4 + 16 * DSW(0xDFE0));
            if ((DSB((u16)(s + 7)) & 2) && abs16((s16)(DSW(U) - DSW(s))) < (s16)(DSS(0x9900) >> 5) && abs16((s16)(DSW((u16)(U + 2)) - DSW((u16)(s + 2)))) < (s16)(DSS(0x9B06) >> 5))
                shadow_alt = 0x80;
            if (!(DSW(0x2DF4) == 0x80 && shadow_alt != 0x80))
                r3d_draw_model(4, LONG((u16)(U + 6)), LONG((u16)(U + 0xA)), shadow_alt, DSS((u16)(U + 0xE)), 0, 0, (s16)(4 - lib_sign(hi)));
        }
        r3d_unit_terrain_class((u16)i);
        if (DSB((u16)(U + 0x16)) & 0x20) DSB(0x49C6) |= 0x20;
        s16 alt = DSS((u16)(U + 4));
        if (!(DSB(0xC07A) & 0x80)) alt = (s16)(alt - 0x10);
        u16 far_ = DSS(0xDE92) <= -0x10 ? 1 : 0;
        r3d_draw_model(DSW((u16)(0x2FDC + 0x20 * DSW((u16)(U + 0x14)) + 2 * far_)), LONG((u16)(U + 6)), LONG((u16)(U + 0xA)), alt, DSS((u16)(U + 0xE)), DSS((u16)(U + 0x10)),
                       DSS((u16)(U + 0x12)), (s16)(4 - hi));
    }
    DSW(0x49C6) = 0;
    /* (C3) air-target commit (weapons logic) */
    if (best != -1) DSW(0x3D64) = (u16)best;
    if (DSB(0x3D64) & 0x80) DSW(0x3D64) = 0xFFFF;

    /* (E) projectile slots */
    for (int i = 0; i < 12; i++) {
        u16 Wp = (u16)(0x3C0C + 0x1C * i);
        if (DSW((u16)(Wp + 0xE)) == 0) continue;
        r3d_project_point(DSS(Wp), DSS((u16)(Wp + 2)), DSS((u16)(Wp + 4)));
        if (DSW(0x49E0) == 0xFFFF) continue;
        if (DSS(0xDE92) <= -0x20) {
            gfx_set_fg(i < 8 ? 0x29 : 0x0D);
            if (enh_shadow_on) enh_dot_begin();   /* ENH: S3.1 */
            hud_line_view(DSS(0x49E0), DSS(0x49E8), DSS(0x49E0), DSS(0x49E8));
            if (enh_shadow_on) enh_dot_end(i < 8 ? 0x29 : 0x0D);   /* ENH: S3.1 */
            continue;
        }
        u32 wx = (u32)DSW(Wp) << 5, wy = (u32)DSW((u16)(Wp + 2)) << 5;
        enh_interp_proj_world(i, &wx, &wy);   /* ENH: in a display pass, between whole cells */
        r3d_draw_model(DSW((u16)(0x3372 + 0x12 * DSW((u16)(Wp + 0x10)))), (s32)wx, (s32)wy, DSS((u16)(Wp + 4)),
                       DSS((u16)(Wp + 8)), DSS((u16)(Wp + 0xA)), (s16)(DSW((u16)(Wp + 0xC)) + 0x2000), (s16)(3 - lib_sign(hi)));
    }
    /* (F) falling wreck (DE92 may be stale: quirk) */
    if (DSS(0xC602) > 0) {
        r3d_project_point(DSS(0xC0A6), DSS(0xC0B8), DSS(0xC602));
        if (DSS(0xDE92) < 0 && DSS(0xDE92) > -0x100)
            r3d_draw_model(0x0B, (s32)((u32)DSW(0xC0A6) << 5), (s32)((u32)DSW(0xC0B8) << 5), DSS(0xC602), 0, 0, 0, DSS(0xB756) > 0 ? 6 : 5);
    }
    /* (G) own aircraft in the other external views */
    if ((DSB(0xC07A) & 0x80) && DSW(0xC07A) != 0x8B && !(DSW(0x2DF4) == 0 && DSW(0xC06E) != 0)) {
        DSW(0x49C6) = tile_class(DSW(0xC0A4), DSW(0xC0B2));
        r3d_draw_model(0x18, LONG(0xBA10), own_y32(), (s16)(DSW(0x2DF4) + 0x10), DSS(0x2DEE), DSS(0x2DF0), DSS(0x2DF2), 4);
        if (DSW(0x2DF4) < 0x3E8 && DSW(0x3658) == 0)
            r3d_draw_model(0x11, LONG(0xBA10), own_y32(), DSS(0xC0A2), DSS(0x2DEE), 0, 0, 4);
    }
}
