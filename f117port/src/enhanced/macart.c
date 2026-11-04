/* The Mac art loader (S7.1, see macart.h). File format (tools/mac_art.py): 'MPIC', w, h, ncolours (u16 LE), ncolours RGB,
 * w * h indices. */
#include "macart.h"
#include "enhanced.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
