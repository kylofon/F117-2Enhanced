/* The sandboxed display pass (sandbox.h; DESIGN.md 4.1, 4.2, 4.7; S2.4). */
#include "enhanced/sandbox.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comm.h"
#include "mem.h"
#include "enhanced/enh_canvas.h"
#include "enhanced/enhanced.h"
#include "enhanced/interp.h"
#include "enhanced/mapmfd.h"
#include "enhanced/record.h"
#include "host.h"
#include "enhanced/wide.h"
#include "gen/module_simflow.h"
#include "gen/module_video.h"
#include "sim/sim_priv.h"
#include "platform/timer.h"
#include "platform/vga.h"
#include "sound/snd_priv.h"

bool enh_sbx;

/* ---------------------------------------------------------------- the region table (the machine image without the canvas) */

#define MAX_REGIONS 128
typedef struct { u8 *p; size_t n; } Region;
static Region reg[MAX_REGIONS];
static int nreg;
static size_t reg_bytes;
static bool reg_overflow;

static void reg_add(void *p, size_t n)
{
    if (!p || !n) return;
    if (nreg == MAX_REGIONS) { reg_overflow = true; return; }
    reg[nreg].p = p; reg[nreg].n = n; nreg++;
    reg_bytes += n;
}

/* Built at every frame start (cheap), so a driver image loaded again is followed. */
static void reg_build(void)
{
    nreg = 0; reg_bytes = 0;
    reg_add(mem, MEM_SIZE);                                   /* every DGROUP, the comm block, the DOS arena, the BIOS data area */
    reg_add(&cur_dgroup, sizeof cur_dgroup);
    reg_add(&dg_sp, sizeof dg_sp);                            /* the emulated stack of the render's locals */
    reg_add((void *)&timer_ticks, sizeof timer_ticks);
    vga_sandbox_regions(reg_add);
    mgraphic_sandbox_regions(reg_add);
    pic_sandbox_regions(reg_add);
    r3d_sandbox_regions(reg_add);
    hr_sandbox_regions(reg_add);
}

typedef struct {
    u8 *buf;
    size_t cap;
    EnhCanvasSnap cv;
    bool valid;
} Image;

static Image img_a;           /* A_k: the pre-render image of this frame */
static Image img_live;        /* the live machine during a pass */
static EnhCanvasSnap cv_scratch;   /* the copy of A_k's canvas a pass draws on; holds the live canvas while the pass runs */

static bool image_save(Image *im)
{
    if (reg_overflow) return false;
    if (im->cap < reg_bytes) {
        free(im->buf);
        im->buf = malloc(reg_bytes);
        im->cap = im->buf ? reg_bytes : 0;
        if (!im->buf) return false;
    }
    u8 *q = im->buf;
    for (int i = 0; i < nreg; i++) { memcpy(q, reg[i].p, reg[i].n); q += reg[i].n; }
    return true;
}

static void image_load(const Image *im)
{
    const u8 *q = im->buf;
    for (int i = 0; i < nreg; i++) { memcpy(reg[i].p, q, reg[i].n); q += reg[i].n; }
}

/* ---------------------------------------------------------------- statistics */

static unsigned long long n_passes, n_takes;
static unsigned long long ns_take, ns_snap, ns_render, ns_restore;
static unsigned violations;
static const char *first_violation;

bool enh_sandbox_deny(const char *what)
{
    if (!enh_sbx) return false;
    if (!violations++) first_violation = what;
    return true;
}

/* S4.1: the display stream */
static unsigned long long n_refresh, n_refresh_lerp, n_frames_stream, n_steps_late;
static unsigned long long ns_r_wide, ns_r_main, ns_r_snap, ns_r_compose, ns_r_total, ns_r_max;
static Uint64 stream_t0, stream_t1;          /* display clock at the first / last stream frame */

static unsigned long long chk_refresh, chk_diff_refresh, chk_diff_px, n_reused, n_begin_refresh;
static unsigned long long n_rate_switch, n_refresh_div[4];   /* S4.3: the rate fallback (defined with rate_update) */
static Uint64 base_period(void);
static double est_cost;   /* S4.3 --fps-check */

/* S4.3: per refresh samples of each stage, for the average and the 95th percentile at exit (the display-stages line) */
enum { ST_SNAP, ST_RENDER, ST_COMPOSE, ST_TOTAL, ST_TAKE, ST_PRESENT, ST_N };
static const char *const st_name[ST_N] = { "snapshot", "render", "compose", "refresh", "take", "present" };
#define ST_CAP 65536
static u32 *st_s[ST_N];
static unsigned st_n[ST_N];
static unsigned long long st_sum[ST_N];

void enh_stage_sample(int st, u64 ns)
{
    if (st < 0 || st >= ST_N) return;
    st_sum[st] += ns;
    if (!st_s[st]) st_s[st] = malloc(ST_CAP * sizeof(u32));
    if (st_s[st] && st_n[st] < ST_CAP) st_s[st][st_n[st]] = ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)ns;
    st_n[st]++;
}

static int cmp_u32(const void *a, const void *b) { const u32 x = *(const u32 *)a, y = *(const u32 *)b; return x < y ? -1 : x > y; }

static void report_stages(FILE *f)
{
    if (!st_n[ST_TOTAL]) return;
    fprintf(f, "display-stages (ms avg / p95):");
    for (int i = 0; i < ST_N; i++) {
        if (!st_n[i]) continue;
        const unsigned m = st_n[i] < ST_CAP ? st_n[i] : ST_CAP;
        qsort(st_s[i], m, sizeof(u32), cmp_u32);
        fprintf(f, " %s %.2f / %.2f (%u);", st_name[i], st_sum[i] / (double)st_n[i] / 1e6, st_s[i][(size_t)((m - 1) * 0.95)] / 1e6, st_n[i]);
    }
    fprintf(f, "\n");
}

static void report_display(FILE *f)
{
    if (!n_refresh) return;
    report_stages(f);
    fprintf(f, "display-rate: refreshes at every display period %llu, every 2nd %llu, every 3rd or later %llu; rate switches %llu; pass cost EMA %.2f ms; display period %.2f ms\n",
            n_refresh_div[1], n_refresh_div[2], n_refresh_div[3], n_rate_switch, est_cost / 1e6, (double)base_period() / 1e6);
    fprintf(f, "display-begin: %llu refreshes at a frame start\n", n_begin_refresh);
    enh_interp_stats(f);   /* S4.2 */
    enh_map_stats(f);      /* S4.6 */
    fprintf(f, "display-reuse: %llu refreshes showed the canonical render (alpha 1, not extended)\n", n_reused);
    if (enh.fps_check) fprintf(f, "fps-check: %llu refreshes compared, %llu differ, %llu pixels differ\n", chk_refresh, chk_diff_refresh, chk_diff_px);
    const double r = (double)n_refresh, secs = stream_t1 > stream_t0 ? (double)(stream_t1 - stream_t0) / 1e9 : 0;
    u64 pn = 0, pns = 0;
    host_present_stats(&pn, &pns);
    fprintf(f, "display: %llu refreshes in %llu stream frames (%llu interpolated), %.1f per s; per refresh %.2f ms (snapshot + restore %.2f, wide render %.2f, main render %.2f, "
            "compose %.2f; max %.2f); steps late %llu; presents %llu, %.2f ms each\n", n_refresh, n_frames_stream, n_refresh_lerp, secs > 0 ? r / secs : 0.0,
            ns_r_total / r / 1e6, ns_r_snap / r / 1e6, ns_r_wide / r / 1e6, ns_r_main / r / 1e6, ns_r_compose / r / 1e6, ns_r_max / 1e6, n_steps_late,
            (unsigned long long)pn, pn ? (double)pns / (double)pn / 1e6 : 0.0);
}

static void report(FILE *f)
{
    report_display(f);
    if (!n_passes && !n_takes) return;
    const double p = n_passes ? (double)n_passes : 1.0, t = n_takes ? (double)n_takes : 1.0;
    fprintf(f, "sandbox: %llu passes, image %zu B + canvas %zu B; A_k %.0f us, pass snapshot %.0f us, render %.0f us, restore %.0f us; violations %u%s%s\n",
            n_passes, reg_bytes, enh_canvas_snap_bytes(&img_a.cv), ns_take / t / 1000.0, ns_snap / p / 1000.0, ns_render / p / 1000.0,
            ns_restore / p / 1000.0, violations, violations ? " first " : "", violations ? first_violation : "");
}

/* ---------------------------------------------------------------- --sandbox-hash */

static FILE *hash_f;
static unsigned long long hash_frame;
static unsigned long long opl_n;
static u64 opl_h = 0xCBF29CE484222325ull;

static u64 fnv(u64 h, const void *p, size_t n)
{
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

void enh_opl_trace(u8 reg_, u8 val)
{
    const u8 w[2] = { reg_, val };
    opl_h = fnv(opl_h, w, 2);
    opl_n++;
}

static void at_exit(void)
{
    if (hash_f) {
        fprintf(hash_f, "final comm=%016llx opl=%llu:%016llx passes=%llu violations=%u\n",
                (unsigned long long)fnv(0xCBF29CE484222325ull, comm_get(), COMM_SIZE), opl_n, (unsigned long long)opl_h, n_passes, violations);
        report(hash_f);
        fclose(hash_f);
        hash_f = NULL;
    }
    report(stdout);
}

/* One line per VGAME frame: the state the sim step k-1 left (DGROUP, comm block, the LCG 926E, the sound driver memory, the OPL stream). */
static void hash_line(void)
{
    static bool opened;
    if (!opened) {
        opened = true;
        hash_f = fopen(enh.sandbox_hash, "w");
        atexit(at_exit);
    }
    if (!hash_f) return;
    const u64 h0 = 0xCBF29CE484222325ull;
    u64 dg = fnv(h0, DSP(0), 0x10000);
    u64 cm = fnv(h0, comm_get(), COMM_SIZE);
    u64 sd = snd_m ? fnv(h0, snd_m, SND_MEM) : 0;
    u64 all = fnv(h0, mem, MEM_SIZE);                         /* also the pages, the DOS arena, the other programs' memory */
    u64 vg = fnv(h0, vga_vram(), VGA_VRAM_SIZE);              /* the canonical frame shown: a display leak would show here */
    for (int i = 0; i < 256; i++) { u8 rgb[3]; vga_dac_get((u8)i, &rgb[0], &rgb[1], &rgb[2]); vg = fnv(vg, rgb, 3); }
    EnhCanvasSnap c;                                          /* the canvas: shadow plane and the four layers (a leak only the hr picture shows) */
    enh_canvas_snap_peek(&c);
    u64 cv = fnv(h0, &c.cv, sizeof c.cv);
    cv = fnv(cv, c.sh_work, 64000 * 4); cv = fnv(cv, c.sh_shown, 64000 * 4);
    const size_t lb = c.layer_px * sizeof(u16);
    if (lb) { cv = fnv(cv, c.l3d_work, lb); cv = fnv(cv, c.l3d_shown, lb); cv = fnv(cv, c.lvec_work, lb); cv = fnv(cv, c.lvec_shown, lb); }
    if (lb && enh_true_night_on()) {                          /* S3.6: the night planes, when not empty (an empty plane is the original picture) */
        bool any = false;
        for (size_t i = 0; i < c.layer_px && !any; i++) any = c.l3n_work[i] || c.l3n_shown[i];
        if (any) { cv = fnv(cv, c.l3n_work, c.layer_px); cv = fnv(cv, c.l3n_shown, c.layer_px); }
    }
    fprintf(hash_f, "%llu dg=%016llx comm=%016llx seed=%08x snd=%016llx opl=%llu:%016llx mem=%016llx vga=%016llx canvas=%016llx\n", hash_frame++,
            (unsigned long long)dg, (unsigned long long)cm, (unsigned)DSL(0x926E), (unsigned long long)sd, opl_n, (unsigned long long)opl_h,
            (unsigned long long)all, (unsigned long long)vg, (unsigned long long)cv);
}

/* ---------------------------------------------------------------- hooks */

typedef enum { PASS_TEST, PASS_WIDE_FRAME, PASS_WIDE_DISP, PASS_MAIN_DISP, PASS_DISP } PassKind;   /* PASS_DISP (S4.3): the one pass of a refresh */
static void display_pass(int v, double alpha, PassKind kind);

static bool stream;            /* S4.1: this frame is shown by display refreshes (--fps on, no cockpit picture load) */
static bool stream_show;       /* the presenter shows the display pictures (false: the live page; it stays true over a live frame until its flip) */
static bool stream_wide;       /* a refresh renders the wide pass too (an extended view) */
static int frame_refreshes;
static unsigned long long vg_frame;
static FILE *trace_f;
static u32 *disp;              /* the newest display picture (disp_cv), new = not presented yet */
static size_t disp_cap;
static EnhCanvas disp_cv;
static bool disp_new;
static bool disp_presented;    /* S4.3: the picture host_pump presents next is a display picture (its present cost is a stage) */

void enh_present_done(u64 ns)
{
    if (disp_presented) enh_stage_sample(ST_PRESENT, ns);
    disp_presented = false;
}
static u8 *look_live;          /* the live DAC / CRTC / attribute / sequencer while a refresh's main pass composes */
static Uint64 t_flip, flip_ema, t_last_refresh;   /* display clock (ns) */
static double est_cost;        /* ns a refresh takes (EMA) */
static u64 deadline_tick;
static bool deadline_set;

/* the display clock: the SDL performance counter in ns (never the tick count or the sim's own clocks) */
static Uint64 dclock(void)
{
    static Uint64 f;
    if (!f) f = SDL_GetPerformanceFrequency();
    const Uint64 c = SDL_GetPerformanceCounter();
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}

static int frame_passes;
static int enh_test_night(void) { static int v = -1; if (v < 0) { const char *e = getenv("F117_TEST_NIGHT"); v = e && (*e == '1' || *e == '2') ? *e - '0' : 0; } return enh.classic ? 0 : v; }
static bool wide_due;          /* S3.4: this frame gets a wide display pass */

static void frame_begin_refresh(void);

void enh_frame_begin(void)
{
    const int tn = enh_test_night();
    if (tn) {                                /* S3.4 test aid (F117_TEST_NIGHT=1, real-path tests only): the night flag, so 7898 draws its bars in 0x2A */
        const bool was = DSW(0x3658) != 0;
        DSW(0x3658) = 1;
        if (tn == 2 && !was) pal_flight_load();   /* S3.6: F117_TEST_NIGHT=2 also loads the night palette (B) as a night mission does */
    }
    if (!enh_sbx && !enh.classic) {          /* S3.6 test aid (F117_TEST_POSE=N, real-path tests only): at VGAME frame N the aircraft is put in level, banked
                                              * flight at 3000 ft, 380 kt over the sea north of Tripoli (as F117_VGAME_DIVE, without the dive), so the sky shows */
        static int pose_at = -2, pose_hdg = -1, pose_alt = 3000, pose_bank = 0x1800;
        if (pose_at == -2) {
            const char *e = getenv("F117_TEST_POSE");      /* N[,heading word[,altitude ft[,bank word]]] (the last two: video capture aid) */
            pose_at = e && *e ? atoi(e) : -1;
            const char *c = e ? strchr(e, ',') : NULL;
            if (c) pose_hdg = (int)strtol(c + 1, NULL, 0);
            if (c && (c = strchr(c + 1, ','))) pose_alt = (int)strtol(c + 1, NULL, 0);
            if (c && (c = strchr(c + 1, ','))) pose_bank = (int)strtol(c + 1, NULL, 0);
        }
        if (pose_at >= 0 && pose_at-- == 0) {
            DSL(0xBA10) = 4288u * 32;
            DSL(0xC02A) = (0x8000u - 13700u) * 32;
            DSW(0x2DF6) = (u16)pose_alt;
            DSW(0xB052) = 380 * 27;
            DSW(0x2DFE) = DSW(0xB170) = 100;
            DSW(0x9B04) |= 1;
            DSW(0x2DF0) = 0x0200;                /* nose slightly up */
            DSW(0x2DF2) = (u16)pose_bank;        /* banked about 34 degrees by default */
            if (pose_hdg >= 0) DSW(0x2DEE) = (u16)pose_hdg;
            DSB(0x2DFB) = 1;
        }
    }
    if (enh.sandbox_hash[0]) hash_line();
    frame_passes = 0;
    img_a.valid = false;
    if (!enh_sbx) enh_canvas_frame_view(DSW(0xC07A), DSB(0x294B));   /* S3.4 fix 4: this frame's placement first */
    wide_due = !enh_sbx && enh_wide_due();
    if (!enh_sbx) {                          /* S4.1: the fields F_k, and whether display refreshes show this frame */
        vg_frame++;
        enh_interp_frame();
        enh_map_frame();                                      /* S4.6 */
        const bool cpic = DSW(0x2E48) != DSW(0xC07A);   /* view_frame loads a cockpit picture (a file read: no main display pass) */
        stream = enh_fps_on() && enh_canvas_active() && !cpic;
        if (stream) {
            stream_show = true;
            stream_wide = wide_due;
            if (!enh.mfd_probe[0]) wide_due = false;   /* the refreshes render the wide picture; the --mfd-probe test aid keeps the frame's own, which it measures */
            n_frames_stream++;
            if (!stream_t0) stream_t0 = dclock();
            stream_t1 = dclock();
            if (enh.fps_trace[0] && !trace_f) trace_f = fopen(enh.fps_trace, "w");
        } else stream_wide = false;
        frame_refreshes = 0;
        deadline_set = false;
    }
    if (!enh_sandbox_on() && !wide_due && !stream) return;
    static bool at_exit_set;
    if (!at_exit_set) { at_exit_set = true; if (!enh.sandbox_hash[0]) atexit(at_exit); }
    Uint64 t0 = SDL_GetTicksNS();
    reg_build();
    img_a.valid = image_save(&img_a) && enh_canvas_snap_take(&img_a.cv);
    ns_take += SDL_GetTicksNS() - t0;
    n_takes++;
    if (stream) frame_begin_refresh();                        /* S4.3 */
}

static void refresh(double alpha, bool before_flip);

void enh_idle(void)
{
    if (!img_a.valid || enh_sbx) return;
    while (frame_passes < enh.sandbox_test) {
        frame_passes++;
        enh_display_pass(frame_passes);
    }
    if (wide_due) {                                           /* S3.4: the frame again with the culling widened to the canvas; its picture is shown with the frame */
        wide_due = false;
        display_pass(0, 1.0, PASS_WIDE_FRAME);
    }
    if (stream && enh.fps_test > 0 && frame_refreshes == 0)  /* S4.1 test aid: K refreshes at alpha 1/K .. 1, no clock */
        for (int j = 1; j <= enh.fps_test; j++) refresh((double)j / enh.fps_test, true);
}

/* ---------------------------------------------------------------- S4.1: the display stream */

/* the time from one refresh to the next: the display's refresh period (vsync) or 1/60, 1/30 s, less 1.5 ms so that VSync, not this timer, decides (AotP) */
static int rate_div = 1;               /* S4.3: refresh every rate_div-th display period (the fallback when a refresh does not fit one period) */
/* S4.3 test aid (F117_TEST_REFRESH_MS=X): every pass refresh takes X ms longer (busy), as on a slower machine: the 30 Hz fallback must engage */
static Uint64 test_cost_ns(void) { static double v = -1; if (v < 0) { const char *e = getenv("F117_TEST_REFRESH_MS"); v = e && *e ? atof(e) : 0; } return (Uint64)(v * 1e6); }

static Uint64 base_period(void) { return enh.fps == 60 ? 16666667ull : enh.fps == 30 ? 33333333ull : host_refresh_ns(); }

static Uint64 refresh_interval(void)
{
    const Uint64 p = base_period() * (Uint64)rate_div;
    return p > 5500000ull ? p - 1500000ull : 4000000ull;
}

/* S4.3: the budget. A refresh (pass + compose; the presenter's copy and the present come on top) must take less than 3/4 of its interval, or the display
 * misses VSyncs at uneven times (judder): then refreshes go to every 2nd period (60 Hz -> 30 Hz; on a faster monitor the smallest divisor that fits, not below
 * ~30 Hz); back to the faster rate when the cost is under half of it. Decided from the EMA of the pass cost (est_cost), so a single slow refresh changes nothing. */
static void rate_update(void)
{
    if (enh.fps == 30 || enh.fps_test > 0 || est_cost <= 0) return;
    const double p = (double)base_period();
    int k = rate_div;
    while (est_cost > 0.75 * p * k && p * (k + 1) <= 34e6) k++;
    while (k > 1 && est_cost < 0.5 * p * (k - 1)) k--;
    if (k != rate_div) { rate_div = k; n_rate_switch++; }
}

/* alpha of a refresh at display time `now`: the time since the last flip over the step period (frame_ticks ticks, or the measured flip interval when longer) */
static double alpha_at(Uint64 now)
{
    if (!enh_interp_lerp()) return 1.0;
    const int n = host_frame_ticks();
    double period = n > 0 ? (double)n * (double)VGA_FRAME_NS_NUM / (double)VGA_FRAME_NS_DEN : 0.0;
    if ((double)flip_ema > period) period = (double)flip_ema;
    if (period <= 0 || !t_flip) return 1.0;
    const double a = (double)(now - t_flip) / period;
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

/* S4.3 test aid (--fps-check): the refresh built the S4.1 way (the wide pass, then the main pass that composes) and compared with the one pass */
static u32 *chk_buf;
static bool in_check;
static size_t chk_cap;

static void refresh_check(double alpha)
{
    in_check = true;
    if (stream_wide) display_pass(0, alpha, PASS_WIDE_DISP);
    display_pass(0, alpha, PASS_MAIN_DISP);
    in_check = false;
    const size_t px = (size_t)disp_cv.lw * (size_t)disp_cv.lh;
    if (!disp || !disp_new) return;
    if (chk_cap < px) { free(chk_buf); chk_buf = malloc(px * sizeof(u32)); chk_cap = chk_buf ? px : 0; }
    if (chk_buf) memcpy(chk_buf, disp, px * sizeof(u32));
}

/* S4.3 test aid (F117_DISP_HASH=FILE): per refresh picture its frame, refresh, size and FNV-1a hash (the picture before / after an optimisation) */
static void disp_hash(void)
{
    static FILE *f;
    static int state;
    if (!state) { const char *e = getenv("F117_DISP_HASH"); f = e && *e ? fopen(e, "w") : NULL; state = 1; }
    if (!f || in_check) return;
    const size_t px = (size_t)disp_cv.lw * (size_t)disp_cv.lh;
    fprintf(f, "%llu %d %dx%d %016llx\n", vg_frame, frame_refreshes + 1, disp_cv.lw, disp_cv.lh, (unsigned long long)fnv(0xCBF29CE484222325ull, disp, px * sizeof(u32)));
}

/* S4.6 test aid (F117_DISP_DUMP=PREFIX, F117_DISP_DUMP_FRAMES=a,b,...): those frames' refresh pictures as PREFIX_<frame>_<refresh>.raw (u32 w, h, n, fx, fy, then
 * w x h XRGB pixels) */
static void disp_dump(void)
{
    static const char *pre, *frames;
    static int state;
    if (!state) { pre = getenv("F117_DISP_DUMP"); frames = getenv("F117_DISP_DUMP_FRAMES"); state = 1; }
    if (!pre || !*pre || !frames || in_check || !disp) return;
    char key[32];
    snprintf(key, sizeof key, ",%llu,", vg_frame);
    char list[512];
    snprintf(list, sizeof list, ",%s,", frames);
    if (!strstr(list, key)) return;
    char path[1200];
    snprintf(path, sizeof path, "%s_%llu_%d.raw", pre, vg_frame, frame_refreshes + 1);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const u32 hd[5] = { (u32)disp_cv.lw, (u32)disp_cv.lh, (u32)disp_cv.n, (u32)disp_cv.fx, (u32)disp_cv.fy };
    fwrite(hd, sizeof hd, 1, f);
    fwrite(disp, sizeof(u32), (size_t)disp_cv.lw * (size_t)disp_cv.lh, f);
    fclose(f);
}

static void check_compare(void)
{
    const size_t px = (size_t)disp_cv.lw * (size_t)disp_cv.lh;
    if (!chk_buf || chk_cap < px || !disp) return;
    unsigned long long d = 0;
    for (size_t i = 0; i < px; i++) d += chk_buf[i] != disp[i];
    chk_refresh++;
    if (d) { chk_diff_refresh++; chk_diff_px += d; }
}

/* one display refresh: one pass on A_k at alpha (S4.3: in an extended view the wide pass, which composes its own picture; S4.1 ran the wide pass and then the
 * main pass) */
static u64 r_snap, r_render, r_compose;   /* S4.3: this refresh's stages */

/* S4.3: a refresh at alpha 1 in a view that is not extended, in the frame_floor wait (before the flip): the canonical frame's own render is the picture */
static bool reuse_canonical(void)
{
    const size_t px = (size_t)enh_cv.lw * (size_t)enh_cv.lh;
    if (disp_cap < px) { free(disp); disp = malloc(px * sizeof(u32)); disp_cap = disp ? px : 0; }
    if (!disp) return false;
    const Uint64 t0 = SDL_GetTicksNS();
    if (!enh_compose_canonical(disp, (unsigned)(u16)(DSW(0x2E34) * 160) * 2u)) return false;
    r_compose = SDL_GetTicksNS() - t0;
    disp_cv = enh_cv; disp_new = true;
    if (trace_f) enh_interp_trace(trace_f, vg_frame, frame_refreshes + 1, 1.0, (double)(t_last_refresh - stream_t0) / 1e6);   /* the live camera = the pass's */
    n_reused++;
    return true;
}

static void refresh(double alpha, bool before_flip)
{
    if (enh.fps_check) refresh_check(alpha);
    const Uint64 t0 = SDL_GetTicksNS();
    r_snap = r_render = r_compose = 0;
    t_last_refresh = dclock();
    const bool reused = before_flip && alpha >= 1.0 && !stream_wide && !enh.mfd_probe[0] && !enh_map_active() && reuse_canonical();   /* S4.6: not when the map's symbols
                                                              * are redrawn (their alpha-1 position keeps its sub-pixel fraction, the canonical frame's does not) */
    if (!reused) display_pass(0, alpha, PASS_DISP);
    if (!reused && test_cost_ns()) { const Uint64 e = SDL_GetTicksNS() + test_cost_ns(); while (SDL_GetTicksNS() < e) {} }   /* test aid: a slower machine */
    const Uint64 d = SDL_GetTicksNS() - t0;
    disp_hash();                                              /* test aid, outside the timed stages */
    disp_dump();                                              /* S4.6 test aid */
    if (enh.fps_test > 0 && disp_new) enh_record_sub(disp, disp_cv.lw, disp_cv.lh, alpha);   /* capture aid (F117_RECORD): every test refresh's picture */
    if (enh.fps_check) check_compare();
    frame_refreshes++;
    n_refresh++;
    if (enh_interp_lerp()) n_refresh_lerp++;
    ns_r_total += d;
    if (d > ns_r_max) ns_r_max = d;
    enh_stage_sample(ST_SNAP, r_snap); enh_stage_sample(ST_RENDER, r_render); enh_stage_sample(ST_COMPOSE, r_compose); enh_stage_sample(ST_TOTAL, d);
    if (!reused) { est_cost = est_cost > 0 ? 0.75 * est_cost + 0.25 * (double)d : (double)d; rate_update(); }   /* the cost of a pass (a reused picture is nearly free) */
    n_refresh_div[rate_div < 4 ? rate_div : 3]++;
}

/* S4.3: a refresh at the frame start (A_k just taken, nothing rendered yet), when the last one began at least half an interval ago. Between the flip and
 * the frame_floor wait the canonical frame waits for the frame tick ([262A], up to 14 ms) and renders: without this refresh no picture came for 15-20 ms
 * after every step, and one display period a step showed the same picture twice (a hitch at the step rate). Not in holds (their one picture is at alpha 1)
 * nor with --fps-test (its K refreshes stay in the wait). */
static void frame_begin_refresh(void)
{
    if (!img_a.valid || enh_sbx || enh.fps_test > 0 || !enh_interp_lerp() || !t_flip) return;
    const Uint64 now = dclock();
    if (t_last_refresh && now - t_last_refresh < refresh_interval() / 2) return;
    refresh(alpha_at(now), false);
    n_begin_refresh++;
}

void enh_display_wait(u64 dl)
{
    deadline_tick = dl; deadline_set = true;
    if (!stream || !img_a.valid || enh_sbx || enh.fps_test > 0) return;
    if (!enh_interp_lerp() && frame_refreshes > 0) return;  /* holding: the one picture at alpha 1 is the frame */
    const Uint64 now = dclock();
    if (frame_refreshes > 0) {                                /* it must end before the step is due: the flip keeps its time */
        const u64 due = host_tick_time_ns(dl), hn = host_ns();
        if (due <= hn || (double)(due - hn) < est_cost) return;
        const Uint64 since = now - t_last_refresh, iv = refresh_interval();
        if (since < iv) {                                     /* S4.3: early, when the refresh due at the interval would no longer fit before the step and this
                                                               * one does: the step's last picture then comes near its end, not a whole period before it */
            const bool late_misses = (double)(due - hn) < (double)(iv - since) + est_cost;
            if (!(late_misses && since >= iv / 2)) return;
        }
    }
    refresh(alpha_at(now), true);
}

void enh_frame_end(void)
{
    if (enh_sbx) return;
    if (stream && img_a.valid && frame_refreshes == 0) refresh(alpha_at(dclock()), true);   /* no time in the wait (or no wait): one refresh */
    if (!stream) stream_show = false;                         /* a live frame (or --fps off): from its flip on the presenter shows the live page */
    if (deadline_set && host_ticks() > deadline_tick) n_steps_late++;
    const Uint64 now = dclock();
    if (t_flip) { const Uint64 iv = now - t_flip; flip_ema = flip_ema ? (3 * flip_ema + iv) / 4 : iv; }
    t_flip = now;
}

void enh_display_hold(void)
{
    if (!stream || !stream_show || !img_a.valid || enh_sbx || enh_compose_menu()) return;
    refresh(1.0, false);                                      /* the paused frame as the faithful one (the step has not run yet: A_k is still this frame's) */
}

void enh_flight_end(void)
{
    stream = stream_show = stream_wide = false;
    disp_new = false;
    img_a.valid = false;
    t_flip = flip_ema = 0;
    enh_interp_reset();
    if (trace_f) fflush(trace_f);
}

int enh_display_take(u32 *out)
{
    if (!stream_show) return -1;
    if (!disp_new) return 0;
    disp_new = false;
    if (memcmp(&disp_cv, &enh_cv, sizeof disp_cv)) return 0;  /* the canvas changed since (a window resize): the next refresh has the new size */
    const Uint64 t0 = SDL_GetTicksNS();
    memcpy(out, disp, (size_t)disp_cv.lw * (size_t)disp_cv.lh * sizeof(u32));
    enh_stage_sample(ST_TAKE, SDL_GetTicksNS() - t0);
    disp_presented = true;
    return 1;
}

bool enh_display_ready(void) { return stream_show && disp_new; }

/* The test view of --sandbox-test: pass v of a frame looks from another place and heading than the canonical frame (v x 5.6 degrees,
 * v x 96 / 64 world units), so that its pictures, designators, tracer hits and driver state really differ from the frame's and any of
 * it that leaked out of the pass would show in the next frames' hashes. 2D94 (the camera matrix of the front view) is rebuilt from
 * the angles as DESIGN 4.2 step 2 does for the interpolated passes of S4.1. */
static void test_view(int v)
{
    if (v <= 0) return;
    DSW(0x2DEE) = (u16)(DSW(0x2DEE) + 0x0400 * v);
    DSSL(0xBA10) += 96 * v;
    DSSL(0xC02A) += 64 * v;
    sim_matrix_from_angles(0x2D94, (s16)DSW(0x2DEE), (s16)DSW(0x2DF0), (s16)DSW(0x2DF2));
}

/* DESIGN 4.2 steps 1-5. S2.4 renders A_k (with the test view v > 0, or as it is with v = 0; no interpolated fields, no wide culling)
 * and presents nothing. S3.4: the wide pass of the frame. S4.1: the two passes of a display refresh, at alpha. */
void enh_display_pass(int v) { display_pass(v, 1.0, PASS_TEST); }
void enh_sandbox_reload(void) { if (enh_sbx && img_a.valid) image_load(&img_a); }   /* S4.6 */

static void display_pass(int v, double alpha, PassKind kind)
{
    if (!img_a.valid || enh_sbx) return;
    Uint64 t0 = SDL_GetTicksNS();
    const bool disp_pass = kind == PASS_MAIN_DISP || kind == PASS_DISP;   /* the pass composes the refresh's picture */
    if (disp_pass) {                                          /* S4.1: the live DAC / CRTC (the timer's flicker and shake since A_k) */
        if (!look_live) look_live = malloc(vga_look_size());
        if (!look_live) return;
        vga_look_get(look_live);
    }
    if (!image_save(&img_live) || !enh_canvas_snap_copy(&cv_scratch, &img_a.cv)) { img_a.valid = false; return; }
    enh_canvas_snap_swap(&cv_scratch);                        /* the live canvas is now a copy of A_k's; cv_scratch holds the live one */
    image_load(&img_a);
    Uint64 t1 = SDL_GetTicksNS();

    enh_sbx = true;
    enh_map_prepare(kind >= PASS_WIDE_DISP ? alpha : -1.0);   /* S4.6: the map cache rendered when due (then A_k is loaded again); -1: not a display pass */
    enh_wide_drawn = false;
    enh_wide_pass = kind == PASS_WIDE_FRAME || kind == PASS_WIDE_DISP || (kind == PASS_DISP && stream_wide);
    if (enh_wide_pass) DSW(0x2E48) = DSW(0xC07A);             /* S3.4 fix 4: the cockpit picture counts as loaded (no file read in a pass); only the pass's 3D layer is used */
    test_view(v);
    if (kind >= PASS_WIDE_DISP) enh_interp_apply(alpha);   /* S4.1: the own aircraft at alpha (DESIGN 4.3) */
    if (kind >= PASS_WIDE_DISP) enh_map_apply(alpha);      /* S4.6: the map MFD's symbols at alpha (DESIGN 4.11) */
    view_frame();                                             /* the faithful frame's render, as frame_loop_body runs it */
    if (disp_pass && trace_f) enh_interp_trace(trace_f, vg_frame, frame_refreshes + 1, alpha, (double)(t_last_refresh - stream_t0) / 1e6);   /* S4.1 test aid: the camera of this refresh */
    hud_overlay_frame();
    if (DSW(0xC07A) == 0) {
        hud_draw_far();
        hud_side_strips(DSB(0x4088));
        cockpit_draw_adi();
    }
    flip(DSW(0x2E34) * 160);
    if (disp_pass && trace_f) enh_map_trace(trace_f, vg_frame, frame_refreshes + 1);   /* S4.6: after the HUD (the scope is drawn there) */
    if (kind == PASS_WIDE_FRAME) enh_wide_capture_to(0);      /* S3.4: the pass's layers (after its flip) are the pending wide picture */
    else if (kind == PASS_WIDE_DISP) enh_wide_capture_to(2);  /* S4.1: ... the wide picture of this refresh */
    Uint64 tc = SDL_GetTicksNS();
    if (disp_pass) {                                          /* S4.1: the refresh's picture, composed with the live look (DESIGN 4.2 step 4) */
        vga_look_set(look_live);
        const size_t px = (size_t)enh_cv.lw * (size_t)enh_cv.lh;
        if (disp_cap < px) { free(disp); disp = malloc(px * sizeof(u32)); disp_cap = disp ? px : 0; }
        if (disp) {
            if (kind == PASS_DISP && enh_wide_pass) enh_compose_display_self(disp);   /* S4.3: the wide pass's own picture */
            else enh_compose_display(disp);
            disp_cv = enh_cv; disp_new = true;
        }
    }
    enh_wide_on = enh_wide_pt = false;
    enh_wide_pass = false;
    enh_sbx = false;
    Uint64 t2 = SDL_GetTicksNS();

    image_load(&img_live);
    enh_canvas_snap_swap(&cv_scratch);                        /* the live canvas back, untouched */
    Uint64 t3 = SDL_GetTicksNS();
    ns_snap += t1 - t0; ns_render += t2 - t1; ns_restore += t3 - t2;
    n_passes++;
    if (kind == PASS_WIDE_DISP || (kind == PASS_DISP && enh_wide_drawn)) ns_r_wide += tc - t1;
    if (kind == PASS_MAIN_DISP || (kind == PASS_DISP && !enh_wide_drawn)) ns_r_main += tc - t1;
    if (disp_pass) ns_r_compose += t2 - tc;
    if (kind == PASS_DISP) {
        ns_r_snap += (t1 - t0) + (t3 - t2);
        r_snap += (t1 - t0) + (t3 - t2);
        r_render += tc - t1;
        r_compose += t2 - tc;
    }
}
