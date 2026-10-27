/* The Enhanced options (see enhanced.h). */
#include "enhanced.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* S3.4: the wider view is on by default (S2.1 note: "the default turns on with S3.4"); S4.1: --fps vsync by default */
EnhOptions enh = { .canvas = true, .wide = true, .fps = -1 };

/* F117_CLASSIC=1 in the environment acts as --classic: the faithful ctests set it, because their Python drivers start the program with
 * their own command lines. */
static bool env_classic(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("F117_CLASSIC"); v = e && *e && *e != '0'; }
    return v != 0;
}

bool enh_canvas_on(void) { return !enh.classic && enh.canvas && !env_classic(); }
bool enh_smooth_on(void) { return enh.smooth_shading && enh_canvas_on(); }
bool enh_true_night_on(void) { return enh.true_night && enh_canvas_on(); }
bool enh_fps_on(void) { return (enh.fps != 0 || enh.fps_test > 0) && enh_canvas_on(); }
bool enh_sandbox_on(void) { return !enh.classic && enh.sandbox_test > 0 && !env_classic(); }

int enh_parse_arg(int argc, char **argv, int i)
{
    if (!strcmp(argv[i], "--classic")) {              /* every enhancement off, whatever else is on the command line */
        enh.classic = true; enh.canvas = false; enh.wide = false; enh.render_scale = 0; enh.dither_fine = false; enh.hr_check = false; enh.hr_dump[0] = 0;
        enh.sandbox_test = 0; enh.wide_test_w = enh.wide_test_h = 0; enh.smooth_shading = false; enh.true_night = false;
        enh.fps = 0; enh.fps_test = 0;
        return 1;
    }
    if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) {
        int n = atoi(argv[i + 1]);
        if (!enh.classic) enh.render_scale = n < 0 ? 0 : n > 16 ? 16 : n;
        return 2;
    }
    if (!strcmp(argv[i], "--wide")) { if (!enh.classic) enh.wide = true; return 1; }
    if (!strcmp(argv[i], "--no-wide")) { enh.wide = false; return 1; }
    if (!strcmp(argv[i], "--wide-test") && i + 1 < argc) {        /* S3.4 test aid */
        int w = 0, h = 0;
        if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 8192 && h <= 8192 && !enh.classic) { enh.wide_test_w = w; enh.wide_test_h = h; enh.wide = true; }
        return 2;
    }
    if (!strcmp(argv[i], "--smooth-shading")) { if (!enh.classic) enh.smooth_shading = true; return 1; }   /* S3.5 */
    if (!strcmp(argv[i], "--no-smooth-shading")) { enh.smooth_shading = false; return 1; }
    if (!strcmp(argv[i], "--true-night")) { if (!enh.classic) enh.true_night = true; return 1; }        /* S3.6 */
    if (!strcmp(argv[i], "--no-true-night")) { enh.true_night = false; return 1; }
    if (!strcmp(argv[i], "--fps") && i + 1 < argc) {               /* S4.1 */
        const char *v = argv[i + 1];
        int f = !strcmp(v, "vsync") ? -1 : !strcmp(v, "off") ? 0 : !strcmp(v, "60") ? 60 : !strcmp(v, "30") ? 30 : -2;
        if (f == -2) return 0;
        if (!enh.classic) enh.fps = f;
        return 2;
    }
    if (!strcmp(argv[i], "--fps-test") && i + 1 < argc) {          /* S4.1 test aid */
        int k = atoi(argv[i + 1]);
        if (!enh.classic) enh.fps_test = k < 0 ? 0 : k > 64 ? 64 : k;
        return 2;
    }
    if (!strcmp(argv[i], "--fps-trace") && i + 1 < argc) { if (!enh.classic) snprintf(enh.fps_trace, sizeof enh.fps_trace, "%s", argv[i + 1]); return 2; }
    if (!strcmp(argv[i], "--compose-test")) { enh.compose_test = 1; return 1; }
    if (!strcmp(argv[i], "--wide-geometry-test")) { enh.compose_test = 2; return 1; }
    if (!strcmp(argv[i], "--dither") && i + 1 < argc) {
        if (!strcmp(argv[i + 1], "fine")) enh.dither_fine = !enh.classic;
        else if (!strcmp(argv[i + 1], "grid")) enh.dither_fine = false;
        else return 0;
        return 2;
    }
    if (!strcmp(argv[i], "--hr-check")) { if (!enh.classic) enh.hr_check = true; return 1; }
    if (!strcmp(argv[i], "--hr-dump") && i + 1 < argc) {
        if (!enh.classic) { snprintf(enh.hr_dump, sizeof enh.hr_dump, "%s", argv[i + 1]); enh.hr_check = true; }
        return 2;
    }
    if (!strcmp(argv[i], "--sandbox-test") && i + 1 < argc) {
        int n = atoi(argv[i + 1]);
        if (!enh.classic) enh.sandbox_test = n < 0 ? 0 : n > 1000 ? 1000 : n;
        return 2;
    }
    if (!strcmp(argv[i], "--mfd-probe") && i + 1 < argc) { if (!enh.classic) snprintf(enh.mfd_probe, sizeof enh.mfd_probe, "%s", argv[i + 1]); return 2; }
    if (!strcmp(argv[i], "--sandbox-hash") && i + 1 < argc) { snprintf(enh.sandbox_hash, sizeof enh.sandbox_hash, "%s", argv[i + 1]); return 2; }
    return 0;
}

void enh_env_args(void)
{
    static bool done;
    if (done) return;
    done = true;
    const char *e = getenv("F117_ENH_ARGS");
    if (!e || !*e) return;
    static char buf[512];
    char *argv[32];
    int argc = 0;
    snprintf(buf, sizeof buf, "%s", e);
    for (char *t = buf; *t && argc < 32;) {                 /* not strtok: the caller may be inside a strtok loop (simtest) */
        while (*t == ' ' || *t == '\t') *t++ = 0;
        if (!*t) break;
        argv[argc++] = t;
        while (*t && *t != ' ' && *t != '\t') t++;
    }
    for (int i = 0; i < argc;) { int used = enh_parse_arg(argc, argv, i); i += used ? used : 1; }
}
