/* The interpolated fields of a display pass (DESIGN.md 4.1, 4.3; S4.1).
 *
 * At every VGAME frame start (enh_frame_begin, the state S_k the step k-1 left = the pre-render image A_k) the own aircraft's fields F_k are read;
 * F_{k-1} is kept. A display pass at alpha (0..1, from the display clock) writes lerp(F_{k-1}, F_k, alpha) into its throwaway image A_k before it
 * renders, so alpha = 1 is the canonical frame k. S4.1 interpolates the own aircraft and so the camera that view_frame builds from it: world x / y
 * (BA10, C02A, s32), the cell position C0A4 / C0B2 (from x / y as 39C4 builds it), the altitude word 2DF4, heading / pitch / roll (2DEE / 2DF0 / 2DF2,
 * on the shortest 16-bit arc) and the cockpit camera matrix 2D94 (rebuilt from the lerped angles plus the lerped residual of the integrated matrix,
 * so both ends are exact). Objects stay at A_k (S4.2).
 *
 * No lerp (hold: the pass shows F_k) when the previous frame is missing, the view (C07A, [294B]) changed, the ejection / crash sequence runs
 * ([C06E] != 0), the mission is ending ([E552]), or a field jumps (position > 1024 world units = 32 cells, altitude > 0x800, an angle > 0x4000:
 * the pitch passing the vertical flips heading and roll by 0x8000). */
#pragma once

#include <stdio.h>

#include "types.h"

void enh_interp_reset(void);            /* a program ended: no previous frame */
void enh_interp_frame(void);            /* enh_frame_begin (live state = A_k): F_{k-1} = F_k, F_k = the fields now; the lerp decision */
bool enh_interp_lerp(void);             /* the frame's passes interpolate (false = hold: every pass shows F_k) */
const char *enh_interp_hold_reason(void);   /* why not (test aid, the jerk trace) */
void enh_interp_apply(double alpha);    /* in a display pass, on A_k: the fields at alpha (nothing when holding or alpha >= 1) */
void enh_interp_trace(FILE *f, unsigned long long frame, int pass, double alpha, double ms);   /* ms: display clock */   /* --fps-trace: the camera view_frame built in this pass */
