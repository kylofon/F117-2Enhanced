# Changelog

## 0.2.0 (2026-10-29)

Smooth frames. The game still runs its simulation at the original rate, about 10 steps a second at the
default Game speed. The picture is now drawn at the monitor's refresh rate, and everything that moves is
placed between two game steps.

### What is new

- **Smooth frame rate** (`--fps vsync|60|30|off`, default vsync = the monitor's refresh rate). The display runs on its own clock, separate from the game's. Between two game steps every picture shows the world part   of the way from one step to the next:
  - the camera, your aircraft and the horizon;
  - ground units and other aircraft;
  - missiles, bombs and tracers, and the weapon view that follows them;
  - smoke, falling wrecks and turning radar dishes;
  - the views that lag behind your aircraft (chase, fly-by).
- **The HUD moves with the world.** The pitch ladder, horizon, heading tape, ADI, target boxes and velocity vector are redrawn every picture, so they stay on the scenery instead of stepping.
- **Smooth cockpit MFDs.**
  - **Map:** the map scrolls continuously under the aircraft marker, which stays in the centre. Unit and missile blips glide.
  - **Scope (F3):** the radar grid and its symbols turn and move smoothly, with solid lines.
- **One render per picture.** The wide view and the main view are drawn in a single pass. When a picture
  takes longer than one refresh, for example with Smooth distance shading and True night at a high render
  scale, the display drops to a lower but still even rate instead of stuttering.
- **Launcher:** a **Frame rate** choice (Monitor, 60, 30, Off) in the Enhanced box.
- **No console window.** Started from Explorer or the launcher, the game no longer opens an empty console
  window beside it.

### Command line

- New: `--fps vsync|60|30|off`. `off` shows one picture per game step, as in 0.1.0. `--classic` implies
  `off`.

### Fixed

- Near the edge of the world, the map MFD could show an empty band beyond the world.

### Known limits

- Placing pictures between two steps means the display shows the world up to one game step late, about
  0.1 s at the default Game speed. Your controls still reach the game exactly as before.
- The map's aircraft and unit markers still turn in the original's 45-degree steps. The map's blinking
  symbols and the detection arc still update once per game step.
- Everything 0.1.0 listed applies, except its frame-rate limit.

### How it was verified

- The tests pass unchanged in `--classic`. With smooth frames, the simulation is checked to be
  identical to `--fps off`.
- The motion was traced per object class: camera, units, missiles, smoke, radar dishes, map and scope. The traces show no uneven steps and nothing moving backward.
- Recorded takeoff, HARM-attack and enemy-missile flights, smooth vs `off` side by side; playtested.

## 0.1.0 (2026-10-26)

The first Enhanced release: the faithful port's game (its v0.1.0), drawn sharper and wider. The simulation, rules and the original art are unchanged. You need your own copy of the original game files (see README.md, How to play).

### What is new

- **Higher resolution.** The 3D view, the HUD, the MFDs, the TV camera window and the map are drawn at N times   the original 320x200 (the render scale; **Auto** picks it from the window size, or choose 1 to 8). The   pictures, fonts, sprites, palettes and 3D models are still the original's.
- **Wider view.** On a wide window the first cockpit view and the outside views show more of the world beside the 4:3 picture instead of black bars. The side, rear and second cockpit views are fitted to the window height; the menus keep their 4:3 picture, centred.
- **Smooth distance shading** (option, off by default): the sky and ground colour bands and the depth shading of terrain and objects fade smoothly instead of in steps.
- **True night** (option, off by default): night missions are much darker, and terrain and objects fade into the dark sooner.
- **The launcher** (`f117-enhanced-launcher.exe`): the faithful launcher (game folder, window, speed, sound, key bindings) plus an **Enhanced** box with Render scale, Wide view, Smooth distance shading, True night and Classic. The settings are kept in `%APPDATA%\F-117A Enhanced\settings.ini`.
- **`--classic`**: every enhancement off; the game behaves as the faithful port.
- `roster/ROSTER.FIL` is the game's pilot roster as shipped by this package; to start with it, copy it into your game folder (back up your own `ROSTER.FIL` first).

### Command line

- `--render-scale N`, `--wide` / `--no-wide`, `--smooth-shading` / `--no-smooth-shading`,
  `--true-night` / `--no-true-night`, `--classic` for `f117-enhanced.exe` (the launcher passes them).
- The faithful options are unchanged: `--game-dir`, `--scale`, `--aspect`, `--fullscreen`, `--frame-ticks`, `--keys`, `--check`, `--version`.

### Known limits

- The frame rate is still the original's (one picture per game step, about 10 a second at the default Game speed). Smoother frames are the next stage.
- Wide view: at very wide windows
  (about 21:9 and wider) a thin sliver of the sky band can show at the far edge in steep rolls; objects
  that the wide view adds at the sides can overlap into the 4:3 picture; the wide view makes each frame
  slower to draw. Portrait windows fit the picture to the width.
- Everything the faithful 0.1.0 listed as untested (mouse and joystick in the shell and the debrief) applies.

### How it was verified

- The faithful tests pass unchanged in `--classic`, and the enhanced drawing is checked against the original
  frames (the 3D scenes, the wide view across aspect ratios, and a scripted run through every view).
- Playtested.
