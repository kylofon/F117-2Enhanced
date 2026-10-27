#pragma once
/* VGA hardware model (video.md; PORTING.md "Video"). Copied from aotpport and kept as is for the skeleton;
 * F-117A only needs the flat mode 13h of MGRAPHIC (video.md: a 320x200x256 2D library), so the
 * unchained-mode and latch-copy parts below stay unused until S5.2 decides whether to trim them.
 *
 * Video memory is 256 KB (4 planes x 64 KB) stored so that planar byte address a, plane p is
 * vram[a * 4 + p]. This makes every 16 KB page a LINEAR 320x200 pixel buffer: pixel (x, y) of the page
 * at segment S is
 *     vga_page(S)[y * 320 + x]        (S = A000, A400, A800, AC00)
 * because planar offset y*80 + x/4, plane x&3 -> (y*80 + x/4)*4 + (x&3) = y*320 + x. Mode 13h is
 * therefore simply vga_page(0xA000). Each page buffer is 65536 bytes (64000 visible).
 *
 * The CRTC start address (0Ch/0Dh), the offset register (13h) and the attribute controller's horizontal
 * pel panning select what is shown, as the hardware composes it (video.md: the shake effect shifts the
 * display by 0..3 CRTC units). The DAC holds 256 6-bit RGB entries with the 3C7h/3C8h/3C9h index
 * protocol. The 70.086 Hz refresh (25.175 MHz / (800 x 449), host.h) drives port 3DAh: vertical retrace
 * (bit 3) is active 2 of 449 lines.
 *
 * Port code calls these functions where the original accesses ports 3C0h-3DAh or INT 10h. Busy-wait
 * loops on 3DAh become vga_wait_* (they pump the host). */
#include "types.h"

#define VGA_VRAM_SIZE 0x40000u

void vga_init(void);                         /* installs the frame source; text mode (black) */

u8  *vga_vram(void);
u8  *vga_page(u16 seg);                      /* linear page buffer of segment A000/A400/A800/AC00 */
static inline u32 vga_planar_index(u16 planar_addr, int plane) { return (u32)planar_addr * 4u + (u32)plane; }
void vga_latch_copy(u16 dst_planar, u16 src_planar, u16 nbytes);   /* write mode 1, all planes */

/* INT 10h mode set: 03h (text: shown black) or 13h. vga_set_unchained() = the driver's register
 * programming after mode 13h (chain-4 / odd-even / dword mode off, byte mode; clears all 256 KB). */
void vga_set_mode(u8 mode);
void vga_set_unchained(void);
u8   vga_mode(void);

/* Port I/O. */
void vga_out(u16 port, u8 value);
void vga_outw(u16 port, u16 value);          /* out port, lo; out port+1, hi */
u8   vga_in(u16 port);                       /* 3DAh also resets the attribute flip-flop */

/* Direct access for ported code. */
void vga_dac_set(u8 index, u8 r, u8 g, u8 b);        /* 6-bit components */
void vga_dac_get(u8 index, u8 *r, u8 *g, u8 *b);
u16  vga_start(void);                        /* CRTC start address (planar bytes) */
u16  vga_shown_seg(void);
void vga_compose_13(u32 *xrgb320x200);          /* mode 13h as shown (CRTC start, pan, DAC) as XRGB: the faithful frame */
size_t vga_look_size(void);                     /* ENH: S4.1, DAC + CRTC + attribute + sequencer (how the shown page looks), for a display pass */
void vga_look_get(u8 *buf);
void vga_look_set(const u8 *buf);
bool vga_shown_frame(u8 *idx64000, u16 *page_off64000, u32 *pal256);   /* ENH: the same as indices, for the compositor */                    /* A000 + start / 16: the page being displayed */

/* Vertical retrace (3DAh bit 3) model and the waits that replace the original's polling loops. */
bool vga_in_vretrace(void);
void vga_wait_vretrace_start(void);          /* "while (in 3DAh & 8); while (!(in 3DAh & 8));" */
void vga_wait_vretrace_active(void);         /* "while (!(in 3DAh & 8));" */
void vga_wait_vretrace_end(void);            /* "while (in 3DAh & 8);" */

/* Text mode 03h (S5.3, vgatext.c): the 80 x 25 screen at B800:0000 (mem[]) composed into a 640 x 400 XRGB frame, 8 x 16 cells
 * (the 8 x 8 font with doubled rows, CP437 box drawing for SETUP's frame), the 16 standard colours. PORT: no cursor, no blink. */
void vga_text_render(u32 *xrgb640x400);
#define VGA_TEXT_BASE 0xB8000u
