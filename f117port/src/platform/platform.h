#pragma once
/* The platform layer (S5.1, port/spec/platform.md): what the seven programs share below the game code.
 * The functions the specs name are declared in gen/module_platform.h (include it for the C runtime, the DOS
 * wrappers, the timer/joystick/mouse/keyboard routines); this header holds what the port adds around them:
 * loading a program image, the per-program DGROUP offsets of the library state, the keyboard model and
 * the self test.
 *
 * Address model. Every near pointer of the original is a DS offset; a `void *` argument here is a host
 * pointer into mem[] (DSP(off), mp(seg, off)); ds_off() converts back. */
#include "mem.h"
#include "prog.h"
#include "types.h"

/* ---- Program images (exeload.c): LZEXE 0.91 / plain MZ ------------------------------------------------- */
typedef struct {
    u8  *image;            /* load image (bytes after the header), relocations NOT applied; malloc'd */
    u32  image_len;
    u16  cs, ip, ss, sp;   /* initial registers as stored (relative to the load segment) */
    u16  min_alloc;        /* extra paragraphs the program asks for */
    int  nreloc;
    u32 *reloc;            /* (seg << 16) | off of each 16-bit word to relocate, relative to the image start */
} ExeFile;

bool exe_parse(const u8 *file, size_t len, ExeFile *out);   /* false: not an MZ / LZ91 file */
void exe_free(ExeFile *e);
/* The build check (issue #2): true if the program is the build the port was made from (load image size, DGROUP, and for
 * VGAME the first sin_tab words); otherwise false and msg = the player's message. game_build_problem() runs it on the
 * seven programs in the game folder: NULL = all supported (or missing: see game_files_missing), else the message. */
bool exe_build_check(ProgId id, const ExeFile *e, char *msg, size_t n);
const char *game_build_problem(void);

/* A program loaded into mem[] the way DOS does: PSP, image at PSP+10h with relocations applied, DGROUP =
 * load segment + SS (small model), BSS zero, the stack model at the end of the stack area. */
typedef struct {
    u16 psp;               /* PSP segment (the block owner) */
    u16 load;              /* load segment = psp + 10h */
    u16 dgroup;            /* DGROUP segment = load + the `mov di, imm16` of the c0 start-up (not the EXE header's SS) */
    u16 stack_top;         /* DS offset of the initial SP after the c0 start-up (MSC: sp + the `add sp,N` of c0) */
    u16 stack_limit;       /* the stack's lower end = the end of the BSS (the `add sp,N` constant) */
    u16 paras;             /* block size in paragraphs */
} ProgImage;

bool prog_image_load(ProgId id, const char *tail, ProgImage *out);   /* false: file missing / no memory */
void prog_image_free(const ProgImage *img);                          /* INT 21h/4Ch: every block of the program */

/* ---- Per-program DGROUP offsets of the library state ----------------------------------------------------
 * The MSC runtime and the MicroProse library are linked into each program at different addresses; the ported
 * routines find their state through this table (startup.c). 0 = the program does not have that part. */
typedef struct {
    u16 rand_seed;                       /* dword: srand/rand */
    u16 break_flag; u8 break_value;      /* byte set by break_isr (END, START store 1; the others FFh) */
    u16 joy_cal;                         /* 20-word calibration table; raw words at +28h, scaled bytes at +30h */
    u16 mouse_x, mouse_y, mouse_btn;     /* words; btn = left byte, right byte at +1 */
    u16 dosbuf, dosbuf_fd;               /* 512-byte decoder buffer and the handle word (read position at fd-2) */
} ProgDs;

extern ProgId plat_prog;                 /* the running program */
const ProgDs *plat_ds(void);
static inline u16 ds_off(const void *p) { return (u16)((const u8 *)p - mp(cur_dgroup, 0)); }

/* Runs a program body as a process: loads the image, sets cur_dgroup, calls c0_start(), runs main(tail),
 * and turns exit_fn(code) anywhere inside it into a return. Returns the exit code. */
int platform_exec(ProgId id, ProgMain main, const char *tail);

/* Near heap in DGROUP (startup.c): offsets instead of pointers. malloc_fn()/rtl_nmalloc() return mp(DGROUP, off). */
void rtl_heap_init(u16 lo);                           /* the heap = DS:lo .. FFF0h (c0_start does this from the stack top) */
u16  rtl_malloc_ds(unsigned n);                       /* 0 = out of memory */
void rtl_free_ds(u16 off);
/* "run-time error R60xx": the message and exit(255); rtl_divide_error() = the divide-error vector (message 3).
 * Division by zero / quotient overflow in the ported code (DIV, the long helpers) calls it. */
_Noreturn void rtl_amsg_exit(int msgno);
_Noreturn void rtl_divide_error(void);
/* The 16-bit DIV instruction: dx:ax / d with the 8086 overflow rules. */
u16  rtl_div32_16(u32 dxax, u16 d, u16 *rem);

/* ---- Keyboard (kbd.c): BIOS INT 9 / INT 16h model on the host's XT byte stream ---------------------------- */
void kbd_init(void);                                  /* installs the host handlers; clears the type-ahead buffer */
void kbd_host_byte(u8 raw);                           /* one byte on port 60h = one IRQ 1 */
/* A program's own INT 9 handler (VGAME's keypad-as-stick hook, flight.md 2.4; the placeholder's Esc): called
 * with the raw byte BEFORE the BIOS routine it chains to (the original ISRs end with `jmp far old INT 9`).
 * NULL restores the plain BIOS handler. */
typedef void (*Int9Hook)(u8 raw);
void kbd_set_int9_hook(Int9Hook h);
/* A host-level tap on the raw bytes that no program removes (the phase 4 placeholders' Esc). */
void kbd_set_observer(Int9Hook h);
bool kbd_key_waiting(void);                           /* INT 16h/01 (pumps the host once) */
u16  kbd_key_get(void);                               /* INT 16h/00: waits (pumps) for a key; scan << 8 | ascii */
void kbd_flush(void);
void kbd_inject(u16 word);                            /* test aid: a key word into the type-ahead buffer */

/* ---- Joystick / mouse (input.c) ------------------------------------------------------------------------- */
void break_check_window(void);                        /* tick: a closed window is Ctrl-Break */
void break_isr_dispatch(void);                        /* INT 1Bh: calls break_isr if the program installed the hook */
u8   joy_port_201(void);                              /* the button bits of port 201h: 4..7 = buttons, 0 = pressed */
bool joy_present(void);

/* ---- Files (dosio.c) ----------------------------------------------------------------------------------- */
void dos_close_all(void);                             /* process termination closes every handle */
/* The (paragraphs, &segment) flavour of dos_allocmem in VGAME / PLAYER / MPS_LOGO (VGAME 0000:F348): returns 0
 * with *out = the segment, or the DOS error (8) with *out = the largest free block. */
int  dos_allocmem_paras(unsigned paras, u16 *out);

/* simflow.md 2 file_load_paras / 0000:EA7C: the whole file into a fresh DOS block, read in 0FFFFh-byte chunks
 * with the original's offset bug (the n-th chunk goes to seg:(0 - (n-1))). Returns the segment, 0 on failure;
 * *size gets the file size. PORT: the file is opened read-only ("rb"), the original's "r+b" fails on a
 * read-only file. */
u16  platform_load_file_paras(const char *name, u32 *size);

/* ---- Timer (timer.c) ----------------------------------------------------------------------------------- */
void platform_init(void);                             /* once, after host_init, mem_init_machine and vga_init */
void timer_poll_bios_clock(void);
void break_hook_reset(void);                          /* program start: no Ctrl-Break handler installed */                     /* refreshes 0040:006C from the host clock */

/* ---- Self test (selftest.c): f117 --platform-test [GAMEDIR] ------------------------------------------- */
int platform_selftest(void);                          /* --platform-test: RTL, files in --game-dir, keyboard, joystick, timer */
int platform_exe_test(void);                          /* --exe-test: the seven programs load from --game-dir (77 = no game files) */
