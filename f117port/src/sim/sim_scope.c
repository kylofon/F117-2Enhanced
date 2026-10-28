/* The left MFD's situation scope (spec simflow.md Part 7 section 2): world to scope coordinates (90B9), the target frame (903F) and the scope itself (8C10): a
 * grid, the air units with their icons, the missile tracks, the sites, the ownship and the countermeasure marks, all clipped to the MFD box. */
#include "enhanced/mapmfd.h"    /* ENH: S4.6 */
#include "enhanced/sandbox.h"   /* ENH: S4.6 */
#include "sim/sim.h"
#include "sim/sim_priv.h"

#define SITE(i) ((u16)(0xB2A2 + 16 * (u16)(i)))
#define UNIT(i) ((u16)(0xC13E + 0x24 * (u16)(i)))

static void *ctx_ptr(u16 ptr_off) { return DSP(DSW(ptr_off)); }

/* 0000:90B9 mfd_w2s: a world point to the scope (ownship at (112,152), heading up); [DE92] = -1 when it is outside the icon window */
void mfd_w2s(int wx, int wy)
{
    DSW(0xDE92) = 0;
    int cl = (7 - DSB(0x4082)) & 31;
    s16 a = (s16)((s16)(wx - DSW(0xC0A4)) >> cl);
    s16 b = (s16)((s16)(DSW(0xC0B2) - wy) >> cl);
    u16 h = DSW(0x2DEE);
    s16 x = (s16)(fx_cos_mul(h, a) - fx_sin_mul(h, b));
    s16 y = (s16)(fx_sin_mul(h, a) + fx_cos_mul(h, b));
    y = (s16)(0x98 - y);
    x = (s16)(x + 0x70);
    DSW(0x49E8) = (u16)y;
    DSW(0x49E0) = (u16)x;
    if (x < 0x4E || x > 0x93) DSW(0xDE92) = 0xFFFF;
    if (y < 0x71 || y > 0xA6) DSW(0xDE92) = 0xFFFF;
}

/* 0000:903F mfd_target_box: a 9x7 frame around a scope icon at (x, y) */
void mfd_target_box(int x, int y, int col)
{
    x = (s16)DSW(0x49E0);                                /* the original ignores its coordinates and uses the last w2s result */
    y = (s16)DSW(0x49E8);
    gfx_set_fg(col);
    hud_line_full2(x - 4, y - 3, x + 4, y - 3);
    hud_line_full2(x + 4, y - 3, x + 4, y + 3);
    hud_line_full2(x + 4, y + 3, x - 4, y + 3);
    hud_line_full2(x - 4, y + 3, x - 4, y - 3);
}

/* 0000:8C10 mfd_scope_draw */
void mfd_scope_draw(int flip)
{
    if (enh_sbx && enh_scope_pass(flip)) return;   /* ENH: S4.6 fix 1, the scope unrounded between steps */
    int r = (s16)DSW(0x4082) + 1;
    gfx_set_fg(0);
    fill_rows_far(ctx_ptr((u8)flip ? CTX_P1_PTR : CTX_P0_PTR), 0x49, 0x6D, 0x97, 0xAA);
    gfx_set_fg(8);
    int n = 1;
    if ((s16)DSW(0x4082) < 2 && DSW(0x98DE) != 0) n = (1 << ((2 - DSB(0x4082)) & 31)) + 1;
    u16 gx = DSW(0xC0A4) & 0xFC00, gy = DSW(0xC0B2) & 0xFC00;
    int tmp1, tmp2;
    for (int i = 1 - n; i <= n; i++) {                   /* vertical grid lines (northwards, 1024 world units apart) */
        mfd_w2s((s16)((i << 10) + gx), (s16)(gy + 0x1400));
        tmp1 = (s16)DSW(0x49E0); tmp2 = (s16)DSW(0x49E8);
        mfd_w2s((s16)((i << 10) + gx), (s16)(gy - 0x1000));
        hud_line_clip(tmp1, tmp2, (s16)DSW(0x49E0), (s16)DSW(0x49E8), 0x4A, 0x96, 0x6E, 0xA9, 0);
    }
    for (int i = 1 - n; i <= n; i++) {                   /* horizontal ones */
        mfd_w2s((s16)(gx + 0x1400), (s16)((i << 10) + gy));
        tmp1 = (s16)DSW(0x49E0); tmp2 = (s16)DSW(0x49E8);
        mfd_w2s((s16)(gx - 0x1000), (s16)((i << 10) + gy));
        hud_line_clip(tmp1, tmp2, (s16)DSW(0x49E0), (s16)DSW(0x49E8), 0x4A, 0x96, 0x6E, 0xA9, 0);
    }
    for (int i = 0; (s16)DSW(0xDED0) > i; i++) {         /* the air units */
        u16 o = UNIT(i);
        if (!(DSB(o + 0x18) & 2)) continue;
        if (DSW(o + 0x1A) == 0) continue;
        mfd_w2s((s16)DSW(o + 2), (s16)DSW(o + 4));
        if (DSW(0xDE92) == 0xFFFF) continue;
        if (DSW(0xE2FE) == 1 && (s16)DSW(0x3D64) == i) mfd_target_box((s16)DSW(0x49E0), (s16)DSW(0x49E8), 7);
        if ((s16)DSW(0x3D6A) > 0 && (u16)(0xFFFF - i) == DSW(0xC07E)) mfd_target_box((s16)DSW(0x49E0), (s16)DSW(0x49E8), (s16)DSW(0x9872));
        s16 a = (s16)(DSW(o + 0x10) - DSW(0x2DEE));
        a = (s16)(a + 0x800);
        s16 dz = (s16)(DSW(o + 6) - DSW(0x2DF4));
        int cls = 0;
        if (dz < -1000) cls = 1;
        if (dz > 1000) cls = 2;
        mfd_icon((a >> 12) & 0xF, cls, (s16)DSW(0x49E0), (s16)DSW(0x49E8));
    }
    for (int i = 0; i < 12; i++) {                       /* the missile and projectile tracks */
        u16 t = (u16)(0x3C0C + 0x1C * i);
        if (DSW(t + 0xE) == 0) continue;
        mfd_w2s((s16)DSW(t), (s16)DSW(t + 2));
        if (DSW(0xDE92) == 0xFFFF) continue;
        gfx_set_fg((s16)DSW((u16)(0x336E + 0x12 * DSW(t + 0x10))) > 0 ? 0x11 : 0x29);
        if (i >= 8) gfx_set_fg(0xF);
        u16 a = (u16)(DSW(t + 8) - DSW(0x2DEE));
        hud_line_clip((s16)DSW(0x49E0), (s16)DSW(0x49E8), (s16)DSW(0x49E0) - fx_sin_mul(a, r), (s16)DSW(0x49E8) + fx_cos_mul(a, r), 0x4A, 0x96, 0x6E, 0xA9, 0);
    }
    for (int i = 0; (s16)DSW(0xC0B4) > i; i++) {         /* the sites */
        u16 s = SITE(i);
        u16 fl = DSW(s + 8);
        if ((fl & 0x80) && !(fl & 0x200)) continue;
        mfd_w2s((s16)DSW(s + 2), (s16)DSW(s + 4));
        if (DSW(0xDE92) == 0xFFFF) continue;
        if (DSW(0xE2FE) == 2 && (s16)DSW(0x3D66) == i) mfd_target_box((s16)DSW(0x49E0), (s16)DSW(0x49E8), 7);
        if ((s16)DSW(0x3D6A) > 0 && DSW(0xC07E) == i) mfd_target_box((s16)DSW(0x49E0), (s16)DSW(0x49E8), (s16)DSW(0x9872));
        int c = 5;
        if (DSW(s + 8) & 0x201) c = ((((u16)(0x1000 - DSW(0x2DEE))) & 0x6000) >> 13) + 8;
        if (DSW(s + 6) != 0) c = 1;
        if (DSB(s + 8) & 8) c = 7;
        mfd_icon(c, 3, (s16)DSW(0x49E0), (s16)DSW(0x49E8));
    }
    mfd_w2s((s16)DSW(0xC0A4), (s16)DSW(0xC0B2));         /* the ownship */
    if (DSW(0xDE92) != 0xFFFF) mfd_icon(0, 3, (s16)DSW(0x49E0), (s16)DSW(0x49E8));
    for (int i = 0; i < 4; i++) {                        /* the countermeasure marks */
        u16 m = (u16)(0x39AA + 12 * i);
        if (DSW(m + 8) == 0) continue;
        mfd_w2s((s16)DSW(m), (s16)DSW(m + 2));
        if (DSW(0xDE92) == 0xFFFF) continue;
        switch (DSW(m + 6)) {
        case 1: mfd_icon(2, 3, (s16)DSW(0x49E0), (s16)DSW(0x49E8)); break;
        case 2: mfd_icon(3, 3, (s16)DSW(0x49E0), (s16)DSW(0x49E8)); break;
        case 3: mfd_icon(6, 3, (s16)DSW(0x49E0), (s16)DSW(0x49E8)); break;
        }
    }

}
