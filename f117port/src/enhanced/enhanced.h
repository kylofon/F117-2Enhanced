/* The Enhanced options (PLAN.md). Every engine edit that reads them is marked ENH:. --classic turns every
 * enhancement off: the game then behaves exactly as the faithful release v0.1.0 (the faithful ctests pass). */
#pragma once

#include <stdbool.h>

typedef struct {
    bool classic;        /* --classic: no enhancement at all */
    bool canvas;         /* the layered compositor and the canvas (default on; off with --classic) */
    int  render_scale;   /* --render-scale N: the frame is shown N x N (0 = the largest whole multiple that fits the window) */
    bool wide;           /* --wide (default on since S3.4) / --no-wide: the canvas follows the window's aspect; the front cockpit and the outside views show the world
                          * beyond the 320x200 frame (DESIGN 3), every other view is the whole picture fitted and centred */
    int  compose_test;   /* --compose-test (1): headless check of the canvas geometry and the compositor; --wide-geometry-test (2, S3.4): the placement per view and window */
    bool dither_fine;    /* --dither fine: the 3D checkerboard per layer pixel; --dither grid (default): per original pixel (DESIGN 2.4, Q2) */
    bool hr_check;       /* --hr-check: the render test (--render-test) also checks the hr 3D layer against the 1x page (DIR/hr_check.txt) */
    char hr_dump[1024];  /* --hr-dump DIR (implies --hr-check): per render-test scenario DIR/<id>.hrd: the hr trace, the layer, the page (S2.3) */
    int  sandbox_test;   /* --sandbox-test N: N sandboxed display passes per VGAME frame, nothing shown (S2.4 test; S4.1 runs them for real) */
    char sandbox_hash[1024];   /* --sandbox-hash FILE: a hash of the sim state per VGAME frame (test aid; also in --classic, it changes nothing) */
    int  wide_test_w, wide_test_h;   /* --wide-test WxH (S3.4 test aid): the render test draws each scene's main 3D pass as a wide pass on a W x H canvas */
    bool smooth_shading; /* --smooth-shading / --no-smooth-shading (S3.5, default off): the distance bands (sky / ground 1442, the depth shade 1088:0120) blended into a
                          * gradient in the compositor (DESIGN 2.11); ignored with --classic */
    bool true_night;     /* --true-night / --no-true-night (S3.6, default off): in night missions the 3D world (sky, ground, objects) much darker by distance, lights kept
                          * (DESIGN 2.12); ignored with --classic */
    int  fps;            /* --fps vsync|60|30|off (S4.1, default vsync = -1; 0 = off, the S3.4 path: one picture per sim step): display passes per monitor refresh
                          * in the frame_floor wait, the own aircraft and the camera interpolated between the last two sim steps (DESIGN 4.8); off with --classic */
    int  fps_test;       /* --fps-test K (S4.1 test aid): K display passes per VGAME frame at alpha 1/K .. K/K, run at once (no clock), instead of the refresh timing */
    char fps_trace[1024];      /* --fps-trace FILE (S4.1 test aid): per display pass the frame, alpha, hold reason and the camera view_frame built (the jerk trace) */
    char mfd_probe[1024];      /* --mfd-probe PREFIX (S3.2 test aid): per page flip PREFIX.txt gets how much of the TV and map windows is shown from the 3D layer; F117_PROBE_FRAMES=a,b dumps those frames */
} EnhOptions;

extern EnhOptions enh;

/* Parses one Enhanced command-line option at argv[i]; returns how many arguments it took (0 = not ours). */
int enh_parse_arg(int argc, char **argv, int i);
/* True when the canvas compositor is wanted (not --classic). */
bool enh_canvas_on(void);
/* Display passes wanted (not --classic, not F117_CLASSIC). */
bool enh_sandbox_on(void);
/* S3.5: smooth distance shading wanted (--smooth-shading, the canvas on, not --classic / F117_CLASSIC). */
bool enh_smooth_on(void);
/* S3.6: true night wanted (--true-night, the canvas on, not --classic / F117_CLASSIC). */
bool enh_true_night_on(void);
/* S4.1: display passes per refresh wanted (--fps not off or --fps-test, the canvas on, not --classic / F117_CLASSIC). */
bool enh_fps_on(void);
/* Options from the environment variable F117_ENH_ARGS (for test drivers that build their own command line); once. */
void enh_env_args(void);
