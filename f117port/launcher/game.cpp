// game.cpp -- the files the game needs and starting it.
#include "game.h"

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <string>
#include <vector>

namespace {

// A file of `dir` whose name matches `name` as written, in upper or in lower case
// (case matters outside Windows); empty if there is none.
wxString FindFile(const wxString& dir, const wxString& name) {
    for (const wxString& n : {name, name.Upper(), name.Lower()}) {
        const wxString path = wxFileName(dir, n).GetFullPath();
        if (wxFileName::FileExists(path)) return path;
    }
    return wxString();
}

}  // namespace

bool IsDosProgram(const wxString& path) {
    // An MZ file without a PE header at e_lfanew (0x3C): the original game's own programs (START.EXE, F117.COM...).
    wxLogNull quiet;
    wxFFile file(path, "rb");
    if (!file.IsOpened()) return false;
    unsigned char head[0x40] = {};
    if (file.Read(head, sizeof head) != sizeof head) return path.Lower().EndsWith(".com");
    if (head[0] != 'M' || head[1] != 'Z') return path.Lower().EndsWith(".com");
    const unsigned long pe = head[0x3C] | head[0x3D] << 8 | head[0x3E] << 16 | static_cast<unsigned long>(head[0x3F]) << 24;
    unsigned char sig[4] = {};
    return !file.Seek(pe) || file.Read(sig, 4) != 4 || sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0;
}

bool FilePresent(const wxString& dir, const wxString& name) { return !dir.empty() && !FindFile(dir, name).empty(); }

wxString MissingGameFile(const wxString& dir) {
    for (const char* name : {"F117.COM", "SETUP.EXE", "MPS_LOGO.EXE", "PLAYER.EXE", "START.EXE", "VGAME.EXE", "END.EXE",
                             "DSWAP.EXE", "MGRAPHIC.EXE", "MISC.EXE"})
        if (!FilePresent(dir, name)) return name;
    return wxString();
}

wxString LauncherDir() { return wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath(); }

wxString DefaultGameDir() {
    // "Game" beside the launcher; in a source tree (launcher in f117port/launcher/build) the repository's Game
    // folder a few levels up.
    wxFileName dir(LauncherDir(), wxEmptyString);
    const wxString beside = wxFileName(LauncherDir(), "Game").GetFullPath();
    for (int up = 0; up < 5; ++up) {
        wxFileName game(dir);
        game.AppendDir("Game");
        if (FilePresent(game.GetPath(), "F117.COM")) return game.GetPath();
        if (dir.GetDirCount() == 0) break;
        dir.RemoveLastDir();
    }
    return beside;
}

wxString DefaultProgram() {
    wxFileName name(LauncherDir(), "f117-enhanced");
#ifdef __WXMSW__
    name.SetExt("exe");
#endif
    return name.GetFullPath();
}

std::vector<wxString> GameArguments(const GameOptions& o) {
    std::vector<wxString> args{o.program,
                               "--game-dir", o.gameDir,
                               "--scale", wxString::Format("%d", o.scale),
                               "--aspect", o.aspect43 ? "4:3" : "1:1",
                               "--frame-ticks", wxString::Format("%d", o.frameTicks)};
    if (o.fullscreen) args.push_back("--fullscreen");
    if (!o.keys.empty()) {
        args.push_back("--keys");
        args.push_back(o.keys);
    }
    if (o.classic) {
        args.push_back("--classic");  // every enhancement off: the other Enhanced options are not passed
    } else {
        if (o.renderScale > 0) {
            args.push_back("--render-scale");
            args.push_back(wxString::Format("%d", o.renderScale));
        }
        // Monitor (0) is the engine's default (--fps vsync), so it is not passed, as Render scale Auto is not.
        static const char* const frameRates[] = {nullptr, "60", "30", "off"};
        if (o.frameRate > 0 && o.frameRate <= 3) {
            args.push_back("--fps");
            args.push_back(frameRates[o.frameRate]);
        }
        if (!o.wide) args.push_back("--no-wide");
        if (o.smoothShading) args.push_back("--smooth-shading");
        if (o.trueNight) args.push_back("--true-night");
    }
    return args;
}

bool LaunchGame(const GameOptions& o, wxString& error) {
    if (!wxFileName::FileExists(o.program)) {
        error = wxString::Format("The game's program isn't there:\n\n%s", o.program);
        return false;
    }
    std::vector<wxString> args = GameArguments(o);

    std::vector<std::wstring> wide;
    for (const wxString& a : args) wide.push_back(a.ToStdWstring());
    std::vector<const wchar_t*> argv;
    for (const std::wstring& w : wide) argv.push_back(w.c_str());
    argv.push_back(nullptr);

    wxExecuteEnv env;  // an empty variable map: the game inherits the launcher's environment
    env.cwd = wxFileName(o.program).GetPath();
    if (!o.sound) {
        // Sound off: SDL opens its silent audio driver, the game runs as with a sound card.
        wxGetEnvMap(&env.env);
        env.env["SDL_AUDIO_DRIVER"] = "dummy";
    }
    long pid;
    {
        wxLogNull quiet;  // wxExecute would show its own error box
        pid = wxExecute(argv.data(), wxEXEC_ASYNC, nullptr, &env);
    }
    if (pid == 0) {
        error = wxString::Format("Couldn't start %s.\n\n%s", wxFileName(o.program).GetFullName(),
                                 wxSysErrorMsgStr(wxSysErrorCode()));
        return false;
    }
    return true;
}
