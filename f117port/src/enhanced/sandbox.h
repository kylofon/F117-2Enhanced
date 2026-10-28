/* The sandboxed display pass (DESIGN.md 4.1, 4.2; S2.4).
 *
 * A display pass re-renders the frame from the pre-render image A_k (taken by enh_frame_begin where the faithful frame starts) and then
 * puts the live machine back exactly: whatever the render changes (designators, tracer hits, the TV fire-puff LCG, the spinners, the
 * collision flag, the driver statics, the layers) is thrown away. The image is the whole emulated memory mem[], the VGA model and the
 * engine statics a render touches (each module registers its own with an ENH: *_sandbox_regions function), plus the canvas layers and
 * shadow plane (swapped, not copied back). While enh_sbx is set: no sound command, poll, noise or OPL write; no wait that consumes ticks
 * and no host_pump (so no tick reaches the sim); no DOS file call. A forbidden call is counted (enh_sandbox_deny) and refused.
 *
 * S2.4 runs passes only for the test option --sandbox-test N (N passes per frame, from enh_idle); the interpolated passes are S4.1.
 * --sandbox-hash FILE writes a hash of the sim state per frame (ctest enh_sandbox_hash). --classic: no snapshot, no pass. */
#pragma once

#include <stddef.h>

#include "types.h"

extern bool enh_sbx;                                    /* a display pass is running */

typedef void (*EnhRegionAdd)(void *p, size_t n);
/* Each module with statics a render may write lists them (called once, when the sandbox builds its region table). */
void vga_sandbox_regions(EnhRegionAdd add);             /* platform/vga.c: vram, DAC, CRTC, sequencer, GC, attribute, DAC ports */
void mgraphic_sandbox_regions(EnhRegionAdd add);        /* video/mgraphic.c: page table, draw segment, colour, rng, shake, pool, driver image */
void pic_sandbox_regions(EnhRegionAdd add);             /* video/pic.c: the decoder state, rle_out */
void r3d_sandbox_regions(EnhRegionAdd add);             /* render3d/r3d_object.c: bsp_out */
void hr_sandbox_regions(EnhRegionAdd add);              /* enhanced/hr_*.c: the hr pass context and the persistent scalars */

/* Hooks */
void enh_frame_begin(void);                             /* sim_main.c frame_loop_body, before view_frame: A_k, the --sandbox-hash line */
void enh_idle(void);                                    /* sim_main.c frame_floor and its wait loop: the display passes due now */
bool enh_sandbox_deny(const char *what);                /* true (and counted) inside a pass: the caller must not go on */
void enh_opl_trace(u8 reg, u8 val);                     /* host.c host_opl_write: the OPL register stream for --sandbox-hash */

/* One display pass now (from enh_idle; needs a valid A_k). v > 0: the test view v of --sandbox-test (sandbox.c test_view). */
void enh_display_pass(int v);
void enh_sandbox_reload(void);                         /* S4.6: in a display pass, the image A_k again (mem, VGA, statics; not the canvas): after the map cache render */

/* S4.1, the display stream (DESIGN 4.1, 4.8): with --fps on, a VGAME frame that loads no cockpit picture is shown only by display passes: one display refresh
 * (S4.3: one pass on A_k with the fields at alpha, the wide pass when the view is extended, composed with the live DAC / CRTC; at alpha 1 in a view that is not
 * extended the canonical render itself) per monitor refresh (or every 2nd one when a refresh does not fit the budget) in the frame_floor wait and at the frame
 * start, alpha = (display clock - last flip) / step period. The presenter shows their pictures (enh_display_take), never the live page. */
void enh_display_wait(u64 deadline_tick);              /* sim_main.c frame_floor wait loop: a refresh when one is due and fits before the step is due */
void enh_frame_end(void);                               /* sim_main.c frame_floor, just before the flip: a frame without a refresh gets one; the flip time */
void enh_display_hold(void);                            /* sim_input.c sim_pause_wait: the paused frame at alpha 1 */
void enh_flight_end(void);                              /* platform/startup.c platform_exec: a program ended, the stream stops */
int  enh_display_take(u32 *out);                        /* enh_compose_vga: -1 = no stream (compose the live page), 0 = no new picture, 1 = the newest copied into out */
bool enh_display_ready(void);                           /* host_pump: a new picture waits (present it now) */
void enh_present_done(u64 ns);                          /* S4.3: host.c present: the cost of a present (a stage when it showed a display picture) */
void enh_stage_sample(int stage, u64 ns);               /* S4.3: a per refresh sample of a stage (the display-stages line at exit: average, p95) */
