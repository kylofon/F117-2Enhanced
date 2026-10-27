#pragma once
/* Host services on top of SDL3: window/present, the 70.09 Hz retrace tick, keyboard (XT set-1 byte
 * stream for the programs' own INT 9 logic), mouse, gamepad, audio (OPL2 through Nuked-OPL3), game file
 * lookup, fatal errors and the headless developer aids. No game logic lives here; only host.c includes
 * SDL (mem.c has no SDL either). */
#include "types.h"

/* The monitor in VGA mode 13h (PLAN.md Decisions 2026-10-13, platform.md §4, §6): 25.175 MHz dot clock,
 * 800 x 449 clocks per frame = 70.0866 Hz. One timer tick = one vertical retrace; the same period
 * drives the 3DAh retrace model (platform/vga.c). Written as 14 368 000 000 / 1007 ns (359 200 clocks
 * at 25.175 MHz, reduced) so that tick n x NUM does not overflow 64 bits for 200 days. */
#define VGA_FRAME_NS_NUM 14368000000ull
#define VGA_FRAME_NS_DEN 1007ull

/* Start-up options (main.c fills them from the command line). */
typedef struct {
    const char *game_dir;
    int  scale;          /* window = 320 x 240 (4:3) times scale */
    bool fullscreen;
    bool aspect_4_3;     /* false: square pixels (--aspect 1:1) */
    bool headless;       /* --headless: no window, no audio (tests); the tick clock still runs */
} HostConfig;

bool host_init(const HostConfig *cfg);
void host_shutdown(void);

/* Called once per retrace tick (70.09 Hz) from host_pump(): the port's replacement for the timer
 * IRQ. platform/timer.c installs its ISR here; per-program ISR behaviour (7th-tick skip, frame-rate
 * estimator, sound poll) is built on that (S5.1, S5.9). */
void host_set_tick_handler(void (*handler)(void));
u64  host_ticks(void);                       /* ticks delivered since host_init */
/* PORT: the game speed (--frame-ticks N, the launcher's "Game speed"; PLAN.md Decisions 2026-10-16): VGAME's frame loop takes at least N
 * ticks (70.09 a second) a frame, on top of the original's own pacing. The original's estimator caps itself at about 17 frames a second on
 * any fast PC; the default 7 gives 10 (a 1991 PC's rate, the middle of the 4..15 the estimator was built for). 0 = the original pacing only. */
#define HOST_DEFAULT_FRAME_TICKS 7
void host_set_frame_ticks(int ticks);
int  host_frame_ticks(void);

/* Source of the displayed image: fills a w x h XRGB8888 frame and returns true if it changed since the
 * last call. Installed by the VGA model (platform/vga.c). Shown with 4:3 aspect unless --aspect 1:1. */
#define HOST_FRAME_MAX_W 640            /* S5.3: 640 x 400 = the text-mode screen (SETUP); mode 13h is 320 x 200 */
#define HOST_FRAME_MAX_H 400
void host_set_frame_source(bool (*compose)(u32 *xrgb), int w, int h);

/* Runs due timer ticks, handles window/input events, runs the scripted keys and presents the screen
 * when it changed. Every busy-wait loop of the original (tick waits, key polls, retrace polls) must
 * call this (or a helper that does). Sleeps briefly when nothing was due. Returns false once the
 * window was closed (host_quit_requested()). */
void host_pump(void);
bool host_quit_requested(void);              /* window closed: the chain loop stops (Ctrl-Break model) */

/* Nanoseconds since host_init (the time base of the tick clock and of the VGA retrace model). */
uint64_t host_ns(void);
uint64_t host_tick_time_ns(uint64_t tick);   /* ENH: S4.1, when tick number `tick` is due, on the host_ns() scale */
uint64_t host_refresh_ns(void);              /* ENH: S4.1, the refresh period of the window's display (60 Hz when unknown or headless) */
void host_present_stats(uint64_t *n, uint64_t *ns);   /* ENH: S4.1, presents so far and the time spent in them (texture upload + present) */
/* Pumps until host_ns() >= t. */
void host_wait_until_ns(uint64_t t);

/* ---- Keyboard: the byte sequence an XT keyboard would put on port 60h, in event order: normal keys
 * sc / sc|80h, grey keys E0 sc / E0 sc|80h, Pause E1 1D 45 E1 9D C5, Print Screen E0 2A E0 37 /
 * E0 B7 E0 AA. Key repeats feed the make code again. The platform keyboard module runs the ported
 * INT 9 logic on it (platform.md §5.1). */
void host_set_kbd_handler(void (*handler)(u8 byte));
void host_set_focus_lost_handler(void (*handler)(void));   /* keys released outside the window */

/* ---- Mouse (INT 33h model, START and END only): position in 320x200 screen pixels, buttons bit0 left,
 * bit1 right. The handler is called from host_pump() whenever position or buttons changed. */
void host_set_mouse_handler(void (*handler)(int x, int y, u8 buttons));
void host_mouse_read(int *x, int *y, u8 *buttons);
void host_mouse_warp(int x, int y);

/* ---- Joystick (port 201h model): first gamepad. Stick 0 = left stick (A X/Y), stick 1 = right stick
 * (B X/Y), axes -32768..32767; buttons bit0 A1 (south), bit1 A2 (east), bit2 B1 (west), bit3 B2 (north). */
bool host_joy_present(void);
void host_joy_axes(s16 axes[4]);
u8   host_joy_buttons(void);

/* ---- Audio. OPL2 = one Nuked-OPL3 in OPL2 mode, rendered at 49716 Hz on the audio thread. The sound
 * driver's poll (slot 103) is NOT run there: it is part of the timer ISR and runs from the tick handler
 * on the main thread (platform.md §4, sound.md); its register writes go through host_opl_write, which
 * takes the audio lock. Game-side sound calls take the lock around their work (the original's cli). */
void host_opl_write(u8 reg, u8 value);
u8   host_opl_status(void);                                  /* port 388h read (timer flags) */
void host_audio_lock(void);
void host_audio_unlock(void);

/* ---- Game files: case-insensitive lookup inside the game directory. Returns a malloc'd path (free with
 * host_free) or NULL if the file does not exist; with create = true the path of a new file. */
char *host_game_path(const char *name, bool create);
const char *host_game_dir(void);
void  host_free(void *p);

/* ---- Errors: prints (and shows unless headless) a message, shuts down and exits with code 3. */
_Noreturn void host_fatal(const char *fmt, ...);
/* PORT: an error worth reporting: printed to stderr and appended, with the date, to f117.log beside the program (the launcher
 * hides stderr). host_fatal and the crash handler log through it. */
void host_log_error(const char *fmt, ...);
/* PORT: a crash (access violation, division by zero, ...) is logged as "crash: ... in <where()>" before the program dies. */
void host_install_crash_log(const char *(*where)(void));

/* ---- Developer aids (environment variables; work headless with SDL_VIDEO_DRIVER=dummy and
 *      SDL_AUDIO_DRIVER=dummy; "sound off" in the launcher is SDL_AUDIO_DRIVER=dummy):
 *   F117_SNAPSHOT_DIR=dir     every presented frame >= F117_SNAPSHOT_MS (default 1000) after the
 *                             previous snapshot is saved as snapNNNN.png
 *   F117_KEYS="<seconds>:<xt>[+<xt>...],..."  presses (in order) and releases (in reverse) the XT keys
 *                             that many seconds after start-up; "p" after the codes only presses (held),
 *                             "r" only releases: "9:48p,20:48r" holds Up for 11 s. Grey keys are written
 *                             with their E0 prefix, e.g. e048.
 *   F117_TRACE=1              prints program switches and exit codes to stderr */
void host_snapshot_now(const char *tag);                     /* forces a snapshot (if enabled) */
bool host_trace(void);
