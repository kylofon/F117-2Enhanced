// game.h -- what a game folder must hold, where f117 is, and starting it.
#pragma once

#include <wx/string.h>

#include <vector>

// The folder the launcher runs from.
wxString LauncherDir();

// Where things are by default: "Game" and f117(.exe) beside the launcher.
wxString DefaultGameDir();
wxString DefaultProgram();

// Whether `dir` holds the file `name` (as written, or in upper or lower case).
bool FilePresent(const wxString& dir, const wxString& name);

// Whether `path` is a DOS program (such as the original game's START.EXE) rather than one this system can run.
bool IsDosProgram(const wxString& path);

// The first of the game's own files missing from `dir` (F117.COM, the programs and the MGRAPHIC / MISC
// drivers), or empty if they are all there.
wxString MissingGameFile(const wxString& dir);

struct GameOptions {
    wxString program;         // f117(.exe)
    wxString gameDir;         // --game-dir
    int scale = 3;            // --scale: the window is 320x240 (4:3) or 320x200 (1:1) times this
    bool aspect43 = true;     // --aspect 4:3 | 1:1
    bool fullscreen = false;  // --fullscreen
    bool sound = true;        // false: SDL_AUDIO_DRIVER=dummy
    int frameTicks = 7;       // --frame-ticks: the game speed (minimum retrace ticks a frame in flight; 0 = original pacing)
    // Enhanced (f117-enhanced); --classic switches all of them off.
    bool classic = false;         // --classic
    int renderScale = 0;          // --render-scale N (0 = Auto: not passed, the game picks from the window)
    int frameRate = 0;            // --fps: 0 = Monitor (not passed, the engine's default vsync), 1 = 60, 2 = 30, 3 = off
    bool wide = true;             // false: --no-wide
    bool smoothShading = false;   // --smooth-shading
    bool trueNight = false;       // --true-night
    wxString keys;            // --keys: the actions not on their default keys (keys::Argument), empty if none
};

// The program and its command-line arguments, as LaunchGame runs them.
std::vector<wxString> GameArguments(const GameOptions& options);

// Starts the game. On failure returns false and says why in `error`.
bool LaunchGame(const GameOptions& options, wxString& error);
