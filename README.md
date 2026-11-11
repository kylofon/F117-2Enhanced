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
* **Mac cockpit** (optional): the Mac release's 640x480 cockpit and side-view pictures from your own Mac copy
  (below).
* **Launcher** with an Enhanced box, and `--classic`: the original picture and behaviour, as the faithful port.

See [CHANGELOG.md](CHANGELOG.md).

## How to play

1. **You need the original game**: your own copy of the *F-117A Nighthawk Stealth Fighter 2.0* files (`F117.COM`,
   `SETUP.EXE`, `START.EXE`, `VGAME.EXE` and the rest). The launcher tells you which file is missing.
2. **Unzip** `F117-Enhanced-v0.3.0-win64.zip` into a folder of your choice.
3. **Start `f117-enhanced-launcher.exe`** and point **Folder** at your game files (or copy them into the `Game`
   folder beside it). The folder must be writable: the game saves your pilot roster there.
4. Set the Enhanced options if you like and press **Play**. In SETUP answer **N** for the joystick and **2** for AdLib.

`roster/ROSTER.FIL` is the game's pilot roster as shipped by this package; to start with it, copy it into your game
folder (back up your own `ROSTER.FIL` first).

The errors the game reports go to `f117.log` beside the program.

### Supported game version

The engine reads each program's data at the offsets of the release it was made from: the MicroProse DOS release of
*F-117A Nighthawk Stealth Fighter 2.0* (1991). At start (and with `--check`) it unpacks the seven programs and
compares them with that release; another release stops with a message (also in `f117.log`) instead of crashing.
What it expects (LZEXE-packed files; the size of the unpacked load image and the DGROUP paragraph):

| File | Load image (bytes) | DGROUP |
|---|---|---|
| `SETUP.EXE` | 13 437 | 024C |
| `MPS_LOGO.EXE` | 14 721 | 0332 |
| `PLAYER.EXE` | 18 455 | 02A3 |
| `START.EXE` | 89 023 | 0A95 |
| `VGAME.EXE` | 162 460 | 1E23 (and its sine table at DS:2084 starts 0, 804, 1608, 2411, 3212, 4011) |
| `END.EXE` | 46 039 | 05FA |
| `DSWAP.EXE` | 19 495 | 0225 |

**The GOG release is not supported yet**: its `VGAME.EXE` (version 473.04, 95 979 bytes, load image 163 004 bytes,
DGROUP 1E42) is a different build ([issue #2](https://github.com/kylofon/F117-2Enhanced/issues/2)); support for it
is planned.

## Command line

```text
f117-enhanced [--game-dir DIR] [--scale N] [--aspect 4:3|1:1] [--fullscreen] [--frame-ticks N]
              [--keys NAME=CODE,...] [--check] [--version]
              [--classic] [--render-scale N] [--fps vsync|60|30|off] [--wide|--no-wide]
              [--smooth-shading|--no-smooth-shading] [--true-night|--no-true-night] [--mac-art FILE|DIR]
```

* `--classic`: every enhancement off, whatever else is given. `--render-scale N`: 1 to 8 times 320x200 (default Auto).
* `--fps vsync|60|30|off`: pictures a second (default vsync: the monitor's refresh).
* `--wide` (default) / `--no-wide`; `--smooth-shading`, `--true-night`: off by default.
* `--mac-art FILE|DIR`: the Mac cockpit pictures from your Mac release (below); off by default and with `--classic`.
* The other options are the faithful port's; the remaining test options are listed at the top of `f117port/src/main.c`.

## Mac cockpit art (optional)

The Mac release of F-117A (2.x) drew its cockpit and its rear, left and right views as 640x480 pictures. If you own the Mac
version, the game can use them instead of the PC pictures: the front cockpit with its own panel (the PC displays, lamps and
readouts are shown in the Mac panel's places) and the looking-out views. Nothing of the Mac release is part of this
project: point the game at your own copy (the release is at the
[Internet Archive](https://archive.org/details/f117a_stealth_fighter); while the Mac art is missing, clicking the
launcher's warning sign opens that page). In the launcher tick **Mac cockpit** and choose the release's StuffIt archive as
**Mac art** (e.g. `f117a_stealth_fighter.sit` or `F-117a_Stealth_Fighter_2.1.sit`; the update archives hold no pictures);
on the command line, `--mac-art "C:/Games/f117a_stealth_fighter.sit"`. The pictures are read from the archive each start,
nothing is written. Play stays greyed out while the archive has no cockpit pictures.

`--mac-art` also takes the `F117a Art` resource fork (`F117a Art.rsrc` as written by an unstuffer, or
`F117a Art/..namedfork/rsrc` on a Mac), or a folder of pictures converted once with the Python tool (Python 3):

```bash
python tools/mac_art.py "f117a_stealth_fighter.sit" "C:/Games/F117 Mac art"
```

A missing archive or picture leaves the PC art (the reason is in `f117.log`). The Mac front cockpit needs the wide view (on by default).

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

### macOS

Builds natively with Homebrew. Tested on an Apple Silicon Mac (macOS 27, SDL 3.4); Intel Macs are untested.

```bash
tools/build_macos.sh /path/to/your/F-117      # installs sdl3, cmake, ninja if missing
./play.command                                 # or: ./play.command --fullscreen --render-scale 4
```

The launcher is not built on macOS (`-DF117_LAUNCHER=OFF`); use the command-line options above.

To make a double-clickable `F-117A Enhanced.app` (SDL3 bundled; asks for the game folder on first launch, Option-click to change it):
`tools/make_macos_app.sh` (output in `release/`). Extra command-line options go in
`~/Library/Application Support/F-117A Enhanced/options` (e.g. `--fullscreen --render-scale 4`).

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
