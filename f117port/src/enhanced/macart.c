/* The Mac art loader (S7.1, see macart.h). File format (tools/mac_art.py): 'MPIC', w, h, ncolours (u16 LE), ncolours RGB,
 * w * h indices. */
#include "macart.h"
#include "enhanced.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "video/video.h"
#include "enhanced/sandbox.h"

static const char *const names[MAC_PIC_COUNT] = { "CockpitOff", "LeftCock", "RightCock", "RearCock" };
static MacPic pics[MAC_PIC_COUNT];
static u8 tried[MAC_PIC_COUNT];

bool enh_mac_on(void) { return enh.mac_art[0] && enh_canvas_on(); }

static bool load(MacPic *p, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    u8 hd[10];
    bool ok = fread(hd, 1, 10, f) == 10 && !memcmp(hd, "MPIC", 4);
    int w = hd[4] | hd[5] << 8, h = hd[6] | hd[7] << 8, nc = hd[8] | hd[9] << 8;
    ok = ok && w > 0 && h > 0 && w <= 4096 && h <= 4096 && nc > 0 && nc <= 256;
    u8 pal[768];
    ok = ok && fread(pal, 3, (size_t)nc, f) == (size_t)nc;
    u8 *pix = ok ? malloc((size_t)w * (size_t)h) : NULL;
    ok = ok && pix && fread(pix, 1, (size_t)w * (size_t)h, f) == (size_t)w * (size_t)h;
    fclose(f);
    if (!ok) { free(pix); return false; }
    memset(p, 0, sizeof *p);
    for (int i = 0; i < nc; i++) {
        p->rgb[i] = (u32)pal[3 * i] << 16 | (u32)pal[3 * i + 1] << 8 | pal[3 * i + 2];
        p->key[i] = p->rgb[i] == 0x00FFFF;
    }
    p->w = w; p->h = h; p->pix = pix;
    return true;
}

const MacPic *enh_mac_pic(MacPicId id)
{
    if (!enh_mac_on() || (unsigned)id >= MAC_PIC_COUNT) return NULL;
    if (!tried[id]) {
        tried[id] = 1;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s.mpic", enh.mac_art, names[id]);
        if (!load(&pics[id], path))
            host_log_error("--mac-art: %s not found or not a picture of tools/mac_art.py; the PC art is used", path);
    }
    return pics[id].pix ? &pics[id] : NULL;
}

const MacPic *enh_mac_view_pic(int m)
{
    return m == 0x41 ? enh_mac_pic(MAC_REAR) : m == 0x42 ? enh_mac_pic(MAC_LEFT) : m == 0x43 ? enh_mac_pic(MAC_RIGHT) : NULL;
}

int enh_mac_selftest(void)
{
    if (!enh_mac_on()) { printf("mac-art-test: no --mac-art DIR (or --classic): skipped\n"); return 77; }
    int fail = 0, found = 0;
    for (int id = 0; id < MAC_PIC_COUNT; id++) {
        const MacPic *p = enh_mac_pic((MacPicId)id);
        if (!p) { printf("mac-art-test: %s missing\n", names[id]); continue; }
        found++;
        long keyed = 0;
        for (long i = 0; i < (long)p->w * p->h; i++) keyed += p->key[p->pix[i]];
        bool ok = p->w == 640 && p->h == 480 && keyed > 0;
        printf("mac-art-test: %-10s %dx%d, %ld see-through pixels %s\n", names[id], p->w, p->h, keyed, ok ? "ok" : "UNEXPECTED");
        fail += !ok;
    }
    return found == 0 ? 77 : fail ? 1 : 0;
}

bool enh_mac_front(void) { return enh.wide && enh_mac_on() && enh_mac_pic(MAC_FRONT); }

static u8 pit[64000];
static bool have_pit;

void enh_mac_pit_loaded(void)
{
    const u16 seg = video_page_seg(2);
    if (!enh_mac_on() || enh_sbx || !seg) return;
    memcpy(pit, seg_ptr(seg), sizeof pit);
    have_pit = true;
}

const u8 *enh_mac_pit(void) { return have_pit ? pit : NULL; }

/* DESIGN 7.2: the PC panel elements (simflow.md "Screen layout of the cockpit", the lamp table 46D8) and their Mac places in CockpitOff (measured: the cyan
 * displays, the labels' boxes padded to the lens) */
#define D(sx0, sy0, sx1, sy1, mx0, my0, mx1, my1) { MAC_EL_DISPLAY, sx0, sy0, sx1, sy1, mx0, my0, mx1, my1 }
#define R(sx0, sy0, sx1, sy1, mx0, my0, mx1, my1) { MAC_EL_READOUT, sx0, sy0, sx1, sy1, mx0, my0, mx1, my1 }
#define L(sx0, sy0, sx1, sy1, mx0, my0, mx1, my1) { MAC_EL_LAMP, sx0, sy0, sx1, sy1, mx0 - 4, my0 - 3, mx1 + 4, my1 + 3 }
const MacEl enh_mac_front_els[] = {
    D(74, 110, 150, 169, 247, 271, 392, 414),     /* left MFD (map / scope) -> the centre display */
    D(169, 110, 245, 169, 429, 300, 528, 398),    /* right MFD (TV / nav / weapons / objectives) -> the right display */
    D(265, 140, 296, 167, 135, 325, 188, 378),    /* ADI horizon -> the ADI ball */
    R(261, 179, 274, 185, 470, 251, 495, 263),    /* fuel -> FUEL */
    R(261, 191, 274, 197, 470, 269, 495, 281),    /* throttle -> THRUST */
    R(35, 149, 43, 155, 158, 426, 176, 436),      /* chaff */
    R(35, 159, 43, 165, 158, 443, 176, 453),      /* flare */
    R(35, 169, 43, 175, 158, 460, 176, 470),      /* decoy */
    L(119, 94, 137, 102, 248, 237, 271, 243),     /* target mode 0 -> NAV */
    L(139, 94, 157, 102, 287, 237, 308, 243),     /* 1 -> AIR */
    L(159, 94, 177, 102, 325, 237, 348, 243),     /* 2 -> GND */
    L(184, 94, 205, 102, 366, 237, 394, 243),     /* 3 seeker lock -> LOCK */
    L(38, 105, 62, 113, 176, 254, 213, 260),      /* 4 stall -> STALL */
    L(257, 113, 282, 121, 431, 428, 460, 434),    /* 32 autopilot -> AUTO */
    L(257, 123, 282, 131, 431, 445, 460, 451),    /* 34 gear -> GEAR */
    L(284, 113, 309, 121, 474, 445, 508, 451),    /* 33 key-9 device -> FLAPS */
    L(284, 123, 309, 131, 428, 462, 463, 468),    /* 35 brakes -> BRAKE */
    L(257, 103, 275, 111, 481, 462, 503, 468),    /* 31 bay doors -> BAY */
    L(4, 191, 22, 199, 59, 366, 82, 372),         /* 0 radar / SAM lock -> RAD */
    L(24, 191, 42, 199, 62, 383, 74, 389),        /* 1 IR missile -> IR */
    L(44, 191, 62, 199, 53, 400, 81, 406),        /* 21 threat -> TRAN */
    L(4, 179, 32, 187, 60, 333, 83, 339),         /* 17 chaff mode -> ECM */
    L(36, 179, 62, 187, 60, 350, 81, 356),        /* 18 jammer -> IRJ */
    L(80, 173, 98, 181, 151, 272, 173, 278),      /* 22 map -> MAP */
    L(100, 173, 118, 181, 190, 272, 212, 278),    /* 23 scope -> TAC */
    L(170, 181, 188, 189, 559, 333, 582, 339),    /* 28 nav page -> NAV */
    L(190, 181, 208, 189, 561, 350, 582, 356),    /* 26 weapons page -> WPN */
    L(210, 181, 228, 189, 562, 366, 582, 372),    /* 29 objectives page -> ORD */
    L(170, 191, 188, 199, 564, 418, 588, 424),    /* 30 TV page -> FLIR */
    L(4, 117, 22, 125, 248, 445, 263, 451),       /* damage bit 0 -> NW */
    L(24, 117, 42, 125, 292, 445, 306, 451),      /* bit 1 -> FC */
    L(44, 117, 62, 125, 328, 445, 355, 451),      /* bit 2 -> FIRE */
    L(24, 137, 42, 145, 378, 445, 391, 451),      /* bit 3 -> BD */
    L(4, 137, 22, 145, 244, 462, 267, 468),       /* bit 4 -> ENG */
    L(51, 169, 62, 175, 192, 462, 210, 468),      /* 16 store 3 -> the lamp right of DECOY */
};
const int enh_mac_front_el_count = (int)(sizeof enh_mac_front_els / sizeof enh_mac_front_els[0]);
