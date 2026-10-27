/* VGAME.EXE: the program body (spec simflow.md Part 1): start-up (0010), the load order of sim_init (00F0), the file loaders (097A..0AF9, BD24, EA7C), the
 * run loop (0233, 1BB0, 1BBC), the timer ISR body (1C89..1E67, installed with timer_set_hook), the mission world hand-off (4AD0, 4C29..4D2F) and the end of the
 * mission (4926, 499E). The exit code goes back to F117.COM (prog.c): 81h = on to END, 0 = quit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prog.h"
#include "platform/platform.h"
#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "sim/simtest.h"
#include "enhanced/sandbox.h"   /* ENH: S2.4 */

SimTest sim_test = { .frames_left = -1, .dive_at = -1, .select_at = -1 };

static inline u8 *pilot_rec(void) { return far_mp(ds_far(0x98F4)); }
#define PILOT(o) (*(u16 *)(pilot_rec() + (o)))

/* ---------------------------------------------------------------------------------------------------------- timer ISR body */

/* VGAME 0000:1C89 timer_isr + 1E67 timer_tick_work: one tick per retrace (PLAN.md Decisions 2026-10-13: the ISR runs several times per frame in the original
 * but only every class-th call is a tick; the sub-ticks are folded in here). The 7th tick skips the sound poll (60.08 Hz at 70.09 Hz). The poll result
 * switches the tick divisor: fast (> 0) or idle (< 0). PORT: no PIT, no resync; a closed window ends the program (the original has Ctrl-Break only). */
static void vgame_tick(void)
{
    DSB(0x262A) = 0;                                    /* timer_frame_flag */
    DSW(0x2646)++;                                      /* tick_count */
    DSB(0x264A)++;                                      /* tick_byte */
    DSW(0x2648)++;                                      /* tick_count2 */
    explosion_fx_tick();                                /* slot 46: DAC[13] flicker + CRTC shake */
    int r;
    if (++DSB(0x264B) >= 7) { DSB(0x264B) = 0; r = 0; }
    else r = (s16)sim_snd_poll();                       /* slot 103 */
    if (r > 0) {                                        /* 1C5B timer_rate_fast */
        DSW(0x2636) = DSW(0x2639);
        DSW(0x2630) = (u16)(DSW(0x263B) / DSW(0x2636));
        DSB(0x2638) = 1;
    } else if (r < 0) {                                 /* 1C73 timer_rate_idle */
        DSW(0x2636) = 1;
        DSW(0x2630) = DSW(0x263B);
        DSB(0x2638) = 1;
    }
    if (DSW(0x2636) != 1)                               /* snd_noise (slot 104) on every ISR call: class times per frame */
        for (unsigned i = 0; i < DSW(0x2639); i++) sim_snd_noise();
    if (host_quit_requested()) { DSB(0xE552) = 1; DSB(0x008D) = 0; }   /* PORT: a closed window leaves the loop and quits to DOS */
}

/* TEST: one tick of the ISR body (simtest.c, ticks mode) */
void sim_tick_for_test(void) { vgame_tick(); }

/* 0000:1BFC timer_install: [2636] = [2640] = 1, clock = 0, calibrate; PORT: no PIT or INT 8, the host tick calls vgame_tick */
void timer_install_sim(void)
{
    DSW(0x2636) = 1;
    DSW(0x2640) = 1;
    DSL(0x262C) = 0;
    timer_calibrate();
    DSW(0x2639) = 4;                                    /* PORT: the retrace-locked class of a 70 Hz mode 13h (platform.md section 4) */
    DSW(0x263B) = 17024;                                /* PIT clocks per frame at 70.09 Hz */
    DSW(0x2630) = DSW(0x2632) = DSW(0x263B);
    DSB(0x2638) = 1;
    DSB(0x263F) = 1;
    timer_set_hook(vgame_tick);
    timer_install();
    DSB(0x262B) = 1;
}

/* 0000:1C3A timer_remove */
void timer_remove_sim(void)
{
    timer_remove();
    DSB(0x262B) = 0;
}

/* TESTSTUB 0000:04bb frame_delay */
/* 0000:04BB frame_delay (timer_wait_ticks): spin until the tick byte has advanced `ticks` times (PORT: not in the unit tests, which run without ticks) */
void frame_delay(int ticks)
{
    if ((s16)ticks <= 0 || sim_test.active || enh_sbx) return;   /* ENH: S2.4, no wait in a display pass */
    u8 target = (u8)(ticks + DSB(0x264A));
    while (DSB(0x264A) != target) host_pump();
}

/* ------------------------------------------------------------------------------------------------------------ pictures, files */

/* 0000:D81D file_open_ro = 1199:000C dos_open; D82F file_close = 1199:00C8 dos_close */
int file_open_ro(char *name, int mode) { return dos_open(name, mode); }
void file_close(int h) { dos_close(h); }

/* 0000:D7F4 pic_show: open, decode a .PIC into a page, close */
void pic_show(char *name, int page)
{
    int h = file_open_ro(name, 0);
    pic_to_page(h, page);
    file_close(h);
}

/* 0000:EA7C file_load_paras: the whole file into a fresh DOS block, size in [C074:C076]; 0 = missing (platform_load_file_paras keeps the 64K chunk bug) */
u16 file_load_paras(const char *name)
{
    u32 size;
    DSW(0xC074) = DSW(0xC076) = 0;
    u16 seg = platform_load_file_paras(name, &size);
    if (seg) { DSW(0xC074) = (u16)size; DSW(0xC076) = (u16)(size >> 16); }
    return seg;
}

/* 0000:0CD1 load_error_msg: the message on the back page, then a key; the original goes on with an unloaded bank (a crash), the port stops */
void load_error_msg(const char *msg)
{
    hud_text((char *)msg, 0, 0x60, 0xF);
    host_fatal("VGAME: %s", msg);                       /* PORT: the original waits for a key and carries on with a stale segment */
}

/* 0000:0C2C load_3dg_terrain */
void load_3dg_terrain(void)
{
    char *stem = ds_str(DSW(0x008E));
    set_file_ext(stem, ".3dG");
    FILE *f = fopen_fn(stem, "rb");
    if (!f) host_fatal("VGAME: cannot open %s", stem);
    fread_fn(DSP(0x075E), 2, 1, f);
    fread_fn(DSP(0x9F68), 1, 16, f);
    fread_fn(DSP(0x9F68), 1, 256, f);
    fread_fn(DSP(0x9D10), 1, 512, f);
    fread_fn(DSP(0x9B08), 1, 512, f);
    fread_fn(DSP(0x9904), 1, 512, f);
    fclose(f);
    memcpy(DSP(0xB00C), DSP(0x0A68 + 64 * PILOT(0x38)), 64);
}

/* 0000:0994 load_3dx_banks */
void load_3dx_banks(char *stem)
{
    set_file_ext(stem, ".3DX");
    u16 seg = file_load_paras(stem);
    if (!seg) { load_error_msg("Load Error on *.3DX"); return; }
    DSW(0xB760) = seg;
    u16 n = rd16(seg, 0);
    DSW(0x0610) = n;
    u32 fl = 2 + 2 * (u32)(n + 1);
    for (u16 i = 0; i < n; i++) DSB(0x0960 + i) = DSB(0x07E0 + i) = rd8(seg, (u16)(fl + i));
    u16 idx = 0;
    for (; idx < DSW(0x0610); idx++) DSW(0x0510 + 2 * idx) = (u16)(0x0100 | idx);
    seg = file_load_paras("photo.3dx");
    if (!seg) { load_error_msg("Load Error on PHOTO.3DX"); return; }
    DSW(0xB762) = seg;
    u16 m = rd16(seg, 0);
    fl = 2 + 2 * (u32)(m + 1);
    for (u16 j = 0; (s16)j < (s16)m; j++) DSB(0x0860 + j) = rd8(seg, (u16)(fl + j));
    if (DSB(0xE2DF) || DSB(0xE2F1)) {
        u16 t = DSB(0xE2DF);
        if (t) {
            DSB(0x0960 + idx) = DSB(0x0860 + t);
            DSW(0x0510 + 2 * idx) = (u16)(0x0200 | t);
            idx++;
        }
        t = DSB(0xE2F1);
        if (t) {
            DSB(0x0960 + idx) = DSB(0x0860 + t);
            DSW(0x0510 + 2 * idx) = (u16)(0x0200 | t);
        }
    }
    if (PILOT(0x38) == 6) {                             /* Kuwait: two bank-1 entries become PHOTO objects */
        DSW(0x05C2) = 0x020D;
        DSB(0x09B9) = DSB(0x086D);
        DSW(0x051A) = 0x020A;
        DSB(0x0965) = DSB(0x086A);
    }
}

/* 0000:BD24 load_stflt_3dx */
void load_stflt_3dx(void)
{
    u16 seg = file_load_paras("stflt.3dx");
    if (!seg) { load_error_msg("Load Error on STFLT.3DX"); return; }
    DSW(0xB75E) = seg;
    u16 n = rd16(seg, 0);
    u32 fl = 2 + 2 * (u32)(n + 1);
    for (u16 i = 0; (s16)i < (s16)n; i++) {
        DSB(0x0760 + i) = rd8(seg, (u16)(fl + i));
        DSB(0x09E0 + i) = 6;
    }
}

/* 0000:0AF9 load_3dt_scenery */
void load_3dt_scenery(char *stem)
{
    set_file_ext(stem, ".3dT");
    FILE *f = fopen_fn(stem, "rb");
    if (!f) host_fatal("VGAME: cannot open %s", stem);
    fread_fn(DSP(0x0612), 2, 1, f);
    fread_fn(DSP(0x0614), 2, 5, f);
    for (int L = 0; L < 5; L++) fread_fn(DSP(0x061E + 64 * L), 2, DSW(0x0614 + 2 * L), f);
    u16 p = 0xA06A;
    for (int L = 0; L < 5; L++) {
        for (u16 t = 0; t < DSW(0x0614 + 2 * L); t++) {
            DSW(0xE304 + 2 * (32 * L + t)) = p;
            for (u16 k = 0; k < DSW(0x061E + 64 * L + 2 * t); k++) {
                u16 w;
                fread_fn(DSP(p + 0), 2, 1, f);
                fread_fn(DSP(p + 2), 2, 1, f);
                fread_fn(DSP(p + 4), 2, 1, f);
                fread_fn(&w, 2, 1, f);
                DSB(p + 6) = (u8)w;
                p = (u16)(p + 7);
            }
        }
    }
    fclose(f);
}

/* 0000:097A load_theatre_data */
void load_theatre_data(void)
{
    load_3dg_terrain();
    load_3dx_banks(ds_str(DSW(0x008E)));
    load_3dt_scenery(ds_str(DSW(0x008E)));
    DSW(0x0CA8) = 0;
}

/* ------------------------------------------------------------------------------------------------------- the mission world */

/* 0000:4D2F wld_blob_rewind: the cursor DS:94EC:94EE = comm + 7Ah */
int wld_blob_rewind(void)
{
    DSW(0x94EC) = (u16)(DSW(0xE548) + 0x7A);
    DSW(0x94EE) = DSW(0xE54A);
    return 0;
}

/* 0000:4CEF wld_blob_copy: import (DE8E != 0) blob -> DS:p, export DS:p -> blob; the cursor advances by n (offset only) */
void wld_blob_copy(void *p, u16 n)
{
    u8 *blob = mp(DSW(0x94EE), DSW(0x94EC));
    if (DSW(0xDE8E)) memmove(p, blob, n);
    else memmove(blob, p, n);
    DSW(0x94EC) = (u16)(DSW(0x94EC) + n);
}

/* 0000:4C41 wld_blob_fields: the 16 fields of the blob in order (spec simflow.md section 4) */
void wld_blob_fields(void)
{
    wld_blob_copy(DSP(0xDEA4), 1);
    wld_blob_copy(DSP(0xC0AC), 1);
    wld_blob_copy(DSP(0xC0B4), 2);
    wld_blob_copy(DSP(0x98FC), 2);
    wld_blob_copy(DSP(0xE540), 2);
    wld_blob_copy(DSP(0xB2A2), (u16)(DSW(0xC0B4) << 4));
    wld_blob_copy(DSP(0xDED0), 2);
    wld_blob_copy(DSP(0xC13E), (u16)(DSW(0xDED0) * 0x24));
    wld_blob_copy(DSP(0xC604), 0x80);
    wld_blob_copy(DSP(0xC0BA), 0x80);
    wld_blob_copy(DSP(0xDFEA), 0x2EE);
    wld_blob_copy(DSP(0xB174), 0x100);
    wld_blob_copy(DSP(0xDE88), 2);
    wld_blob_copy(DSP(0x3D6E), 2);
    wld_blob_copy(DSP(0x2E6C), 0x10);
    wld_blob_copy(DSP(0xE2D8), 0x24);
}

/* 0000:4AD0 mission_world_import: unpack comm+7Ah, the name pointer table, the start position */
void mission_world_import(void)
{
    wld_blob_rewind();
    DSW(0xDE8E) = 1;
    wld_blob_fields();
    DSW(0xDEDC) = 0xDFEA;
    int j = 1;
    for (int i = 0; i < 0x2EE; i++)
        if (DSB(0xDFEA + i) == 0 && j < 0x80) DSW(0xDEDC + 2 * j++) = (u16)(0xDFEB + i);
    u16 site = (u16)(DSW(0xE2DC) << 4);
    u32 x = (u32)DSW((u16)(0xB2A4 + site)) << 5;
    u32 y = 0x8000u - DSW((u16)(0xB2A6 + site));         /* 32-bit: the borrow makes it negative for y > 8000h */
    if (DSW((u16)(0xB2A6 + site)) > 0x8000) y |= 0xFFFF0000u;
    DSL(0xBA10) = x + 2;
    DSL(0xC02A) = y << 5;
}

/* 0000:4C29 mission_world_export: the world back to comm+7Ah plus the 0x600-byte event log */
void mission_world_export(void)
{
    wld_blob_rewind();
    DSW(0xDE8E) = 0;
    wld_blob_fields();
    wld_blob_copy(DSP(0xBA2A), 0x600);
}

/* 0000:499E mission_log_event: spec simflow.md 4.3. Record {tick, x>>7, y>>7, type, arg}, up to 255 records, a type-0 terminator after the last */
void mission_log_event(int code, int aux)
{
    if ((s16)DSW(0x94EA) >= 0xFF) return;
    u16 o = (u16)(0xBA2A + 6 * DSW(0x94EA));
    DSW(o) = DSW(0x98E2);
    DSB(o + 2) = (u8)(DSW(0xC0A4) >> 7);
    DSB(o + 3) = (u8)(DSW(0xC0B2) >> 7);
    DSB(o + 4) = (u8)code;
    DSB(o + 5) = (u8)aux;
    DSW(0x94EA)++;
    DSB((u16)(0xBA2A + 6 * DSW(0x94EA) + 4)) = 0;
}

/* 0000:4926 mission_end: spec simflow.md Part 4 sections 2.2 and 2.3. Code 7 = abort (nothing written); while the crash sequence runs only code 0 counts */
void mission_end(int code)
{
    Comm *comm = comm_get();
    if ((s16)code == 7) { DSB(0xE552) = 1; return; }
    if (DSW(0xC06E) != 0 && (s16)code != 0) return;
    DSB(0xE552) = 1;
    comm->end_code = (u16)code;
    if ((s16)code == 0 && DSW(0xC06E) == 0) comm->mission_state = 3;
    comm->final_x = DSW(0xC0A4);
    comm->final_y = DSW(0xC0B2);
    comm->damage_mask = DSW(0x3632);
    comm->hits_taken = DSW(0xC5C8);
    mission_log_event(8, 0);
}

/* --------------------------------------------------------------------------------------------------------------- start-up */

/* 0000:02C2 gfx_pages_init: page 0 = the screen, page 1 = a new page, page 2 = the off-screen picture page F117.COM made (comm+20h) */
void gfx_pages_init(void)
{
    u16 s0 = (u16)gfx_alloc(0);
    DSW(0x85E2) = DSW(0x85EA) = s0;
    u16 s1 = (u16)gfx_alloc(1);
    DSW(0x85E4) = s1;
    page_seg_set_b(s1, 1);
    DSW(0x85E6) = comm_get()->seg_offscreen;
    page_seg_set_b(comm_get()->seg_offscreen, 2);
}

/* 0000:02FE ordnance_panel_build: the weapon name strips of ordnance.pic (page 1) into the right MFD's list on page 2, kept as an RLE sprite in [BA0E] */
void ordnance_panel_build(void)
{
    Comm *comm = comm_get();
    int y = 0x6E;
    pic_show("ordnance.pic", 1);
    int n = PILOT(0x42) == 0 ? 4 : 2;
    for (int i = 0; i < n; i++) {
        u16 w = comm->weapon_id[i];
        u16 spr = DSW((u16)(0x367A + 26 * w));
        blit_rect(1, DSW((u16)(0xA2 + 4 * spr)), DSW((u16)(0xA4 + 4 * spr)), 2, 0xA9, y, 0x4D, 0x0F);
        y += 0x0F;
    }
    color_set(0x2F);
    page_select(2);
    DSW(0x2299) = 0xA9;
    DSW(0x229D) = DSW(0x229F) = 0x6E;
    DSW(0x229B) = 0xF5;
    line_clip_far();
    page_select(1);
    if (pool_init()) {
        DSW(0xBA0E) = (u16)sprite_grab(2, 0xA9, 0x6E, 0x4D, 0x3C);
        if (DSW(0xBA0E)) pool_free();
    }
}

/* 0000:01BF cockpit_daynight_recolor: the asm HUD's colours, then the three instrument areas of the picture in the day or night colour */
void cockpit_daynight_recolor(void)
{
    hud_color_init_far();
    int from, to;
    if (DSW(0x43B0)) { from = 0xF; to = 0xA; } else { from = 0xA; to = 0xF; }
    void *page = DSP(DSW(CTX_P2_PTR));
    rect_recolor(page, 0, 0, 0x8C, 0x1D, from, to);
    rect_recolor(page, 0, 0x1E, 0x40, 0x25, from, to);
    rect_recolor(page, 0x26, 0x4B, 0x5B, 0x57, from, to);
}

/* 0000:01B7 cockpit_palette_refresh */
void cockpit_palette_refresh(void)
{
    pal_flight_load();
    cockpit_daynight_recolor();
}

/* 0000:00F0 sim_init: all the loading (spec simflow.md 0.2) */
void sim_init(void)
{
    Comm *comm = comm_get();
    mission_world_import();
    load_stflt_3dx();
    strcpy_fn(ds_str(DSW(0x008E)), ds_str(DSW((u16)(0x0090 + 2 * PILOT(0x38)))));
    load_theatre_data();
    DSW(0x008A) = file_load_paras("speech.117");
    if (DSW(0x008A)) sim_snd_cmd(0x28, DSW(0x008A), DSW(0xC074), DSW(0xC076));
    screen_off();
    DSW(0x3658) = comm->night;
    DSB(0x0D47) = DSB((u16)(0x00EE + PILOT(0x38)));
    DSW(0x43B0) = DSW(0x3658) < 1;
    DSB(0x008C) = 1;
    pal_flight_load();
    gfx_set_fg(0);
    fill_rows_far(DSP(DSW(CTX_P0_PTR)), 0, 0, 0x13F, 0x5B);
    fill_rows_far(DSP(DSW(CTX_P1_PTR)), 0, 0, 0x13F, 0x5B);
    pic_show("256pit.PIC", 2);
    ordnance_panel_build();
    gfx_restore_rect(0, 0x5B, 0x140, 0x6D);
    cockpit_daynight_recolor();
}

/* PORT: the game speed (--frame-ticks N, PLAN.md Decisions 2026-10-16). The original paces a frame only by the frame_delay([43B8]) of
 * r3d_scene_draw and the tick wait of the scene set-up, so a fast PC runs at the cap of its estimator: about 4 ticks a frame (17.5 frames a
 * second) with sim_rate 15, where the flight model is still right but everything drawn per frame (the radar dishes' 33.75 degrees per scene,
 * render3d.md DB3E) runs at about twice the speed of a 1991 PC. The floor makes each frame last at least N ticks; the estimator [3660] then
 * measures the slower rate and the flight model keeps its speed. N = 0 (and the tests) leave the original's pacing alone. */
static void frame_floor(void)
{
    static u64 last;
    enh_idle();                                          /* ENH: S2.4, display passes (S3.4: the wide pass) */
    int n = host_frame_ticks();
    if (n <= 0 || sim_test.active) { enh_frame_end(); return; }   /* ENH: S4.1 */
    while (host_ticks() - last < (u64)n && !host_quit_requested()) { enh_display_wait(last + (u64)n); host_pump(); }   /* ENH: S4.1, a display refresh when one is due */
    last = host_ticks();
    enh_frame_end();                                     /* ENH: S4.1, the flip time (and a refresh if the wait had none) */
}

/* PORT, test aid of the scripted chain run (F117_VGAME_DIVE=N, ctest debrief_chain_sea): after N frames the aircraft is put in a
 * 45-degree dive at 450 kt, 600 ft over the sea north of Tripoli (Libya; any point at sea level outside an airfield zone is the same),
 * gear up, and Esc is pressed on the host keyboard as a player would after the impact. The flight model and the sim step then crash it
 * for real: AGL 0 with crashes on = mission_end(1), END (killed in action), START. Before the fix of 2026-10-16 that Esc made the
 * chain quit to the desktop when VGAME ended. */
/* PORT, test aid of the real-path run (F117_VGAME_SELECT=N, S3.2): after N frames the nearest site (not the home base, no ring-only entries) is the selected target, as
 * the designation of key n would make it, so the right MFD's TV page (key /) shows the close view of it. */
static void chain_select(void)
{
    int best = -1;
    long bd = 0x7FFFFFFF;
    for (int i = 1; i < (s16)DSW(0xC0B4); i++) {
        u16 s = (u16)(0xB2A4 + 16 * i);
        if (DSW(s) == 0 || DSW((u16)(s + 2)) == 0) continue;
        long dx = (s16)(DSW(s) - DSW(0xC0A4)), dy = (s16)(DSW((u16)(s + 2)) - DSW(0xC0B2)), d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (getenv("F117_VGAME_SITE")) best = atoi(getenv("F117_VGAME_SITE"));                   /* or a given site index */
    if (best < 0) return;
    DSW(0x3D66) = (u16)best;
    if (DSW(0x3D68) == 0) DSW(0x3D68) = 0xFFFF;
}

static void chain_dive(void)
{
    DSL(0xBA10) = 4288u * 32;                            /* x, y in world units (level units x 32, y runs north here) */
    DSL(0xC02A) = (0x8000u - 13700u) * 32;
    DSW(0x2DF6) = 600;
    DSW(0xB052) = 450 * 27;
    DSW(0x2DFE) = DSW(0xB170) = 100;
    DSW(0x9B04) |= 1;                                    /* gear up */
    DSW(0x2DF0) = 0xE000;                                /* pitch: nose 45 degrees down */
    DSW(0x2DF2) = 0;
    DSB(0x2DFB) = 1;                                     /* the flight model rebuilds its matrix from the angles */
    kbd_host_byte(0x01);
    kbd_host_byte(0x81);
}

/* 0000:1BB0 frame_loop / 1BBC frame_loop_body: THE frame loop (spec simflow.md 0.4), until [E552] != 0 */
void frame_loop_body(void)
{
    do {
        enh_frame_begin();                               /* ENH: S2.4, the pre-render image A_k */
        view_frame();
        hud_overlay_frame();
        if (DSW(0xC07A) == 0) {
            hud_draw_far();
            hud_side_strips(DSB(0x4088));
            cockpit_draw_adi();
        }
        frame_floor();                                   /* PORT: the game speed */
        flip(DSW(0x2E34) * 160);
        DSB(0x262A) = 1;
        sim_input_flight();
        sim_frame_step();
        if (sim_test.select_at >= 0 && sim_test.select_at-- == 0) chain_select();   /* PORT: test aid (F117_VGAME_SELECT) */
        if (sim_test.dive_at >= 0 && sim_test.dive_at-- == 0) chain_dive();   /* PORT: test aid (F117_VGAME_DIVE) */
        if (sim_test.frames_left >= 0 && sim_test.frames_left-- == 0) mission_end(getenv("F117_VGAME_END") ? atoi(getenv("F117_VGAME_END")) : 7);   /* PORT: F117_VGAME_END=0 lands (the chain test hands END a finished mission) */
    } while (DSB(0xE552) == 0);
}

void frame_loop(void) { frame_loop_body(); }

/* 0000:0233 sim_run: timer, keypad hook, the frame loop, shutdown, the world back to the comm block */
void sim_run(void)
{
    Comm *comm = comm_get();
    if (sim_test.stop_at_run) {                          /* test mode: the start-up state is what the test compares */
        sim_dump_state(sim_test.dump_dir, "startup_port.bin");
        exit_fn(0x7F);
    }
    if (mem[0x440] > 1) mem[0x440] = 1;                  /* the floppy motor-off counter (BIOS data 0040:0040): stops after one tick */
    DSW(0x98E6) = 0x440;
    DSW(0x98E8) = 0;
    timer_install_sim();
    if (!(comm->input_devices & 1)) keypad_stick_install();
    frame_loop();
    sim_snd_cmd(0x17, 0, 0, 0);
    sim_snd_cmd(0, 0, 0, 0);
    mission_world_export();
    if (DSW(0xBA0E)) dos_freemem(DSW(0xBA0E));
    if (DSW(0x008A)) sim_snd_cmd(0x28, 0, 0, 0);
    if (!(comm->input_devices & 1)) keypad_stick_remove();
    shake_set(1);
    frame_delay(2);
    timer_remove_sim();
}

/* 10A8:0D70 joy_get_calib / 10A8:0D82 joy_set_calib (the latter is platform's): the calibration table <-> comm+48h */
void joy_get_calib(void *dst)
{
    memcpy(dst, DSP(plat_ds()->joy_cal), 40);
}

/* 0000:0010 vgame_start (main) */
void vgame_start(void)
{
    Comm *comm = comm_get();
    u16 cs = rd16(0, COMM_SEG_PTR);
    DSW(0xE54A) = cs;
    DSW(0xE548) = 0;
    DSW(0x98F6) = cs;
    DSW(0x98F4) = 0x1272;
    /* drv_bind x3: MGRAPHIC, MISC and the sound driver are function tables in the port (platform.md section 6) */
    break_hook_install();
    if (comm->input_devices & 1) joy_set_calib(comm->joy_calib);
    else DSB(0x2CD2) = DSB(0x2CD3) = 0x80;
    gfx_pages_init();
    page_select_1();
    gfxlib_mode_init('M');
    DSW(0xE558) = comm->seg_offscreen;
    sim_init();
    hud_draw_store_counts();
    sim_run();
    if (comm->input_devices & 1) joy_get_calib(comm->joy_calib);
    break_hook_remove();
    if (DSB(0x008D) == 0) {                              /* the quit requester's Y: BIOS text mode 3 */
        DSB(0xC5CB) = 0;
        DSB(0xC5CA) = 3;
        video_set_mode(3);
    }
    exit_fn(DSB(0x008D));
}

/* VGAME.EXE as a program of the chain */
int vgame_main(const char *tail)
{
    (void)tail;
    placeholder_note_vgame_pass();
    if (!sim_test.active && getenv("F117_VGAME_FRAMES")) sim_test.frames_left = atoi(getenv("F117_VGAME_FRAMES"));   /* PORT: the scripted chain run ends the flight after N frames */
    if (!sim_test.active && getenv("F117_VGAME_SELECT")) sim_test.select_at = atoi(getenv("F117_VGAME_SELECT"));          /* PORT: ... or selects the nearest site as the target */
    if (!sim_test.active && getenv("F117_VGAME_DIVE")) sim_test.dive_at = atoi(getenv("F117_VGAME_DIVE"));            /* PORT: ... or dives into the sea after N frames */
    vgame_start();
    return 0;
}
