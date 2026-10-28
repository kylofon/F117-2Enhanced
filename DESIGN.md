# F-117A Enhanced — render and timing design (S1.1)

Binding inputs: PLAN.md Goal and Decisions 2026-10-19 (front cockpit: art in place, world extended around it;
left/right/rear views: whole picture scaled; full-screen 3D and outside views: extended), U3 defaults (render scale =
largest whole multiple, sim at the faithful 10 steps/s, nearest-neighbour art). Spec references are to
`../F117-2/port/spec/` (`render3d.md` = R3D, `simflow.md` = SF, `flight.md` = FL); port files are under `f117port/src/`.

## 0 Decisions in one table

| Topic | Decision |
|---|---|
| N× layer | A second, passive back end (`enhanced/hr_*.c`) fed at the points where render3d produces screen geometry. The faithful 1× page drawing keeps running unchanged; the hr back end rasterises the same primitives at N× into an 8-bit **3D layer** (DAC indices). |
| Precision | N enters inside the innermost rounding of each projection formula, so the result at N=1 is the original expression (rule R1, section 2.2). Screen geometry in the hr path is int32 (int64 intermediates). |
| Compositing | A 320x200 **shadow plane** records, per screen pixel, who wrote it last (3D / vector) and the value written. At present time a pixel whose page-0 value still equals its shadow value shows the N× layer block; any other pixel is original art, scaled N×N nearest. No hand-made masks. |
| Vectors at N× | HUD/MFD/map lines and 3D-scene dots/tracers are drawn thin into a **vector layer**; fonts, PIC/PAN art, lamps and sprites stay original pixels (sprites at N× positions, N×N blocks). |
| Wider view | A canvas of any aspect; the 320x200 frame at N× sits bottom centre; the projection keeps its centre and scale (256N px per unit), so the field of view grows with the canvas. Culling is widened only in display passes. |
| Frame rate | The faithful frame (render + flip + input + `39c4` step) runs unchanged at the step rate. Between steps, **sandboxed display passes** re-render a saved pre-render memory image with interpolated fields, compose, present, and restore the live state. The sim never sees a display pass. |
| Snapshot | Whole machine image (`mem[]`, VGA model, driver statics, hr layers + shadow) taken once per step; 11 scalar fields + 4 object tables are interpolated (section 4.3). |
| `--classic` | Every hook is behind an option test; `--classic` clears all options, so no hook body runs and the host keeps today's 320x200 present path. |

## 1 Facts the design rests on (measured from code and data)

### 1.1 One faithful frame (SF Part 3 section 0, `sim/sim_main.c frame_loop_body`)
`view_frame` (2F82, `sim/sim_view.c`) → `r3d_scene_draw` (03CE) into page 1: `r3d_begin_scene` (DAE8: `r3d_set_viewport` DB71,
camera DC64, wait `[262A]`, sky 1442), terrain 04DA, world objects 9ACA, `r3d_end_scene` DB3E (queue DF08, spinners), `[7D50]`,
overlay pass A17E (`obj_overlay_frame`, which also renders the TV window `mfd_view_render` bf71 as a nested 3D pass) →
cockpit pastes (SF 2.12) → `hud_overlay_frame` 7898, `hud_draw_far`, `hud_side_strips`, `cockpit_draw_adi` → `frame_floor` →
`flip(160*[2E34])` copies rows 0..h-1 of page 1 to A000 → `sim_input_flight` 1e9e → `sim_frame_step` 39c4.
The instrument panel rows live on A000 and are refreshed by explicit page 1 → page 0 copies (MFD windows, ADI).

### 1.2 3D windows and art per view (SF 1, 2.9–2.12; key colour 0 = `blit_key_body`, `video/mgraphic.c`)

| View `[C07A]` | 3D viewport (DB71, frame px) | Centre `[85D2],[85D4]` | Art over the 3D after the scene | Key-0 pixels in that art (measured, `tools/picdec.py`) |
|---|---|---|---|---|
| 0 front cockpit | x 0..319, y 0..90 (`[2DFC]`=0) or 0..106 (`[2DFC]`=1, initial) | 159, 52 (60 if `[294B]`) | h=107 only: rows 91..106 x 32..287 opaque strip (pit rows 91..106); x 0..31 and 288..319 keyed from pit rows 74..89 | strip 0/4096; each corner 272/512 |
| 0x41 rear | 0..319, 0..156 | 159, 100 | centre x 91..228 opaque; x 0..90, 229..319 keyed (256REAR) | centre 0; each side 9793/14287 |
| 0x42 / 0x43 left/right | 0..319, 0..160 | 159, 100 | full 320x161 keyed (256LEFT / 256RIGHT) | 37995/51520 each (mirror images) |
| 0x44 front clean | 0..319, 0..199 | 159, 100 | grey nose triangle (160,185)(230,199)(90,199), colour 5 | — |
| 0x84..0x8C outside | 0..319, 0..199 | 159, 100 | none | — |
| TV (bf71, in A17E) | x 169..245, y 110..169 on page `[4088]`, blitted to page 0 at the same place | window centre | labels (font) | — |
| map MFD (119C, `mfd_map_redraw` 8387) | left MFD x 74..150, y 110..169, ortho mode `[7D5E]` | — | map symbols, text | — |

Every path from a 3D pixel to the screen is a copy to the **same coordinates** (flip, the 77x60 MFD `blit_rect`s, keyed blits).
This is what lets the shadow plane (2.5) work per screen coordinate. There is no full-screen map view in VGAME's view list;
the map is an MFD (panel, not extended); the shell's map screens are 2D and pillarboxed (3.4).

### 1.3 The render is not pure (why display passes must be sandboxed)
The render path changes sim state: 9ACA runs the target designators (`[3D66]`, `[3D64]`, the `[B28E]` cooldown, R3D Part 2);
A17E moves and hit-tests cannon tracers, kills units, damages the player, starts explosions, posts messages (SF Part 9 S2);
the TV fire puffs C73C draw from the sim LCG `DS:926E` (R3D C73C); DB3E advances the spinners `[C04C]/[C04E]`; DAE8 sets the
collision flag `[C686]` → `[9F66]`; the graphics driver has its own state (`mgraphic.c` statics incl. `rng`, `shake`).
So: (1) the canonical frame must run exactly once per step with faithful viewport and culling; (2) anything rendered more
often, or with a widened view, runs on a saved image whose every change is thrown away.

## 2 (a) High-resolution rendering

### 2.1 Layers
* **Canvas** (layer pixels) `Lw x Lh` (section 3.1); frame origin `(fx, fy)`, scale N (integer).
* **3D layer**: `u8[Lw*Lh]`, DAC indices; written only by the hr back end (`enhanced/hr_raster.c`).
* **Vector layer**: `u8[Lw*Lh]` + coverage bit (0 = transparent); written by the vector emitter (`enhanced/hr_vec.c`).
* **Shadow plane**: per frame pixel `{src: none/3D/vec, val, under, under3d}` (4 B x 64000), see 2.5.
* Colours stay 8-bit indices end to end; the DAC is applied once, in the compositor, from the **live** VGA state, so
  palette fades, the DAC[13] flicker and day/night recolours (`explosion_fx_tick`, `cockpit_daynight_recolor`) work as today.

### 2.2 The hr back end: dual emission, and where N enters
The faithful functions run as now and write the 1× page. At each geometry point an `ENH:` hook passes the same primitive,
recomputed at N×, to the hr back end. `hr_clip.c`, `hr_raster.c`, `hr_edge2d.c` are **copies** of `r3d_clip.c`, `r3d_raster.c`,
`edge2d.c` with: coordinates int32, records/tables in host memory (`HrCtx`: window, centre, L/R tables of `Lh` rows,
edge records, target layer), and N plus the frame offset folded in. The faithful files are not edited beyond the hooks.

**Rule R1.** Put N inside the innermost rounding: `trunc(a/b)` becomes `trunc(a*N/b)`, `v >> k` becomes `(v*N) >> k`,
`mul15r(a,b)` becomes `(a*b*N + 0x4000) >> 15`; screen offsets become `fx + N*centre`. Any 16-bit wrap that can happen at
1× is applied at the 1× magnitude first, then scaled. At N=1, fx=fy=0 every formula is the original one.

| Geometry point (spec) | Port function | hr version |
|---|---|---|
| 11EB:0906 perspective | `r3d_project` (`r3d_object.c`) | `q = idiv((v*N)>>8, zh)`; small-z branch `idiv((v>>1)*N, d)`, INT 0 fallback `idiv(((v>>1)*N)>>8, d)*256`; "behind" kept as a separate flag, not the `8000h` marker; hr vertex array beside `D688` |
| 11EB:0906 ortho (map, `[7D5E]`) | same | `(coord_hi*N) << c` / `(coord_hi*N) >> c` with the original round-half-up |
| 11EB:0B3F edges + near point | `r3d_make_edge` | record in `HrCtx`; centre `fx + N*[85D2]`, `fy + N*[85D4]`; near point `((7D42*N) >> 8)*2` written as `(([7D42]>>8)*2)` at N=1 |
| 12EF clipper (fast16, path32 bisection, walls) | `r3d_clip_edge`, `r3d_poly_add_edge`, `r3d_poly_finish` | same algorithm, window `[N*xmin + fx, N*(xmax+1)-1 + fx]` etc.; the fast16/path32 choice uses the same "fits s16" test on N× values; sums/products int64 |
| 1359 edge DDA, walls, `fill_M` (dither/AND/OR/stipple), `line_M` | `r3d_edge_dda`, `r3d_wall_fill`, `r3d_fill_M`, `r3d_line_M` | unchanged integer DDA on N× coordinates; colour from `[85D6]` after `r3d_setcolor_M` (shade ramp included); dither phase see 2.4 |
| 1442 sky/ground bands | `band_points`, `bands` (`r3d_sky.c`) | boundary points by R1 (`mul15r` with N), `w2,h2` → layer centre; quads to `hr_edge2d` poly fill |
| 1001 `hz_draw` (detail < 3), `poly_fill_far` | `edge2d.c` | same; half-length uses the hr viewport's larger half-width |
| B85D world→screen (dots, billboards, tracers) | `r3d_project_point` (`sim/sim_objovl.c`) | also leaves hr `sx, sy` (layer px, int32) in an enh global for the emitters of the same object |
| 2D sprites in the scene (DF08 sprite records, 1AD6/1B1B/1A85) | `sprite_scaled` (`mgraphic.c`) | sprite pixels as NxN blocks at the hr position of the record (original pixels, sub-pixel placement) |

The hr back end never writes `mem[]`, the VGA model or driver statics (it is invisible to the sim and to the 1× page).

### 2.3 Overflow risks (16-bit fixed point)
| Place | 1× range | hr handling |
|---|---|---|
| 0906 `n = v>>8`, out up to 2^23 (INT 0 path) | s32 | int64 for `n*N`; output int32 (≤ 2^23·N, N ≤ 64) |
| B85D `lsl32(X,8)` wraps for \|X\| ≥ 2^23 (quirk) | wrap mod 2^32 | wrap at 1× (`(s32)((u32)X<<8)`), then `*N` in int64 |
| 12EF fast16 intercept `(xb-bx)*dy` | s16·s16 | int64 |
| 12EF path32 midpoint `(p0+p1+1)>>1` (33rd bit lost) | s32 | int64 sum; N× coordinates stay < 2^30 |
| 1359 L/R tables, sentinels 7FFFh / 8001h, rows ≤ 480 | s16 | int32 tables of `Lh` rows, sentinels INT32_MAX / INT32_MIN+1 |
| 1001 tables (YMAX ≤ 219), INT 0 saturation for huge band quads | s16 | `Lh` rows; saturation kept (copy), int32 values |
| E8B5 cull adds wrap (class 7, \|Ch\| > 13016) | s16 | widening (3.3) scales the **wrapped** 1× tolerance, so a wrap-culled object stays culled |

### 2.4 Colours, dither, line width
Indices are copied as is (shade ramp of `r3d_setcolor_M`, AND `FEh`/OR `FDh` modes read-modify-write the 3D layer).
The checkerboard dither (`fill_M`, `((x^r)&1)`) is evaluated on **original-pixel phase** `((X-fx)/N ^ (Y-fy)/N) & 1`,
so dithered faces keep the original look at any N (option `--dither fine` for a per-layer-pixel checkerboard; Q2).
Lines (`line_M`, HUD lines) are drawn `max(1, N/2)` layer pixels wide (Q3).

### 2.5 Compositing: the shadow plane
Emitters record into the shadow at screen coordinates `(off % 320, off / 320)` of every page-0/page-1 write they make
(`draw_off` / viewport offset included); page-2 (picture page) writes are ignored.
* 3D emitter (1359 spans/lines, 1001 polys and sprites inside a 3D pass): `src = 3D, val = byte written`.
* Vector emitter (`line()`/`put_pixel` outside a 3D pass): if the old record is stale (page value ≠ `val`) clear that
  block of the vector layer first; then `under = previous page value`, `under3d = (old src == 3D && fresh)`, `src = vec, val`.
* `r3d_set_viewport` clears the vector layer and shadow inside the new pass's window.

Compose (per displayed pixel, honouring CRTC start/pan as `vga.c compose` does, so the explosion shake moves the layers too):

| Shadow of the page-0 pixel | Shown in its NxN block |
|---|---|
| `3D` and page value == `val` | 3D layer block |
| `vec` and page value == `val` | vector-layer pixels over (`under3d` ? 3D layer block : `under` scaled) |
| anything else | the page-0 value, NxN (original art, fonts, panel, keyed frames) |
| canvas outside the frame | 3D layer (extended views) or black |

This reproduces section 1.2 exactly without tables: keyed frame pixels (key 0) are never written, so the 3D shows through;
opaque strips overwrite and win. Known artefact: an overlay pixel of the very same index as the 3D pixel under it is shown
as hi-res 3D (invisible in practice). Debug: `--enh-mask-dump DIR` writes the classification as a PNG (U1).

### 2.6 What is drawn at N×
| N× (thin / sub-pixel) | Original pixels (NxN nearest) |
|---|---|
| all 3D polygons, 3D lines, sky/ground, terrain, objects (main view, TV window, map MFD terrain) | fonts and all text (HUD digits, labels, messages) |
| HUD pitch ladder, horizon, heading/speed/altitude tape lines and ticks (`hud_tick_strip`), reticle, flight-path marker, target boxes, ILS bars (`line()` calls of `sim_hudasm.c`, `sim_hud.c hud_line_*`, `sim_overlay.c`) | PIC art (cockpit, panel, frames), PAN animations, lamps (`rect_recolor`) |
| ADI horizon lines (`cockpit_draw_adi`), map MFD lines and blips (`mfd_map_redraw`), TV window crosshair | sprites: explosions, smoke, billboards, ordnance MFD sprite (placed at N× positions) |
| far unit/projectile dots (9ACA 873F), cannon tracers (A17E S2) | `hud_fill_rect`, MFD fills, shell/debrief screens |

HUD line endpoints are integers computed at 1× (centre of the 1× pixel → layer `fx + N*x + N/2`), so they are thin but move in
1× steps; giving ladder/tapes sub-pixel endpoints is an optional later step inside S3.3 (only where the port's C computes them).

### 2.7 As built (S2.2): choices where 2.2–2.5 were silent
* Files: `enhanced/hr.h`, `hr_pass.c` (pass context, B85D point, `--hr-check`), `hr_clip.c` (12EF + 0906 + 0B3F), `hr_raster.c` (1359),
  `hr_edge2d.c` (1001 + 1442). Hooks (`grep -rn "ENH:" src/render3d src/sim/sim_objovl.c`): `r3d_set_viewport` / `r3d_end_scene` (pass, route),
  `r3d_project`, `r3d_build_edges` (after each `r3d_make_edge`, whose near-plane point 7D42 the hr edge reads), `r3d_poly_add_edge`,
  `r3d_poly_finish`, `r3d_line_M` (+ shadow records in `line_M` / `fill_M`), `r3d_draw_sky_ground` (both exits), `r3d_project_point`.
* R1 at the 16-bit edges: 0906's INT 0 test is made on the 1× quotient (its fallback from the N× numerator); the fast16 / path32 choice and
  every "fits s16" / overflow test of 12EF and 1001 use the 1× magnitude (`|v| <= 32767 N`), so the hr clip takes the 1× path; the 1001
  intercept saturates to `N * 7F00h`. A band / horizon point is computed three ways (the original s16 code, exact at 1×, R1 at N×): if
  the 1× value wrapped, the point is `N *` the wrapped value, else the N× value. `half_q` and the other path quirks are kept literally.
* State: the 1359 row tables (gap rows) and the 12EF assembler state persist between polygons in the original, so each pass starts from
  the faithful ones, scaled (row r → rows `fy + N r ..`, L → `fx + N L`, R → `fx + N R + N - 1`); at N = 1 a copy.
* Ortho (map): `((x N) >> 16) >> c` with the round bit, which equals R1 one-stage; slot 37's skip of a one-pixel span at 13Fh uses
  `N 320 - 1`. The hz horizon line itself is drawn 1 px (the fills cover it); 3D lines use the `max(1, N/2)` brush at offset `(N - w) / 2`.
* Shadow: page 1 is MGRAPHIC's `video_page_seg(1)` (a DOS block in the game, not A400; S2.1 assumed A400). 1359 writes record `3D` without
  touching the layer (the hr polygon follows); any other 1× write inside a 3D pass (sprites, dots, the 1× backdrop) **stamps** its N×N block
  with the 1× value (bit 9), so it shows as the original pixel until an hr write replaces it. A write to the other page during a pass (the
  TV camera's backdrop lands on MGRAPHIC's current page) does not stamp; the hr backdrop is drawn only when that page is the pass page.
* Not yet (later subtasks): sprites at sub-pixel positions (stamped at their 1× place; S3.1 uses `enh_hr_pt` of B85D), the wide view (S3.4). (The TV camera's
  second `begin_scene` in the same window cleared the first pass's layer: fixed in S3.2, 2.9.)
* Test `--hr-check` (`port/notes/e22/r3d_hr.py`, ctests `r3d_hr_n1`, `r3d_hr_n234`): the 110 golden scenes with the enhancements on stay
  byte for byte; N = 1: layer == page for all 2.51 M checked 3D pixels (99.1 % drawn by the hr back end, the rest stamped); N = 2 / 3 / 4:
  95.7 / 94.3 / 94.0 % of layer pixels agree with their 1× pixel, 99.98 / 99.96 / 99.94 % away from edges (tolerance 90 % and 99.5 %).

### 2.8 As built (S3.1): the objects on top of the scene, and the views
* File `enhanced/hr_obj.c`. Every object keeps its faithful 1x drawing (page, shadow records); a hook before it asks for a hold and a hook after it draws
  the same object from B85D's hr point `enh_hr_pt` (layer px, sub-pixel; `size` = the sprite size recomputed at N x, rule R1):
  | Object (original) | Hook | N x look |
  |---|---|---|
  | smoke billboards (9ACA B, queue DCC0, 1AD6 via DF08) | `enh_bill_size`, `enh_bill_note` (before `r3d_queue_insert`), `enh_bill_pending` (before `r3d_sprite_main`), `sprite()` | the original sprite bitmap scaled to `|0x100 N / d|`-based size with the column / row DDA of `sprite_scaled` (source k = floor(i sw / dw)), centred on the hr point, into the **3D layer** |
  | explosion 1A85 (A17E S3, after the pass) | `enh_expl_pending`, `sprite()` | the same; the 1x writes are recorded as 3D writes (route 3D during the call) and the pixels go into the layer of the last pass |
  | far unit / projectile dots (9ACA D, E: `hud_line_view` of one point) | `enh_dot_begin/end` | an N x N block centred on the hr point (the apparent size of the 1x dot), 3D layer |
  | cannon tracers (A17E S2) | `enh_tracer_p0`, `enh_tracer_begin/end` | thin line (`max(1, N/2)`, Q3) between the hr points, **vector layer** |
  | target boxes / hexagons B3E6, the diamond B339 (A17E S4, `hud_ping_label`) | `enh_box_begin/end`, `enh_diamond_end` | the outline at hr centre +- N x the 1x half sizes, thin, vector layer, clipped to the `hud_line_win` window |
  Vector lines are clipped with a 64-bit port of the 1001 clip core (`clip_core`, same Cohen-Sutherland and rounding block) and drawn with the 1x Bresenham of
  MGRAPHIC slot 31 on layer coordinates, so at N = 1 they are the original pixels.
* Decision: the dots are in the 3D layer, not the vector layer (DESIGN 2.5 said "thin into a vector layer"): they are drawn inside the pass, so later polygons must
  cover them, and a shrunk dot (N/2 px) would make a gameplay marker smaller than the classic one. Tracers and boxes are thin as 2.6 says.
* Hold mechanism (`enh_canvas.c`): `enh_stamp_hold` (the 1x pixels of a sprite / dot are recorded as 3D writes but not stamped into the layer) and `enh_vec_hold`
  (1x line pixels become `SRC_VECHR` records: where no hr pixel lands the compositor shows what was under the line, not the 1x line). `enh_vec_put` is the
  emitters' pixel: it marks the page pixel of its block `SRC_VECHR` too, so an hr line shows where the 1x line did not reach. Compose rule of 2.5 unchanged otherwise.
* `HrCtx.have` stays true after `r3d_end_scene` (the window and layer of the last pass on a tracked page), for A17E, which runs after the pass.
* Views: every mode goes through `r3d_scene_draw` (so one pass, 2.7) and `view_frame`'s keyed blits / strips over the window are page writes that the shadow marks as art
  (rows 91..106 strip, the corners, 256REAR / LEFT / RIGHT, the nose triangle), so the 3D layer shows through exactly the key-0 pixels. Checked on all of them at N = 1
  (the composed frame equals the page for the 13 view scenes) and at N = 2..4 (art blocks unchanged, interior blocks agree).
* The HUD lines were done in S3.3 (2.10). The TV camera sprites were done in S3.2 (2.9).
* Tests: `--hr-check` now also composes the frame (`fpx fdiffer finner finner_differ`, vector footprints `vtot vhit`, object counts) and `--hr-dump` writes `<id>.cmp` (the
  composed canvas) and the vector layer; trace types 7 (sprite + source bitmap), 8 (dot), 9 (clipped vector line) let `r3d_ref.py` judge them with its independent rule.

### 2.9 As built (S3.2): the TV camera window and the map MFD window
* The viewport hook (`r3d_set_viewport`, 2.7) already opens the right N x rectangle for these windows (TV x 169..245, y 110..169 of the page `[4088]`; map x 74..150,
  y 110..169): window = `DS:85CA..85D0`, centre `85D2/85D4`, layer origin `fx + N x0`; the DB71 pixel offset is the frame-pixel window, so nothing to add. Ortho (`[7D5E]`)
  was R1 from S2.2. Measured: the TV and map scenes (35 of the 164 golden scenes) are judged by the reference rasteriser with gap 0, wrong 0, stray 0 at N = 2 and 4.
* Two passes in one window (`mfd_view_render`, close view of an air target: terrain, then the object): `enh_pass_continue` (called before the second `begin_scene`) makes the
  next `enh_r3d_viewport` keep the shadow and layers if the window and page are the same; trace type 0 carries the flag (`keep`) for the reference.
* The 1x backdrop of the sensor window (fill, sky / ground bands: axis-aligned rows, so their N x look is the 1x one) is drawn before the first pass opens: `enh_tv_backdrop`
  (called at the top of the backdrop in `mfd_view_render`) clears the window, sets route 3D and arms the keep flag, so those writes are stamped 3D pixels and the pass'
  polygons are not cut to the 1x footprint (the shadow's "3D" rule shows layer pixels only in blocks whose 1x pixel is a 3D write).
* TV sprites (fire puffs 24h..27h, the smoke column 25h; `r3d_draw_queued` -> `r3d_sprite_tv`): `enh_tv_sprite_pending(e, size)` takes the first end of the last edge's hr
  record (`hr_edge_point`) as the sprite centre and N x the 1x `size`; the S3.1 sprite path draws it into the 3D layer (`enh_sprite_begin` accepts the TV context).
* FLIR tint (`mfd_view_flir_tint`, poly_fill mode 3: `((c & 0Eh) >> 1) | 98h` on the page): the poly_fill hook `enh_rec_remap` runs the same function over the N x N layer block
  of every pixel whose record is a fresh 3D write and keeps the record 3D; other pixels are recorded as art as before. The reference applies the ramp to its expected colours
  (trace type 10 = the tinted rectangle).
* Map MFD: the finished window is saved to the backup page 2 (`mfd_map_save_bg`) and copied back every frame (`mfd_map_restore_bg`) to erase the aircraft marker. The blit is a
  plain page write, so `enh_window_save` / `enh_window_restore` (one slot, not in display passes) keep the window's records and layers beside the backup. The map's route lines,
  symbols and text are art or 1x vector writes (S3.3, 2.10, turns the lines into vector-layer lines).
* Tests: golden scenes `tvclose / tvfire / tvflir / tvsmoke / tvsimple_flir` x 3 sites (two passes, fire puffs with live puff state, smoke column, FLIR, one-pass simple path),
  `mapfp3/6/9` (flight palette), `mapbg3/6/9` (save, a line over the window, restore); all with `pal_flight_load` and `[4088] = 1` (the TV pass page is MGRAPHIC's draw page, as in the game).
  `--hr-check` prints `wpx w3d tpx tramp` (the window still shown from the 3D layer; the tinted window in the green ramp); `r3d_hr.py` asserts them.

* **Real-game fix (S3.2, after the first U1):** in play the TV and map windows were 1x pixels because of how the game moves them: the MFDs are drawn into the draw page (page 1)
  and `view_frame` copies the two 77x60 windows to the other page with `blit_rect` every frame, while the flip (`enh_flip`) copies the shadow only for the rows of the 3D window.
  The shown plane's records for the MFD rows were therefore never refreshed. `enh_blit` (hook in `blit_rect`) now follows a blit between the tracked pages: records and the
  3D / vector layers go along (page 1 work -> page 0 shown and back); a blit from any other page (picture page 2) is art and voids the destination records. Measured in the
  real path (`port/notes/e32/real_path.py`, `--mfd-probe`): TV window shown from the 3D layer 0 % -> 93.7..95.7 %, map window 0 % -> 99.1 %. The golden scenes `tv0..5` ran with
  the pass page != the draw page (not a state of the game: `[4088]` = 1 = draw page = pass page there); they now poke `[4088]` = 1 and are 98..100 % shown.
  Test aids: `--mfd-probe PREFIX` (+ `F117_PROBE_FRAMES`), `F117_VGAME_SELECT` / `F117_VGAME_SITE` (select a site as the target, like the designation of key `n`).

### 2.10 As built (S3.3): the HUD and MFD vector graphics

Every 1x vector line of the cockpit goes through MGRAPHIC `line()` (slot 31): the asm HUD (`hud_draw_front`: frame, reference marks, the pitch ladder's rungs, reticle bars), `7898`
(`hud_overlay_frame`: designator cross, throttle / sink-rate bars, the ILS needles `hud_ils`, rings), the map MFD (`mfd_map_line`, `mfd_map_arc`, `mfd_map_route`), the scope and the TV
crosshair, all through `hud_line_clip` -> `line_clip_far` -> `line`. So one hook pair in `line()` routes them all; nothing in the callers changes (`hr_obj.c`, "the HUD and MFD lines").
* `enh_hud_begin` (before the 1x pixels are written; `line()` after its endpoint swap) decides and holds the footprint (`enh_vec_hold`: the 1x pixels become `SRC_VECHR` records, which show
  what was under them where no hr pixel lands); `enh_hud_end_line` draws the same line thin in the vector layer: `max(1, N/2)` layer pixels (Q3), brush offset `(N - w) / 2`, from the 1x
  endpoints (frame pixels incl. `draw_off`). The brush runs through the **1x Bresenham on layer coordinates** (`vbres`, the S3.1 code), so at N = 1 it is the original line pixel for pixel.
  Horizontal and vertical lines keep the full 1x length (block edge to block edge: ticks and the frame corners join); diagonals run between the block origins plus the brush offset.
* **Not taken** (they stay the original's pixels, N x N): colour 0 (the HUD's bar gauges draw black / dark lines that erase; also keeps the DAC-index rule: the lines are DAC indices, so the
  HUD colour sets F4 / Alt-N / the flashes still work), a single point (`hud_pixel`, the map's 2x2 blips and dots: they keep their solid look; an N x N block is also what the S3.1 dots are),
  lines of a 3D pass (`enh_route` 3D: 3D-layer geometry) and lines inside another emitter's hold (tracers, target boxes, S3.1).
* `hud_tick_strip` (071A, the airspeed / altitude / heading tapes' scale lines and ticks) writes the page directly: its pixels are now vector records (`TICK_PX`), and `enh_hud_tick` draws
  the 2- and 3-pixel ticks as thin horizontal lines and every scale-line pixel as a dash as tall as the 1x pixel (the scale line is dotted in the original, every second row).
* The ADI (`cockpit_draw_adi`, 17A3): its horizon is a filled quadrilateral. `enh_adi_begin/end` hold the `poly_fill` footprint (`enh_rec_vecfill`: the same record as a held line pixel) and fill
  the polygon again at N x (N >= 2): edges through the vertex-pixel centres, a layer pixel in when its centre is inside or on an edge: a straight horizon instead of the staircase. The
  bezel on top (`cockpit_keyblit`) and the copy to the other page stay art.
* **Page 0 and page 1.** The map's lines are drawn on both pages (`hud_line_clip` both), and the step draws the route legs on both pages after the flip. A vector write on page 0 (the displayed
  page) now belongs to the **shown** plane and layer (`plane_shown`), one on page 1 to the work ones; before, both went to the work plane and the second copy overwrote the first one's
  `under` with the other page's stale pixel. (3D writes and the other emitters are unchanged.)
* Test hooks: `--mfd-probe` prints the vector records of the shown page that are shown from the vector layer (`hudv`, `mapv`, `allv` and the HUD elements `tapel`, `taper`, `framel`, `framer`,
  `centre`; colour 0 excluded) next to the TV / map counts; `--hr-check` adds `vthin` (footprint blocks that show less than the line colour: the lines are thin) and `vstray` (blocks with a
  colour that is neither the line, nor what it replaced, nor a colour within two original pixels). Trace types for the reference rasteriser: 9 (a clipped vector line, S3.1), 11 (the ADI
  polygon), 12 / 13 (the map window is saved to / restored from the backup page: the reference keeps and restores its lines).
* What stays original pixels: all text (tape numbers, ladder labels, messages), the reticle and the tick bitmaps blitted from `256PIT.PIC` (`blit_key`, `cockpit_keyblit`: the flight-path
  marker, the aircraft marker, the lamps, the panel), the filled bars and boxes (`fill_rows_far`, `hud_fill_rect`: the threat bar, fuel bar, MFD backgrounds).
* Tests: 12 golden scenes `hud_l0 / l1 / l2 / l0s / lvl` (the HUD at levels 0 / 1 / 2 and the small view, rolled and pitched), `hud_adi0..2`, `hud_ils`, `hud_scope`, `hud_map3 / 6` (route lines,
  both pages, arcs, the backup round trip); golden total 176, original-vs-port byte for byte. N = 1: 0 differing frame pixels in all 176 scenes; N = 2 / 3 / 4: interior blocks agree
  99.98 / 99.94 / 99.92 %, every footprint pixel has an hr pixel within one original pixel (4631 of 4631 at N >= 2), `vstray` 0, the lines are thin (`vthin`); the reference rasteriser judges
  255 lines and polygons of the HUD scenes (52 484 layer pixels at N = 4): gap 0, wrong 0, stray 0. The real game path (`port/notes/e32/real_path.py`, ctest `r3d_hr_realpath`: the scripted
  chain with F4, v, c, F8, F3, Alt-N): 99.4-99.8 % of the HUD's vector records show a vector-layer pixel (tapes, frame and the ladder / reticle centre 100 %), the map window with the route
  legs 82 % (the rest are the blips), the scope 100 %.

### 2.11 Smooth distance shading (S3.5, option `--smooth-shading`, default off)
How the original shows distance (render3d.md Part 1 `1088:0120`, Part 6 B `1442`; FORMATS.md "Flight palette", "3D objects"):
* **Sky / ground bands (1442).** Detail >= 3 draws 16 sky bands in colours `80h + i` and 16 ground bands in `70h + i` (`i` = 0 at the horizon; ramps of 16 DAC entries). Band
  `i` lies between two parallel lines at perpendicular offsets `t_i`, `t_(i+1)` (1x px, vertical scaled by 0.75): a pixel q of the window has
  `t(q) = -(sn (qx - w2) + cs (qy - h2) 4/3)`, sn = -[498A] / 32768, cs = [498C] / 32768 (band_points: centre `(w2 - sn t, h2 - 0.75 cs t)`, direction `(cs, -0.75 sn)`).
  Sky: `t_i = H + DS:0D22[i]` = H + 0, 2, 3, 4, 6, 8, 11, 15, 20, 27, 36, 48, 64, 87, 119, 164, band 15 to +22600 (widths 2, 1, 1, 2, 2, 3, 4, 5, 7, 9, 12, 16, 23, 32, 45 px). Ground:
  `t_i = H - G_i`, `G_0 = 0`, `G_i = alt / ((16 - i) 32) - alt / 512` (wider with altitude; at low altitude bands 0..14 collapse onto the horizon and the ground is 7Fh), band 15 to
  -22600. H = the horizon offset `sin(pitch) 256 / cos(pitch)` (zoom `[433A]`, halved by `[294B]`). Each band is one colour: a step of one ramp entry per band. Detail < 3
  (`hz_draw`) and the mono TV picture (88h / 7Ch) are flat: nothing to smooth.
* **Faces on the ground (AND / OR fills, FEh / FDh).** Roads, fields and water are AND 7Fh / AND 6Fh / OR 80h polygons over the backdrop: they keep the band's low nibble and move
  it to the ramps 70h, 60h, F0h, so they show the same distance bands in their own ramp.
* **Depth shade of objects and terrain (1088:0120).** Every placed object (the terrain tiles and scenery of 04DA, sites, aircraft, the queued objects of DDFE / DF08) gets one shade
  `s = clamp((C >> 25) - class, 0, 31) >> 1` (0..15; C = the 32-bit view depth [4960] of its origin, class = [497A] 0..7; 0 in the map (ortho) or with Alt-A off [43B6]): a step
  every 2^26 of depth. `set_colour_M` (1359:01D4) draws a shaded colour (hi FFh, lo >= A0h) as `max(lo - s, lo & F0h)`: the ramps A0h..FFh run from the far (haze) entry at the
  ramp base up to the near colour. The shade is per object, not per pixel; the steps show as an object (or a ring of terrain tiles) jumps one ramp entry when its depth crosses a step.

Smooth (as built):
* **Encoding.** The 3D layer word keeps the original index in bits 0..7 (bit 8 written, bit 9 stamped, as before) and a blend code in bits 10..15: 0 = none, else
  `d = code - 32` (-31..31) = the continuous colour position minus the index, in 1/64 of a ramp entry. Every writer that does not know about it (sprites, stamps, solid
  faces, lines, the FLIR tint) leaves 0, so the option-off layer and the index of every pixel are unchanged; only shaded colours gain a code.
* **Bands.** After the hr bands of 1442 (`hr_sky_ground`) every layer pixel of the pass window showing `70h + i` / `80h + i` gets `f = (t(q) - t_i) / (t_(i+1) - t_i)`
  (band 15 uses the width of band 14), clamped to 0..1, and `d = f - 1/2` (none below the centre of band 0 or above the centre of band 15): the band colours sit at the band
  centres and the colour runs linearly between neighbouring centres. The band index stays the fill's, so a pixel that the edge rounding put in the neighbour band clamps to the
  shared boundary value (no reversal). AND / OR fills keep the code when the result is >= 60h (the ground ramps), any other write clears it.
* **Objects.** At the shade (E7AC, DDFE record, DF08) the hr back end keeps the continuous shade `sc = clamp(C / 2^25 - class, 0, 32) / 2` in 1/64 (the queue per record
  slot). A shaded fill or line (hi FFh, lo >= A0h; lo from a hook in `set_colour_M`) gets `d = max(lo - (sc - 1/2), base) - index` (centred like the bands): an object fades
  through the ramp as its depth changes instead of jumping; it stays one colour per face (the original shade is per object).
* **Compositor.** A 3D layer pixel with a code shows `mix(DAC[idx], DAC[idx +- 1], |d| / 64)` from the live DAC (the neighbour in the same 16-entry ramp; none across a ramp
  edge), so fades, flashes, the night palettes and the FLIR (which clears the code) still apply. Dither / stipple faces: the colours they write are unshaded, the pixels they leave
  keep the backdrop's code. HUD, MFD, cockpit art and sprites are other layers / page pixels: untouched.
* Option off, `--classic`: no code is ever written; the compositor then shows exactly the indices (the golden / hr-check tests). The sim and the 1x page are not touched (hooks
  only store hr state). Tests: `port/notes/e35/` (section 2.11 numbers in PLAN.md S3.5 status).

### 2.12 True night (S3.6, option `--true-night`, default off)
How the original does night (simflow.md 0000:00F0, 4857, 4d43, cba0; FORMATS.md "Flight palette"; render3d.md 04DA, 1088:0120):
* **Only a palette.** START sets comm[33h] (night: 1 if a target type is 4, else random, 70 % / 90 %) and the clock base (18:00 / 06:00); VGAME copies it to
  `[3658]` and `[43B0] = !night`. `pal_flight_load` (1088:0006) loads palette **B** (DS:194A) at night and **C** (DS:1C4A) by day (with `[43AE]` = 1 after the
  debug key Alt-N, which also flips `[3658]`: C at night, A (DS:164A, bright) by day). `sim_reset_clock_palette` pre-steps the DAC `2 h` units toward the target
  (night C, day A; h = the start clock index 0..15) and `pal_time_of_day_step` moves one range (10h-4Fh, 60h-9Fh, A0h-CFh, D0h-FFh) one unit toward it every
  16 s (each range one unit per 64 s): **dusk** (B -> C, darker) at night, **dawn** (C -> A, brighter) by day. Nothing else in the renderer reads `[3658]` except
  the visual-ID range (8 units by day, 4 at night) and the 7898 bar colour; the light faces of the models (condition var 10 / 11 = `[C042]` / `[C044]` = `[9874]`)
  are drawn by day and night alike (`DS:11E2` has no writer, so the lights value is always 1).
* **Night palette B** (6-bit): sky ramp 80h + i from (31,16,30) at the horizon to (8,11,17) at the top; ground ramp 70h + i from (0,0,0) at the horizon band to
  (0,20,0) near; the shaded ramps A0h..FFh start at black (the far / haze entry) and reach only (14,20,0), (0,22,20), (26,20,0), (26,0,0), (12,12,12), (14,20,29);
  60h..6Fh (AND 6Fh faces) blue. C is darker still (ground at most (0,12,0)). **Unchanged at night**: 00h..0Fh (the EGA colours), 50h..5Fh (the fire / explosion
  ramp), 92h, 93h (63,0,0), 94h, 95h (63,63,46), 97h (0,63,0); 96h is grey in A and yellow (50,44,0) in B - the light colours. Of the model faces, 8991 of the 9387
  light elements (var 10 / 11) are lines in 94h, the rest E6h, 92h, 07h, 13h, CBh, D9h; the other unshaded faces use 92h (lines), 0Dh, 90h..9Fh, 06h, 03h, 0Fh.
* So distance at night is the same as by day: the ground bands (band i = ground ring (16 - i) 8192 units: `G_i + alt/512 = alt / (32 (16 - i))` at focal 256) and
  the object shade (a ramp step per 1024 depth units minus 512 per size class) fade to the black ramp bases; the far ground at the horizon is already black.

True night (as built; `src/enhanced/hr_night.c`):
* **Gate.** The option is on, not `--classic`, the pass is the main 3D window (x0 = 0, y0 = 0, x1 = 319: cockpit, full screen, outside views, the S3.4 wide
  pass) and `[3658]` != 0 and not the map (`[7D5E]`), when the pass opens. The TV / FLIR camera, the map MFD, HUD, MFDs, cockpit art, lamps and menus never get it
  (other passes, the vector layer or page pixels). Day missions and Alt-N to day: nothing changes.
* **The night plane.** A third canvas plane `u8[Lw*Lh]` beside the 3D layer (work / shown / wide picture / window store / display-pass snapshot): 0 = no night
  pass (as the original), 1..254 = the brightness factor `k = nd / 256`, 255 = a light (as the original). It is written only by the hr back end in a night pass
  and cleared with the layer. The layer word is untouched, so with the option off, `--classic` or by day the picture is the same bytes as before.
* **Continuous colours (fix 1).** A night pass turns on the S3.5 blend codes of the bands and the shaded ramps whatever `--smooth-shading` says: darkening the
  stepped band colours kept the band steps visible (U1). The factor itself is a continuous function of the pixel's distance and never steps per band.
* **Backdrop** (after 1442 at every detail level, every written pixel of the pass window, not in the S3.4 hole): the pass geometry from the band geometry of 2.11,
  `t(q)` and the horizon `H` (1x t-units, focal `f = 256 << [433A]`, halved with `[294B]`). Above the horizon line (or a sky colour 80h..8Fh): the elevation
  `e = (t - H) / f`, `k_sky = SKY_TOP + (SKY_HORIZON - SKY_TOP) exp(-e / SKY_E)`. Below (or a ground colour 70h..7Fh): the ground distance
  `D = 1 / (G / (f alt) + 1 / 131072)`, `G = H - t` (the band formula solved for a continuous ring, so D = (16 - i) 8192 at the band edges and 131072 on the horizon
  line; works at low altitude where the bands collapse), `k = curve(D)`. AND / OR ground faces (roads, fields, water: FEh / FDh) keep the factor of the pixel
  they recolour.
* **Objects** (the shade hooks of 2.11 E7AC / DDFE / DF08): per pixel `D = min(D_ground(q), C / 65536 + r_class)` (r_class = the cull x radius DS:48D6, so the
  object's far side; above the horizon line the far side). Fix 1: a single depth per object made each terrain tile, road and sea face one flat factor that
  differed from its neighbours and from the backdrop (the seams U1 saw on the sea); now a face lying on the ground gets exactly the ground's per-pixel factor and
  adjacent faces join, while a standing object is never darker than its far side.
* **Lights, by source** (fix 1; before, a palette rule exempted colours 00h..0Fh, 50h..5Fh, 90h..97h wherever they were used): the faces and lines under a
  lights condition (var 10 / 11; hooks in `r3d_face_list`), the model lines (40h elements) in the light colours 90h..97h, and everything the overlay pass after
  the scene draws (A17E: explosions, tracers, flares, target boxes) get 255. Everything else the scene pass leaves without a factor (smoke and the other sprites,
  the far dots, 1x stamps, 3D records shown as their 1x pixel) gets the factor of its place (sky / ground) at the end of the pass (`hr_night_pass_end`); before
  fix 1 those stayed at their original brightness (the bright dots and dotted lines U1 saw in the black sky).
* **Fix 2 (dots at the frame's right edge, U1 again).** The dots were 1x-pixel blocks of the undarkened sky colour: 3D-world pixels that reach the screen
  without the 3D layer's factor. Three such paths are closed: (a) the 3D pixel under a vector (HUD) record whose hr line left that block empty is darkened
  like the layer (before: the 1x value under it, as is); (b) 3D-layer pixels of the night window written after its pass ended (1x stamps of later writers)
  get their place's factor at the next pass and at the flip (`hr_night_late`); (c) a sky / ground band colour (70h..8Fh, drawn only by the 3D backdrop) shown
  as its 1x page pixel inside a night window (a lost record) gets that window's factor. Only the band colours are covered by (c), so cockpit art and HUD text
  over the window stay unchanged. The headless runs (banked level flight over the sea, `F117_TEST_POSE`, at 1167 x 821 auto N, 1459 x 1026 N = 4 and
  1920 x 1080) show none of these pixels, so which path the user's run took is not confirmed; the test counts all three per frame and at the frame edges.
* **curve(D)** = `K_FAR + (K_NEAR - K_FAR) exp(-D / D_LIT)`, monotonic: near ground and objects keep about half their night brightness, beyond a few D_LIT they
  are near black, sooner than the original's ramp. **Tuned constants** (hr_night.c): `K_NEAR = 0.50`, `K_FAR = 0.06`, `D_LIT = 3000` depth units (k = 0.37 at
  1000, 0.22 at 3000, 0.12 at 6000, 0.075 at 10000; the object range at detail 3 is 13332), `SKY_HORIZON = 0.30`, `SKY_TOP = 0.10`, `SKY_E = 0.15` (k = 0.17 at
  e = 0.25, i.e. 14 deg up). Night B: the near ground (0,80,0) RGB -> (0,40,0) at the aircraft, (0,18,0) at 3000; the horizon sky (124,64,120) -> (37,19,36),
  the top (32,44,68) -> (3,4,7).
* **Compositor.** A 3D pixel (layer or 1x) with `nd` 1..254 shows `(rgb * nd + 128) >> 8` of its colour from the live DAC (after the S3.5 mix), so palette fades, the
  dusk drift, the explosion flashes and Alt-N still apply; the compositor's index-only paths (the hr check `.cmp`) ignore it.
* Rendering only: the hooks store hr state; the sim, the 1x page and the DAC are untouched. Option `--true-night` / `--no-true-night` (and F117_ENH_ARGS), launcher
  switch S5.1. Tests: `port/notes/e36/` (numbers in PLAN.md S3.6 status).

## 3 (b) The wider view

### 3.1 Canvas geometry (window of any aspect)
Output `Wo x Ho`, pixel aspect `a` (1.2 for the 4:3 option, 1 for square pixels; the host stretches the canvas by `a`).
`Lw = Wo`, `Lh = floor(Ho / a)`; `N = floor(min(Lw/320, Lh/200))` (U3-1; or the fixed `--render-scale`), at least 1.
Frame: `fx = (Lw - 320N) / 2`, `fy = Lh - 200N` (bottom centre). Frame coordinate u (continuous) → layer `fx + N*u`.
Extensions in original pixels: `El = fx/N`, `Er = (Lw - fx - 320N)/N`, `Et = fy/N`. With `--wide off` the canvas is the frame
(`Lw = 320N`, `Lh = 200N`, host letterbox as today).

### 3.2 Per view
| View | hr 3D viewport (layer px) | Projection centre | Extended area shows |
|---|---|---|---|
| 0 front cockpit | x 0..Lw-1, y 0..Lh-1 (frame panel rows covered by art via compose) | `fx+159N, fy+52N` (60 with `[294B]`) | the world left, right, above and beside the panel (Q4) |
| 0x44, 0x84..0x8C (full-screen 3D, outside) | whole canvas | `fx+159N, fy+100N` | the world |
| 0x41..0x43 rear/left/right | frame only | frame-mapped | not extended: the whole 320x200 picture (3D at N×, art NxN) **filling the window height** (scale = window height / 200 with the pixel aspect, non-integer: the canvas is the frame at N = ceil(height / 200) and the host scales it), black only at the sides (Decisions 2026-10-21 b) |
| second cockpit view (window frame up top) = **view 0 with `[294B]` = 1** (identified in S3.4, below) | canvas | frame-mapped | **filling the window height** (as 0x41..0x43) and **extended left and right only**: the canvas is 200N high and as wide as the window's aspect, the frame top centre, the world beside it from the wide pass; nothing above (no art for the frame above) (Decisions 2026-10-21 b) |
| TV, map MFD | their frame rect at N× | frame-mapped | not extended (panel) |
| SETUP/shell/START/END/debrief, VGAME menus | — | — | 320x200 (or 640x400 text) scaled by the largest whole N, centred, black bars |

**The second cockpit view (S3.4).** VGAME has two cockpit views of `[C07A]` = 0, switched with the key `c` (`1e9e`: `[294B] = ([294B] == 0)`, simflow.md 2.1;
reset to 0 at mission start by `39c4`): `[294B]` = 0 is the first (wide HUD, instrument panel at the bottom, the 3D window rows 0..90 with only the HUD over it),
`[294B]` = 1 the second, the "small view": half-scale projection (B85D and 1442 halve X / Y, `[85D4]` = 60), the compact HUD, and `hud_side_strips` (d9a3, simflow.md
section 4, port `sim_hud.c`) `cockpit_keyblit`s two 54 x 91 frames from the top of 256PIT.PIC (picture x 141 and 196, rows 0..90) onto screen x 33..86 and 233..286,
rows 0..90: the window-frame struts, which run up to row 0 of the frame and have no art above it. No other view mode has a second cockpit variant (`[2DFC]`, Alt-C,
only makes the window 107 rows tall with keyed panel corners at the bottom; 0x41..0x43 are the looking-out views). Measured in the real path
(`port/notes/e34/wide_realpath.py`, 16:9): the second cockpit view shows 32.5 % art in the canvas against 28.6 % in the first (the struts); it is placed FIT.

### 3.3 Field of view, culling and clip edges (display passes only; canonical pass stays faithful)
The scale stays 256N px per unit tangent in x and 192N in y (128N/96N with `[294B]`), so angular size is unchanged and the
half-angles become `atan((159+El)/256)` left, `atan((160+Er)/256)` right, `atan((52+Et)/192)` up (front cockpit).
Let `kx = (160 + max(El,Er)) / 160`, `ky = (up0 + max(Et, down extension)) / up0` (Q8, ≥ 1), up0 from `[0D20]`.
| Test | Widening |
|---|---|
| E8B5 `r3d_cull_core` x/y radius (tables 48D6..4946) | `sx *= kx`, `by *= ky` (int32, on the wrapped 1× values) |
| 11EB:02B6 `r3d_frustum_test` (45° pyramid) | per axis, `t *= max(1, T)` with T = tangent of the extended half-angle (x: `0.625·kx`, y: `(up0·ky)/192`) |
| B85D `r3d_project_point` (90° x-field, rows 0..0x60/0xC7, columns 0..319) | x-field `|X| <= -Z·max(1, 0.625·kx)`, accept `sx` in `[-El, 319+Er]`, `sy` in `[-Et, lim]`; the `0xFFFF` sentinel: an extended `sx == -1` becomes -2; applies only while the route is "3D" (9ACA blocks D/E/F/B, A17E S2/S3), not to HUD symbology |
| 04DA terrain cells (octant tables `DS:034C`, 9 or 15 cells) | when `kx > 1` or `ky > 1`: an Enhanced table of the forward half of the 5x5 block per octant, far → near, camera cell last (generated, `enhanced/wide_cells.h`); the down-look table `04CC/04DE` becomes the full 5x5 |
| far limit `[48B6]` | unchanged (view distance parked) |
The hr clip window is the hr viewport; the 1× page still clips to the original window, so the frame content equals the
original's except where newly drawn side cells/objects overlap into it (more correct terrain at the frame edge).

### 3.4 HUD anchor
The HUD, MFDs, panel and messages stay in frame coordinates (1× drawing), so they sit with the cockpit at bottom centre.
The HUD symbology lines up with the world because the projection centre and scale inside the frame are the original ones.

### 3.5 As built (S3.4): choices where 3.1-3.4 were silent
* **How the extension stays out of the sim.** The canonical frame runs exactly as before (its main pass is not widened). When the view is extended and the canvas is
  larger than the frame (`enh_wide_due`, decided at `enh_frame_begin` from `[C07A]` / `[294B]` and the canvas), A_k is taken and `enh_idle` runs **one wide display
  pass** (`enh_display_pass(0)` with `enh_wide_pass`): the same frame from A_k, with the main 3D pass widened (`enhanced/wide.c`). After its flip the pass's 3D layer
  (whole canvas) and vector layer are copied out (`enh_wide_capture`, pending) and the live state is restored as in S2.4; the canonical flip makes the pending
  picture current (`held_promote`). The compositor (`compose_src_live`) takes the **3D layer of the wide picture inside the frame's 3D window and everywhere
  outside the frame**, its vector layer outside the frame (tracers), and everything else (page, shadow records, the HUD's vector layer, the MFD windows below the 3D
  window) from the live frame, so HUD, panel and MFDs are the canonical frame's. The wide picture is used only while its geometry equals the live canvas and no menu
  is up. A frame that changes the view (`[2E48]` != `[C07A]`: `view_frame` would read the cockpit picture, a file access refused in a pass) gets no pass: that one
  frame shows the canonical picture (black outside). This is 4.1's "`--interp off`: one display pass with alpha = 1 per step"; S4.1 runs it at every refresh.
* **The main pass** is the pass whose window starts at x 0, y 0 and ends at x 319 (view_frame's 3D window; the TV and map windows are smaller). Wide: the hr clip and
  1001 windows are the canvas (`HrCtx.wide`; `wx, wy` keep the original window's layer origin for the R1 "fits s16" tests and the sprite placement; `e2dx, e2dy` move
  the 1001 window-relative points of the bands / horizon into the canvas window; the horizon line of hz_draw is at least the canvas half diagonal long; the slot-37
  single-pixel quirk is applied at the canvas edge). The frame below the 3D window (the panel and its MFD windows) is a **hole** that is never written (`HR_IN_HOLE` in
  `hr_put` / `put3d`, trace type 14), so the TV / map windows keep their layers. The layers outside the frame are cleared when the wide pass opens.
* **Widening factors** (`enh_wide_begin`, from the canvas and the projection centre, in 1x px): mx = the larger extension left / right of the centre, my = up / down.
  E8B5: `kx = max(1, mx / 160)`, `ky = max(1, my / [0D20])` (Q8, on the wrapped s16 tolerances, both branches of `[4360]`); 02B6: the bounds times
  `max(1, mx / 256)` and `max(1, my / 192)` (the 45-degree pyramid only grows past tangent 1); B85D (`enh_wide_pt`: the main pass, and A17E S2 tracers / S3 explosion
  right after it, `enh_wide_ovl`; never the S1 / S4 boxes, the HUD symbology): `|X| <= -Z max(1, mx / 256)`, sx in `[-ceil(fx / N), 319 + ceil(right / N)]`,
  sy in `[-ceil(fy / N), (lh - fy) / N - 1]` (the front view's objects beside the panel count), `sx = -1 -> -2`. The 1x sprites / lines of an out-of-frame point
  are clipped by their own windows (`sprite_scaled`, `hud_line_view`), so only the hr emitters draw them; tracers in a wide pass are clipped to the canvas.
* **Terrain cells** (04DA, `enh_wide_cell`): built at run time from the image's own tables (`DS:034C`, `04CC/04DE`), so no generated header: per octant the original
  cells in their order (path B 9, path A the 15 with the quirky overread tail) plus the cells of the 5 x 5 block with `dot(cell, octant direction) >= 0` they do not
  visit, each put before the first original cell that is not farther (far -> near, outer first; the camera cell and path A's tail stay last). New far cells (dot >=
  that of the original's first four) count as "i < 4" for the low-detail skip. The down-look table becomes the full 5 x 5 (the 16 ring cells first) when the pitch
  is below 60 degrees down **or** the lower edge of the extended view looks past the vertical (`pitch < -(90 deg - atan(down / 192))`).
* **Placement.** `enh_view_class(C07A, [294B])`: view 0 with `[294B]` = 0 FRONT, with `[294B]` = 1 SIDE (the second cockpit view), 0x41..0x43 FIT, the rest FULL.
  **S3.4 fix (Decisions 2026-10-21 b):** FIT and SIDE fill the window height. Both render at N = ceil(Lh / 200) (or the fixed `--render-scale`) with a canvas 200N high:
  FIT's canvas is the frame (320N x 200N), SIDE's is as wide as the window's aspect (`Lw 200N / Lh`), the frame at the top centre, the 3D viewport the whole canvas
  (extended left and right only; the wide pass runs for it with the half-scale tangents of `[294B]`: the frustum's mx / 128, my / 96; B85D's halved X keeps mx / 256).
  The host's letterbox presentation scales the canvas to the window height, so the shown scale is height / 200 (non-integer; black only at the sides of FIT).
  `--render-scale N` is kept also with wide when it is larger than the window holds (S2.1 lowered it to the fit): the canvas then holds at least the frame and the
  host scales it down, as without wide (so the headless tests keep their N). VGAME's own screens (the quit requester Alt-Q, the boss screen Alt-B) set the menu
  placement while they are up (`enh_menu`, hooks in `sim_input.c`): pillarboxed, no extended world. Pause (Alt-P) keeps the frozen extended frame.
* **The red lines beside the tapes (S3.4 fix 2)** are the original night bars of 7898 (colour 0x2A: the stall-margin bar right of the airspeed tape, x 72, and the
  VSI bar, x 247; colour 0 by day). They are hr vector lines (thin, Q3) of the same rows as the 1x bars: measured in a night take-off (`F117_TEST_NIGHT`, probe `bars`),
  0 rows differ. A 1x line over an hr line clears that vector block (`enh_rec_line`), every other 1x write makes the record art, so no hr line stays where the 1x page
  no longer shows it (probe `vstale`).
* **No resize under an open pass (S3.4 fix 3).** The canvas geometry (and so the layer buffers) changes only outside a 3D pass and outside a display pass:
  `enh_canvas_update` defers otherwise (the real-time `[262A]` wait of `r3d_begin_scene` presents, and a view change such as key c resizes the canvas); a
  reallocation voids the hr pass context.
* **View switches without an extra black frame (S3.4 fix 4).** The placement of the view a frame draws is applied at `enh_frame_begin` from `[C07A]` / `[294B]`
  (`enh_canvas_frame_view`), so the canonical frame and its wide pass are drawn for it; until that frame is flipped the presenter keeps the last picture
  (`enh_present_hold`: no compose, no host resize). The wide pass also runs on a frame that loads a cockpit picture: it sets `[2E48]` = `[C07A]` in its image (no file
  read; only its 3D layer is used). What is left is the original's own `screen_off` blank (28-43 ms on most switches, as in `--classic`).
* **`--wide` default: on** (S2.1's note "the default turns on with S3.4"; CLAUDE.md "enhanced defaults are on"); `--no-wide` and `--classic` turn it off; the launcher
  switch is S5.1's. With a 4:3 window (4:3 pixel aspect) the canvas is the frame and nothing changes.
* **HUD anchor (3.4)** needed no code: the HUD, panel and MFDs are drawn in frame coordinates by the canonical frame and composed from it; the projection centre and
  scale inside the frame are the original ones, so the HUD lines up with the extended world.
* **Test aids**: `--wide-test WxH` (the render test draws each scene's main pass wide on a W x H canvas placed for the scene's view; `enh_canvas_test_window` keeps
  the host's window from moving the canvas mid-scene), `--wide-geometry-test`, `F117_WINDOW=WxH` (the window a headless run pretends to have); `--mfd-probe` prints
  per flip the presented canvas by class (world / art / vector / black, outside the frame, seams).
* **Measured** (2026-10-21): sandbox hash with `--wide` (canvas 1280 x 600): 212 VGAME frames identical to the plain run in everything but the canvas, 211 wide passes,
  0 forbidden calls. Real path (16:9 canvas 1920 x 900 and 21:9 2560 x 900, N = 4): outside the frame 100 % world in the first cockpit view and all outside views,
  100 % black in the second cockpit view and left / right / rear; seams at the border 0-0.17 % of world pixel pairs (inside the frame 0.08-2.95 %); 331 wide passes in
  461 frames (the FIT views have none), 0 forbidden calls; a pass costs ~17 ms at 1920 x 900. Wide golden scenes (N = 2, 960 x 480 and 1280 x 480, 115 scenes with a
  wide pass): reference gap 0 / 0.0001 %, wrong 0.003 %, stray 0; unwritten extended pixels 0 / 49 (a sky-band edge sliver at the far right of the 21:9 canvas in two
  steep-roll / high scenes); nothing in the hole; inside the frame 99.6-99.7 % equal to the frame-only render; seams 0.96-0.99 % against 1.24-1.30 % inside.

## 4 (c) Frame rate

### 4.1 Timeline
```
frame k (faithful, unchanged order):
  enh_frame_begin()            snapshot A_k = pre-render image; fields F_k = read from A_k (F_{k-1} kept)
  view_frame ... cockpit_draw_adi        canonical render of S_k (1x page + passive hr emission)
  frame_floor()                waits until frame_ticks since the last flip; its wait loop calls
                               enh_display_frame(alpha) whenever a refresh is due (vsync / --fps)
  flip; [262A]=1; sim_input_flight; sim_frame_step  -> S_{k+1}
```
`alpha = clamp((now - t_last_flip) / (frame_ticks / 70.09 Hz), 0, 1)`; a display pass at alpha shows
`lerp(F_{k-1}, F_k)` on image A_k, so alpha = 1 equals the canonical frame k, which is flipped next. The displayed camera
is never behind the faithful display (which still shows S_{k-1} during that wait). `--interp off`: one display pass with
alpha = 1 per step (needed only for the wider view); N× without wide or interp needs no display pass at all.

### 4.2 The display pass (`enhanced/sandbox.c`)
1. Save live: `mem[]` (1.06 MB), VGA model (vram 256 KB, DAC, CRTC, attr, sequencer), `mgraphic.c` statics
   (`draw_seg`, `draw_off`, `colour`, `rng`, `shake`, `flip_last`, page table), `r3d_object.c bsp_out`, `pic.c rle_out`,
   hr layers + shadow (only the rects that are not fully redrawn: MFDs, TV).
2. Load A_k; write the interpolated fields (4.3); rebuild derived state the step would have rebuilt (`2D94` from the
   lerped angles via `sim_matrix_from_angles`).
3. `enh.sandbox = 1`: `sim_snd_cmd`/`sim_snd_noise` do nothing, `frame_delay` and the `[262A]` wait return at once,
   `host_pump` is never called, `mission_end` and DOS writes assert in debug builds.
4. Run `view_frame`, `hud_overlay_frame`, (view 0) `hud_draw_far`, `hud_side_strips`, `cockpit_draw_adi`, `flip`,
   with the wide culling (3.3) on; compose (2.5) with the **live** DAC/CRTC into the host frame.
5. Restore live; `enh.sandbox = 0`; host presents. A debug ctest hashes everything in step 1 before and after (must match).
Cost: about 2.6 MB of copies per display frame (well under 1 ms) plus one render.

### 4.3 Interpolated fields (DGROUP offsets; SF 2.1, R3D Part 2 "Structures seen", FL 1.1)
| Field | Offsets | Lerp |
|---|---|---|
| own x, y (world, s32) | BA10:BA12, C02A:C02C | linear |
| own level units | C0A4, C0B2 | linear |
| altitude | 2DF4 | linear |
| heading, pitch, roll | 2DEE, 2DF0, 2DF2 | shortest 16-bit arc |
| eject camera altitude (view 0x8C) | DECA | linear |
| spinners (radar dishes, var 15/16) | C04C, C04E | modular (+0x1B00 / +0x1800 per step) |
| falling wreck | C0A6, C0B8, C602 | linear while C602 > 0 in both |
| units, `[DED0]` x 36 B at C140 | +0 x, +2 y, +4 z, +6 x32, +A y32, +E hdg, +10 pitch, +12 roll | if active (+16 bit 1) in both and type (+14) equal |
| projectiles, 12 x 28 B at 3C0C | +0 x, +2 y, +4 z, +8/+A/+C angles | if active (+E) in both, type (+10) equal |
| cannon tracers, (`[B16C]`+4) x 12 B at E44E | +0 x, +2 y, +4 z | if x ≠ 0 in both |
| smoke puffs, 16 x 8 B at 39DA | +4 z | if x (+0) equal and ≠ 0 |
| delayed view 0x84 | pose ring B056 | hook: lerp between the slot read and the next newer slot |
No lerp across a jump (position delta > 2 cells, angle delta > 0x4000, a view-mode change, a slot change): use F_k.
Camera variables built by `view_frame` (E44C, C07C, B770, B288, B752, B75C, B156, DE98…) are recomputed from these.
Everything else (sim state, explosion frame `[9D0C]`, fire puffs, messages, damage) is the discrete A_k value.

### 4.4 HUD/MFDs, effects between steps
The HUD is re-drawn in every display pass from the lerped attitude/position: ladder, horizon, heading tape, ADI, target
boxes and the velocity vector move smoothly (in 1× steps, 2.6); digits show values computed from the lerped fields or A_k.
MFD pages redraw only when the sim changes them, so they update at the step rate, as today (S4.6: except the map MFD, which scrolls under the aircraft marker, 4.11). Rotating parts, missiles,
tracers, smoke and the wreck follow 4.3; explosion and puff animation frames stay per step (they are art frames).

### 4.5 Input and sound
SDL events are taken in `host_pump` (already called in the `frame_floor` wait) and queued into the keyboard / port 201h
models as today; the sim reads them in `sim_input_flight` once per step, so the flight model is unchanged (S4.3 audits the
gamepad path and adds no averaging). Ticks and the ISR body (`vgame_tick`: sound poll, 7th-tick skip, `[3660]` estimator
inputs) run only from `host_pump` in real time, never in a display pass: sound timing is unchanged.

### 4.6 Must not change (sim bit-identical at the same step rate)
* The canonical frame's code path and order (view_frame … step), including `frame_delay([43B8])`, the `[262A]` wait,
  faithful viewport/culling/terrain cells, and its sound commands.
* Tick delivery: one `vgame_tick` per retrace from `host_pump`; `frame_floor`'s pacing (`last`, `frame_ticks`).
* No display pass leaves any byte changed in `mem[]`, VGA model or engine statics; the hr back end never writes them.
* Input bytes reach the keyboard model in the same order; the RNG `926E` is touched only by canonical code.

### 4.7 As built (S2.4): choices where 4.2 was silent
* **Snapshot set** (`enhanced/sandbox.c`, a region table rebuilt at every frame start): `mem[]` whole (1 114 112 B: every DGROUP,
  the comm block, the pages in the DOS arena, the BIOS data area), `cur_dgroup`, `dg_sp` (the emulated stack of the render's
  locals), `timer_ticks`; the VGA model (vram 256 KB, DAC, CRTC/sequencer/GC/attribute registers and indices, the DAC port state,
  the `dirty` flag; the composition cache is output and not in it); `mgraphic.c` (page table, draw seg/off, flip state, shake,
  colour, last x/y, `rng`, the sprite-pool and encoder words, and the driver image itself, whose data segment is the original's);
  `pic.c` (decoder state, `rle_out`); `r3d_object.c bsp_out`, `r3d_bisect_last_bp`; the hr back end's fixed statics (`enh_hr`,
  `enh_hr_pt`, the clipper's vertices/edges/assembler state, the 1001 dirty rows, `hr_miny`). The hr row tables are scratch rebuilt
  from the faithful R3dWork tables when a pass opens and are not in it. Each module lists its own statics in an `ENH:`
  `*_sandbox_regions(add)` function (vga.c, mgraphic.c, pic.c, r3d_object.c, hr_pass.c); `sandbox.c` copies the regions into one
  buffer with one memcpy each. Image 1 418 421 B. No sound or flight static is in it: a pass never reaches them (below).
* **Canvas** (`enh_canvas.c`, `EnhCanvasSnap`): geometry, view, route, shadow plane (2 x 256 000 B) and the four layers (4 x lw x lh x 2 B;
  5 120 000 B in all at the headless 960 x 600, N = 3). A_k takes a deep copy once per frame; a pass copies A_k's canvas into a
  scratch set and **swaps** pointers with the live canvas, and swaps back afterwards (no copy back). The shadow planes became
  pointers for this (`sh_buf`).
* **Order**: `enh_frame_begin` (A_k) → canonical render → `frame_floor`: `enh_idle()` first, then once per wait iteration (S4.1
  will run one pass per refresh there; S2.4's `--sandbox-test N` runs its N passes at the first call) → flip → step.
  A pass: save live, canvas swap-in, load A_k, `enh_sbx = 1`, `view_frame`, `hud_overlay_frame`, (view 0) `hud_draw_far`,
  `hud_side_strips`, `cockpit_draw_adi`, `flip`; `enh_sbx = 0`, load live, canvas swap-out. Nothing is presented in S2.4 (S4.1
  composes the pass into the host frame), so a pass is invisible.
* **Inside a pass** (`enh_sbx`): `sim_snd_cmd` returns 0, `sim_snd_noise` returns; `frame_delay`, the `[262A]` wait,
  `vga_wait_vretrace_*` and `host_wait_until_ns` return at once. Refused and counted (`enh_sandbox_deny`, reported as violations):
  `host_pump` (so no tick, ISR body, sound poll, event or present), `host_opl_write`, `sim_snd_poll`, and every DOS/stdio file
  entry (`dos_open/creat/read/write`, `rtl_open/write`, `fopen_fn`, `unlink_fn`, `platform_load_file_paras`). Measured: 0 in the
  test chain. The comm block is in `mem[]` and comes back with it.
* **Test view**: `--sandbox-test N` renders pass v (1..N) of a frame with the heading + v x 0x400, own x/y + v x 96 / 64 world units
  and 2D94 rebuilt from the angles (`sim_matrix_from_angles`, as 4.2 step 2), so its pictures, designators and tracer hits really
  differ from the canonical frame's; a pass that rendered the same frame would hide leaks that the canonical frame overwrites.
* **Cost** (measured 2026-10-20, headless chain, canvas 960 x 600): A_k 0.4–0.8 ms per frame; per pass 0.55–0.95 ms snapshot (live save
  1.4 MB, canvas copy 5.1 MB, A_k load 1.4 MB) and 0.10–0.17 ms restore, plus the render (~1.9 ms). 60 passes a second cost
  about 40–70 ms of copies per second; the canvas copy dominates and shrinks to the not-redrawn rects if needed (4.2 step 1).
* **Determinism for the test**: `F117_VCLOCK=1` makes the host clock virtual (`host.c clk_ns`: +1 us per read, a pump without a
  tick due jumps to the next tick, no sleep; a pass reads it without advancing it), so two runs of the whole chain are the same
  machine run tick for tick (seeds from the BIOS clock included). `--sandbox-hash FILE` writes per VGAME frame (at
  `enh_frame_begin`, i.e. the state the previous step left) FNV-1a hashes of DGROUP, the comm block, the LCG `926E`, the sound
  driver's memory, the OPL write stream, all of `mem[]`, vram + DAC and the canvas, and a final line at exit. No field is excluded.

### 4.8 As built (S4.1): the display stream
* **Option** `--fps vsync|60|30|off` (`enh.fps`, default vsync; `--classic` = off; also in `F117_ENH_ARGS`; the launcher control is S4.5). `off` is the S3.4
  path: one picture per sim step (the canonical frame, with its one wide pass). Any other value makes every VGAME frame that loads no cockpit picture a
  **stream frame** (`sandbox.c enh_frame_begin`): the screen shows only display refreshes, never the live page.
* **A display refresh** (`refresh(alpha)`) = the wide pass (only in an extended view whose canvas is larger than the frame: it captures its 3D / vector layers
  into a third held slot, `enh_wide_capture_to(2)`) and then the main pass, both on A_k with the fields at alpha (`enh_interp_apply`, below). After the main
  pass's flip, still inside the sandbox, the live DAC / CRTC / attribute / sequencer registers (taken before the pass: `vga_look_get/set`, so the timer's DAC
  flicker and CRTC shake between steps show) are put back and the pass's page, shadow plane and layers are composed with the slot-2 wide picture
  (`enh_compose_display`) into the display picture; then the live machine is restored as in 4.7. In a stream frame the S3.4 per-frame wide pass is not run
  (the refreshes carry the wide picture); the `--mfd-probe` test aid keeps it, because the probes measure the canonical frame.
* **Timing (display clock).** `frame_floor` calls `enh_display_wait(last + n)` in each wait iteration instead of `enh_idle`, and `enh_frame_end()` just before
  the flip. The display clock is the SDL performance counter (ns); ticks and the sim's counters are never used for it. A refresh is due when the refresh
  interval has passed since the last one started: the display's refresh period (`host_refresh_ns`, `SDL_GetCurrentDisplayMode`, 60 Hz headless) for vsync,
  1/60 s or 1/30 s, each less 1.5 ms so that VSync, not this timer, decides (AotP). After the frame's first refresh a refresh is started only if it ends
  (EMA of the measured cost) before the step is due (`host_tick_time_ns(last + n)`): the flip keeps its time. A frame that had no refresh in its wait (no time
  left, or `frame_ticks` 0) gets one in `enh_frame_end`. `alpha = clamp((now - t_flip) / P, 0, 1)` with `t_flip` the display time of the last flip and `P` =
  `frame_ticks` x 1/70.086 s, or the EMA of the measured flip interval when that is longer (a step that overran). The presenter (`enh_compose_vga` ->
  `enh_display_take`) copies the newest picture; `host_pump` presents it at once (`enh_display_ready`, besides its 8 ms gate); with no new picture nothing is
  presented (VSync, the window repaint and the snapshot aid keep the last).
* **Interpolated fields** (`enhanced/interp.c`, read at every frame start: F_k; F_{k-1} kept): own x, y (BA10, C02A, linear), the cells C0A4 / C0B2 (F_k's value
  moved by the cells the lerped position is off, `(x + 16) >> 5` / `0x8000 - ((y + 16) >> 5)` as 39C4), altitude 2DF4 (linear), heading / pitch / roll
  (2DEE / 2DF0 / 2DF2, shortest 16-bit arc), the cockpit camera matrix 2D94 (view_frame copies it to B156 in view 0): the matrix of the lerped angles
  (`sim_matrix_from_angles`, as `fm_mat_from_euler`) plus the lerped residual of the integrated matrix against its own angles, so alpha 0 / 1 give exactly
  F_{k-1} / F_k. Everything view_frame builds (B288, B752, B75C, DE98.., E44C, C07C, B770, B156) follows. Objects, the delayed view's pose ring, the target
  views' objects, spinners and the HUD's digits stay A_k's (S4.2); the HUD lines (ladder, horizon, heading tape, ADI) are redrawn in every refresh from the
  lerped attitude, because the refresh runs the whole frame.
* **Hold** (every refresh of the frame at alpha 1 = the canonical frame k; only one refresh then): no previous frame (the first frame of a flight), the view
  (C07A, [294B]) changed, the ejection / crash sequence ([C06E] != 0) or the mission ending ([E552]) in either frame, a jump (position > 1024 world units =
  32 cells, altitude > 0x800, an angle > 0x4000: the pitch over the vertical flips heading and roll by 0x8000). Pause (Alt-P, `sim_pause_wait`): one refresh at
  alpha 1 (A_k is still the paused frame's), held while paused. VGAME menus (quit requester, boss screen: `enh_menu`) use the live presenter as before.
  A frame that loads a cockpit picture ([2E48] != [C07A], a view switch) is a **live frame**: the S3.4 path (its wide pass at alpha 1); the presenter keeps the
  last refresh's picture until that frame is flipped, then shows the live page until the next stream frame's first refresh (so no black surround from a missing
  wide picture; the original's 28-43 ms `screen_off` blank of a view switch is not shown with the stream on). `platform_exec` ends the stream when a program
  returns (`enh_flight_end`).
* **Must-not-change kept** (4.6): passes only in the `frame_floor` wait, `enh_frame_end` and the pause hook, never inside the canonical render (its `[262A]` /
  `frame_delay` waits pump without passes, and the presenter shows no live page then); no geometry change inside a pass (S3.4 fix 3); the sim, sound, ticks and
  input bytes are untouched (`sim_interp_equal`).
* **Test aids**: `--fps-test K` (K refreshes per stream frame at alpha 1/K .. 1, run at once at the start of the wait, no clock: deterministic), `--fps-trace FILE`
  (per refresh: frame, refresh, alpha, display ms, lerp, hold reason, view, the camera B288 / B752 / B75C and E44C / C07C / B770, the own fields), a `display:`
  statistics line at exit (refreshes, per refresh cost by stage, steps late, presents and their cost).
* **Measured** (2026-10-27, headless): `sim_interp_equal` 212 frames identical (canvas included) for `--fps off / --fps-test 4 / --fps 60 / vsync` and night +
  smooth shading; `enh_fps_jerk` 321 own-camera frames x 4 refreshes: 0 uneven, 0 backward, worst 5.7 % of the mean step; real time 1280 x 720, N = 2: 4.7
  refreshes per 100 ms step, alpha monotone, 0 backward steps. **Cost per refresh at 1920 x 1080, N = 4, wide** (canvas 1920 x 900): 29.2 ms (snapshot +
  restore 4.2, wide render 14.5, main render 6.0, compose 4.5) -> about 22 refreshes a second in real time (vsync, 2.5 per step), 1 step late in 301; with
  smooth shading + true night 73.5 ms (wide 43.0, main 17.9, compose 8.5) -> one refresh per step, 25 steps of 301 late. 60 Hz is not reachable with two
  renders per refresh; S4.3 merges them and reuses the canonical render at alpha 1.

### 4.9 As built (S4.3): one render per refresh, the budget, the rate fallback
* **One pass per refresh.** A refresh of an extended view is the wide pass alone (`PASS_DISP` with `enh_wide_pass`); after its flip it composes its own picture
  (`enh_compose_display_self`: its shown 3D / night layers are the wide picture inside the frame's 3D window and outside the frame, its vector layer outside the
  frame, its page, shadow records, HUD vector layer and TV / map windows everywhere else). S4.1's main pass rendered the same frame from the same A_k with the
  same fields, unwidened; the widening changes only the hr layers, which the compositor took from the wide pass anyway, so the main pass was redundant.
  A view that is not extended (FIT views, 4:3 windows, `--no-wide`) refreshes with the main pass, as before. `--fps-check` (test aid) builds each refresh both
  ways (the S4.1 wide + main passes, then the one pass) and counts differing pixels: 0 in every run (view sweep, night + smooth, `--no-wide` day and night).
* **The canonical render reused.** A refresh at alpha 1 in the `frame_floor` wait or `enh_frame_end` (before the flip) of a view that is not extended runs no
  pass: `enh_compose_canonical` composes the canonical frame's own render as its flip will show it (the shown page's first `2E34 x 320` bytes previewed from the
  back page and put back, the shadow records likewise, the work layers, the live look; `hr_night_late` runs now, the flip then finds nothing left). Equal to
  the pass at alpha 1 (`--fps-check`: 0 pixels). Holds of such views and the K-th `--fps-test` refresh use it; an extended view needs its wide pass anyway
  (the canonical render is not widened); the pause hold (after the flip) keeps its pass.
* **The pass made cheaper** (every change pixel-identical: per-refresh picture hashes, `F117_DISP_HASH`, equal before / after on the day and night chains):
  the hr span fill writes a row at a time (`span_run`: solid, grid / fine dither, FCh, the night factor; `hr_put_span` for the 1001 bands; `span_andor` for the
  AND / OR ground faces FEh / FDh, which read the page under unwritten pixels per original pixel instead of per layer pixel); in a pass the flip swaps the work
  and shown layer pointers instead of copying 4 layers (the pass's canvas is dropped after its compose); `lround` of the S3.5 band blend is an exact inline
  (half away from zero on `v - trunc(v)`); the per-pixel loops run in bands of rows on a worker pool (`enhanced/par.c`, up to 8 threads, `F117_THREADS`, 1 with
  `--classic`): the compositor, the S3.5 band blend, the S3.6 backdrop / pass end / late factors (`hr_night_late` sums its count atomically). Each band
  writes only its rows, so the result does not depend on the threads.
* **Scheduling.** A refresh also runs at the frame start (`enh_frame_begin`, right after A_k is taken, nothing rendered yet; not in holds, not with
  `--fps-test`) when the last one began half an interval ago or more: between the flip and the `frame_floor` wait the canonical frame waits for the frame tick
  (`[262A]`, up to 14 ms) and renders, and without it no picture came for 15-20 ms after every step (one display period a step showed the same picture). In
  the wait a refresh may start before its interval (but not before half of it) when the one due at the interval would no longer end before the step, so the
  step's last picture comes near its end. Real time, 1280 x 720, N = 4, `--fps 60`: 6.2 refreshes a step (S4.1: 4.7 at N = 2), refresh-to-refresh 15.9 ms on
  average, 8 % of the gaps over 17 ms.
* **Budget and fallback.** The budget is the display period (16.7 ms at 60 Hz). From the EMA of the pass cost (`est_cost`): when it is over 3/4 of the
  interval, refreshes go to every 2nd display period (60 -> 30 Hz; on a faster monitor the smallest divisor that fits, not slower than ~30 Hz); back when it
  is under half of the faster interval (hysteresis: no flapping). `--fps 30` and `--fps-test` are not changed. A reused picture does not count as a pass.
  Exit statistics: `display-stages` (per refresh, ms average / p95: snapshot, render, compose, refresh; the presenter's copy `take`; `present` with a window),
  `display-rate` (refreshes at every period / every 2nd / slower, switches, the EMA, the display period), `display-begin`, `display-reuse`.
  Test aid `F117_TEST_REFRESH_MS=X` makes every pass X ms slower (busy): with X = 20 at `--fps 60`, 595 of 595 refreshes at 30 Hz, 1 switch.
* **Measured** (2026-10-27; `port/notes/e43/profile.py`: the airbase, banked flight from frame 60, `--render-scale 4 --wide`, first cockpit view, headless,
  virtual clock, `--fps-test 4`, 804 refreshes; ms per refresh, average / p95):

  | Window, mode | snapshot | render | compose | refresh | take | refresh before (S4.1) |
  |---|---|---|---|---|---|---|
  | 1920 x 1080 day | 1.74 / 2.06 | 3.29 / 6.22 | 0.94 / 1.06 | **5.98 / 9.00** | 0.39 / 0.53 | 26.97 / 47.92 |
  | 1920 x 1080 smooth + night | 1.68 / 1.94 | 9.55 / 14.94 | 2.39 / 2.53 | **13.63 / 19.08** | 0.40 / 0.51 | 71.41 / 91.75 |
  | 2560 x 1440 day | 2.69 / 3.08 | 4.07 / 7.98 | 1.82 / 1.98 | **8.58 / 12.64** | 0.88 / 1.06 | 42.22 / 61.36 |
  | 2560 x 1440 smooth + night | 2.67 / 3.01 | 14.60 / 24.34 | 4.14 / 4.45 | **21.41 / 31.03** | 0.81 / 0.93 | 106.41 / 133.87 |

  Before, by stage: 1920 day snapshot 3.47, render 19.71 (wide 14.41 + main 5.30), compose 3.79; 1920 night 4.71, 58.29, 8.41; 2560 day 7.03, 28.39, 6.80;
  2560 night 5.93, 84.87, 15.61. Present (a real window on a 1920 x 1080 164 Hz monitor): 1.24 / 1.39 ms day, 1.15 / 1.28 night. Real time, headless
  (60 Hz display): 1920 day keeps 60 Hz (1299 of 1308 refreshes at every period); 2560 day 60 Hz with short drops in the costlier ground segment (868 of
  1077); smooth + night falls back to 30 Hz at both sizes (pass EMA 12.0 and 20.2 ms against the 12.5 ms limit). With the window at 164 Hz the divisor is 3
  (54.7 Hz) day and night.

### 4.10 As built (S4.2): the objects and the HUD between steps
* **Where.** `enhanced/interp.c` only; no engine hook (the HUD and every object are drawn by the pass from its copy of A_k, so writing the lerped fields
  into that copy before `view_frame` is enough). At every frame start (`enh_interp_frame`, live A_k) the object state is read with the own aircraft's F_k
  (F_{k-1} kept): the unit table (`[DED0]` x 36 B at C140, clamped to 48), the 12 projectile slots (3C0C), the `[B16C] + 4` tracer rounds (E44E), the 16
  smoke puffs (39DA), the wreck (C0A6 / C0B8 / C602), the spinners (C04C / C04E), the pose-ring slot the delayed view reads (`(3D60 - 3660) & 15`, 16 B at
  B056 + 16 i, with 3D60 / 3660) and the flight-path marker offset 98C6. The match is decided once per frame (`objects_match`), the values written per pass
  (`objects_apply`, after the own aircraft in `enh_interp_apply`). Nothing interpolates while the frame holds (4.8: start, view, eject / crash, end, jump).
* **Matching (slot + type) and jumps**; a slot that fails shows F_k (A_k's own value), the others still interpolate:
  | Class | Same object when | Jump (no lerp) | Fields lerped |
  |---|---|---|---|
  | units | +16 bit 1 in both, +14 type equal | x32 / y32 > 2048 world units, +4 > 0x800, an angle > 0x4000 | +0 / +2 cells, +4 height (16-bit signed move), +6 / +A x32 / y32 (linear), +E / +10 / +12 (shortest arc) |
  | projectiles | +E != 0 in both, +10 type equal | +0 / +2 > 512 cells, +4 > 0x2000, an angle > 0x4000 | +0 / +2 / +4, +8 / +A / +C |
  | tracers | x != 0 in both, `[B16C]` equal | not exactly moved by its own velocity (465B: x_k = x_{k-1} + d, d kept), i.e. a new round in the slot | x / y / z = x_{k-1} + d alpha |
  | smoke puffs | x / y equal, != 0 | height (+4) > 0x400 | +4 only (the size / art stays A_k's) |
  | wreck | C602 > 0 in both | > 512 cells, height > 0x2000 | C0A6, C0B8, C602 |
  | spinners | always | none: forward modular (they only go up, by up to 0x5100 a step) | C04C, C04E |
  | pose ring (view 0x84) | the view 0x84, 3660 equal, 3D60 + 1 | as the own aircraft (1024, 0x800, 0x4000) | the slot read in frame k gets the pose between the slot read in k-1 and it |
  | flight-path marker | always | > 0x1000 | 98C6 (the HUD velocity vector's offset; a sim value read only by the HUD and the step) |
  The pose ring is changed in the pass's copy, so `sim_view.c` needs no hook: view_frame's case 0x84 reads the lerped slot; the fly-by sway that also
  reads the ring runs in the step, never in a pass. The camera of the target views (units / projectiles) follows the lerped records the same way.
* **HUD.** Ladder, horizon, heading tape and ADI already used the lerped 2DEE / 2DF0 / 2DF2 and the matrix (4.8; the ADI's 98C8 / 98E4 / 98F8 / 9F12
  are rebuilt from the camera by the sky pass in every pass); the target boxes and dots are projected by B85D from the lerped objects and camera, the
  flight-path marker uses the lerped 98C6. The digits (speed, altitude 2DF6, heading number) show the lerped heading or A_k's values. The designator logic in
  9ACA runs in the pass on the lerped positions; what it selects is thrown away with the pass (the sim's selection is the canonical frame's).
* **Not interpolated (discrete, per step):** explosions and puff animation frames, the TV fire puffs (C73C consumes the RNG; in a pass it is the pass's copy),
  smoke size, MFD pages, messages, the HUD digits that are not computed from lerped fields.
* **Tests.** ctest `enh_interp_rules` (`--interp-test`, no game files): synthetic records for every class in a scratch DGROUP: matched slots step evenly
  over 4 refreshes and end on F_k, a type change / a freed unit / a unit jump / a new tracer round are held, the spinner goes forward over the wrap, the delayed
  view's slot moves, a view change holds everything. `enh_fps_jerk` now checks the camera of **every** view (S4.1: own-aircraft views only; 433 frames, 0
  uneven, 0 backward, worst 5.7 %) and the per-object-class trace (`--fps-trace` lines `o <frame> <refresh> <class> <slot> v= a= b=`): units 2608
  object-frames, projectiles 89, spinners 444, pose ring 29, fpm 444, all 0 uneven / 0 backward / 0 not ending on F_k (worst 7.3 % of the mean step);
  tracers, smoke and the wreck do not occur in the scripted chains (no gun, no hit) and are covered by `enh_interp_rules`. `sim_interp_equal` adds the
  airbase chain (461 frames, units / projectiles / the pose ring / target views interpolated) with `--fps-test 4` against `--fps off`: every frame hash
  identical. Exit line `display-objects:` (per class frames interpolated / held, the largest move a step: units 173 world units, projectiles 15 cells, the
  pose ring 54, fpm 19, spinners 0x5100).
* **Cost.** Reading the objects once per frame (about 3 KB) and writing them per pass is lost in the noise: `port/notes/e43/profile.py` at 1920 x 1080,
  N = 4, wide, 804 refreshes, ms per refresh avg / p95: day 5.51 / 6.95 (before S4.2: 5.58 / 6.96), smooth + true night 13.23 / 16.67 (13.06 / 16.28).

### 4.11 As built (S4.6): the map MFD between steps, scrolling continuously
* **Why it stepped (root cause).** The original draws the map's moving symbols in the **sim step**, not in the render (`sim_frame_step`, 39C4 ff.; `sim/sim_step.c`):
  `mfd_map_restore_bg` (9770) copies the window back from the backup page 2, then the two target markers (`cockpit_keyblit` 0x104), the route (`mfd_map_route`
  94A5, its first leg from the aircraft), the detection plot and arc (`hud_threat_update` -> `mfd_map_arc` 863C), the unit blips (`ai_update_units`, 2x2 colour
  29h), the missile blips (`wpn_proj_update_all`, colour 11h, blinking) and last the aircraft marker (one of 8 heading sprites of `256PIT.PIC`) go into the draw
  page. The terrain (`map_render_terrain` 119C via `mfd_map_redraw` 8387, with the site dots) is drawn only when the marker leaves the window (the map recentres,
  the aircraft 32 pixels behind the centre) or the zoom changes, then saved to the backup. `view_frame` only copies the window to the other page. A display pass
  (4.2) re-runs the render on A_k, so every refresh showed step k-1's drawing: the marker and blips moved at the step rate by whole original pixels, the map jumped
  at every recentre. Zoom `[4080]` 2..9, an original pixel = 2^(10 - z) cells; at 380 kt the aircraft moves about 2.3 cells a step: 0.01 pixel a step at zoom 2,
  1.15 at zoom 9 (a 4-layer-pixel jump ~10 times a second at N = 4, a recentre every ~55 steps). North-up, no every-Nth-frame logic.
* **What it does now (user request 2026-10-28: "scroll continuously").** With the stream on (`--fps vsync / 60 / 30`) the map is **centred on the aircraft
  marker** and the terrain, the grid of roads / rivers, the site dots, target squares, route, detection arc and blips move under it at layer-pixel precision; there
  is no recentre jump. Centred, not the original's offset: the original has no constant marker-to-centre offset (the marker starts 32 pixels behind the centre
  along the heading of the moment of the recentre and then flies across the window), and a look-ahead that followed the heading would swing the whole map sideways
  in every turn. The marker keeps its 8 heading sprites. `--fps off` and `--classic` keep the original map (nothing is logged).
* **The log (live).** With the stream on (`enh_fps_on`) the step's map drawing is logged in order (`enhanced/mapmfd.c`): hooks at `mfd_map_save_bg` / `restore_bg`
  (the log starts again; also covers `det_site_radar_update`'s restore-plot-save of a newly detected site), `mfd_map_plot` (cells, colour, size; tagged with its
  unit / projectile slot by a hook in front of the plot in `fl_ai.c` / `fl_weapons.c`), `mfd_map_route`, `mfd_map_arc` (its own plots not logged), and the two
  `cockpit_keyblit` calls of `sim_frame_step` (a target marker with its site's cells; the aircraft marker with BA10 / C02A / C0A4 / C0B2 / 2DEE). At the frame start
  (`enh_map_frame`, after `enh_interp_frame`) the log of step k-1 becomes this frame's; the marker keeps its last two steps. The map is redrawn by every pass
  while it is shown and the log is complete, holds included (the marker then at its last step): otherwise a hold would show the original's placement for a frame.
* **The map's centre** is the marker's position between its last two steps (the cells with the fraction 39C4's rounding dropped), lerped by alpha (shortest
  arc for the heading), never extrapolated; held at the last step when they are not consecutive steps or more than 32 cells apart.
* **The terrain cache** (cheaper than a terrain render per refresh): in the first pass that needs it (`enh_map_prepare`, before `enh_interp_apply`), the map
  window grown by 24 pixels on every side (125 x 108 at (50, 86), same centre, so 119C's camera and tile bounds follow the window) is cleared (colour 6) and drawn
  as 8387 draws the window: `map_render_terrain` at N x and the site dots, with the map at cells on the grid of whole map pixels in x and of whole 3/4-row groups
  in y (4 cell rows -> 3 pixel rows), so every cache has the terrain's pixels in the same place and scrolling is a translation. `enh_map_cache_grab` keeps it as
  3D-layer pixels (a 3D write keeps its layer pixels, the clear colour and the dots become their 1x value N x N); the rectangle's canvas is put back and the pass
  reloads A_k (`enh_sandbox_reload`: mem, VGA, statics), so the pass goes on as if nothing happened. A new cache when the zoom, N, the backup window (the original
  redrew: recentre, zoom, a detected site) or the site dots change, or when the centre is more than 16 pixels from the cache's (its 8-pixel rim is never shown: the
  polygons are clipped there). Per refresh `enh_map_cache_put` copies the window from the cache moved by the scroll in whole layer pixels (round(scroll x N)):
  records 3D, the vector layer cleared.
* **The symbols.** Target squares, route, detection plot and arc (the "statics") are drawn by the original functions in one `enh_win_shift` with the map at the
  cells of the same pixel grid nearest below the centre and the clip grown by 4 pixels, and what they drew (1x pixels as N x N blocks, the hr route / arc lines
  as their vector-layer pixels) is moved by the rest of the scroll (up to 1 pixel in x, 3 in y), clipped to the window. The blips are at their objects' cells
  (`enh_interp_cells`: lerped unrounded when S4.2 matches them, else F_k) relative to the centre, moved by sub-pixels with `enh_cap_shift` (the 1x op over two
  sentinel backgrounds finds its pixels, which become N x N vector-layer blocks at the offset). The marker is drawn at the window's centre (112, 139) as the
  original sprite.
* **Faithfulness.** The terrain and the dots are the original's render, translated. Compared with the original's own window (a cache rendered at its map cells,
  test aid), pixels differ only near the window's edge, where the original clips the polygons to the window and rounds the new vertices (a shallow edge's sliver
  can reach 13 pixels in: at most 2 pixels a render more than 7 pixels in, over the zoom run). Detected sites that the original plotted in colour 0 into its
  backup show with 8387's colour after the next cache render (as after the original's next recentre).
* **Alpha 1.** The map's alpha-1 picture is not the canonical frame's, so a refresh at alpha 1 is a pass while the map is redrawn (`enh_map_active`, S4.3 reuse
  off in the views that are not extended: `--no-wide` / the second cockpit view ~5 ms instead of ~1 by day). A frame that loads a cockpit picture (switching
  into the cockpit) is shown live after its flip with the original's map for that one frame.
* **Not changed:** the 8 marker sprites (45-degree steps), the missile blips' blink and the arc's growth (per step), the 1x look of the target squares and dots
  (N x N blocks, now at layer-pixel positions), the route's 1x endpoints.
* **Tests.** `enh_fps_jerk` (ctest): per pass o-lines `mapscroll` (the centre from a fixed world point in 1/64 pixel, and the drawn layer pixel of the world
  under the window), `mapown`, `mapunit`, `mapproj`, checked like the S4.2 objects (even, never backward, ending on F_k), plus the scroll's continuity from frame
  to frame (the first refresh of a frame starts where the last one of the frame before ended). The exit line `display-map:` has a replay check (`F117_MAP_CHECK=1`
  or `--fps-trace`: the log replayed as logged gives the window A_k shows), the terrain check (each cache render also rendered at the original's map cells and
  compared with the backup window) and the overlap check (two caches with the same sites agree pixel for pixel where they overlap). View sweep (zoom 2):
  160 frames redrawn, scroll 149 frame pairs with 0 jumps, 6 cache renders (terrain check 0 pixels inside / 10 near the edge; 1 overlap, 0 pixels), 1 original recentre, replay check 159 frames 0 differ, the trace 0 uneven / 0 backward (mapscroll and mapown 153 frames, blips 271 + 19 object-frames). Zoom run (zoom 9 for 250 frames, then 8 .. 2): 507 frames redrawn at zooms 2..9, scrolled 224 original pixels, 476 frame pairs with 0 jumps across 4 original recentres and 36 cache renders (terrain check 2 pixels inside in one render at zoom 5, 66 near the edge; 13 overlaps, 0 pixels), replay check 507 frames 0 differ, mapscroll 491 frames 0 uneven (worst 7.3 % of the mean step) / 0 backward. `enh_one_render`: the wide runs compare the map of the S4.1 and S4.3 passes
  (identical); its two `--no-wide` reuse runs press F3 at 62 s (the scope in the left MFD), since with the map redrawn a refresh at alpha 1 is no longer the
  canonical render. `sim_interp_equal` unchanged (the log and the cache are not sim state).
* **Cost** (`port/notes/e43/profile.py`, 1920 x 1080, N = 4, wide, 804 refreshes, ms avg / p95): the map work 0.46 .. 0.51 ms a pass (the terrain copy about 0.15, the statics about 0.25); a cache render 2.3 .. 2.5 ms, in the refresh that needs it (7 in 200 frames at zoom 2; at zoom 9 about every 15 steps plus the original's recentres); per refresh day 7.06 / 10.13, smooth + true night 13.73 / 17.34 (the first S4.6 build, symbols only, in a quieter run: 6.10 / 8.18 and 13.77 / 17.68; the machine's load varied about 2x during the session). One render per step instead would cost the 2.5 ms 10 times a second, one per refresh 60 times.

## 5 (d) Hooks, `--classic`, tests

### 5.1 `ENH:` hooks (each a guarded call of 1–3 lines; bodies live in `src/enhanced/`)
| Function | File | Hook | Subtask |
|---|---|---|---|
| `main` option loop | `main.c` | `enh_parse_arg` (exists) | S0.1 |
| `host_set_frame_source`, `present`, `HOST_FRAME_MAX_*` | `host.c`, `host.h` | canvas size, `a` stretch, Enhanced frame source | S2.1 |
| `compose` | `platform/vga.c` | mode 13h in VGAME → `enh_compose` (layers + shadow + live DAC/CRTC) | S2.1 |
| `line`, `put_pixel`, `sprite_scaled`, `poly_fill` | `video/mgraphic.c` | shadow record + vector/sprite emit (route aware) | S2.1 (shadow), S3.1/S3.3 (emit) |
| `r3d_set_viewport` | `render3d/r3d_scene.c` | hr viewport/centre for this pass; clear rect | S2.2 |
| `r3d_begin_scene`, `r3d_end_scene` | `render3d/r3d_scene.c` | route push/pop; skip `[262A]` wait in sandbox | S2.2, S2.4 |
| `r3d_project`, `r3d_make_edge` | `render3d/r3d_object.c` | hr vertex, hr edge record + clip | S2.2 |
| `r3d_poly_add_edge`, `r3d_poly_finish`, `r3d_line_emit` | `render3d/r3d_clip.c` | mirror into the hr back end | S2.2 |
| `band_points` / `bands` | `render3d/r3d_sky.c` | hr band quads | S2.2 |
| `hz_draw`, `poly_fill_far` | `render3d/edge2d.c` | hr polygon | S2.2 |
| `r3d_project_point` | `sim/sim_objovl.c` | hr sx/sy (+ invalid on early return); extended range when wide and route 3D | S2.2, S3.1, S3.4 |
| `r3d_draw_world_objects` (B billboards, D/E dots), `obj_overlay_frame` (S2 tracers, S3 explosion), `hud_target_box`, `hud_draw_diamond` | `render3d/r3d_world.c`, `sim/sim_objovl.c` | `enh_bill_size`, `enh_dot_begin/end`, `enh_tracer_*`, `enh_expl_pending`, `enh_box_*` (2.8) | S3.1 |
| `sprite`, `r3d_queue_billboard`, `r3d_draw_queued` | `render3d/r3d_scene.c` | `enh_sprite_begin/end`, `enh_bill_note`, `enh_bill_pending` (2.8) | S3.1 |
| `mfd_view_render`, `map_render_terrain` | `sim/sim_mfdview.c`, `render3d/r3d_terrain.c` | the DB71 hook covers them; `enh_tv_backdrop`, `enh_pass_continue`, `enh_tint_note` in `mfd_view_render` (2.9) | S3.2 |
| `line`, `hud_tick_strip`, `poly_fill`, `cockpit_draw_adi` | `video/mgraphic.c`, `sim/sim_cockpit.c` | `enh_hud_begin/_end_line`, `enh_hud_begin_tick/_tick/_end_tick`, `enh_rec_vecfill`, `enh_adi_begin/_end` (2.10) | S3.3 |
| `r3d_draw_queued` (TV sprite), `poly_fill` mode 3, `mfd_map_save_bg` / `mfd_map_restore_bg` | `render3d/r3d_scene.c`, `video/mgraphic.c`, `sim/sim_mfd.c` | `enh_tv_sprite_pending`, `enh_rec_remap`, `enh_window_save/restore` (2.9) | S3.2 |
| `r3d_cull_core`, `r3d_frustum_test` | `r3d_scene.c`, `r3d_object.c` | widen by kx/ky in wide display passes (`enh_wide_kx/ky`, `enh_wide_frustum`) | S3.4 |
| `r3d_draw_terrain` | `render3d/r3d_terrain.c` | wide cell list in wide display passes (`enh_wide_cell`) | S3.4 |
| `obj_overlay_frame` (S2 .. S3), `r3d_project_point` | `sim/sim_objovl.c` | `enh_wide_ovl`, `enh_wide_xfield`, `enh_wide_range` (3.5) | S3.4 |
| `sim_quit_requester`, `sim_boss_screen` | `sim/sim_input.c` | `enh_menu` (pillarboxed while up) | S3.4 |
| `r3d_face_list` | `render3d/r3d_object.c` | `hr_night_cond` (a lights condition holds), `hr_night_elem` (an element begins or is skipped) (2.12) | S3.6 |
| `frame_loop_body` | `sim/sim_main.c` | `enh_frame_begin()` before `view_frame` | S2.4 |
| `frame_floor` wait loop, `frame_delay` | `sim/sim_main.c` | `enh_idle()` → display pass; return at once in sandbox | S2.4, S4.1 |
| `frame_floor` | `sim/sim_main.c` | `enh_display_wait(last + n)` per wait iteration, `enh_frame_end()` before the flip (4.8) | S4.1 |
| `sim_pause_wait` | `sim/sim_input.c` | `enh_display_hold()` (the paused frame at alpha 1) | S4.1 |
| `platform_exec` | `platform/startup.c` | `enh_flight_end()` (the stream stops with the program) | S4.1 |
| `host_pump`, `present`, host helpers | `host.c` | `enh_display_ready` (present a refresh at once), `host_refresh_ns`, `host_tick_time_ns`, `host_present_stats` | S4.1 |
| VGA look | `platform/vga.c` | `vga_look_get/set` (DAC, CRTC, attribute, sequencer) for the refresh's compose | S4.1 |
| `present` | `host.c` | `enh_present_done` (the present stage of a display picture) | S4.3 |
| `sim_snd_cmd`, `sim_snd_noise` | `sim/sim_stubs.c` | no-op in sandbox | S2.4 |
| `sim_snd_poll`, `host_pump`, `host_opl_write`, `dos_*`, `rtl_open/write`, `fopen_fn`, `unlink_fn`, `platform_load_file_paras` | `sim/sim_stubs.c`, `host.c`, `platform/dosio.c` | refused + counted in sandbox (`enh_sandbox_deny`) | S2.4 |
| `host_wait_until_ns`, `vga_wait_vretrace_*` | `host.c`, `platform/vga.c` | return at once in sandbox | S2.4 |
| `host_ns`, `host_pump`, scripted keys | `host.c` | `clk_ns` (F117_VCLOCK virtual clock, test aid) | S2.4 |
| `host_opl_write` | `host.c` | `enh_opl_trace` (the OPL stream for `--sandbox-hash`) | S2.4 |
| statics | `vga.c`, `mgraphic.c`, `pic.c`, `r3d_object.c` | `*_sandbox_regions(add)`: the module's statics for the snapshot | S2.4 |
| `main` | `main.c` | `enh_env_args()` before anything runs | S2.4 |
| `main` test dispatch | `main.c` | `--interp-test` -> `enh_interp_selftest()` | S4.2 |
| `view_frame` case 0x84 | `sim/sim_view.c` | lerp the ring pose in sandbox (as built: no hook, the pass's copy of the slot is lerped, 4.10) | S4.2 |
| `mfd_map_save_bg / restore_bg / plot / route / arc`, `sim_frame_step` (target and aircraft markers), `ai_update_units`, `wpn_proj_update_all` | `sim/sim_mfd.c`, `sim/sim_step.c`, `flight/fl_ai.c`, `flight/fl_weapons.c` | `enh_map_log_*` / `enh_map_tag` (the step's map drawing logged for the display passes, 4.11) | S4.6 |

### 5.2 `--classic`
`enh_parse_arg("--classic")` clears every option; each hook is `if (enh.<option>) …` (or `enh_active()`), so with classic
no hook body runs, `vga.c compose` keeps the 320x200 path and `host.c` keeps its sizes. The ctest commands copied from the
faithful repo run with `--classic` added (CMake list `F117_CLASSIC_TESTS`); 110 golden scenes, `sim_vgame` (1497 scenarios),
shell, debrief, sound, chain must stay green. Hook bodies are unreachable in classic, so the faithful suite also checks that
no hook leaks into classic.

### 5.3 Test plan
| Test | What | Subtask |
|---|---|---|
| `r3d_hr_n1` | every render3d golden scene through the hr back end at N=1, fx=fy=0: 3D layer == the 1× page inside the viewport, byte for byte | S2.2/S2.3 |
| `r3d_hr_ref` | N=2, 4: capture the hr polygons (`r3d_trace`), fill them with an independent pixel-centre reference filler; differences only within 1 layer px of an edge, summary count per scene | S2.3 |
| `enh_sandbox_hash` | as built (4.7): the `debrief_chain_sea` chain on the virtual clock, plain / 3 test-view display passes per VGAME frame / `--classic`: per-frame hashes of DGROUP, comm, LCG, sound driver, OPL stream, `mem[]`, VGA, canvas identical (canvas not compared with classic), final comm block and roster identical, 0 forbidden calls | S2.4 |
| `sim_interp_equal` | as built (4.8): the `enh_sandbox_hash` chain in a 1280 x 720 window, `--fps off` vs `--fps-test 4`, `--fps 60`, `--fps vsync`, and night + smooth with `--fps-test 3`: every frame hash (canvas included), final comm block, OPL stream, roster identical | S4.1 |
| `enh_fps_jerk` | the real-path view sweep in banked flight, `--fps-test 4 --fps-trace`: own-camera steps even and never backward, holding frames one picture; a real-time `--fps 60` run: alpha monotone, no backward cockpit step | S4.1 |
| `enh_one_render` | as built (4.9): `--fps-check` (S4.1 two-pass picture vs the one pass / the reused canonical render) on the view sweep, night + smooth, `--no-wide` day and night: 0 differing pixels; the 30 Hz fallback in real time with `F117_TEST_REFRESH_MS=20` | S4.3 |
| `enh_wide_geometry`, `r3d_hr_wide`, `r3d_hr_wide_realpath`, `enh_sandbox_hash` (`--wide` run) | the placement per view and window; the golden scenes with a wide main pass (reference, gaps, hole, seams); the real-path view sweep at 16:9 / 21:9 (3.5) | S3.4 |
| headless dumps | `--headless --enh-dump DIR` + `F117_KEYS` scripts: front cockpit (h 91/107), rear, left, right, 0x44, chase, TV, map; N=2/4; 4:3, 16:9, 21:9; plus mask PNGs → U1 | S2.3, S3.x |

## 6 (e) Risks, open questions, plan changes

Risks: (1) sandbox completeness: an engine static or host-side state missed by 4.2 would make display passes leak; the
hash test and a debug "no host_pump/no DOS write in sandbox" assert catch it. (2) Re-rendering per display frame costs a full
1× + N× render; a view change makes each display pass of that one step reload the PIC from disk (acceptable; cache later).
(3) Wide passes may draw side scenery overlapping the frame that the original never drew (accepted, 3.3). (4) The B85D range
change must be limited to the 3D route; some HUD consumer might use `[49E0]` as an index (S3.4 lists all readers: 8C10 903F
90B9 99A7 9ACA A17E B339 B77A). (5) Interpolation adds no lag over the faithful display, but input-to-picture latency stays one
step (100 ms at 10 steps/s); 15 steps/s (U3-2 alternative) halves the step error.

Questions (defaults apply if unanswered):
* Q1 (decided 2026-10-19: fit the window's height) left/right/rear views: default **fit** (the whole picture, black side bars); alternative **cover** (fill the window, crop top/bottom).
* Q2 (decided after seeing it in the game: both modes built, S2.3 shows them) dither: default original-pixel checkerboard; alternative fine (per layer pixel, smoother tone).
* Q3 (decided 2026-10-19: max(1, N/2)) line width at N×: default `max(1, N/2)` layer pixels; alternative always 1.
* Q4 (decided 2026-10-19: the world beside the panel) front cockpit: default the world also beside the instrument panel; alternative black side strips below the window line.
* Q5 (decided 2026-10-19: 10/s) (U3-2) sim step rate: default 10/s (Game speed 7); alternative 15/s.

Plan changes (applied to PLAN.md, ids kept): the wider view needs the sandboxed display pass before Phase 4, so the sandbox
becomes its own foundation **S2.4 (O, M)**; S4.1 shrinks to the loop, alpha and the own-aircraft/camera lerp (**O, M**, was L);
S2.3 gains the reference-rasteriser test (**S, M**, was H, S); S3.1 is routing + sprites/dots/tracers + all view modes on a
finished back end (**S, M**, was O); S3.2 is mostly verification (**S, S**); S3.4 depends on S2.4.
