/* Headless checks of the platform layer: f117 --platform-test (writes P51TEST.* in --game-dir, so point it at a scratch
 * folder) and f117 --exe-test (reads the seven programs from --game-dir). Run by ctest (CMakeLists.txt).
 * The reference numbers in selftest_vectors.h come from the ORIGINAL code under Unicorn (port/notes/s51/platform_emu.py). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen/module_platform.h"
#include "host.h"
#include "platform/platform.h"
#include "platform/timer.h"
#include "selftest_vectors.h"

static int checks, fails;

static void check(bool ok, const char *what, ...)
{
    checks++;
    if (ok) return;
    fails++;
    va_list ap;
    va_start(ap, what);
    printf("FAIL: ");
    vprintf(what, ap);
    printf("\n");
    va_end(ap);
}

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* A zeroed DGROUP-sized block standing in for a program image, with plat_prog = id. */
static u16 scratch_dgroup(ProgId id)
{
    u16 seg;
    if (!dos_mem_alloc(0x1000, &seg)) host_fatal("selftest: no memory");
    memset(mp(seg, 0), 0, 0x10000);
    cur_dgroup = seg;
    plat_prog = id;
    return seg;
}

static void scratch_free(u16 seg)
{
    dos_mem_free(seg);
    cur_dgroup = 0;
    plat_prog = PROG_SETUP;
}

/* ------------------------------------------------------------------ RTL */
static void test_rtl(void)
{
    cur_dgroup = 0;
    dos_arena_reset(PSP_SEG, 0x7C);
    for (size_t i = 0; i < ARRAY_LEN(vec_rand); i++) {
        srand_fn(vec_rand[i].seed);
        for (int k = 0; k < 8; k++) {
            int r = rand_fn();
            check(r == vec_rand[i].rnd[k], "rand seed %u #%d: %d, original %d", vec_rand[i].seed, k, r, vec_rand[i].rnd[k]);
        }
        for (int k = 0; k < 5; k++) {
            int r = rand_n(vec_rand_n[k]);
            check(r == vec_rand[i].rn[k], "rand_n(%d) seed %u: %d, original %d", vec_rand_n[k], vec_rand[i].seed, r, vec_rand[i].rn[k]);
        }
    }
    /* the seed lives in the program's DGROUP (END 5158) */
    u16 seg = scratch_dgroup(PROG_END);
    srand_fn(0x1234);
    check(DSL(0x5158) == 0x1234, "srand writes DS:5158 as a dword (low word = seed, high = 0), got %08X", DSL(0x5158));
    int r = rand_fn();
    check(DSL(0x5158) == 0x1234u * 0x343FDu + 0x269EC3u && r == (int)((DSL(0x5158) >> 16) & 0x7FFF), "rand updates DS:5158");
    scratch_free(seg);

    for (size_t i = 0; i < ARRAY_LEN(vec_dist); i++) {
        int r2 = dist_approx(vec_dist[i].x1, vec_dist[i].y1, vec_dist[i].x2, vec_dist[i].y2);
        check(r2 == vec_dist[i].r, "dist_approx(%d,%d,%d,%d) = %d, original %d", vec_dist[i].x1, vec_dist[i].y1, vec_dist[i].x2,
              vec_dist[i].y2, r2, vec_dist[i].r);
    }
    for (size_t i = 0; i < ARRAY_LEN(vec_ldiv); i++) {
        long q = rtl_aFldiv(vec_ldiv[i].a, vec_ldiv[i].b);
        check(q == vec_ldiv[i].q, "aFldiv(%ld,%ld) = %ld, original %ld", vec_ldiv[i].a, vec_ldiv[i].b, q, vec_ldiv[i].q);
    }
    for (size_t i = 0; i < ARRAY_LEN(vec_lmul); i++) {
        long p = rtl_aFlmul(vec_lmul[i].a, vec_lmul[i].b);
        check(p == vec_lmul[i].p, "aFlmul(%ld,%ld) = %ld, original %ld", vec_lmul[i].a, vec_lmul[i].b, p, vec_lmul[i].p);
    }
    for (size_t i = 0; i < ARRAY_LEN(vec_shift); i++) {
        long a = vec_shift[i].a;
        int n = vec_shift[i].n;
        check(rtl_aFlshl(a, n) == vec_shift[i].shl, "aFlshl(%ld,%d) = %ld, original %ld", a, n, rtl_aFlshl(a, n), vec_shift[i].shl);
        check(rtl_aFlshr(a, n) == vec_shift[i].sar, "aFlshr(%ld,%d) = %ld, original %ld", a, n, rtl_aFlshr(a, n), vec_shift[i].sar);
        check(rtl_aFulshr((unsigned long)a & 0xFFFFFFFFul, n) == vec_shift[i].shr, "aFulshr(%ld,%d)", a, n);
    }

    check(abs_fn(-5) == 5 && abs_fn(7) == 7 && abs_fn(0) == 0 && abs_fn(-32768) == -32768, "abs_fn (neg of 8000h stays 8000h)");

    u8 *a = mp(0x3000, 0), *b = mp(0x3000, 0x100);
    memset(a, 0xAA, 64);
    mem_fill(a, 0x55, 16);
    check(a[0] == 0 && a[15] == 0 && a[16] == 0xAA, "mem_fill always fills with zero (original bug)");
    mem_fill_w(a, 0x1234, 4);
    check(a[0] == 0x34 && a[1] == 0x12 && a[6] == 0x34 && a[7] == 0x12 && a[8] == 0 && a[16] == 0xAA, "mem_fill_w honours the value");
    mem_zero_w(a, 2);
    check(a[0] == 0 && a[3] == 0 && a[4] == 0x34, "mem_zero_w zeroes whole words");
    for (int i = 0; i < 32; i++) a[i] = (u8)i;
    mem_copy(a, a + 1, 8);                      /* overlapping forward copy repeats the first byte, as rep movsb */
    check(a[1] == 0 && a[2] == 0 && a[8] == 0 && a[9] == 9, "mem_copy is a forward copy");
    mem_copy_w(a + 16, b, 2);
    check(b[0] == 16 && b[3] == 19 && b[4] != 20, "mem_copy_w copies words");
    strcpy_fn((char *)b, "NIGHT");
    strcat_fn((char *)b, "HAWK");
    check(!strcmp((char *)b, "NIGHTHAWK") && strlen_fn((char *)b) == 9, "strcpy/strcat/strlen");
    movedata(0x3000, 0x100, 0x3001, 0x200, 10);
    check(!memcmp(mp(0x3000, 0x100), mp(0x3001, 0x200), 10), "movedata");

    /* the near heap */
    seg = scratch_dgroup(PROG_END);
    rtl_heap_init(0x7A4E);
    u16 h1 = rtl_malloc_ds(100), h2 = rtl_malloc_ds(50), h3 = rtl_malloc_ds(60000);
    check(h1 && h2 && h2 > h1 && !h3, "near heap: two blocks, the 60000-byte request fails");
    rtl_free_ds(h1);
    u16 h4 = rtl_malloc_ds(60);
    check(h4 == h1, "near heap: a freed block is reused first-fit");
    rtl_free_ds(h4);
    rtl_free_ds(h2);
    u16 h5 = rtl_malloc_ds(30000);
    check(h5 == h1, "near heap: free neighbours are coalesced");
    scratch_free(seg);
}

/* ------------------------------------------------------------------ DOS files */
static int write_pattern(const char *name, u32 n)
{
    int fd = dos_creat((char *)name, 0);
    u16 seg = 0x4000;
    u32 done = 0;
    u8 *p = mp(seg, 0);
    while (done < n) {
        u32 k = n - done > 4096 ? 4096 : n - done;
        for (u32 i = 0; i < k; i++) p[i] = (u8)((done + i) * 7 + ((done + i) >> 8));
        dos_write_far(fd, (unsigned)k, 0, seg, 0);
        done += k;
    }
    dos_close(fd);
    return 0;
}

static u8 pattern(u32 i) { return (u8)(i * 7 + (i >> 8)); }

static void test_files(void)
{
    /* create / write / read through DOS handles; the lookup is case-insensitive */
    memcpy(mp(0x2000, 0), "HELLO, WORLD", 12);
    int fd = dos_creat("p51test.tmp", 0);
    check(fd >= 5, "dos_creat returns a handle >= 5 (%d)", fd);
    check(dos_write_far(fd, 5, 0, 0x2000, 7) == 5, "dos_write_far writes from seg:(off+add)");   /* "WORLD" */
    dos_close(fd);
    char *p = host_game_path("P51TEST.TMP", false);
    check(p != NULL, "the created file is found as P51TEST.TMP");
    if (p) host_free(p);
    fd = dos_open("P51Test.Tmp", 0);
    memset(mp(0x2100, 0), 0, 16);
    check(dos_read(fd, 16, mp(0x2100, 0)) == 5 && !memcmp(mp(0x2100, 0), "WORLD", 5), "dos_open (mixed case) + dos_read returns 5 bytes");
    check(lseek_fn(fd, 1, 0) == 1, "lseek_fn from the start");
    check(dos_read_far(fd, 2, 0x10, 0x2100) == 2 && !memcmp(mp(0x2100, 0x10), "OR", 2), "dos_read_far at seg:off");
    check(lseek_fn(fd, 0, 2) == 5, "lseek_fn to the end gives the size");
    dos_close(fd);
    check(unlink_fn("p51test.tmp") == 0, "unlink_fn");
    p = host_game_path("P51TEST.TMP", false);
    check(p == NULL, "the unlinked file is gone");
    if (p) host_free(p);

    /* the whole-file loader with the original's chunk-offset bug (simflow.md 2) */
    u32 size = 0;
    u16 before = dos_mem_largest();
    write_pattern("p51big.tmp", 70000);
    u16 seg = platform_load_file_paras("P51BIG.TMP", &size);
    check(seg != 0 && size == 70000, "platform_load_file_paras loads a 70000-byte file (seg %04X, size %u)", seg, size);
    bool ok = seg != 0;
    for (u32 i = 0; ok && i < 70000; i++) if (mp(seg, 0)[i] != pattern(i)) { ok = false; check(false, "70000-byte file differs at %u", i); }
    check(ok, "a file up to 2 x 65535 bytes is loaded correctly (chunk 2 is at seg:FFFF)");
    if (seg) dos_freemem(seg);
    write_pattern("p51big.tmp", 150000);
    seg = platform_load_file_paras("p51big.tmp", &size);
    check(seg != 0 && size == 150000, "platform_load_file_paras loads a 150000-byte file");
    if (seg) {
        check(mp(seg, 0)[1] == pattern(1) && mp(seg, 0)[65533] == pattern(65533), "first chunk intact");
        check(mp(seg, 0)[65534] == pattern(131070), "BUG KEPT: the third chunk is read to seg:FFFE, over the second one");
        dos_freemem(seg);
    }
    check(platform_load_file_paras("NOSUCH.TMP", &size) == 0 && size == 0, "a missing file gives 0");
    unlink_fn("p51big.tmp");
    check(dos_mem_largest() == before, "all blocks were freed");

    /* DOS memory */
    unsigned s1 = dos_allocmem(100);
    check(s1 >= 0x10 && dos_freemem(s1) == 0, "dos_allocmem / dos_freemem");
    u16 largest = 0;
    check(dos_allocmem_paras(0xFFFF, &largest) == 8 && largest > 0x100, "dos_allocmem_paras: error 8 and the largest block");
    u16 got = 0;
    check(dos_allocmem_paras(0x100, &got) == 0 && got >= 0x10, "dos_allocmem_paras success");
    mem_free_chk(got);

    /* stdio on the game folder */
    FILE *f = fopen_fn("p51test.tmp", "wb");
    check(f != NULL, "fopen_fn creates");
    if (f) { fwrite("ABCDEFGH", 1, 8, f); fclose(f); }
    f = fopen_fn("P51TEST.TMP", "r+b");
    char buf[16] = { 0 };
    check(f && fread_fn(buf, 1, 8, f) == 8 && !memcmp(buf, "ABCDEFGH", 8), "fopen_fn r+b + fread_fn");
    if (f) fclose(f);
    unlink_fn("p51test.tmp");

    /* low-level open/read/write, text mode */
    int h = rtl_open("p51test.tmp", 0x100 | 0x4000 | 2, 0);
    check(h >= 5, "rtl_open O_CREAT|O_TEXT");
    check(rtl_write(h, "a\nb", 3) == 3, "rtl_write (text)");
    rtl_close(h);
    h = rtl_open("p51test.tmp", 0x8000, 0);
    memset(buf, 0, sizeof buf);
    check(rtl_read(h, buf, 16) == 4 && !memcmp(buf, "a\r\nb", 4), "text mode write turned LF into CR LF");
    rtl_close(h);
    h = rtl_open("p51test.tmp", 0x4000, 0);
    memset(buf, 0, sizeof buf);
    check(rtl_read(h, buf, 16) == 3 && !memcmp(buf, "a\nb", 3), "text mode read drops the CR");
    rtl_close(h);
    check(rtl_open("p51test.tmp", 0x100 | 0x400, 0) == -1, "O_CREAT|O_EXCL on an existing file fails");
    unlink_fn("p51test.tmp");

    /* the 512-byte decoder buffer (END: buffer DS:1C77, handle DS:1EE1, position DS:1EDF) */
    write_pattern("p51test.tmp", 1200);
    u16 dg = scratch_dgroup(PROG_END);
    fd = dos_open("p51test.tmp", 0);
    check(DSW(0x1EDF) == 0x200, "dos_open resets the decoder buffer position to 200h");
    dos_buf_setfd(fd);
    check(DSW(0x1EE1) == fd && DSB(0x1C77) == pattern(0) && DSB(0x1C77 + 511) == pattern(511), "dos_buf_setfd fills DS:1C77 with 512 bytes");
    dos_buf_fill();
    check(DSB(0x1C77) == pattern(512), "dos_buf_fill reads the next 512 bytes");
    dos_close(fd);
    scratch_free(dg);
    unlink_fn("p51test.tmp");
}

/* ------------------------------------------------------------------ keyboard */
static void keys(const u8 *seq, size_t n)
{
    for (size_t i = 0; i < n; i++) kbd_host_byte(seq[i]);
}
#define KEYS(...) do { const u8 s_[] = { __VA_ARGS__ }; keys(s_, sizeof s_); } while (0)

static int hook_calls;
static u8 hook_last;
static u16 hook_tail_before;
static void test_hook(u8 raw)
{
    hook_calls++;
    hook_last = raw;
    if (hook_calls == 1) hook_tail_before = rd16(0x40, 0x1C);
}

/* the key word the BIOS reports for a key sequence, 0 when nothing came */
static int got_key(void)
{
    if (!bios_keyboard(1)) return 0;
    return bios_keyboard(0) & 0xFFFF;
}

static void test_keyboard(void)
{
    kbd_flush();
    check(bios_keyboard(1) == 0, "empty buffer: peek gives 0");
    KEYS(0x1E, 0x9E);                      check(got_key() == 0x1E61, "a");
    KEYS(0x2A, 0x1E, 0x9E, 0xAA);          check(got_key() == 0x1E41, "Shift-A");
    check((bios_keyboard(2) & 3) == 0, "shift flags clear after the break codes");
    KEYS(0x2A);                            check((bios_keyboard(2) & 2) != 0, "shift flags: left shift bit");
    KEYS(0xAA);
    KEYS(0x3A, 0xBA, 0x1E, 0x9E);          check(got_key() == 0x1E41, "Caps Lock A");
    KEYS(0x2A, 0x1E, 0x9E, 0xAA);          check(got_key() == 0x1E61, "Caps Lock + Shift-A is lower case");
    KEYS(0x3A, 0x3A, 0xBA);                check((bios_keyboard(2) & 0x40) == 0, "a typematic repeat of Caps Lock does not toggle again");
    KEYS(0x01, 0x81);                      check(got_key() == 0x011B, "Esc");
    KEYS(0x1C, 0x9C);                      check(got_key() == 0x1C0D, "Enter");
    KEYS(0x1D, 0x1C, 0x9C, 0x9D);          check(got_key() == 0x1C0A, "Ctrl-Enter");
    KEYS(0x39, 0xB9);                      check(got_key() == 0x3920, "Space");
    KEYS(0x3B, 0xBB);                      check(got_key() == 0x3B00, "F1");
    KEYS(0x2A, 0x3B, 0xBB, 0xAA);          check(got_key() == 0x5400, "Shift-F1");
    KEYS(0x1D, 0x3C, 0xBC, 0x9D);          check(got_key() == 0x5F00, "Ctrl-F2");
    KEYS(0x38, 0x44, 0xC4, 0xB8);          check(got_key() == 0x7100, "Alt-F10");
    KEYS(0x38, 0x10, 0x90, 0xB8);          check(got_key() == 0x1000, "Alt-Q");
    KEYS(0x38, 0x19, 0x99, 0xB8);          check(got_key() == 0x1900, "Alt-P");
    KEYS(0x38, 0x02, 0x82, 0xB8);          check(got_key() == 0x7800, "Alt-1");
    KEYS(0x1D, 0x2E, 0xAE, 0x9D);          check(got_key() == 0x2E03, "Ctrl-C");
    KEYS(0x0F, 0x8F);                      check(got_key() == 0x0F09, "Tab");
    KEYS(0x2A, 0x0F, 0x8F, 0xAA);          check(got_key() == 0x0F00, "Shift-Tab");
    KEYS(0xE0, 0x48, 0xE0, 0xC8);          check(got_key() == 0x4800, "grey Up");
    KEYS(0xE0, 0x50, 0xE0, 0xD0);          check(got_key() == 0x5000, "grey Down");
    KEYS(0xE0, 0x2A, 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0xAA); check(got_key() == 0x4800, "grey Up with the fake shift of NumLock-on keyboards");
    KEYS(0x48, 0xC8);                      check(got_key() == 0x4800, "keypad 8, NumLock off: Up");
    KEYS(0x45, 0xC5, 0x48, 0xC8);          check(got_key() == 0x4838, "keypad 8, NumLock on: '8' (flight.md keypad digits)");
    KEYS(0xE0, 0x48, 0xE0, 0xC8);          check(got_key() == 0x4800, "grey Up ignores NumLock");
    KEYS(0x2A, 0x48, 0xC8, 0xAA);          check(got_key() == 0x4800, "keypad 8, NumLock on + Shift: Up");
    KEYS(0x45, 0xC5);                      /* NumLock off */
    KEYS(0x2A, 0x48, 0xC8, 0xAA);          check(got_key() == 0x4838, "keypad 8 + Shift, NumLock off: '8'");
    KEYS(0x4E, 0xCE);                      check(got_key() == 0x4E2B, "keypad +");
    KEYS(0x4A, 0xCA);                      check(got_key() == 0x4A2D, "keypad -");
    KEYS(0x4C, 0xCC);                      check(got_key() == 0x4C00, "keypad 5, NumLock off");
    KEYS(0x1D, 0x4B, 0xCB, 0x9D);          check(got_key() == 0x7300, "Ctrl-Left");
    KEYS(0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5); check(bios_keyboard(1) == 0, "Pause makes no key and does not toggle NumLock");
    check((bios_keyboard(2) & 0x20) == 0, "NumLock still off after Pause");
    KEYS(0x57, 0xD7, 0x58, 0xD8);          check(bios_keyboard(1) == 0, "F11 / F12: nothing on the legacy INT 16h");
    KEYS(0x9E);                            check(bios_keyboard(1) == 0, "a lone break code gives nothing");
    KEYS(0x52, 0xD2);                      check(got_key() == 0x5200 && (bios_keyboard(2) & 0x80) != 0, "Ins toggles the insert flag");
    KEYS(0x52, 0xD2);                      got_key();

    /* the buffer holds 15 keys; the 16th is dropped (the BIOS beeps) */
    for (int i = 0; i < 20; i++) KEYS(0x1E, 0x9E);
    int n = 0;
    while (bios_keyboard(1)) { bios_keyboard(0); n++; }
    check(n == 15, "the type-ahead buffer holds 15 keys (%d)", n);

    /* a program's INT 9 hook sees the byte first, before the BIOS routine puts the key in the buffer */
    hook_calls = 0;
    kbd_set_int9_hook(test_hook);
    u16 tail = rd16(0x40, 0x1C);
    KEYS(0x30, 0xB0);
    check(hook_calls == 2 && hook_last == 0xB0, "INT 9 hook called per byte");
    check(hook_tail_before == tail, "the hook runs before the BIOS routine fills the buffer");
    check(got_key() == 0x3062, "the key still reaches the buffer after the hook (jmp far old INT 9)");
    kbd_set_int9_hook(NULL);
    hook_calls = 0;
    KEYS(0x30, 0xB0);
    got_key();
    check(hook_calls == 0, "removed hook is not called");

    /* Ctrl-Break: 0000 in the buffer and the program's flag */
    u16 dg = scratch_dgroup(PROG_END);
    break_hook_install();
    KEYS(0x1D, 0xE0, 0x46, 0xE0, 0xC6, 0x9D);
    check(DSB(0x2608) == 1, "Ctrl-Break sets the break flag (END: DS:2608 = 1)");
    break_hook_remove();
    DSB(0x2608) = 0;
    KEYS(0x1D, 0xE0, 0x46, 0xE0, 0xC6, 0x9D);
    check(DSB(0x2608) == 0, "no flag once the hook is removed");
    break_hook_install();
    break_isr();
    check(DSB(0x2608) == 1, "break_isr");
    break_hook_remove();
    while (bios_keyboard(1)) bios_keyboard(0);
    scratch_free(dg);
    dg = scratch_dgroup(PROG_VGAME);
    break_isr();
    check(DSB(0x2624) == 0xFF, "break_isr in VGAME stores FFh at DS:2624");
    scratch_free(dg);
}

/* ------------------------------------------------------------------ joystick, mouse */
static void test_joystick(void)
{
    u16 dg = scratch_dgroup(PROG_END);
    const u16 base = 0x23C4;
    /* the calibration table as joy_set_calib copies it from the comm block: min, max, centre, slopes */
    u16 tbl[20] = { 0 };
    tbl[0] = 500; tbl[4] = 1500; tbl[8] = 1000; tbl[12] = 500; tbl[16] = 500;     /* axis 0 of each group of 4 words */
    joy_set_calib(tbl);
    check(DSW(base) == 500 && DSW(base + 0x08) == 1500 && DSW(base + 0x10) == 1000, "joy_set_calib copies 20 words to DS:23C4");
    for (size_t i = 0; i < ARRAY_LEN(vec_joy); i++) {
        DSW(base + 0x28) = (u16)vec_joy[i].raw;
        joy_scale(0);
        check(DSB(base + 0x30) == vec_joy[i].out && DSW(base) == vec_joy[i].min && DSW(base + 0x08) == vec_joy[i].max &&
              DSW(base + 0x18) == vec_joy[i].slo && DSW(base + 0x20) == vec_joy[i].shi,
              "joy_scale raw %u: out %u (original %u), min %u/%u max %u/%u slo %u/%u shi %u/%u", vec_joy[i].raw, DSB(base + 0x30),
              vec_joy[i].out, DSW(base), vec_joy[i].min, DSW(base + 0x08), vec_joy[i].max, DSW(base + 0x18), vec_joy[i].slo,
              DSW(base + 0x20), vec_joy[i].shi);
    }
    joy_read_raw();                              /* no gamepad in a headless run: the 0FFFFh timeout of port 201h */
    check(!joy_present() && DSW(base + 0x28) == 0xFFFF && DSW(base + 0x2A) == 0xFFFF, "no stick: raw counts FFFFh");
    check(joy_port_201() == 0xFF, "no stick: port 201h buttons read as not pressed");
    scratch_free(dg);

    dg = scratch_dgroup(PROG_START);
    mouse_set_pos();
    mouse_poll();
    check(DSW(0xE08A) < 320 && DSW(0xE08C) < 200 && mouse_button(0) == 0 && mouse_button(1) == 0, "mouse_poll fills START's x/y (E08A/E08C) and buttons (69B6/7)");
    scratch_free(dg);
}

/* ------------------------------------------------------------------ timer */
static int hook_ticks;
static void tick_hook(void) { hook_ticks++; }

static void test_timer(void)
{
    hook_ticks = 0;
    timer_set_hook(tick_hook);
    u32 t0 = timer_ticks;
    u16 b0 = (u16)bios_timer();
    host_wait_until_ns(host_ns() + 100000000ull);
    check(hook_ticks == 0 && timer_ticks > t0, "the hook does not run before timer_install, but the tick counter does");
    timer_install();
    check(timer_installed(), "timer_install");
    hook_ticks = 0;
    t0 = timer_ticks;
    host_wait_until_ns(host_ns() + 500000000ull);
    u32 dt = timer_ticks - t0;
    check(dt >= 30 && dt <= 38, "70.09 Hz: %u ticks in 500 ms", dt);
    check((u32)hook_ticks == dt || (u32)hook_ticks + 1 == dt || (u32)hook_ticks == dt + 1, "the hook runs once per tick (%d hook calls, %u ticks)", hook_ticks, dt);
    u16 b1 = (u16)bios_timer();
    check((u16)(b1 - b0) >= 8 && (u16)(b1 - b0) <= 14, "the BIOS clock advanced %u counts in about 0.6 s (18.2 Hz)", (u16)(b1 - b0));
    timer_remove();
    hook_ticks = 0;
    host_wait_until_ns(host_ns() + 100000000ull);
    check(hook_ticks == 0 && !timer_installed(), "timer_remove stops the hook");
}

/* ------------------------------------------------------------------ exit and exec */
static int exec_deep(void) { exit_fn(7); return 0; }
static int exec_main(const char *tail) { (void)tail; int fd = dos_creat("p51test.tmp", 0); (void)fd; return exec_deep(); }
static int exec_ret(const char *tail) { return tail[0] == ' ' ? 33 : 34; }

static void test_exec(void)
{
    check(platform_exec(PROG_DSWAP, exec_main, "") == 7, "exit_fn(7) deep inside a program is the program's exit code");
    check(cur_dgroup == 0, "cur_dgroup restored");
    int fd = dos_creat("p51test2.tmp", 0);
    dos_close(fd);                                 /* handles of the finished program were closed: this one is the first again */
    check(fd == 5, "the handles of the finished program were closed (new handle %d)", fd);
    unlink_fn("p51test.tmp");
    unlink_fn("p51test2.tmp");
    check(platform_exec(PROG_START, exec_ret, " x") == 33, "a program that returns n");
}

/* ------------------------------------------------------------------ the build check (issue #2) */

/* A plain MZ file standing in for a program: c0's `mov di, dg; mov si,[2]` at the entry point (0:0), and VGAME's sin_tab
 * head at DS:2084 (sin_bump is added to T[2]). */
static u8 *fake_exe(u32 image_len, u16 dg, int sin_bump, size_t *len)
{
    *len = 0x20 + image_len;
    u8 *f = calloc(1, *len);
    if (!f) host_fatal("selftest: no memory");
    f[0] = 'M'; f[1] = 'Z';
    f[2] = (u8)(*len % 512); f[3] = (u8)(*len % 512 >> 8);
    f[4] = (u8)((*len + 511) / 512); f[5] = (u8)((*len + 511) / 512 >> 8);
    f[8] = 2;                                      /* header 2 paragraphs, no relocations, cs:ip 0:0 */
    f[24] = 0x1C;
    u8 *img = f + 0x20;
    static const u8 c0[] = { 0xBF, 0, 0, 0x8B, 0x36, 0x02, 0x00 };
    memcpy(img, c0, sizeof c0);
    img[1] = (u8)dg; img[2] = (u8)(dg >> 8);
    static const s16 head[6] = { 0, 804, 1608, 2411, 3212, 4011 };
    size_t at = (size_t)dg * 16 + 0x2084;
    for (int i = 0; i < 6 && at + 2 * i + 2 <= image_len; i++) {
        s16 v = (s16)(head[i] + (i == 2 ? sin_bump : 0));
        img[at + 2 * i] = (u8)v;
        img[at + 2 * i + 1] = (u8)((u16)v >> 8);
    }
    return f;
}

static bool build_ok(ProgId id, u32 image_len, u16 dg, int sin_bump, char *msg, size_t n)
{
    size_t len;
    u8 *f = fake_exe(image_len, dg, sin_bump, &len);
    ExeFile e;
    bool parsed = exe_parse(f, len, &e);
    free(f);
    check(parsed && e.image_len == image_len, "build check: the test MZ parses (%u bytes)", (unsigned)image_len);
    if (!parsed) return false;
    bool ok = exe_build_check(id, &e, msg, n);
    exe_free(&e);
    return ok;
}

static void test_builds(void)
{
    char msg[512];
    check(build_ok(PROG_VGAME, 162460, 0x1E23, 0, msg, sizeof msg) && !msg[0], "build check: the supported VGAME is accepted");
    check(build_ok(PROG_START, 89023, 0x0A95, 0, msg, sizeof msg), "build check: the supported START is accepted");
    check(build_ok(PROG_SETUP, 13437, 0x024C, 0, msg, sizeof msg), "build check: the supported SETUP is accepted");
    check(!build_ok(PROG_VGAME, 163004, 0x1E42, 0, msg, sizeof msg) && strstr(msg, "VGAME.EXE is a different release") &&
          strstr(msg, "163004 bytes") && strstr(msg, "DGROUP 1E42") && strstr(msg, "GOG 473.04") && strstr(msg, "162460"),
          "build check: the GOG VGAME (163004 bytes, DGROUP 1E42) is refused: %s", msg);
    check(!build_ok(PROG_VGAME, 162460, 0x1E23, -1, msg, sizeof msg) && strstr(msg, "sine table"),
          "build check: a VGAME with another sin_tab is refused: %s", msg);
    check(!build_ok(PROG_VGAME, 162460, 0x1E24, 0, msg, sizeof msg) && strstr(msg, "DGROUP 1E24"),
          "build check: a VGAME with another DGROUP is refused");
    check(!build_ok(PROG_END, 46040, 0x05FA, 0, msg, sizeof msg) && strstr(msg, "END.EXE"),
          "build check: an END of another size is refused");
}

int platform_selftest(void)
{
    test_rtl();
    test_files();
    test_keyboard();
    test_joystick();
    test_timer();
    test_exec();
    test_builds();
    printf("platform-test %s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ the program images */
int platform_exe_test(void)
{
    const char *missing = game_files_missing();
    if (missing) {
        printf("exe-test SKIPPED: %s not found in %s\n", missing, host_game_dir());
        return 77;
    }
    dos_arena_reset(PSP_SEG, 0x7C);
    u16 before = dos_mem_largest();
    for (int id = 0; id < PROG_COUNT; id++) {
        ProgImage img;
        bool ok = prog_image_load((ProgId)id, " x", &img);
        check(ok, "%s loads", prog_table[id].exe);
        if (!ok) continue;
        cur_dgroup = img.dgroup;
        plat_prog = (ProgId)id;
        u8 x = 0;
        for (int i = 0; i < 0x42; i++) x ^= DSB(i);
        check(x == 0x55, "%s: the xor-sum of DS:0..41h is 55h (%02X): DGROUP found and relocated", prog_table[id].exe, x);
        check(img.stack_limit > 0x400 && img.stack_top > img.stack_limit && img.stack_top < 0xFFF0,
              "%s: stack %04X..%04X", prog_table[id].exe, img.stack_limit, img.stack_top);
        check(rd8(img.psp, 0) == 0xCD && rd8(img.psp, 0x80) == 2 && rd8(img.psp, 0x81) == ' ', "%s: PSP and command tail", prog_table[id].exe);
        if (id == PROG_VGAME) {                     /* flight.md 2.4: the keypad-as-stick table at DS:2CDE */
            check(DSB(0x2CDE) == 0x01 && DSB(0x2CE0) == 0x04 && DSB(0x2CDE + 0x47 - 0x29) == 0x05 && DSB(0x2CDE + 0x4C - 0x29) == 0x80 &&
                  DSB(0x2CDE + 0x51 - 0x29) == 0x0A, "VGAME: the keypad table at DS:2CDE comes from the image");
        }
        if (id == PROG_END) {
            check(!strncmp((char *)DSP(0x1E77), ":File not found", 15), "END: the DOS error text at DS:1E77 comes from the image");
        }
        prog_image_free(&img);
    }
    cur_dgroup = 0;
    check(dos_mem_largest() == before, "all program blocks were freed");
    printf("exe-test %s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
