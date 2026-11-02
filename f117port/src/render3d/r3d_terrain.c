/* Terrain and scenery of render3d (spec port/spec/render3d.md Part 1 section 2 and Part 3): level conversion 0804, tile types 0865, the scenery override 0FB1,
 * the terrain walk 04DA, the map-MFD terrain 119C / 11F2 / 1353 / 13AC / 13E3, the scenery collision probe 0FFB / 77D9 and the site / unit helpers BC12 / BCD7. */
#include "render3d/r3d.h"
#include "sim/sim.h"
#include "enhanced/wide.h"   /* ENH: S3.4 */
#include "enhanced/enhanced.h"   /* ENH: fine terrain */

static inline s16 abs16(s16 v) { return (s16)(v < 0 ? (u16)-(u16)v : (u16)v); }

/* 0000:0804 terr_to_level: world units -> level-L units (uint32, carries kept in 32 bits) */
u32 terr_to_level(s16 level, u32 v)
{
    switch (level) {
    case 0: return v << 1;
    case 1: return v;
    case 2: return (v + 2) >> 2;
    case 3: return (v + 8) >> 4;
    case 4: return (v + 0x20) >> 6;
    default: return (u16)(level - 5);                                   /* never used */
    }
}

/* 0000:0865 terr_tile_type: the tile type of cell (x, y) at a level; level 4 adds 2 to x and y; out of range -> 0 */
u16 terr_tile_type(s16 level, s16 x, s16 y)
{
    if (level == 4) { x = (s16)(x + 2); y = (s16)(y + 2); }
    s16 dim = DSS((u16)(0x0506 + 2 * level));
    if (x < 0 || y < 0 || dim <= x || dim <= y) return 0;
    if (level == 4) return DSB((u16)(0xB00C + y * 8 + x));
    if (level == 3) return DSB((u16)(0x9F68 + y * 16 + x));
    static const u16 base[3] = { 0x9904, 0x9B08, 0x9D10 };
    u16 p = terr_tile_type((s16)(level + 1), (s16)(x >> 2), (s16)(y >> 2));
    return DSB((u16)(base[level] + p * 16 + (y & 3) * 4 + (x & 3)));
}

/* 0000:0FB1 terr_scenery_override: the replacement object of a flagged record; AH = high byte of [9902]-1 throughout (0xFF00 on an empty table: quirk) */
u16 terr_scenery_override(s16 level, s16 idx, s16 cx, s16 cy)
{
    u16 ax = (u16)(DSW(0x9902) - 1);
    u16 ah = ax & 0xFF00;
    for (DSW(0x94DC) = ax; DSS(0x94DC) >= 0; DSW(0x94DC)--) {
        u16 e = (u16)(0xB772 + 5 * DSW(0x94DC));
        if (DSB(e) == (u8)level && DSB((u16)(e + 1)) == (u8)idx && DSB((u16)(e + 2)) == (u8)cx && DSB((u16)(e + 3)) == (u8)cy) return (u16)(ah | DSB((u16)(e + 4)));
    }
    return ah;
}

/* 0000:04DA r3d_draw_terrain(a1 = -heading, pitch, x, y, z): levels 4 -> 1, cells far -> near (octant tables, path A quirk), records -> E7AC */
void r3d_draw_terrain(s16 a1, s16 pitch, s32 x, s32 y, s32 z)
{
    bool near_ = DSS(0x98DE) >= 2 || DSW(0x0348) != 0;
    u16 oct = (u16)((u16)(0x1000 - a1) >> 13);
    for (DSW(0xE2FC) = 4; DSS(0xE2FC) >= 1; DSW(0xE2FC)--) {
        s16 L = DSS(0xE2FC);
        if (DSW((u16)(0x04FA + 2 * L)) == 0) continue;
        u32 u = terr_to_level(L, (u32)x);
        s16 cx = (s16)(u16)(u >> 12), fx = (s16)(u & 0x0FFF);
        u = terr_to_level(L, (u32)y);
        s16 cy = (s16)(u16)(u >> 12), fy = (s16)(u & 0x0FFF);
        u = terr_to_level(L, (u32)z);
        if ((u >> 16) != 0 || (u16)u >= 0x7FFF) continue;
        DSW(0x94DA) = (u16)u < 2 ? 2 : (u16)u;
        enh_terr_dz = 0;
        if (z >= 0 && enh_fine_terrain_on()) {                            /* ENH: the exact camera height in level units (Q8), not the rounded one clamped to 2 */
            s64 zq = (s64)z << (L == 4 ? 2 : L == 3 ? 4 : L == 2 ? 6 : 8);
            if (zq < 32) zq = 32;
            enh_terr_dz = (int)(((s64)DSW(0x94DA) << 8) - zq);
        }
        for (int i = 0;; i++) {
            bool pathA = L == 4 && DSS(0x98DE) >= 2;
            s16 ox, oy;
            int ii = i;                                                       /* ENH: S3.4, the index the "i < 4" tests see */
            if (enh_wide_on) {                                                /* ENH: S3.4, the wide cell list in a wide display pass */
                ii = enh_wide_cell(pathA, pitch, (int)oct, i, &ox, &oy);
                if (ii < 0) break;
                if (!pathA && L != 4 && DSS(0x98DE) < 2 && ii < 4) continue;
            } else {
            if (pathA) { if (i == 15) break; }
            else {
                if (i == 9) break;
                if (L != 4 && DSS(0x98DE) < 2 && i < 4) continue;
            }
            if (!pathA && pitch < (s16)0xD555) { ox = DSS((u16)(0x04CC + 2 * i)); oy = DSS((u16)(0x04DE + 2 * i)); }
            else { ox = DSS((u16)(0x034C + 2 * (oct * 9 + i))); oy = DSS((u16)(0x034C + 2 * (((oct + 2) & 7) * 9 + i))); }
            }
            DSW(0x94D6) = (u16)(fx - (u16)(ox << 12) - 0x800);
            DSW(0x94D8) = (u16)(fy - (u16)(oy << 12) - 0x800);
            if (pathA) {
                DSW(0x497A) = 7;
                if (r3d_cull_point((s16)-DSS(0x94D6), (s16)-DSS(0x94D8), (s16)-DSS(0x94DA))) continue;
            }
            r3d_set_origin(DSS(0x94D6), DSS(0x94D8), DSS(0x94DA));
            s16 tx = (s16)(ox + cx), ty = (s16)(oy + cy);
            u16 tile = terr_tile_type(L, tx, ty);
            if (tile == 0xFFFF) continue;
            u16 idx = (u16)(L * 32 + tile);
            if (ii < 4 && !near_) {                                           /* ENH: S3.4, ii (= i but in a wide pass) */
                if (L == 4) {
                    u16 p = DSW((u16)(0xE304 + 2 * idx));
                    DSW(0xE44A) = p;
                    DSW(0x0504) = 0x400;
                    r3d_place_object((u16)(0x100 | DSB((u16)(p + 6))), 0, 0, 0, DSS(p), DSS((u16)(p + 2)), DSS((u16)(p + 4)));
                }
                continue;
            }
            DSW(0x0504) = DSW(0x98DE) == 2 ? 0 : (u16)(L << 8);
            u16 p = DSW((u16)(0xE304 + 2 * idx));
            DSW(0xE44A) = p;
            for (u16 j = 0; DSW((u16)(0x061E + 2 * idx)) > j; j++) {
                DSW(0xC044) = DSW(0x9874); DSW(0xC042) = DSW(0x9874);
                u8 ob = DSB((u16)(p + 6));
                u16 obj;
                if (!(ob & 0x80)) obj = ob;
                else {
                    u16 ov = terr_scenery_override(L, (s16)j, tx, ty);
                    if (ov == 0) obj = ob & 0x7F;
                    else if (((u8)ov ^ ob) & 0x7F) obj = ov;
                    else { DSW(0xC044) = 0; DSW(0xC042) = 0; obj = ov | 0x8000; }
                }
                r3d_place_object(r3d_scenery_id(obj | 0x0100), 0, 0, 0, DSS(p), DSS((u16)(p + 2)), DSS((u16)(p + 4)));
                DSW(0xE44A) += 7;
                p = DSW(0xE44A);
                DSW(0x0504)++;
            }
        }
    }
    enh_terr_dz = 0;                                                      /* ENH: fine terrain */
}

/* ------------------------------------------------------------------------------------------------ the map MFD */

/* 0000:13AC map_pixel_to_tile */
void map_pixel_to_tile(s16 px, s16 py, s16 *tx, s16 *ty)
{
    s16 T = DSS(0x94DE);
    *tx = (s16)((s16)(DSW(0x94E2) - DSW(0x0D1E) + px) / T);
    s16 t = (s16)((s16)(u16)((u16)(py - DSS(0x0D20)) << 2) / 3);
    *ty = (s16)((s16)(t + DSS(0x94E4)) / T);
}

/* 0000:1353 map_tile_bounds */
void map_tile_bounds(s16 *x0, s16 *x1, s16 *y0, s16 *y1)
{
    map_pixel_to_tile(0, 0, x0, y0);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    map_pixel_to_tile(DSS(0x2293), DSS(0x2295), x1, y1);
    if (*x1 >= DSS(0x94E0)) *x1 = (s16)(DSS(0x94E0) - 1);
    if (*y1 >= DSS(0x94E0)) *y1 = (s16)(DSS(0x94E0) - 1);
}

/* 0000:13E3 map_draw_object: one ground record in orthographic map mode, identity orientation */
void map_draw_object(u16 obj, s16 x, s16 y)
{
    if (DSS(0xE2FC) >= 3)
        if ((DSB((u16)(DSW(0x0A62) + (obj & 0xFF))) & 0x80) != DSW(0x94E6)) return;
    DSW(0x8560) = DSW(0xB760);
    mat_from_euler(0, 0, 0, 0x49A0);
    r3d_transpose49A0();
    r3d_draw_object(obj & 0xFF, x, 0, (s16)-y, 0x49A0, 0xC02E);
}

/* 0000:11F2 map_draw_tiles: five passes over the levels {4,3,2,4,3} (flat objects first, solid ones last) */
void map_draw_tiles(s16 px, s16 py, s16 shift)
{
    DSW(0x94E2) = (u16)(px >> (shift & 0x1F));
    DSW(0x94E4) = (u16)(py >> (shift & 0x1F));
    for (DSW(0x94E8) = 4; DSS(0x94E8) >= 0; DSW(0x94E8)--) {
        DSW(0x94E6) = DSS(0x94E8) <= 1 ? 0x80 : 0;
        s16 L = DSS((u16)(0x0CFC + 2 * DSW(0x94E8)));
        DSW(0xE2FC) = (u16)L;
        DSW(0xDECC) = (u16)((4 - L) * 2 + shift);
        s16 T = (s16)(0x1000 >> (DSB(0xDECC) & 0x1F));
        DSW(0x94DE) = (u16)T;
        if (T <= 0x10) continue;
        DSW(0x94E0) = (u16)(4u << ((u8)((4 - (u8)L) * 2) & 0x1F));
        s16 x0, x1, y0, y1;
        map_tile_bounds(&x0, &x1, &y0, &y1);
        for (s16 ty = y0; !(y1 < ty); ty++)
            for (s16 tx = x0; !(tx > x1); tx++) {
                s16 sx = (s16)((s16)(tx * T) - DSS(0x94E2) + (T >> 1));
                s16 sy = (s16)((T >> 1) + (s16)((s16)(T * ty) - DSS(0x94E4)));
                u16 tile = terr_tile_type(L, tx, ty);
                if (tile == 0xFFFF) continue;
                u16 idx = (u16)(L * 32 + tile);
                u16 p = DSW((u16)(0xE304 + 2 * idx));
                DSW(0xE44A) = p;
                for (u16 j = 0; DSW((u16)(0x061E + 2 * idx)) > j; j++, p = (DSW(0xE44A) += 7)) {
                    if (DSW((u16)(p + 4)) != 0) continue;
                    DSW(0x7D5E) = (u16)(DSW(0xDECC) + 0x0E);
                    int c = DSB(0xDECC) & 0x1F;
                    s16 yy = (s16)((u16)((u16)sy << c) + DSW((u16)(p + 2)));
                    s16 xx = (s16)((u16)((u16)sx << c) + DSW(p));
                    map_draw_object(DSB((u16)(p + 6)), xx, yy);
                }
            }
    }
}

/* 0000:119C map_render_terrain(ctx, px, py, shift): the left MFD map (straight-down camera, orthographic) */
void map_render_terrain(void *ctx, int px, int py, int shift)
{
    u16 vp = (u16)((u8 *)ctx - DSP(0));
    DSB(0x4894) = 0;
    r3d_begin_scene(vp, 0, (s16)0xC000, 0, 0, 0, 0x2000, 1, 0x100);
    offset_set(pixel_offset(DSS((u16)(vp + 0x12)), DSS((u16)(vp + 0x0E))));
    map_draw_tiles((s16)px, (s16)py, (s16)shift);
    r3d_end_scene();
    DSW(0x7D5E) = 0;
}

/* ------------------------------------------------------------------------------------------------ the scenery collision probe */

/* 0000:0FFB terr_probe_point(x, y, z): byte [C686] = 80h if the point is inside the nearest solid model of its level-4 tile (quirks: the last record is never
 * tested, the id byte is tested with 50h) */
void terr_probe_point(u32 x, u32 y, u32 z)
{
    DSW(0xC686) = 0;
    DSW(0xBA16) = 0x7FFF;
    u32 u = terr_to_level(4, x);
    s16 cx = (s16)(u16)(u >> 12), fx = (s16)(u & 0x0FFF);
    u = terr_to_level(4, y);
    s16 cy = (s16)(u16)(u >> 12), fy = (s16)(u & 0x0FFF);
    DSW(0x4980) = (u16)terr_to_level(4, z);
    DSW(0x497C) = (u16)(fx - 0x800);
    DSW(0x497E) = (u16)(fy - 0x800);
    u16 tile = terr_tile_type(4, cx, cy);
    if (tile != 0xFFFF) {
        u16 p = DSW((u16)(0xE404 + 2 * tile));
        DSW(0xE44A) = p;
        for (u16 i = 1; DSW((u16)(0x061E + 2 * (4 * 32 + tile))) > i; i++, p = (DSW(0xE44A) += 7)) {
            u8 ob = DSB((u16)(p + 6));
            if (ob & 0x50) continue;
            if (!(DSB((u16)(0x0960 + ob)) & 0x10)) continue;
            s16 d = (s16)(abs16((s16)(DSW(p) + (0x800 - fx))) + abs16((s16)(DSW((u16)(p + 2)) + (0x800 - fy))));
            DSW(0x4978) = (u16)d;
            if (d < DSS(0xBA16)) { DSW(0xBA20) = p; DSW(0xBA16) = (u16)d; }
        }
    }
    if (DSW(0xBA16) == 0x7FFF) return;
    u16 p = DSW(0xBA20);
    DSW(0xE44A) = p;
    DSW(0x4964) = (u16)(DSW(p) - DSW(0x497C));
    DSW(0x4966) = (u16)(DSW((u16)(p + 2)) - DSW(0x497E));
    DSW(0x4968) = (u16)(DSW((u16)(p + 4)) - DSW(0x4980));
    u16 id = DSW((u16)(0x0510 + 2 * DSB((u16)(p + 6))));
    DSW(0x8560) = DSW((u16)(0xB75E + 2 * (id >> 8)));
    mat_from_euler(0, 0, 0, 0x49A0);
    r3d_transpose49A0();
    r3d_probe_object(id & 0xFF, DSS(0x4964), DSS(0x4968), (s16)-DSS(0x4966), 0x49A0);
}

/* 0000:77D9 terr_probe_cell(cellx, celly, alt) */
void terr_probe_cell(s16 cellx, s16 celly, s16 alt)
{
    terr_probe_point((u32)(s32)cellx << 5, (u32)((s32)0x8000 - (s32)celly) << 5, (u32)(s32)alt);
}

/* ------------------------------------------------------------------------------------------------ site and unit helpers */

/* 0000:BC77 obj_cls_soft (owner weapons, flight.md 5.4): the site's class is 9, 0Bh, 0Ch or 0Dh */
int obj_cls_soft(int o)
{
    u16 c = DSB((u16)(0xC604 + (DSB((u16)(0xB2B0 + 16 * o)) & 0x7F))) & 0x0F;
    return c == 0x0C || c == 0x0D || c == 9 || c == 0x0B;
}

/* 0000:BC12 obj_wreck_shape: the model id to draw for a site (destroyed variants; a replacement id >= 80h loses its bank: quirk) */
u16 obj_wreck_shape(s16 o)
{
    u16 S = (u16)(0xB2A2 + 16 * o), shape = DSW((u16)(S + 0x0E));
    u16 id = r3d_scenery_id(shape);
    if (!(DSB((u16)(S + 8)) & 0x80)) return shape;
    u16 tb = DSW((u16)(0x0A60 + 2 * ((s16)id >> 8)));
    if (DSB((u16)(tb + (id & 0xFF))) & 0x20) return (u16)(shape | 0x0100);
    u8 b = obj_cls_soft(o) ? DSB(0xC0AC) : DSB(0xDEA4);
    return (u16)((s16)(s8)b + 0x100);
}

/* 0000:BCD7 r3d_unit_terrain_class: [49C6] = the shading class of unit u */
void r3d_unit_terrain_class(u16 u)
{
    u16 U = (u16)(0xC13E + 0x24 * u);
    u16 c = DSB((u16)(0xB174 + (DSW((u16)(U + 4)) >> 11) * 16 + (DSW((u16)(U + 2)) >> 11))) & 3;
    if (!((s16)(DSW(0xDED0) - 4) > (s16)u)) DSW(0x49C6) = c;
    else DSW(0x49C6) = (c == 0 && DSW(0x3D62) == 0) ? 1 : 0;
}
