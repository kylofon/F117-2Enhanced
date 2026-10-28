/* The multi-function displays (spec simflow.md Part 7 sections 2 and 3, Part 3 section 8, Part 5 section 4): the left MFD (moving map / scope) and the right MFD
 * pages (TV, navigation, weapons, objectives) with their selection, zoom and map primitives. The cockpit map's terrain drawing (119C) is render3d's (render3d/r3d_terrain.c). */
#include <stdio.h>
#include <string.h>

#include "gen/module_render3d.h"
#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "sim/sim_stubs.h"
#include "enhanced/enh_canvas.h"   /* ENH: S3.2 */
#include "enhanced/mapmfd.h"       /* ENH: S4.6 */

#define SITES 0xB2A2                                     /* 16-byte site records */
#define SITE(i) ((u16)(SITES + 16 * (i)))

static inline void *ctx_ptr(u16 ptr_off) { return DSP(DSW(ptr_off)); }
static inline char *strbuf(void) { return ds_str(0x9876); }   /* the shared scratch string */

/* ------------------------------------------------------------------------------------------------------------ the moving map */

/* 0000:8514 mfd_map_sx / 852B mfd_map_sy: world to the map window (zoom [4080]; the window centre is (112, 139), the pixel aspect 3/4) */
int mfd_map_sx(int wx)
{
    int sh = (10 - (s8)DSB(0x4080)) & 31;
    return (s16)((((s16)(wx - DSW(0x4084))) >> sh) + 0x70);
}
int mfd_map_sy(int wy)
{
    int sh = (10 - (s8)DSB(0x4080)) & 31;
    s16 v = (s16)(((s16)(wy - DSW(0x4086))) >> sh);
    return (s16)((((s16)(v * 3)) >> 2) + 0x8B);
}

/* 0000:8548 mfd_map_plot: a dot (2x2 when `big`) in the map window; 1 when the point is outside the window */
int mfd_map_plot(int wx, int wy, int color, int big)
{
    if (enh_map_log_on) enh_map_log_plot(wx, wy, color, big);   /* ENH: S4.6 */
    if (DSW(0xDEDA) || DSW(0x365E) == 0) return 0;
    int sx = mfd_map_sx(wx), sy = mfd_map_sy(wy);
    if (sx < (s16)DSW(0xDE94) || sx >= (s16)DSW(0xE302) - 1 || sy < (s16)DSW(0xDE96) || sy >= (s16)DSW(0xE444) - 1) return 1;
    if ((s16)color == -1) return 0;
    hud_pixel(sx, sy, color);
    if (big) {
        hud_pixel(sx + 1, sy, color);
        hud_pixel(sx, sy + 1, color);
        hud_pixel(sx + 1, sy + 1, color);
    }
    return 0;
}

/* 0000:85ED mfd_map_to_screen: 1 when the point is strictly inside the map window */
int mfd_map_to_screen(int wx, int wy, int *sx, int *sy)
{
    if (DSW(0x365E) == 0) return 0;
    s16 x = (s16)mfd_map_sx(wx), y = (s16)mfd_map_sy(wy);
    if (sx) *sx = x;
    if (sy) *sy = y;
    return x > (s16)DSW(0xDE94) && x < (s16)DSW(0xE302) - 1 && y > (s16)DSW(0xDE96) && y < (s16)DSW(0xE444) - 1;
}

/* 0000:86E5 mfd_map_line: a world line, clipped to the map window, on both pages */
void mfd_map_line(int wx0, int wy0, int wx1, int wy1)
{
    hud_line_clip(mfd_map_sx(wx0), mfd_map_sy(wy0), mfd_map_sx(wx1), mfd_map_sy(wy1), (s16)DSW(0xDE94), (s16)DSW(0xE302), (s16)DSW(0xDE96), (s16)DSW(0xE444), 1);
}

/* 0000:863C mfd_map_arc: an arc of a circle of radius r world units, angles in 1/256 turn, dots or a solid polyline */
void mfd_map_arc(int cx, int cy, int r, int color, int solid, int a0, int a1)
{
    if (enh_map_log_on) enh_map_log_arc(cx, cy, r, color, solid, a0, a1);   /* ENH: S4.6 */
    int prev_x = 0, prev_y = 0;
    a0 = (s16)a0; a1 = (s16)a1;
    if (a1 < a0) a0 = (s16)((a0 & 0xFF) | (((a0 >> 8) + 1) & 0xFF) << 8);     /* the original adds 1 to the HIGH byte of a0 (not a1): then the loop does not run */
    gfx_set_fg(color);
    for (int a = a0; a <= a1; a += 0x10) {
        u16 ang = (u16)((a & 0xFF) << 8);
        u16 px = (u16)(cx + fx_sin_mul(ang, r));
        u16 py = (u16)(cy - fx_cos_mul(ang, r));
        if (px > 0xC000) px = 0;
        if (py > 0xC000) py = 0;
        if (solid && a != a0) mfd_map_line(px, py, prev_x, prev_y);     /* the line runs from the new point back to the previous one */
        else mfd_map_plot(px, py, color, 0);
        prev_x = px; prev_y = py;
    }
    if (enh_map_log_on) enh_map_log_arc_end();   /* ENH: S4.6 */
}

/* 0000:94A5 mfd_map_route: player -> current waypoint in white, the remaining legs in green */
void mfd_map_route(void)
{
    if (enh_map_log_on) enh_map_log_route();   /* ENH: S4.6 */
    if (DSW(0xDEDA)) return;
    int px = (s16)DSW(0xC0A4), py = (s16)DSW(0xC0B2);
    gfx_set_fg(0xF);
    for (int i = (s16)DSW(0x2E7C); i < 4; i++) {
        u16 w = (u16)(0x2E6C + 4 * i);
        int x = (s16)DSW(w), y = (s16)DSW(w + 2);
        if (x != 0) {                                    /* a waypoint with x = 0 is unused: no line, no new start, the colour stays */
            mfd_map_line(px, py, x, y);
            px = x; py = y;
            gfx_set_fg(0xA);
        }
    }
}

/* 0000:973C mfd_map_save_bg / 9770 mfd_map_restore_bg: the map window (74,110) 77x60 to / from the backup page 2 */
void mfd_map_save_bg(void)
{
    int page = *(s16 *)ctx_ptr(DSB(0x4088) ? CTX_P1_PTR : CTX_P0_PTR);
    blit_rect(page, 0x4A, 0x6E, *(s16 *)ctx_ptr(CTX_P2_PTR), 0x4A, 0x6E, 0x4D, 0x3C);
    if (enh_shadow_on) enh_window_save(0x4A, 0x6E, 0x4D, 0x3C);   /* ENH: S3.2, the records and layers go with the backup */
    if (enh_map_log_on) enh_map_log_reset();                       /* ENH: S4.6, the map's drawing log starts again */
}
void mfd_map_restore_bg(void)
{
    int page = *(s16 *)ctx_ptr(DSB(0x4088) ? CTX_P1_PTR : CTX_P0_PTR);
    blit_rect(*(s16 *)ctx_ptr(CTX_P2_PTR), 0x4A, 0x6E, page, 0x4A, 0x6E, 0x4D, 0x3C);
    if (enh_shadow_on) enh_window_restore(0x4A, 0x6E, 0x4D, 0x3C);   /* ENH: S3.2 */
    if (enh_map_log_on) enh_map_log_reset();                          /* ENH: S4.6 */
}

/* 0000:8387 mfd_map_redraw: the map around the aircraft (cellx, celly); the terrain is render3d's map_render_terrain (119C) */
void mfd_map_redraw(int cellx, int celly)
{
    DSW(0xDEDA) = 0;
    if (DSW(0x365E) == 0) return;
    DSW(0xC044) = DSW(0xC042) = 0;
    mfd_clear(1, 6);
    int z = (s8)DSB(0x4080);
    u16 lim = (u16)(0x4D << ((9 - z) & 31));
    int ahead = (u16)(0x8000u >> (z & 31));
    DSW(0x4084) = (u16)fx_clamp_wrap(fx_sin_mul(DSW(0x2DEE), ahead) + cellx, lim, (s16)(0x7FFF - lim));
    s16 t = (s16)((0x3C << ((9 - z) & 31)) / 3);
    u16 limy = (u16)(t << 2);
    DSW(0x4086) = (u16)fx_clamp_wrap(-(s16)(fx_cos_mul(DSW(0x2DEE), ahead) - celly), limy, (s16)(0x7FFF - limy));
    map_render_terrain(DSP(DSW(0x402A)), (s16)DSW(0x4084) / 2, 0x4000 - (s16)DSW(0x4086) / 2, 9 - (s16)DSW(0x4080));
    for (int i = 1; i < (s16)DSW(0xC0B4); i++) {
        u16 s = SITE(i);
        if (DSW(s + 6) != 0 && (DSW(s + 8) & 0x84) == 4)
            mfd_map_plot((s16)DSW(s + 2), (s16)DSW(s + 4), (DSW(s + 8) & 2) ? 0 : 8, 1);
    }
    mfd_map_save_bg();
}

/* 0000:8498 mfd_zoom_in / 84D6 mfd_zoom_out: the map zoom, the scope range or (external views) the camera distance */
void mfd_zoom_in(void)
{
    if (DSW(0xC07A) & 0x80) { DSW(0x3D70)--; return; }
    if (DSW(0xDEDA) == 0 && (s16)DSW(0x4080) < 9) { DSW(0x4080)++; mfd_map_redraw((s16)DSW(0xC0A4), (s16)DSW(0xC0B2)); }
    if (DSW(0xDEDA) == 1 && (s16)DSW(0x4082) < 7) DSW(0x4082)++;
}
void mfd_zoom_out(void)
{
    if (DSW(0xC07A) & 0x80) { DSW(0x3D70)++; return; }
    if (DSW(0xDEDA) == 0 && (s16)DSW(0x4080) > 2) { DSW(0x4080)--; mfd_map_redraw((s16)DSW(0xC0A4), (s16)DSW(0xC0B2)); }
    if (DSW(0xDEDA) == 1 && DSW(0x4082) != 0) DSW(0x4082)--;
}

/* 0000:82E9 mfd_init_geometry: the left MFD box (74,110)-(150,169); falls into mfd_zoom_in */
void mfd_init_geometry(void)
{
    DSW(0xDEDA) = 0;
    DSW(0xDE94) = 0x4A;
    DSW(0xE302) = 0x96;
    DSW(0xDE96) = 0x6E;
    DSW(0xE444) = 0xA9;
    DSW(0x98CA) = 0x4D;
    DSW(0x98CC) = 0x3C;
    mfd_zoom_in();
}

/* 0000:8316 hud_threat_bar: a horizontal bar of the signal meter below the left MFD (positive to the right of x = 86, negative to the left of x = 136) */
void hud_threat_bar(int val, int color, int y0, int y1)
{
    if (DSW(0x365E) == 0) return;
    ((TextCtx *)ctx_ptr(CTX_P0_PTR))->fg = (s16)color;
    s16 v = (s16)val;
    if (v > 0) fill_rows_far(ctx_ptr(CTX_P0_PTR), 0x56, 0xBB + y0, 0x56 + (v - (v >> 15)) / 2 * 0 + ((v - (v >> 15)) >> 1), 0xBB + y1);
    else if (v < 0) fill_rows_far(ctx_ptr(CTX_P0_PTR), 0x88 + ((v - (v >> 15)) >> 1), 0xBB + y0, 0x88, 0xBB + y1);
}

/* ---------------------------------------------------------------------------------------------------------- selection */

/* 0000:80A0 mfd_left_select: the left MFD shows the map (0) or the scope (1) */
void mfd_left_select(int mode)
{
    if (DSW(0x365E) == 0) return;
    int cur = (s16)DSW(0xDEDA);
    if ((s16)mode != cur) hud_lamp_recolor(ctx_ptr(CTX_P0_PTR), (s16)DSW((u16)(0x408A + 2 * cur)), 0);
    if ((s16)mode == 0) mfd_map_redraw((s16)DSW(0xC0A4), (s16)DSW(0xC0B2));
    else if ((s16)mode == 1) mfd_clear(1, 0);
    DSW(0xDEDA) = (u16)mode;
    u16 lamp = DSW((u16)(0x408A + 2 * (s16)mode));
    if (DSW((u16)(0x46E2 + 12 * lamp)) == 0) hud_lamp_recolor(ctx_ptr(CTX_P0_PTR), lamp, 1);
}

/* 0000:81F6 mfd_tv_label: Ahead / Right / Rear / Left under the TV page */
void mfd_tv_label(void)
{
    if (DSW(0x365E) == 0) return;
    int fg, bg;
    if (DSW(0x4826) == 1) { fg = 0x2F; bg = 0x2E; } else { fg = 0x2D; bg = 0x2C; }
    switch (DSW(0xE556)) {
    case 0:      strcpy_fn(strbuf(), ds_str(0x40CA)); break;
    case 0x4000: strcpy_fn(strbuf(), ds_str(0x40DA)); break;
    case 0x8000: strcpy_fn(strbuf(), ds_str(0x40D2)); break;
    case 0xC000: strcpy_fn(strbuf(), ds_str(0x40E2)); break;
    }
    TextCtx *c = ctx_ptr(CTX_P0_PTR);
    s16 flags = c->flags, oldbg = c->bg;
    c->bg = (s16)bg;
    c->flags = 1;
    txt_draw(c, strbuf(), 0xC5, 0xC1, fg);
    c->bg = oldbg;
    c->flags = flags;
}

/* 0000:8113 mfd_right_select: the right MFD page 13h TV, 14h navigation, 15h weapons, 18h objectives */
void mfd_right_select(int page)
{
    if (DSW(0x365E) == 0) return;
    void *c0 = ctx_ptr(CTX_P0_PTR);
    int p = (s16)page;
    if (p != (s16)DSW(0xDFDC)) {
        hud_lamp_recolor(c0, (s16)DSW((u16)(0x4068 + 2 * (s16)DSW(0xDFDC))), 0);
        if (DSW(0xDFDC) == 0x13) {
            hud_lamp_recolor(c0, 0x1B, 0);
            if (DSW(0x480E)) hud_lamp_recolor(c0, 0x19, 0);
        }
    }
    switch (p) {
    case 0x13: mfd_tv_label(); mfd_clear(2, 0x9F); break;
    case 0x14: mfd_nav_page(); break;
    case 0x15: mfd_weapons_page(); break;
    case 0x18: mfd_objectives_page(); break;
    }
    DSW(0xDFDC) = (u16)p;
    u16 lamp = DSW((u16)(0x4068 + 2 * p));
    if (DSW((u16)(0x46E2 + 12 * lamp)) == 0) {
        hud_lamp_recolor(c0, lamp, 1);
        if (p == 0x13) {
            hud_lamp_recolor(c0, 0x1B, 1);
            if (DSW(0x43B2) && DSW(0x480E) == 0) hud_lamp_recolor(c0, 0x19, 1);
        }
    }
}

/* 0000:829D mfd_refresh: page 16h refreshes the seven damage lamps; the right MFD is redrawn when it shows `page` */
void mfd_refresh(int page)
{
    if ((s16)page == 0x16) {
        for (int i = 0; i < 7; i++) hud_lamp_update(10 + i, ((1 << i) & DSW(0x3632)) ? 0x29 : 0x0A);
    }
    if ((s16)page == (s16)DSW(0xDFDC)) mfd_right_select(page);
}

/* 0000:950E mfd_fuel_bar: the fuel bar of the navigation page, 4 pixels tall at y 164..167 from x = 170, 135 fuel units per pixel */
void mfd_fuel_bar(int v, int col)
{
    int val = fx_clamp_wrap(v, 0, 10000);
    if (val > 134) {
        gfx_set_fg(col);
        fill_rows_far(ctx_ptr(DSB(0x4088) ? CTX_P1_PTR : CTX_P0_PTR), 170, 164, 170 + val / 135, 167);
    }
}
