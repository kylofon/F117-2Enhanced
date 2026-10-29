/* The view layer of one frame (spec simflow.md Part 3 "views"): the camera of every view mode, the view matrix, the cockpit picture loads, the call into the 3D renderer and the
 * cockpit pieces pasted over its window. The renderer itself (03CE) is render3d's (render3d/r3d_scene.c). */
#include <string.h>

#include "enhanced/enh_canvas.h"   /* ENH: */
#include "enhanced/interp.h"       /* ENH: smooth clips */
#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "sim/simtest.h"

#define LONG(o) ((s32)DSL(o))
#define SETL(o, v) (DSL(o) = (u32)(v))
#define POSE(i, f) ((u16)(0xB056 + 16 * (i) + (f)))

/* 0000:E26B r3d_matrix_from_angles (render3d.md "3. Matrices"; owner render3d, S5.6 takes it over): m = the 3x3 matrix of the angles (yaw a, pitch b, roll c), the six sin/cos
 * words go to 4982..498C */
void sim_matrix_from_angles(u16 m_off, int a, int b, int c)
{
    s16 s1 = (s16)fx_sin16(a), c1 = (s16)fx_cos16(a), s2 = (s16)fx_sin16(b), c2 = (s16)fx_cos16(b), s3 = (s16)fx_sin16(c), c3 = (s16)fx_cos16(c);
    DSW(0x4982) = (u16)s1; DSW(0x4984) = (u16)c1; DSW(0x4986) = (u16)s2; DSW(0x4988) = (u16)c2; DSW(0x498A) = (u16)s3; DSW(0x498C) = (u16)c3;
#define MQ(x, y) ((s16)fx_mul15((x), (y)))
    s16 t_s2s3 = MQ(s2, s3), t_s2c3 = MQ(s2, c3);
    s16 m[9];
    m[0] = (s16)(MQ(c1, c3) + MQ(s1, t_s2s3));
    m[1] = (s16)(MQ(s1, t_s2c3) - MQ(c1, s3));
    m[2] = MQ(s1, c2);
    m[3] = MQ(s3, c2);
    m[4] = MQ(c3, c2);
    m[5] = (s16)-s2;
    m[6] = (s16)(MQ(c1, t_s2s3) - MQ(s1, c3));
    m[7] = (s16)(MQ(c1, t_s2c3) + MQ(s3, s1));
    m[8] = MQ(c1, c2);
#undef MQ
    for (int i = 0; i < 9; i++) DSW((u16)(m_off + 2 * i)) = (u16)m[i];
}

/* 0000:03CE r3d_scene_draw is render3d's (render3d/r3d_scene.c). The sim flow's differential tests (port/notes/s55/sim_ref.py) still hook the original at
 * its address (the marker below) and the port returns at once while sim_test.active, so the view tests stay independent of the renderer; --render-test runs both. */
/* TESTSTUB 0000:03ce r3d_scene_draw */

/* 0000:2F82 view_frame (spec simflow.md Part 3 sections 2.2..2.12) */
void view_frame(void)
{
    int h = 0x5B;
    int z;
    SETL(0xDE98, LONG(0xBA10)); SETL(0xB288, LONG(0xBA10));
    SETL(0xB752, LONG(0xC02A));
    SETL(0xDE9E, 0x100000 - LONG(0xC02A));
    DSW(0xB75C) = (u16)(DSW(0x2DF4) + 0x18);
    DSW(0xDEA6) = DSW(0x2DF4);
    z = (s16)fx_clamp_wrap((s16)DSW(0x3D70), 2, 8);
    DSW(0x3D70) = (u16)z;
    u16 mode = DSW(0xC07A);
    if (enh_shadow_on) enh_set_view(mode);   /* ENH: canvas placement per view */
    u16 yaw = DSW(0x2DEE), pitch = DSW(0x2DF0), roll = DSW(0x2DF2);

    switch (mode) {
    case 0: case 0x44:
        DSW(0xE44C) = yaw; DSW(0xC07C) = pitch; DSW(0xB770) = roll;
        break;
    case 0x41:
        DSW(0xE44C) = (u16)(yaw + 0x8000); DSW(0xC07C) = (u16)-pitch; DSW(0xB770) = (u16)-roll;
        break;
    case 0x42:
        DSW(0xE44C) = (u16)(yaw - 0x4000); DSW(0xC07C) = roll; DSW(0xB770) = (u16)-pitch;
        break;
    case 0x43:
        DSW(0xE44C) = (u16)(yaw + 0x4000); DSW(0xC07C) = (u16)-roll; DSW(0xB770) = pitch;
        break;
    case 0x84: {
        int i = (DSB(0x3D60) - DSB(0x3660)) & 15;
        DSW(0xE44C) = DSW(POSE(i, 10)); DSW(0xC07C) = DSW(POSE(i, 12)); DSW(0xB770) = DSW(POSE(i, 14));
        SETL(0xB288, LONG(POSE(i, 0))); SETL(0xB752, LONG(POSE(i, 4)));
        DSW(0xB75C) = DSW(POSE(i, 8));
        break;
    }
    case 0x85: {
        int d = (s16)(0x18 << z);
        DSW(0xE44C) = (u16)(yaw - 0x4000); DSW(0xC07C) = 0; DSW(0xB770) = 0;
        SETL(0xB288, (s32)fx_sin_mul((u16)(yaw + 0x4000), d) + LONG(0xBA10));
        SETL(0xB752, (s32)fx_cos_mul((u16)(yaw + 0x4000), d) + LONG(0xC02A));
        break;
    }
    case 0x86: {
        int d = (s16)(0x18 << z);
        DSW(0xE44C) = 0x8000; DSW(0xC07C) = 0; DSW(0xB770) = 0;
        SETL(0xB752, (s32)(s16)d + LONG(0xC02A));
        break;
    }
    case 0x87: {
        int d = (s16)(0x18 << z);
        DSW(0xE44C) = yaw; DSW(0xC07C) = 0; DSW(0xB770) = 0;
        SETL(0xB288, (s32)fx_sin_mul((u16)(yaw + 0x8000), d) + LONG(0xBA10));
        SETL(0xB752, (s32)fx_cos_mul((u16)(yaw + 0x8000), d) + LONG(0xC02A));
        DSW(0xB75C) = (u16)((4 << z) + DSW(0x2DF4));
        break;
    }
    case 0x88: case 0x89: case 0x8B: {
        if (mode != 0x89) {
            if (DSW(0xE2FE) == 1) { if (!(DSB(0x3D64) & 0x80)) DSW(0xDEA8) = (u16)(DSW(0x3D64) + 0x80); }
            else if (!(DSB(0x3D66) & 0x80)) DSW(0xDEA8) = (u16)(DSW(0x3D66) + 0x100);
        } else DSW(0xDEA8) = DSW(0xB04C);
        u16 sel = DSW(0xDEA8);
        if (sel & 0x100) {                                                /* a ground site */
            u16 r = (u16)(0xB2A0 + 16 * (sel & 0x7F));
            SETL(0xDE98, (u32)DSW(r + 4) << 5);
            SETL(0xDE9E, (u32)DSW(r + 6) << 5);
            DSW(0xDEA6) = 0x32;
        } else if (sel & 0x80) {                                          /* an air object */
            u16 r = (u16)(0xC140 + 0x24 * (sel & 0x7F));
            SETL(0xDE98, LONG(r + 6));
            SETL(0xDE9E, LONG(r + 0xA));
            DSW(0xDEA6) = DSW(r + 4);
        } else {                                                          /* a weapon */
            u16 r = (u16)(0x3C0C + 0x1C * sel);
            if (DSW(r + 0xE) != 0) {
                u32 wx = (u32)DSW(r) << 5, wy = (u32)DSW(r + 2) << 5;
                enh_interp_proj_world(sel, &wx, &wy);                     /* ENH: in a display pass, between whole cells */
                SETL(0xDE98, wx);
                SETL(0xDE9E, wy);
                DSW(0xDEA6) = DSW(r + 4);
            } else { DSW(r + 8) = DSW(0x2DEE); DSW(r + 0xA) = DSW(0x2DF0); }
        }
        s16 dx = (s16)((LONG(0xDE98) >> 5) - (s16)DSW(0xC0A4));
        s16 dy = (s16)((LONG(0xDE9E) >> 5) - (s16)DSW(0xC0B2));
        s16 dist = (s16)fx_dist(dx, dy);
        DSW(0xE44C) = fx_heading(dx, (s16)-dy);
        DSW(0xC07C) = (u16)-(s16)fx_heading((s16)((s16)(DSW(0xDEA6) - DSW(0x2DF4)) >> 5), dist);
        DSW(0xB770) = 0;
        int d = (s16)(0x18 << z);
        s16 hd = (s16)fx_cos_mul(DSW(0xC07C), d);
        if (sel & 0x180) {
            if (mode == 0x88) {
                SETL(0xB288, (s32)fx_sin_mul((u16)(DSW(0xE44C) + 0x8000), hd) + LONG(0xBA10));
                SETL(0xB752, (s32)fx_cos_mul((u16)(DSW(0xE44C) + 0x8000), hd) + LONG(0xC02A));
                DSW(0xB75C) = (u16)(((4 << z) + fx_sin_mul(DSW(0xC07C), d)) + DSW(0x2DF4));
                DSW(0xC07C) = (u16)-DSW(0xC07C);
            } else {
                SETL(0xB288, (s32)fx_sin_mul(DSW(0xE44C), hd) + LONG(0xDE98));
                SETL(0xB752, (s32)fx_cos_mul(DSW(0xE44C), hd) - LONG(0xDE9E) + 0x100000);
                DSW(0xB75C) = (u16)(((4 << z) - fx_sin_mul(DSW(0xC07C), d)) + DSW(0xDEA6));
                DSW(0xE44C) = (u16)(DSW(0xE44C) + 0x8000);
            }
        } else {                                                          /* chase camera behind the weapon */
            u16 r = (u16)(0x3C0C + 0x1C * sel);
            DSW(0xE44C) = DSW(r + 8);
            DSW(0xC07C) = (u16)(DSW(r + 0xA) - 0x400);
            int dd = (s16)(0x10 << z);
            s16 hr = (s16)fx_cos_mul(DSW(0xC07C), dd);
            SETL(0xB288, LONG(0xDE98) - (s32)fx_sin_mul(DSW(0xE44C), hr));
            SETL(0xB752, 0x100000 - (s32)fx_cos_mul(DSW(0xE44C), hr) - LONG(0xDE9E));
            DSW(0xB75C) = (u16)-(s16)(fx_sin_mul(DSW(0xC07C), dd) - DSW(0xDEA6));
        }
        break;
    }
    case 0x8C:
        DSW(0xC07C) = 0xF400; DSW(0xB770) = 0;
        SETL(0xB288, (u32)(s32)(s16)DSW(0xDEA2) << 5);
        SETL(0xB752, (u32)(0x8000 - (s32)(s16)DSW(0xDEC6)) << 5);
        DSW(0xB75C) = DSW(0xDECA);
        break;
    default: break;                                                       /* stale camera (unreachable values) */
    }

    /* keep the pitch inside 90 degrees */
    {
        s16 p = (s16)DSW(0xC07C);
        s16 ap = (s16)(p < 0 ? -p : p);
        if (ap > 0x4000 || DSW(0xC07C) == 0x8000) {
            DSW(0xC07C) = (u16)(0x8000 - DSW(0xC07C));
            DSW(0xE44C) = (u16)(DSW(0xE44C) + 0x8000);
            DSW(0xB770) = (u16)(0x8000 - DSW(0xB770));
        }
    }
    if (mode == 0) memcpy(DSP(0xB156), DSP(0x2D94), 18);
    else sim_matrix_from_angles(0xB156, (s16)DSW(0xE44C), (s16)DSW(0xC07C), (s16)DSW(0xB770));
    if ((s16)DSW(0xB75C) < 0x10) DSW(0xB75C) = 0x10;

    /* cockpit class change */
    u16 old = DSW(0x365E);
    DSW(0x365E) = (DSB(0xC07A) & 0xC0) == 0;
    if (DSW(0x365E) != old) {
        u16 y2 = DSW(0x365E) ? 0x5A : 0xC7;
        DSW((u16)(DSW(0x3FE2) + 0x10)) = y2;
        DSW((u16)(DSW(0x3FFA) + 0x10)) = y2;
        screen_off();
        if (DSW(0x365E)) {
            gfx_restore_rect(0, 0x5B, 0x140, 0x6D);
            hud_draw_throttle();
            if (DSW(0xDEDA) == 0) mfd_map_redraw((s16)DSW(0xC0A4), (s16)DSW(0xC0B2));
            if (DSW(0xDFDC) == 0x13) {
                DSW(0x3D64) = DSW(0x3D66) = 0xFFFF;
                mfd_fill(2, 0x9F);
                DSW(0x9D0E) = 0;
            }
        } else {
            blit_rect(DSW(DSW(0x4012)), 0, 0x96, DSW(DSW(0x3FE2)), 0, 0x96, 0x21, 0x32);
            blit_rect(DSW(DSW(0x3FE2)), 0, 0x5B, DSW(DSW(0x4012)), 0, 0x5B, 0x140, 0x6D);
        }
        screen_on();
    }

    /* cockpit picture change */
    if (mode != DSW(0x2E48)) {
        screen_off();
        if ((u8)mode == 0) {
            pic_show("256pit.pic", 2);
            gfx_restore_rect(0, 0x5B, 0x140, 0x6D);
            if (DSW(0xBA0E)) sprite_scaled(DSP(DSW(0x4012)), 0xA9, 0x6E, 0x4D, 0x3C, 0, DSW(0xBA0E));
            cockpit_refresh_panels();
            cockpit_daynight_recolor();
            pal_mfd_ramp_green();
            DSW(0x46C2) = 0;
        } else if (mode == 0x41 || mode == 0x42 || mode == 0x43) {
            pic_show(mode == 0x41 ? "256rear.pic" : mode == 0x42 ? "256left.pic" : "256right.pic", 2);
            blit_rect(2, 0, 0, 0, 0, 0, 0x140, 0xC8);
        } else pal_mfd_ramp_rt();
        screen_on();
        DSW(0x2E48) = mode;
    }

    /* the viewport and the scene */
    DSB(0xC686) = 0;
    if (DSW(0x365E)) h = DSW(0x2DFC) ? 0x6B : 0x5B;
    else h = mode == 0x41 ? 0x9D : (mode == 0x42 || mode == 0x43) ? 0xA1 : 0xC8;
    u8 saved294b = DSB(0x294B);
    if (mode != 0) DSB(0x294B) = 0;
    r3d_scene_draw((s16)-DSW(0xE44C), (s16)DSW(0xC07C), (s16)DSW(0xB770), LONG(0xB288), LONG(0xB752), (s16)DSW(0xB75C), 0, 0, 0x140, h);
    DSB(0x433A) = 0;
    if (DSW(0x2E00)) DSW(0x2E00)--;
    else DSB(0x9F66) = DSB(0xC686);
    DSB(0x294B) = saved294b;

    /* the cockpit pieces over the window */
    int p = (s8)DSB(0x4088);
    if (DSW(0x365E)) {
        blit_rect(p, 0x4A, 0x6E, 1 - p, 0x4A, 0x6E, 0x4D, 0x3C);
        blit_rect(p, 0xA9, 0x6E, 1 - p, 0xA9, 0x6E, 0x4D, 0x3C);
        if (DSW(0x2DFC)) {
            blit_rect(2, 0x20, 0x5B, p, 0x20, 0x5B, 0x100, 0x10);
            cockpit_keyblit(0, 0x5B, 0, 0x4A, 0x20, 0x10, 0);
            cockpit_keyblit(0x120, 0x5B, 0x120, 0x4A, 0x20, 0x10, 0);
        }
    }
    switch (mode) {
    case 0x41:
        blit_rect(2, 0x5B, 0, p, 0x5B, 0, 0x8A, 0x9D);
        cockpit_keyblit(0, 0, 0, 0, 0x5B, 0x9D, 0);
        cockpit_keyblit(0xE5, 0, 0xE5, 0, 0x5B, 0x9D, 0);
        break;
    case 0x42: case 0x43:
        cockpit_keyblit(0, 0, 0, 0, 0x140, 0xA1, 0);
        break;
    case 0x44:
        poly_fill_far(DSP(DSW(0x344)), 4, (const s16 *)DSP(0x2E38), 5);
        break;
    default: break;
    }
    DSW(0x2E34) = (u16)h;
}
