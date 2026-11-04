/* The Mac art option (PLAN.md Phase 7, DESIGN.md 7): the cockpit pictures of the Mac release (640x480, the original
 * MicroProse art), converted by tools/mac_art.py from the user's own Mac files into a folder given with --mac-art DIR.
 * Off by default and with --classic; a missing folder or picture leaves the PC art (one log line). */
#pragma once

#include "types.h"
#include <stdbool.h>

typedef enum { MAC_FRONT, MAC_LEFT, MAC_RIGHT, MAC_REAR, MAC_PIC_COUNT } MacPicId;

typedef struct {
    int w, h;
    u32 rgb[256];       /* 0x00RRGGBB */
    u8 key[256];        /* 1 = see-through (pure cyan 0,255,255) */
    u8 *pix;            /* w * h colour indices, rows top down */
} MacPic;

/* --mac-art given, the canvas on, not --classic / F117_CLASSIC. */
bool enh_mac_on(void);
/* The picture, loaded on first use; NULL = not available (the PC art is used). */
const MacPic *enh_mac_pic(MacPicId id);
/* The PC view mode (C07A: 0x41 rear, 0x42 left, 0x43 right) -> its Mac picture, or NULL. */
const MacPic *enh_mac_view_pic(int view_mode);
/* --mac-art-test (with --mac-art DIR): loads the pictures and checks them; 0 = pass, 77 = no --mac-art folder (skipped). */
int enh_mac_selftest(void);
