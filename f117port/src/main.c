/* F-117A Nighthawk Stealth Fighter 2.0 SDL3 port - entry point: F117.COM's chain loop (prog.c).
 *
 * usage: f117 [--game-dir DIR] [--scale N] [--aspect 4:3|1:1] [--fullscreen] [--headless]
 *             [--trace] [--check] [--chain-test] [--tick-test [MS]]
 *   --game-dir    folder with the original game files (default "Game"); the port never ships game data
 *   --scale       initial window scale: 320x240 (4:3) times N (default 3)
 *   --aspect      4:3 (default: the 200-line picture fills a 4:3 area, as on a VGA monitor) or 1:1
 *   --fullscreen  start in full screen (Alt+Enter switches)
 *   --headless    no window, no audio: for tests (the 70.09 Hz tick clock still runs)
 *   --trace       print the program switches and exit codes (also F117_TRACE=1)
 *   --check       verify that the game folder holds the files the chain needs, print a summary and exit
 *   --chain-test  run the whole chain with the placeholder programs without waiting, print the trace and
 *                 check it; exits 0 when the sequence was right (needs no game files)
 *   --keys        PORT: key bindings, "name=code,..." for the actions not on their default keys (the launcher's
 *                 Key Bindings dialog; keymap.c, PORTING.md). Without it every key goes to the game as it is
 *   --frame-ticks PORT: the game speed, at least N retrace ticks (70.09 a second) a frame in flight on top of the original's pacing
 *                 (default HOST_DEFAULT_FRAME_TICKS = 7, 10 frames a second; 0 = the original pacing only, about 17 frames a second)
 *   --version     print the version and exit
 *   --keymap-test the key map (keymap_parse and the remapping) against its expected key streams; no game files
 *   --tick-test   count the timer ticks over MS milliseconds (default 1000), print them and exit
 *   --video-test  a composed screen (picture, fonts, sprites, lines, palette fade); F117_SNAPSHOT_DIR writes it as PNG
 *   --video-selftest  the driver primitives against vectors from the original MGRAPHIC under Unicorn (video/videotest.c)
 *   --decoder-test DIR  every PIC / PAN / PAL / font in --game-dir against the Phase 2 Python decoders' output in DIR (77 = no game files)

 *   --debrief-test DIR   END against the references of port/notes/s58/end_ref.py (needs a scratch --game-dir with SCRATCH.MARK)
 *   --sound-test DIR     the sound driver (src/sound): OPL register streams against the original driver under Unicorn (port/notes/s59/sound_ref.py) and the poll rates (77 = no game files)
 *   --sim-test DIR       VGAME (src/sim) against the references of port/notes/s55/sim_ref.py (77 = no game files)
 *   --render-test DIR    the whole 3D scene (render3d) against the original: golden frames of port/notes/s56/r3d_golden.py (77 = no game files)
 *   --r3d-test FILE      the render3d leaf routines (src/render3d) against the vectors port/notes/s56/r3d_leaf_ref.py writes (77 = no game files)
 *   --shell-test DIR     START against the references of port/notes/s54/shell_ref.py (needs a scratch --game-dir with SCRATCH.MARK)
 *   --frontend-test DIR  the front end (SETUP, MPS_LOGO, PLAYER) against the references in DIR (port/notes/s53/frontend_ref.py writes them; 77 = no game files)
 *   --platform-test  headless checks of the platform layer (RTL, DOS files in --game-dir (a scratch folder!), keyboard,
 *                 joystick, timer); --exe-test loads the seven programs from --game-dir and checks their DGROUP (77 = no files)
 * "Sound off" in the launcher is SDL_AUDIO_DRIVER=dummy in the environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "keymap.h"
#include "mem.h"
#include "platform/platform.h"
#include "platform/timer.h"
#include "platform/vga.h"
#include "frontend/fe.h"
#include "shell/shell.h"
#include "debrief/debrief.h"
#include "sim/simtest.h"
#include "sound/snd.h"
int r3d_leaf_selftest(const char *file);                          /* render3d/r3dtest.c */
#include "programs/placeholder.h"
#include "prog.h"
#include "version.h"
#include "video/video.h"
#include "enhanced/enh_canvas.h"   /* ENH */
#include "enhanced/enhanced.h"   /* ENH */

static int usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--game-dir DIR] [--scale N] [--aspect 4:3|1:1] [--fullscreen] [--headless]\n"
            "          [--trace] [--check] [--chain-test] [--tick-test [MS]]\n"
            "          [--keys NAME=CODE,...] [--keymap-test] [--frame-ticks N] [--version] [--classic]\n"
            "          [--render-scale N] [--wide] [--no-wide] [--smooth-shading] [--true-night] [--fps vsync|60|30|off]\n"
            "          [--compose-test]   (Enhanced)\n", prog);
    return 2;
}

/* The chain with the placeholders must go SETUP, MPS_LOGO, PLAYER intro, then twice round
 * DSWAP 0 - START - DSWAP 1 - [PLAYER title-2] - VGAME - DSWAP 2 - END, and a third DSWAP 0 - START that quits. */
static int chain_test(int runs)
{
    static const int expect[PROG_COUNT] = {
        [PROG_SETUP] = 1, [PROG_MPS_LOGO] = 1, [PROG_PLAYER] = 2, [PROG_START] = 3, [PROG_VGAME] = 2,
        [PROG_END] = 2, [PROG_DSWAP] = 7,
    };
    int bad = 0, total = 0;
    printf("chain-test: runs per program:");
    for (int i = 0; i < PROG_COUNT; i++) {
        printf(" %s=%d", prog_table[i].exe, prog_run_count[i]);
        total += prog_run_count[i];
        if (prog_run_count[i] != expect[i]) bad++;
    }
    printf("\n");
    if (bad || total != runs) {
        printf("chain-test FAILED (%d programs differ from the expected sequence)\n", bad);
        return 1;
    }
    printf("chain-test OK: %d programs, START -> VGAME -> END -> START twice\n", runs);
    return 0;
}

int main(int argc, char **argv)
{
    HostConfig cfg = { "Game", 3, false, true, false };
    bool check = false, chain = false, tick = false, ptest = false, etest = false, vtest = false, vself = false;
    bool ktest = false;
    const char *simref = NULL;
    const char *r3dref = NULL;
    const char *decref = NULL, *feref = NULL, *shref = NULL, *dbref = NULL, *sndref = NULL;
    int tick_ms = 1000;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        { int used = enh_parse_arg(argc, argv, i); if (used) { i += used - 1; continue; } }   /* ENH: --classic and the Enhanced options */
        if (!strcmp(a, "--game-dir") && v) { cfg.game_dir = v; i++; }
        else if (!strcmp(a, "--scale") && v) { cfg.scale = atoi(v); i++; }
        else if (!strcmp(a, "--fullscreen")) cfg.fullscreen = true;
        else if (!strcmp(a, "--headless")) cfg.headless = true;
        else if (!strcmp(a, "--aspect") && v) {
            if (!strcmp(v, "4:3")) cfg.aspect_4_3 = true;
            else if (!strcmp(v, "1:1")) cfg.aspect_4_3 = false;
            else return usage(argv[0]);
            i++;
        }
        else if (!strcmp(a, "--trace")) prog_trace = true;
        else if (!strcmp(a, "--check")) check = true;
        else if (!strcmp(a, "--keys") && v) {                                             /* PORT */
            if (!keymap_parse(v)) { fprintf(stderr, "bad --keys list: %s\n", v); return usage(argv[0]); }
            i++;
        }
        else if (!strcmp(a, "--keymap-test")) ktest = true;
        else if (!strcmp(a, "--version")) { printf("f117-enhanced %s\n", F117_VERSION); return 0; }
        else if (!strcmp(a, "--frame-ticks") && v) { host_set_frame_ticks(atoi(v)); i++; }    /* PORT: the game speed */
        else if (!strcmp(a, "--chain-test")) chain = true;
        else if (!strcmp(a, "--platform-test")) ptest = true;
        else if (!strcmp(a, "--exe-test")) etest = true;
        else if (!strcmp(a, "--video-test")) vtest = true;
        else if (!strcmp(a, "--video-selftest")) vself = true;
        else if (!strcmp(a, "--decoder-test") && v) { decref = v; i++; }
        else if (!strcmp(a, "--frontend-test") && v) { feref = v; i++; }
        else if (!strcmp(a, "--shell-test") && v) { shref = v; i++; }
        else if (!strcmp(a, "--debrief-test") && v) { dbref = v; i++; }
        else if (!strcmp(a, "--sound-test") && v) { sndref = v; i++; }
        else if (!strcmp(a, "--sim-test") && v) { simref = v; i++; }
        else if (!strcmp(a, "--render-test") && v) { simref = v; sim_test_render_mode(); i++; }
        else if (!strcmp(a, "--r3d-test") && v) { r3dref = v; i++; }
        else if (!strcmp(a, "--tick-test")) {
            tick = true;
            if (v && v[0] != '-') { tick_ms = atoi(v); i++; }
        }
        else return usage(argv[0]);
    }
    enh_env_args();                                            /* ENH: S2.4, F117_ENH_ARGS before anything runs (--sandbox-hash sees the whole chain) */
    if (ktest) return keymap_selftest();                       /* needs neither window nor game files */
    if (check || tick || ptest || etest || vself || decref || feref || shref || dbref || sndref || simref || r3dref || enh.compose_test) cfg.headless = true;
    if (host_trace()) prog_trace = true;
    if (chain) { prog_trace = true; prog_use_stubs = true; }

    if (!host_init(&cfg)) return 1;
    host_install_crash_log(prog_current_name);                 /* PORT: f117.log beside the program */

    if (check) {
        const char *missing = game_files_missing();
        if (missing) {
            fprintf(stderr, "f117: game file %s not found in %s\n", missing, host_game_dir());
            host_shutdown();
            return 1;
        }
        printf("game folder %s: all files of the chain are present\n", host_game_dir());
        host_shutdown();
        return 0;
    }

    mem_init_machine();       /* IVT, BIOS data area */
    vga_init();               /* frame source, text mode */
    platform_init();          /* the 70.09 Hz tick -> timer_isr, the keyboard model */

    video_init();             /* the driver's state, the fonts of MGRAPHIC.EXE (silently none without the game files) */

    if (enh.compose_test) {                                    /* ENH: canvas geometry and compositor check */
        int rc = enh.compose_test == 2 ? enh_wide_geometry_test() : enh_compose_selftest();   /* ENH: S3.4 */
        host_shutdown();
        return rc;
    }

    if (vself || decref || vtest) {
        int rc = vself ? video_selftest() : decref ? video_decoder_test(decref) : video_screen_test(!cfg.headless);
        host_shutdown();
        return rc;
    }

    if (feref) {
        int rc = frontend_selftest(feref);
        host_shutdown();
        return rc;
    }

    if (shref) {
        int rc = shell_selftest(shref);
        host_shutdown();
        return rc;
    }

    if (sndref) {
        int rc = sound_selftest(sndref);
        host_shutdown();
        return rc;
    }

    if (dbref) {
        int rc = debrief_selftest(dbref);
        host_shutdown();
        return rc;
    }

    if (r3dref) {
        int rc = r3d_leaf_selftest(r3dref);
        host_shutdown();
        return rc;
    }

    if (simref) {
        int rc = sim_selftest(simref);
        host_shutdown();
        return rc;
    }

    if (ptest || etest) {
        int rc = ptest ? platform_selftest() : platform_exe_test();
        host_shutdown();
        return rc;
    }

    if (tick) {
        u64 end = host_ns() + (u64)tick_ms * 1000000ull;
        host_wait_until_ns(end);
        printf("ticks in %d ms: %u (%.2f Hz)\n", tick_ms, timer_ticks, timer_ticks * 1000.0 / tick_ms);
        host_shutdown();
        return 0;
    }

    if (!chain) {
        const char *missing = game_files_missing();
        if (missing) host_fatal("Game file %s not found in %s.\nUse --game-dir to point at the original game folder.",
                                missing, host_game_dir());
    }

    if (chain) placeholder_init();   /* the stub chain only: the real programs read their own keys, the chain quits on their exit codes */
    int runs = chain_run("");
    int rc = chain ? chain_test(runs) : 0;
    host_shutdown();
    return rc;
}
