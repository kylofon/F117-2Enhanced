/* Start-up, exit, run-time errors and the near heap (platform.md §2 "Start-up, exit, fatal errors", §3.2, §7).
 *
 * In the original F117.COM EXECs a program, the MSC c0 code sets up DGROUP, the stack and the near heap, runs
 * main and ends with exit(code) / INT 21h/4Ch. Here platform_exec() is the EXEC: it loads the image (exeload.c),
 * calls c0_start() for the DGROUP stack/heap model, runs `<name>_main(tail)` and turns exit_fn(code) anywhere in
 * the program into the return value of platform_exec() with a longjmp, so a fatal error deep in a DOS wrapper
 * ends the program with the original's exit code, as INT 21h/4Ch does. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen/module_platform.h"
#include "host.h"
#include "platform/platform.h"
#include "platform/timer.h"
#include "enhanced/sandbox.h"      /* ENH: S4.1 */

ProgId plat_prog;
static ProgImage plat_img;
static bool plat_loaded;                    /* a real image (false: placeholder programs without game files) */
static jmp_buf exit_jmp;
static int exit_code;
static bool in_exec;

/* The library state of each program at its own DGROUP offsets. Derived from the disassembly (the offsets are
 * in the instruction operands of srand/break_isr/joy_scale/mouse_poll/dos_buf_fill of each image):
 *   srand        END 52CA, SETUP 179E, START 96BC, VGAME F310     break_isr  DSWAP 0563, END 4597, SETUP 0C75,
 *   joy_scale    END 441D, START 84B1, SETUP 0237:00A4, VGAME 10A8:0CFF      START 85F8, VGAME 1BA0
 *   mouse_poll   END 44B9, START 8545                 dos_buf_fill  END 4160, START 81C6, SETUP 0F19, DSWAP 06EA,
 * NB the spec's "[5158:515A]" for VGAME's seed is END's; VGAME's seed is at 926E (F310). */
static const ProgDs ds_table[PROG_COUNT] = {
    [PROG_SETUP]    = { .rand_seed = 0x0EB4, .break_flag = 0x0AC8, .break_value = 0xFF, .joy_cal = 0x0DF4,
                        .dosbuf = 0x0B88, .dosbuf_fd = 0x0DF2 },
    [PROG_MPS_LOGO] = { 0 },
    [PROG_PLAYER]   = { .break_flag = 0x2092, .break_value = 3 },     /* PLAYER break_isr 0560 sets [2092] = 3 (frontend.md 8.3.3) */
    [PROG_START]    = { .rand_seed = 0xAE8C, .break_flag = 0x73F2, .break_value = 1, .joy_cal = 0x6982,
                        .mouse_x = 0xE08A, .mouse_y = 0xE08C, .mouse_btn = 0x69B6,
                        .dosbuf = 0x6235, .dosbuf_fd = 0x649F },
    [PROG_VGAME]    = { .rand_seed = 0x926E, .break_flag = 0x2624, .break_value = 0xFF, .joy_cal = 0x2CA2,
                        .dosbuf = 0x43C8, .dosbuf_fd = 0x4632 },
    [PROG_END]      = { .rand_seed = 0x5158, .break_flag = 0x2608, .break_value = 1, .joy_cal = 0x23C4,
                        .mouse_x = 0x7214, .mouse_y = 0x7216, .mouse_btn = 0x23F8,
                        .dosbuf = 0x1C77, .dosbuf_fd = 0x1EE1 },
    [PROG_DSWAP]    = { .break_flag = 0x00C8, .break_value = 0xFF, .dosbuf = 0x0187, .dosbuf_fd = 0x03F1 },
};

/* Once, after host_init, mem_init_machine and vga_init: the keyboard model and the timer's BIOS clock. */
void platform_init(void)
{
    timer_init();
    kbd_init();
    break_hook_reset();
    timer_poll_bios_clock();
}

const ProgDs *plat_ds(void) { return &ds_table[plat_prog]; }

/* ------------------------------------------------------------------ the near heap */
/* PORT: the MSC near heap (rtl_nmalloc / rtl_nfree / rtl_nh_*) lives between the stack and the end of DGROUP;
 * its bookkeeping words ([50EE..50F8] in END) and the first-fit/rover details are not reproduced. This one
 * is a first-fit allocator in the same DGROUP range, so near pointers are real DS offsets: block = 2-byte
 * size word (bit 0 = in use) followed by the data. */
#define HEAP_END 0xFFF0u
static u16 heap_lo, heap_hi;                /* [heap_lo, heap_hi) */

void rtl_heap_init(u16 lo)
{
    heap_lo = (u16)((lo + 3) & ~1u);
    heap_hi = HEAP_END;
    if (lo == 0 || heap_lo >= heap_hi) { heap_lo = heap_hi = 0; return; }
    DSW(heap_lo) = (u16)(heap_hi - heap_lo);           /* one free block */
}

u16 rtl_malloc_ds(unsigned n)
{
    if (!heap_hi || n == 0 || n > 0xFFF0) return 0;
    u16 need = (u16)(((n + 1) & ~1u) + 2);
    for (u16 b = heap_lo; b < heap_hi;) {
        u16 sz = (u16)(DSW(b) & ~1u);
        if (sz < 2) host_fatal("near heap corrupt at DS:%04X", b);
        if (!(DSW(b) & 1)) {
            u16 nb = (u16)(b + sz);                    /* coalesce free neighbours (rtl_nh_search) */
            while (nb < heap_hi && !(DSW(nb) & 1)) { sz = (u16)(sz + (DSW(nb) & ~1u)); nb = (u16)(b + sz); }
            DSW(b) = sz;
            if (sz >= need) {
                if (sz - need >= 4) { DSW((u16)(b + need)) = (u16)(sz - need); sz = need; }
                DSW(b) = (u16)(sz | 1);
                return (u16)(b + 2);
            }
        }
        b = (u16)(b + sz);
    }
    return 0;
}

void rtl_free_ds(u16 off)
{
    if (!off || !heap_hi || off < heap_lo + 2u || off >= heap_hi) return;
    DSW((u16)(off - 2)) &= (u16)~1u;
}

void *rtl_nmalloc(unsigned n)
{
    u16 o = rtl_malloc_ds(n);
    return o ? DSP(o) : NULL;
}

void rtl_nfree(void *p) { if (p) rtl_free_ds(ds_off(p)); }

void *malloc_fn(unsigned n) { return rtl_nmalloc(n); }

unsigned stackavail(void) { return plat_loaded ? (unsigned)(dg_sp - plat_img.stack_limit) : 0; }

/* ------------------------------------------------------------------ run-time errors */
/* The message table at DS:535E in END: id, text. "run-time error " is message 255 after the "\r\n" of 252. */
static const struct { int id; const char *text; } nmsg[] = {
    { 0,   "R6000\r\n- stack overflow\r\n" },
    { 3,   "R6003\r\n- integer divide by 0\r\n" },
    { 9,   "R6009\r\n- not enough space for environment\r\n" },
    { 252, "\r\n" },
    { 255, "run-time error " },
    { 2,   "R6002\r\n- floating-point support not loaded\r\n" },
    { 1,   "R6001\r\n- null pointer assignment\r\n" },
};

/* END 0000:5422 / VGAME f562 */
char *rtl_NMSG_TEXT(int msgno)
{
    for (size_t i = 0; i < sizeof nmsg / sizeof nmsg[0]; i++)
        if (nmsg[i].id == msgno) return (char *)nmsg[i].text;
    return NULL;
}

/* END 0000:544d / VGAME f58d: INT 21h/40h to handle 2 (stderr) */
void rtl_NMSG_WRITE(int msgno)
{
    const char *t = rtl_NMSG_TEXT(msgno);
    if (t) fputs(t, stderr);
}

/* END 0000:53da / VGAME f51a: message 252, the hook [515C] (none), message 255 */
void rtl_FF_MSGBANNER(void)
{
    rtl_NMSG_WRITE(252);
    rtl_NMSG_WRITE(255);
}

/* END 0000:4E9A _amsg_exit: banner, the message, exit(255) */
_Noreturn void rtl_amsg_exit(int msgno)
{
    rtl_FF_MSGBANNER();
    rtl_NMSG_WRITE(msgno);
    exit_fn(255);
    for (;;) { }
}

/* The divide-error vector installed by rtl_cinit (END 4E8A): message 3 */
_Noreturn void rtl_divide_error(void) { rtl_amsg_exit(3); }

/* END 0000:5400 / VGAME f540: the xor-sum of DS:0..41h must be 55h; a program that wrote through a null pointer
 * changed it. Returns nonzero when the check fails. */
int rtl_nullcheck(void)
{
    if (!plat_loaded) return 0;
    u8 x = 0;
    for (int i = 0; i < 0x42; i++) x ^= DSB(i);
    return x != 0x55;
}

/* ------------------------------------------------------------------ start-up and exit */

/* END 0000:4DD2 / START 8EDC / VGAME EBB4 / SETUP 1516 / PLAYER 1096 / DSWAP 0C7E: platform.md §2 "c0_start". The
 * port's version builds the DGROUP model of the loaded image: stack pointer for stk_alloc(), the near heap above
 * the stack, the divide-error handler (rtl_divide_error, called by the ported divisions). */
void c0_start(void)
{
    if (!plat_loaded) { dg_sp = 0; heap_lo = heap_hi = 0; return; }
    dg_sp = plat_img.stack_top;
    rtl_heap_init(plat_img.stack_top);
}

void rtl_cinit(void) { }                    /* divide vector, _C_FILE_INFO, initialiser tables: nothing to do */
void rtl_ctermsub(void) { }                 /* restores INT 0 and the optional vectors */
void rtl_cexit(void) { }                    /* _cexit: terminators, return to the caller */
void rtl_c_exit(void) { }                   /* _c_exit */
void rtl_setargv(void) { }                  /* the mains take their tail as a parameter */
void rtl_setenvp(void) { }

/* END 0000:4F8A (exit; _exit 4F91 shares the body): terminators, flush, the null-pointer check, INT 21h/4Ch.
 * The code becomes 0FFh when the check failed and the code was 0. */
void exit_fn(int code)
{
    if (!in_exec) { fflush(NULL); host_shutdown(); exit(code); }
    if (rtl_nullcheck() && code == 0) code = 0xFF;      /* the banner path: R6001 null pointer assignment */
    exit_code = code & 0xFF;
    longjmp(exit_jmp, 1);
}

void rtl_exit_quick(int code) { exit_fn(code); }

static void program_cleanup(void)
{
    timer_remove();
    break_hook_remove();
    kbd_set_int9_hook(NULL);
    dos_close_all();
    fflush(NULL);
}

int platform_exec(ProgId id, ProgMain main, const char *tail)
{
    u16 save_dg = cur_dgroup, save_sp = dg_sp;
    ProgId save_prog = plat_prog;
    ProgImage img;
    bool loaded = prog_image_load(id, tail, &img);
    plat_prog = id;
    plat_loaded = loaded;
    if (loaded) { plat_img = img; cur_dgroup = img.dgroup; }
    else prog_log("[chain] %s: no image (no game files), running without DGROUP", prog_table[id].exe);
    c0_start();
    break_hook_reset();

    int code;
    in_exec = true;
    if (setjmp(exit_jmp) == 0) code = main(tail);
    else code = exit_code;
    in_exec = false;
    enh_flight_end();                    /* ENH: S4.1, the display stream (if any) stops with the program */

    program_cleanup();
    if (loaded) prog_image_free(&img);
    cur_dgroup = save_dg;
    dg_sp = save_sp;
    plat_prog = save_prog;
    plat_loaded = false;
    return code;
}
