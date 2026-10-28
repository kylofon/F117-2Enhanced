/* The map MFD between sim steps (S4.6, DESIGN.md 4.11).
 *
 * The original draws the map's moving symbols in the sim step (sim_frame_step, 0000:39C4 ff.): the window is copied back from its backup page 2 (9770), then
 * the two target markers, the route, the detection arc, the unit and missile blips and last the aircraft marker are drawn into the draw page; the terrain
 * (119C) is drawn only when the window is recentred or zoomed (8387). The render (view_frame) copies the window to the other page. A display pass re-runs
 * the render on A_k, so without help the map showed step k-1's drawing: everything on it moved at the step rate, in whole original pixels.
 *
 * In the step (live, the stream on) the drawing ops after the last backup copy are logged in order. A display pass that interpolates copies the window
 * back from the backup and replays the log: the static ops as they were; the aircraft marker at the position between the last two steps' markers
 * (the marker's own history: what frame k-1 showed at alpha 1 -> what frame k shows at alpha 1) with the heading's sprite, and each unit / missile blip
 * at its interpolated cells (interp.c), both moved by sub-pixels: the 1x op runs at the whole pixel and its pixels are shown as N x N vector-layer blocks
 * shifted by the fraction (enh_cap_shift). Nothing changes in the sim; --fps off and --classic never log. */
#pragma once

#include <stdio.h>

#include "types.h"
#include "enhanced/interp.h"   /* ENH_OBJ_UNIT / ENH_OBJ_PROJ */

extern bool enh_map_log_on;                 /* the step's map drawing is logged (set per frame: the stream is on) */

/* hooks (ENH: S4.6), each only when enh_map_log_on */
void enh_map_log_reset(void);               /* mfd_map_save_bg / mfd_map_restore_bg: what was drawn before is in the backup or gone */
void enh_map_log_plot(int wx, int wy, int c, int big);   /* mfd_map_plot entry */
void enh_map_log_route(void);               /* mfd_map_route entry */
void enh_map_log_arc(int cx, int cy, int r, int c, int solid, int a0, int a1);   /* mfd_map_arc entry ... */
void enh_map_log_arc_end(void);             /* ... and end (its own plots are not logged) */
void enh_map_log_key(int own, int dx, int dy, int sx, int sy, int w, int h, int clip);   /* sim_frame_step's cockpit_keyblit of a target marker (own 0) or the aircraft marker (1) */
void enh_map_tag(int cls, int slot);        /* before a unit's / projectile's mfd_map_plot: the next plot is that object's blip */

void enh_map_frame(void);                   /* enh_frame_begin (live A_k), after enh_interp_frame: this frame's log and whether its passes redraw the map */
bool enh_map_active(void);                  /* this frame's passes redraw the map (then a refresh at alpha 1 is a pass, not the canonical render) */
void enh_map_apply(double alpha);           /* in a display pass on A_k, after enh_interp_apply, before view_frame */
void enh_map_trace(FILE *f, unsigned long long frame, int pass);   /* --fps-trace: the positions this pass drew (o-lines, classes mapown / mapunit / mapproj) */
void enh_map_stats(FILE *f);                /* the display-map exit line */
