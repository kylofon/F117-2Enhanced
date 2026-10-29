# F-117A Enhanced — SDL3

An enhanced native port of MicroProse's *F-117A Nighthawk Stealth Fighter 2.0* (1991), built on the faithful
SDL3 port ([F117-2](https://github.com/kylofon/F117-2), v0.1.0). Not an emulator. The original game files are not included.
Goal: a sharper, smoother game (higher resolution, higher frame rate) with the original art unchanged.

Status and plan: [PLAN.md](PLAN.md) (as a page: `python tools/plan_site.py`, then open
`site/index.html`). Agent rules: [CLAUDE.md](CLAUDE.md). Engine rules: [ENGINE.md](ENGINE.md).

## Features

* **Higher resolution**: the 3D view, HUD, MFDs, TV window and map are drawn at N times the original 320x200
  (render scale: Auto, or 1 to 8). The original art is unchanged.
* **Wider view**: on a wide window the first cockpit view and the outside views show more of the world beside the
  4:3 picture; the side, rear and second cockpit views are fitted to the window height.
* **Smooth distance shading** and **True night**: options, off by default.
* **Smooth frame rate**: drawn at the monitor's refresh rate (`--fps vsync|60|30|off`); the camera, objects,
  HUD and cockpit MFDs move smoothly between the game's steps.
* **Launcher** with an Enhanced box, and `--classic`: the original picture and behaviour, as the faithful port.

See [CHANGELOG.md](CHANGELOG.md).

## How to play

1. **You need the original game**: your own copy of the *F-117A Nighthawk Stealth Fighter 2.0* files (`F117.COM`,
   `SETUP.EXE`, `START.EXE`, `VGAME.EXE` and the rest). The launcher tells you which file is missing.
2. **Unzip** `F117-Enhanced-v0.2.0-win64.zip` into a folder of your choice.
3. **Start `f117-enhanced-launcher.exe`** and point **Folder** at your game files (or copy them into the `Game`
   folder beside it). The folder must be writable: the game saves your pilot roster there.
4. Set the Enhanced options if you like and press **Play**. In SETUP answer **N** for the joystick and **2** for AdLib.

`roster/ROSTER.FIL` is the game's pilot roster as shipped by this package; to start with it, copy it into your game
folder (back up your own `ROSTER.FIL` first).

The errors the game reports go to `f117.log` beside the program.

## Command line

```text
f117-enhanced [--game-dir DIR] [--scale N] [--aspect 4:3|1:1] [--fullscreen] [--frame-ticks N]
              [--keys NAME=CODE,...] [--check] [--version]
              [--classic] [--render-scale N] [--fps vsync|60|30|off] [--wide|--no-wide]
              [--smooth-shading|--no-smooth-shading] [--true-night|--no-true-night]
```

* `--classic`: every enhancement off, whatever else is given. `--render-scale N`: 1 to 8 times 320x200 (default Auto).
* `--fps vsync|60|30|off`: pictures a second (default vsync: the monitor's refresh).
* `--wide` (default) / `--no-wide`; `--smooth-shading`, `--true-night`: off by default.
* The other options are the faithful port's; the remaining test options are listed at the top of `f117port/src/main.c`.

## Building

```bash
cmake -S f117port -B f117port/build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build f117port/build
ctest --test-dir f117port/build
python tools/make_release.py      # clean Release build with the launcher, ctest, the zip in release/
```

Needs CMake 3.24, a C11 compiler and SDL 3 (MSYS2 `mingw64`: `mingw-w64-x86_64-sdl3`, and
`mingw-w64-x86_64-wxwidgets3.2-msw` for the launcher; `-DF117_LAUNCHER=OFF` skips it). The program is
`f117port/build/f117-enhanced.exe`. Tests that need the game files skip themselves when `Game/` is missing.

## Launcher

`f117-enhanced-launcher.exe` is the faithful port's launcher (game folder, window, speed, sound, key bindings)
plus an **Enhanced** box; its settings are kept in `%APPDATA%\F-117A Enhanced\settings.ini`.

* **Render scale**: Auto, or 1 to 8 times the original 320x200.
* **Frame rate** (`--fps`): Monitor (the default, not passed), 60, 30 or Off.
* **Wide view**, **Smooth distance shading**, **True night**.
* **Classic**: the original game; greys out the options above.

## Layout

* `f117port/src/` — the faithful engine (`ENGINE.md`; changes marked `ENH:`).
* `f117port/src/enhanced/` — the enhancements.
* `port/notes/`, `port/spec/*_symbols.csv`, `port/symbols.csv` — the verification harnesses and symbol tables the
  build and tests use (copied from the faithful repo).
* `tools/` — symbol generators, packaging and the plan page.

## License

MIT ([LICENSE](LICENSE)); third-party licences in `licenses/`. *F-117A Nighthawk Stealth Fighter 2.0* is ©
MicroProse; its files are not included.
