#include "host.h"
#include "keymap.h"   /* PORT: key bindings */

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif

#include "opl3.h"
#include "enhanced/enh_canvas.h"   /* ENH: the canvas */
#include "enhanced/enhanced.h"
#include "enhanced/sandbox.h"     /* ENH: S2.4 */
#include "enhanced/record.h"      /* ENH: video capture aid (F117_RECORD) */

#define OPL_RATE 49716u                /* Nuked-OPL3 native rate; SDL resamples */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioStream *audio;
static SDL_Mutex *audio_mutex;
static SDL_Gamepad *gamepad;
static char *game_dir;
static bool headless;
static bool aspect43 = true;
static bool quit_requested;

static void (*tick_handler)(void);
static void (*kbd_handler)(u8);
static void (*focus_lost_handler)(void);
static void (*mouse_handler)(int, int, u8);
static bool (*frame_source)(u32 *);
static u32 *frame;                               /* ENH: dynamic, so the canvas can be larger than 640 x 400 */
static size_t frame_cap;
static int frame_w = 320, frame_h = 200;
static int nominal_w = 960, nominal_h = 720;     /* ENH: the window size a headless run pretends to have */

/* Logical presentation: frame width x 3/4 of it (4:3, as on a VGA monitor) or square pixels. */
static int view_w(void) { return frame_w; }
static int view_h(void)
{
    if (enh_canvas_active()) return aspect43 ? frame_h * 6 / 5 : frame_h;   /* ENH: the canvas is any size (frame_w * 3/4 for 320N x 200N) */
    return aspect43 ? frame_w * 3 / 4 : frame_h;
}

static void frame_reserve(int w, int h)          /* ENH */
{
    size_t n = (size_t)w * (size_t)h;
    if (n <= frame_cap) return;
    free(frame);
    frame = calloc(n, sizeof(u32));
    if (!frame) { fprintf(stderr, "out of memory for a %d x %d frame\n", w, h); exit(3); }
    frame_cap = n;
}

/* Tick clock: tick n is due at start + n * (359 200 / 25 175 000) s = n * 14 368 000 000 / 1007 ns (exact
 * rational arithmetic, one tick per VGA retrace, see host.h). */
static Uint64 clock_start_ns;
static Uint64 ticks_run;

/* ENH: S2.4, F117_VCLOCK=1 (test aid, ctest enh_sandbox_hash): a virtual clock, so that two runs of the chain are the same machine
 * run tick for tick. Every read advances it by 1 us, a host_pump that finds no tick due jumps to the next tick (so each pump
 * delivers one), and nothing sleeps. A display pass reads it without advancing it (a pass takes no time). */
static bool vclock;
static Uint64 vclock_ns;
static Uint64 clk_ns(void)
{
    if (!vclock) return SDL_GetTicksNS();
    if (!enh_sbx) vclock_ns += 1000;
    return vclock_ns;
}

/* Audio: one OPL3 chip in OPL2 mode. */
static opl3_chip opl;
static u8 opl_status;

/* Mouse: position in window-logical units, buttons, "changed" flag for the handler. */
static float mouse_x, mouse_y;
static u8 mouse_buttons;
static bool mouse_changed;

static int trace = -1;
static bool repaint;  /* the window changed size or mode: present the unchanged frame again */

static void process_events(void);

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    (void)userdata; (void)total;
    int frames = additional / 4;
    s16 buf[2 * 1024];
    SDL_LockMutex(audio_mutex);
    while (frames > 0) {
        int n = frames < 1024 ? frames : 1024;
        OPL3_GenerateStream(&opl, buf, (uint32_t)n);
        SDL_PutAudioStreamData(stream, buf, n * 4);
        frames -= n;
    }
    SDL_UnlockMutex(audio_mutex);
}

bool host_init(const HostConfig *cfg)
{
    headless = cfg->headless;
    const char *vd = SDL_getenv("SDL_VIDEO_DRIVER");
    if (vd && SDL_strcasecmp(vd, "dummy") == 0) headless = true;
    game_dir = SDL_strdup(cfg->game_dir);
    aspect43 = cfg->aspect_4_3;
    frame_reserve(HOST_FRAME_MAX_W, HOST_FRAME_MAX_H);
    nominal_w = 320 * (cfg->scale < 1 ? 3 : cfg->scale);
    nominal_h = (aspect43 ? 240 : 200) * (cfg->scale < 1 ? 3 : cfg->scale);
    { const char *w = SDL_getenv("F117_WINDOW"); int ww, wh;   /* ENH: S3.4 test aid, the window size a headless run pretends to have (WxH) */
      if (w && SDL_sscanf(w, "%dx%d", &ww, &wh) == 2 && ww >= 320 && wh >= 200 && ww <= 8192 && wh <= 8192) { nominal_w = ww; nominal_h = wh; } }
    { const char *v = SDL_getenv("F117_VCLOCK"); vclock = v && *v && *v != '0'; }   /* ENH: S2.4 */
    clock_start_ns = clk_ns();
    ticks_run = 0;
    OPL3_Reset(&opl, OPL_RATE);                          /* the chip model exists in every mode: the sound driver writes to it, headless runs just never render it */
    if (cfg->headless) return SDL_Init(0);              /* the clock and the files only: no window, no audio */

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    int scale = cfg->scale < 1 ? 3 : cfg->scale;
    if (!SDL_CreateWindowAndRenderer("F-117A Nighthawk Stealth Fighter 2.0", 320 * scale,
                                     (aspect43 ? 240 : 200) * scale, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window/renderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(renderer, 1);
    if (cfg->fullscreen) SDL_SetWindowFullscreen(window, true);
    SDL_HideCursor();                                    /* the programs draw their own pointer */
    host_set_frame_source(NULL, 320, 200);

    audio_mutex = SDL_CreateMutex();
    OPL3_Reset(&opl, OPL_RATE);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, (int)OPL_RATE };
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
        if (audio) SDL_ResumeAudioStreamDevice(audio);
        else fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    } else {
        fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    }

    clock_start_ns = clk_ns();                           /* ENH: S2.4 (F117_VCLOCK) */
    return true;
}

void host_shutdown(void)
{
    if (audio) SDL_DestroyAudioStream(audio);
    audio = NULL;
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    if (texture) SDL_DestroyTexture(texture);
    texture = NULL;
    if (renderer) SDL_DestroyRenderer(renderer);
    renderer = NULL;
    if (window) SDL_DestroyWindow(window);
    window = NULL;
    if (audio_mutex) SDL_DestroyMutex(audio_mutex);
    audio_mutex = NULL;
    SDL_free(game_dir);
    game_dir = NULL;
    SDL_Quit();
}

void host_set_tick_handler(void (*handler)(void)) { tick_handler = handler; }
u64  host_ticks(void) { return ticks_run; }

static int frame_ticks = HOST_DEFAULT_FRAME_TICKS;
void host_set_frame_ticks(int ticks) { frame_ticks = ticks < 0 ? 0 : ticks > 70 ? 70 : ticks; }
int  host_frame_ticks(void) { return frame_ticks; }
bool host_quit_requested(void) { return quit_requested; }
void host_set_kbd_handler(void (*handler)(u8)) { kbd_handler = handler; }
void host_set_focus_lost_handler(void (*handler)(void)) { focus_lost_handler = handler; }
void host_set_mouse_handler(void (*handler)(int, int, u8)) { mouse_handler = handler; }

/* ENH: sets the frame (and the texture) to w x h, any size up to the canvas limit. */
static void host_resize_frame(int w, int h)
{
    frame_w = w; frame_h = h;
    frame_reserve(w, h);
    if (!renderer) return;                                /* --headless */
    SDL_SetRenderLogicalPresentation(renderer, view_w(), view_h(), SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (texture) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
}

/* ENH: with the canvas on, its size follows the window (and the view): resize the frame to it before it is composed. */
static void canvas_sync(void)
{
    if (!enh_canvas_active()) return;
    int ww = nominal_w, wh = nominal_h;
    if (window && !headless) SDL_GetWindowSizeInPixels(window, &ww, &wh);
    enh_canvas_update(ww, wh, aspect43);
    if (enh_present_hold()) return;                       /* ENH: S3.4 fix 4, the last picture stays on screen until the new view's first frame */
    if (enh_cv.lw != frame_w || enh_cv.lh != frame_h) { host_resize_frame(enh_cv.lw, enh_cv.lh); repaint = true; }
}

void host_set_frame_source(bool (*compose)(u32 *), int w, int h)
{
    frame_source = compose;
    frame_w = SDL_clamp(w, 1, HOST_FRAME_MAX_W);
    frame_h = SDL_clamp(h, 1, HOST_FRAME_MAX_H);
    host_resize_frame(frame_w, frame_h);
}

uint64_t host_ns(void) { return clk_ns() - clock_start_ns; }   /* ENH: S2.4 (F117_VCLOCK) */

static Uint64 tick_due_ns(Uint64 n)
{
    return clock_start_ns + n * VGA_FRAME_NS_NUM / VGA_FRAME_NS_DEN;
}

uint64_t host_tick_time_ns(uint64_t tick) { return tick_due_ns(tick) - clock_start_ns; }   /* ENH: S4.1 */

/* ENH: S4.1, the display's refresh period (checked again every second: the window may move to another display) */
uint64_t host_refresh_ns(void)
{
    static Uint64 iv, checked_at;
    const Uint64 now = SDL_GetTicksNS();
    if (!iv || now - checked_at > SDL_NS_PER_SECOND) {
        float hz = 0;
        SDL_DisplayID d = window && !headless ? SDL_GetDisplayForWindow(window) : 0;
        const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
        if (m) hz = m->refresh_rate;
        if (!(hz > 20)) hz = 60;
        iv = (Uint64)(1e9 / hz);
        checked_at = now;
    }
    return iv;
}

static Uint64 pres_n, pres_ns;                    /* ENH: S4.1 */
void host_present_stats(uint64_t *n, uint64_t *ns) { *n = pres_n; *ns = pres_ns; }

bool host_trace(void)
{
    if (trace < 0) {
        const char *t = SDL_getenv("F117_TRACE");
        trace = t && *t && *t != '0';
    }
    return trace != 0;
}

/* ---------------------------------------------------------------- snapshots (F117_SNAPSHOT_DIR) */

static int snap_n;
static Uint64 snap_last_ns;

static void save_snapshot(const char *dir, const char *tag)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(frame_w, frame_h, SDL_PIXELFORMAT_XRGB8888, frame, frame_w * 4);
    if (!s) return;
    char path[768];
    if (tag) SDL_snprintf(path, sizeof path, "%s/snap%04d_%s.png", dir, snap_n++, tag);
    else SDL_snprintf(path, sizeof path, "%s/snap%04d.png", dir, snap_n++);
    if (!SDL_SavePNG(s, path)) {
        char *bmp = NULL;                                 /* PNG writer unavailable: BMP */
        SDL_asprintf(&bmp, "%.*s.bmp", (int)(strlen(path) - 4), path);
        if (bmp) SDL_SaveBMP(s, bmp);
        SDL_free(bmp);
    }
    SDL_DestroySurface(s);
    snap_last_ns = SDL_GetTicksNS();
    if (host_trace()) fprintf(stderr, "[host] snapshot %s\n", path);
}

static void snapshot(void)
{
    static const char *dir;
    static bool checked;
    static Uint64 interval = SDL_NS_PER_SECOND;
    if (!checked) {
        dir = SDL_getenv("F117_SNAPSHOT_DIR");
        const char *ms = SDL_getenv("F117_SNAPSHOT_MS");
        if (ms && SDL_atoi(ms) > 0) interval = (Uint64)SDL_atoi(ms) * SDL_NS_PER_MS;
        checked = true;
    }
    if (!dir) return;
    if (snap_n && SDL_GetTicksNS() - snap_last_ns < interval) return;
    save_snapshot(dir, NULL);
}

void host_snapshot_now(const char *tag)
{
    const char *dir = SDL_getenv("F117_SNAPSHOT_DIR");
    if (!dir) return;
    canvas_sync();                                        /* ENH: */
    if (frame_source) frame_source(frame);
    save_snapshot(dir, tag);
}

static void present(void);
/* Developer aid: an unchanged picture still gets its periodic snapshot (a static screen or a hang). */
static void snapshot_static(void)
{
    const char *dir = SDL_getenv("F117_SNAPSHOT_DIR");
    const char *ms = SDL_getenv("F117_SNAPSHOT_MS");
    Uint64 interval = (ms && SDL_atoi(ms) > 0) ? (Uint64)SDL_atoi(ms) * SDL_NS_PER_MS : SDL_NS_PER_SECOND;
    if (dir && snap_n && SDL_GetTicksNS() - snap_last_ns >= interval) save_snapshot(dir, NULL);
}

/* ENH: S3.4 test aid (F117_PRESENT_LOG=FILE): per presented picture the time, the size and the share of pure black pixels (the black flash of a view switch) */
static void present_log(void)
{
    static FILE *f;
    static bool checked;
    if (!checked) { const char *p = SDL_getenv("F117_PRESENT_LOG"); if (p && *p) f = fopen(p, "w"); checked = true; }
    if (!f || !frame) return;
    const size_t n = (size_t)frame_w * (size_t)frame_h;
    size_t black = 0;
    for (size_t i = 0; i < n; i++) black += (frame[i] & 0xFFFFFF) == 0;
    fprintf(f, "%.3f %d %d %.4f\n", (double)(clk_ns() - clock_start_ns) / 1e9, frame_w, frame_h, n ? (double)black / (double)n : 0.0);
    fflush(f);
}

static void present(void)
{
    present_log();                                        /* ENH: S3.4 test aid */
    if (enh_record_on()) enh_record_frame(frame, frame_w, frame_h, view_w(), view_h(),   /* ENH: capture aid (F117_RECORD), the clock read without advancing it */
                                          (double)((vclock ? vclock_ns : SDL_GetTicksNS()) - clock_start_ns) / 1e9);
    snapshot();
    if (!texture) return;
    const Uint64 t0 = SDL_GetTicksNS();                   /* ENH: S4.1, the cost per present */
    SDL_UpdateTexture(texture, NULL, frame, frame_w * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_FRect dst = { 0, 0, (float)view_w(), (float)view_h() };
    SDL_RenderTexture(renderer, texture, NULL, &dst);
    SDL_RenderPresent(renderer);
    pres_ns += SDL_GetTicksNS() - t0; pres_n++;           /* ENH: S4.1 */
}

static Uint64 last_present_ns;

/* ---------------------------------------------------------------- scripted keys (F117_KEYS) */

static void scripted_keys(void)
{
    static const char *spec;
    static bool checked;
    if (!checked) { spec = SDL_getenv("F117_KEYS"); checked = true; }
    if (!spec || !*spec || !kbd_handler) return;
    char *end;
    double at = SDL_strtod(spec, &end);
    if (end == spec || *end != ':') { spec = NULL; return; }
    if ((double)(clk_ns() - clock_start_ns) / 1e9 < at) return;   /* ENH: S2.4 (F117_VCLOCK) */
    const char *p = end + 1;
    u16 keys[8];
    int n = 0;
    bool press = true, release = true;
    while (n < 8) {
        keys[n++] = (u16)SDL_strtoul(p, &end, 16);
        p = end;
        if (*p == 'p') { release = false; p++; }        /* held down */
        else if (*p == 'r') { press = false; p++; }      /* let go */
        if (*p != '+') break;
        p++;
    }
    if (host_trace()) fprintf(stderr, "[host] scripted key %04X at %.2f s\n", keys[0], at);
    if (press)
        for (int i = 0; i < n; i++) {
            if (keys[i] >> 8) kbd_handler((u8)(keys[i] >> 8));
            kbd_handler((u8)keys[i]);
        }
    if (release)
        for (int i = n - 1; i >= 0; i--) {
            if (keys[i] >> 8) kbd_handler((u8)(keys[i] >> 8));
            kbd_handler((u8)(keys[i] | 0x80));
        }
    spec = *p == ',' ? p + 1 : NULL;
}

/* ---------------------------------------------------------------- pump */

void host_pump(void)
{
    if (enh_sandbox_deny("host_pump")) return;      /* ENH: S2.4, no tick, event or present in a display pass */
    process_events();
    scripted_keys();
    if (mouse_changed && mouse_handler) {
        mouse_changed = false;
        int x, y;
        u8 b;
        host_mouse_read(&x, &y, &b);
        mouse_handler(x, y, b);
    }

    bool worked = false;
    Uint64 now = clk_ns();                          /* ENH: S2.4 (F117_VCLOCK) */
    if (vclock && tick_due_ns(ticks_run + 1) > now) now = vclock_ns = tick_due_ns(ticks_run + 1);
    int budget = 35;                                /* at most 0.5 s of catch-up per call */
    while (tick_due_ns(ticks_run + 1) <= now && budget-- > 0) {
        ticks_run++;
        if (tick_handler) tick_handler();
        worked = true;
    }
    if (budget < 0)                                 /* fell too far behind: resynchronise the clock */
        clock_start_ns = now - (tick_due_ns(ticks_run) - clock_start_ns);
    if (worked && enh_record_on()) {                /* ENH: capture aid (F117_RECORD), the OPL chip rendered up to the last tick */
        static Uint64 rec_done;
        const Uint64 due = ticks_run * OPL_RATE * (VGA_FRAME_NS_NUM / 1000000ull) / (VGA_FRAME_NS_DEN * 1000ull);
        s16 buf[2 * 1024];
        while (rec_done < due) {
            const int n = (int)SDL_min(due - rec_done, (Uint64)1024);
            OPL3_GenerateStream(&opl, buf, (uint32_t)n);
            enh_record_audio(buf, n);
            rec_done += (Uint64)n;
        }
    }

    /* Present at most once per ~8 ms; VSync paces it further. */
    if (frame_source && (now - last_present_ns >= 8 * SDL_NS_PER_MS || enh_display_ready())) {   /* ENH: S4.1, a display pass's picture at once */
        canvas_sync();                              /* ENH: the canvas follows the window */
        const bool changed = frame_source(frame);
        if (!changed) snapshot_static();
        if (changed || repaint) {
            repaint = false;
            present();
            last_present_ns = clk_ns();             /* ENH: S2.4 (F117_VCLOCK) */
            worked = true;
        }
    }
    if (!worked && !vclock) {                       /* ENH: S2.4, the virtual clock never sleeps */
        Uint64 next = tick_due_ns(ticks_run + 1);
        now = SDL_GetTicksNS();
        if (next > now) SDL_DelayNS(SDL_min(next - now, SDL_NS_PER_MS));
    }
}

void host_wait_until_ns(uint64_t t)
{
    if (enh_sbx) return;                            /* ENH: S2.4, no wait in a display pass */
    while (host_ns() < t) host_pump();
}

/* ---------------------------------------------------------------- keyboard */

/* XT set-1 make code of an SDL key; bit 8 set = grey key sent with an E0 prefix. 0 = not reported. */
#define GREY 0x100
static u16 xt_scan(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        static const u8 letter_scan[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                            0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return letter_scan[sc - SDL_SCANCODE_A];
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return (u16)(0x02 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return (u16)(0x3B + (sc - SDL_SCANCODE_F1));
    switch (sc) {
    case SDL_SCANCODE_F11:          return 0x57;
    case SDL_SCANCODE_F12:          return 0x58;
    case SDL_SCANCODE_ESCAPE:       return 0x01;
    case SDL_SCANCODE_MINUS:        return 0x0C;
    case SDL_SCANCODE_EQUALS:       return 0x0D;
    case SDL_SCANCODE_BACKSPACE:    return 0x0E;
    case SDL_SCANCODE_TAB:          return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET:  return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN:       return 0x1C;
    case SDL_SCANCODE_KP_ENTER:     return GREY | 0x1C;
    case SDL_SCANCODE_LCTRL:        return 0x1D;
    case SDL_SCANCODE_RCTRL:        return GREY | 0x1D;
    case SDL_SCANCODE_SEMICOLON:    return 0x27;
    case SDL_SCANCODE_APOSTROPHE:   return 0x28;
    case SDL_SCANCODE_GRAVE:        return 0x29;
    case SDL_SCANCODE_LSHIFT:       return 0x2A;
    case SDL_SCANCODE_BACKSLASH:    return 0x2B;
    case SDL_SCANCODE_COMMA:        return 0x33;
    case SDL_SCANCODE_PERIOD:       return 0x34;
    case SDL_SCANCODE_SLASH:        return 0x35;
    case SDL_SCANCODE_KP_DIVIDE:    return GREY | 0x35;
    case SDL_SCANCODE_RSHIFT:       return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY:  return 0x37;
    case SDL_SCANCODE_LALT:         return 0x38;
    case SDL_SCANCODE_RALT:         return GREY | 0x38;
    case SDL_SCANCODE_SPACE:        return 0x39;
    case SDL_SCANCODE_CAPSLOCK:     return 0x3A;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK:   return 0x46;
    case SDL_SCANCODE_KP_7:         return 0x47;
    case SDL_SCANCODE_KP_8:         return 0x48;
    case SDL_SCANCODE_KP_9:         return 0x49;
    case SDL_SCANCODE_KP_MINUS:     return 0x4A;
    case SDL_SCANCODE_KP_4:         return 0x4B;
    case SDL_SCANCODE_KP_5:         return 0x4C;
    case SDL_SCANCODE_KP_6:         return 0x4D;
    case SDL_SCANCODE_KP_PLUS:      return 0x4E;
    case SDL_SCANCODE_KP_1:         return 0x4F;
    case SDL_SCANCODE_KP_2:         return 0x50;
    case SDL_SCANCODE_KP_3:         return 0x51;
    case SDL_SCANCODE_KP_0:         return 0x52;
    case SDL_SCANCODE_KP_PERIOD:    return 0x53;
    case SDL_SCANCODE_HOME:         return GREY | 0x47;
    case SDL_SCANCODE_UP:           return GREY | 0x48;
    case SDL_SCANCODE_PAGEUP:       return GREY | 0x49;
    case SDL_SCANCODE_LEFT:         return GREY | 0x4B;
    case SDL_SCANCODE_RIGHT:        return GREY | 0x4D;
    case SDL_SCANCODE_END:          return GREY | 0x4F;
    case SDL_SCANCODE_DOWN:         return GREY | 0x50;
    case SDL_SCANCODE_PAGEDOWN:     return GREY | 0x51;
    case SDL_SCANCODE_INSERT:       return GREY | 0x52;
    case SDL_SCANCODE_DELETE:       return GREY | 0x53;
    default:                        return 0;
    }
}

static void feed(const u8 *bytes, int n)
{
    if (!kbd_handler) return;
    for (int i = 0; i < n; i++) kbd_handler(bytes[i]);
}

static void key_event(SDL_Scancode sc, bool down)
{
    if (sc == SDL_SCANCODE_PAUSE) {                 /* make only, no break sequence */
        static const u8 pause[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
        if (down) feed(pause, sizeof pause);
        return;
    }
    if (sc == SDL_SCANCODE_PRINTSCREEN) {
        static const u8 make[] = { 0xE0, 0x2A, 0xE0, 0x37 }, brk[] = { 0xE0, 0xB7, 0xE0, 0xAA };
        if (down) feed(make, sizeof make); else feed(brk, sizeof brk);
        return;
    }
    u16 x = xt_scan(sc);
    if (!x) return;
    u16 mapped[16];
    int m = keymap_map(x, down, mapped);           /* PORT: key bindings (-1: the key as it is) */
    if (m >= 0) {
        for (int i = 0; i < m; i++) {
            u8 seq[2];
            int n = 0;
            if (mapped[i] & GREY) seq[n++] = 0xE0;
            seq[n++] = (u8)(mapped[i] & 0xFF);
            feed(seq, n);
        }
        return;
    }
    u8 seq[2];
    int n = 0;
    if (x & GREY) seq[n++] = 0xE0;
    seq[n++] = (u8)(down ? (x & 0x7F) : (x & 0x7F) | 0x80);
    feed(seq, n);
}

static void process_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            quit_requested = true;                  /* the chain loop stops like after Ctrl-Break */
            break;
        case SDL_EVENT_KEY_DOWN:
            /* Alt+Enter toggles fullscreen; everything else goes to the game, repeats included. */
            if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
                if (!ev.key.repeat)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            key_event(ev.key.scancode, true);
            break;
        case SDL_EVENT_KEY_UP:
            key_event(ev.key.scancode, false);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:     /* Alt+Enter, resizing, a static picture: redraw it */
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        case SDL_EVENT_WINDOW_EXPOSED:
            repaint = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (focus_lost_handler) focus_lost_handler();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (renderer) SDL_ConvertEventToRenderCoordinates(renderer, &ev);
            mouse_x = ev.motion.x;
            mouse_y = ev.motion.y;
            mouse_changed = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            u8 bit = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_buttons |= bit; else mouse_buttons &= (u8)~bit;
            mouse_changed = true;
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad) gamepad = SDL_OpenGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad && SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
            }
            break;
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------- mouse, joystick */

void host_mouse_read(int *x, int *y, u8 *buttons)
{
    float px = SDL_clamp(mouse_x, 0.0f, (float)view_w() - 1);
    float py = SDL_clamp(mouse_y, 0.0f, (float)view_h() - 1);
    if (x) *x = (int)(px * frame_w / view_w());
    if (y) *y = (int)(py * frame_h / view_h());
    if (x && y && enh_canvas_active()) enh_canvas_to_frame(x, y);   /* ENH: canvas pixel -> 320x200 frame pixel */
    if (buttons) *buttons = mouse_buttons;
}

void host_mouse_warp(int x, int y)
{
    if (enh_canvas_active()) enh_frame_to_canvas(&x, &y);   /* ENH */
    mouse_x = (float)x * view_w() / frame_w;
    mouse_y = (float)y * view_h() / frame_h;
    if (window && !headless) {
        float wx, wy;
        if (SDL_RenderCoordinatesToWindow(renderer, mouse_x, mouse_y, &wx, &wy))
            SDL_WarpMouseInWindow(window, wx, wy);
    }
}

bool host_joy_present(void) { return gamepad != NULL; }

void host_joy_axes(s16 axes[4])
{
    for (int i = 0; i < 4; i++) axes[i] = 0;
    if (!gamepad) return;
    axes[0] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    axes[1] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    axes[2] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    axes[3] = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  axes[0] = -32768;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) axes[0] = 32767;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))    axes[1] = -32768;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  axes[1] = 32767;
}

u8 host_joy_buttons(void)
{
    if (!gamepad) return 0;
    u8 b = 0;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) b |= 1;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST))  b |= 2;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST))  b |= 4;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) b |= 8;
    return b;
}

/* ---------------------------------------------------------------- audio */

void host_audio_lock(void) { if (audio_mutex) SDL_LockMutex(audio_mutex); }
void host_audio_unlock(void) { if (audio_mutex) SDL_UnlockMutex(audio_mutex); }

void host_opl_write(u8 reg, u8 value)
{
    if (enh_sandbox_deny("opl_write")) return;      /* ENH: S2.4, silent in a display pass; the stream for --sandbox-hash */
    if (enh.sandbox_hash[0]) enh_opl_trace(reg, value);
    host_audio_lock();
    if (reg == 0x04) {                               /* timer control: model the status flags only */
        if (value & 0x80) opl_status = 0;
        else {
            if ((value & 0x01) && !(value & 0x40)) opl_status |= 0xC0;   /* timer 1 expires at once */
            if ((value & 0x02) && !(value & 0x20)) opl_status |= 0xA0;   /* timer 2 */
        }
    }
    OPL3_WriteReg(&opl, reg, value);
    host_audio_unlock();
}

u8 host_opl_status(void) { return opl_status; }

/* ---------------------------------------------------------------- files, errors */

char *host_game_path(const char *name, bool create)
{
    char *direct = NULL;
    SDL_asprintf(&direct, "%s/%s", game_dir, name);
    if (SDL_GetPathInfo(direct, NULL)) return direct;

    int count = 0;
    char **entries = SDL_GlobDirectory(game_dir, NULL, 0, &count);
    char *found = NULL;
    for (int i = 0; entries && i < count; i++) {
        if (SDL_strcasecmp(entries[i], name) == 0) {
            SDL_asprintf(&found, "%s/%s", game_dir, entries[i]);
            break;
        }
    }
    SDL_free(entries);
    if (found) { SDL_free(direct); return found; }
    if (create) return direct;
    SDL_free(direct);
    return NULL;
}

const char *host_game_dir(void) { return game_dir; }

void host_free(void *p) { SDL_free(p); }

/* ---------------------------------------------------------------- the error log (f117.log beside the program) */

static void log_line(const char *msg)
{
    fprintf(stderr, "f117: %s\n", msg);
    fflush(stderr);
    const char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof path, "%sf117.log", base ? base : "");
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t now = time(NULL);
    char stamp[32] = "";
    struct tm *tm = localtime(&now);
    if (tm) strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "%s %s\n", stamp, msg);
    fclose(f);
}

void host_log_error(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_line(msg);
}

static const char *(*crash_where)(void);

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e)
{
    host_log_error("crash: exception %08lX at %p in %s", (unsigned long)e->ExceptionRecord->ExceptionCode,
                   e->ExceptionRecord->ExceptionAddress, crash_where ? crash_where() : "?");
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void crash_signal(int sig)
{
    host_log_error("crash: signal %d in %s", sig, crash_where ? crash_where() : "?");
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void host_install_crash_log(const char *(*where)(void))
{
    crash_where = where;
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#else
    signal(SIGSEGV, crash_signal);
    signal(SIGFPE, crash_signal);
    signal(SIGILL, crash_signal);
    signal(SIGBUS, crash_signal);
#endif
}

_Noreturn void host_fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    host_log_error("fatal: %s", msg);
    if (window && !headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "F-117A Nighthawk", msg, window);
    host_shutdown();
    exit(3);
}
