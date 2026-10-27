/* Keyboard side of the sim flow: the keypad-as-stick INT 9 hook (1181:0008 / 0058 / 0072, spec simflow.md Part 1 section 7 and flight.md 2.4: owner flight, ported here
 * because S5.1 left the hook for this module), and the Alt-Q / Alt-P / Alt-B screens of 1E9E, which are drawn with simflow's palettes and pictures (the flight
 * model 1E9E itself is in flight/fl_flight.c). */
#include <string.h>

#include "sim/sim.h"
#include "sim/sim_priv.h"
#include "enhanced/enh_canvas.h"   /* ENH: S3.4 */
#include "enhanced/sandbox.h"      /* ENH: S4.1 */

/* ------------------------------------------------------------------------------------------------------ keypad as stick */

/* 1181:0072 keypad_stick_isr (INT 9, the byte on port 60h = `raw`): the numeric keypad and the grey cursor keys move the virtual stick bytes DS:2CD6 (x) and
 * 2CD7 (y); 80h is centre, a first press gives 5Ah, a second press of the same key within 4 BIOS ticks 7Fh. Runs before the BIOS handler: first the type-ahead
 * buffer loses the repeats of identical keys. */
static void dedupe_buffer(void)
{
    u16 head = rd16(0x40, 0x1A), tail = rd16(0x40, 0x1C);
    while (head != tail) {
        u16 ax = rd16(0x40, head);
        u16 bx = (u16)(head + 2);
        if (bx == rd16(0x40, 0x82)) bx = rd16(0x40, 0x80);
        if (bx == tail) break;
        if (ax != rd16(0x40, bx)) break;
        head = bx;
        wr16(0x40, 0x1A, head);
    }
}

void keypad_stick_isr(u8 al)
{
    if (DSB(0x2CDD)) { DSB(0x2CDD)--; dedupe_buffer(); return; }
    u8 prev = DSB(0x2CDB);
    DSB(0x2CDB) = al;
    if (prev != 0xE0) {
        if (al == 0xE0) { dedupe_buffer(); return; }
        if (al == 0xE1) { DSB(0x2CDD) = 2; dedupe_buffer(); return; }
        u8 fl = mem[0x417];
        int f = 0;
        if (fl & 0x20) f ^= 1;
        if (fl & 3) f ^= 1;
        if (f) { dedupe_buffer(); return; }
    }
    u8 code = al & 0x7F;
    if (code > 0x51 || code < 0x29) { dedupe_buffer(); return; }
    u8 m = DSB((u16)(0x2CDE + code - 0x29));
    if (m == 0) { dedupe_buffer(); return; }
    if (!(al & 0x80)) {
        if (DSB(0x2CD8)) { dedupe_buffer(); return; }
        DSB(0x2CD8) = m;
        int same = DSB(0x2CDC) == m;
        DSB(0x2CDC) = m;
        u16 now = rd16(0x40, 0x6C);
        u8 mag = (same && (u16)(now - DSW(0x2CD9)) < 5) ? 0x7F : 0x5A;
        u8 lo = (u8)(0x80 - mag), hi = (u8)(0x80 + mag);
        if (m & 1) DSB(0x2CD7) = lo;
        if (m & 2) DSB(0x2CD7) = hi;
        if (m & 4) DSB(0x2CD6) = lo;
        if (m & 8) DSB(0x2CD6) = hi;
        DSW(0x2CD9) = now;
    } else if (DSB(0x2CD8) == m) {
        DSB(0x2CD8) = 0;
        DSB(0x2CD6) = DSB(0x2CD7) = 0x80;
    }
    dedupe_buffer();
}

/* 1181:0008 keypad_stick_install: NumLock off, state cleared, the virtual stick centred, the hook in */
void keypad_stick_install(void)
{
    mem[0x417] &= (u8)~0x20;
    DSB(0x2CD8) = 0;
    DSB(0x2CD9) = 0;
    DSB(0x2CDB) = DSB(0x2CDC) = DSB(0x2CDD) = 0;
    DSB(0x2CD6) = DSB(0x2CD7) = 0x80;
    kbd_set_int9_hook(keypad_stick_isr);
}

/* 1181:0058 keypad_stick_remove */
void keypad_stick_remove(void) { kbd_set_int9_hook(NULL); }

/* ------------------------------------------------------------------------------------------------------------ 1E9E stand-in */

/* 0000:3939 sim_pause_wait: engine drone off, wait for a key other than Alt-P, the frame-time counter is kept */
void sim_pause_wait(void)
{
    sim_snd_cmd(0x19, 0, 0, 0);
    enh_display_hold();                  /* ENH: S4.1, the paused frame shown as the faithful one (alpha 1) */
    u16 save = DSW(0x2646);
    int k;
    do {
        while (!dos_kbhit()) host_pump();
        k = bios_keyboard(0);
    } while (k == 0x1900);
    snd_engine_refresh();
    DSW(0x2646) = save;
}

/* the quit requester of 1E9E (205F): ADV.PIC with the question; Y ends the mission with the abort code and the program with exit code 0 */
void sim_quit_requester(void)
{
    u16 save = DSW(0x2646);
    if (enh_shadow_on) enh_menu(true);   /* ENH: S3.4, a menu screen: pillarboxed */
    screen_off();
    pal_save();
    blit_rect(0, 0, 0, 1, 0, 0, 320, 200);
    pal_set_adv();
    pic_show("adv.pic", 0);
    screen_on();
    sim_snd_cmd(0x19, 0, 0, 0);
    while (!kbd_key_waiting()) host_pump();
    u16 k = kbd_key_get();
    if ((k & 0x5F) == 'Y') {
        mission_end(7);
        DSB(0x008D) = 0;
    } else {
        screen_off();
        blit_rect(1, 0, 0, 0, 0, 0, 320, 200);
        pal_restore();
        screen_on();
    }
    snd_engine_refresh();
    DSW(0x2646) = save;
    if (enh_shadow_on) enh_menu(false);   /* ENH: S3.4 */
}

/* the Alt-B boss screen of 1E9E (2110): the lower panel saved to the clean page 2, both display pages black, pause, the panel back; the caller redraws the throttle digits */
void sim_boss_screen(void)
{
    if (enh_shadow_on) enh_menu(true);   /* ENH: S3.4 */
    if (DSW(0x365E)) blit_rect(0, 0, 91, 2, 0, 91, 320, 109);
    gfx_set_fg(0);
    hud_fill_rect(0, 0, 319, 199);
    sim_pause_wait();
    if (DSW(0x365E)) gfx_restore_rect(0, 91, 320, 109);
    if (enh_shadow_on) enh_menu(false);   /* ENH: S3.4 */
}
