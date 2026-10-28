#!/usr/bin/env python3
"""ctest launcher_args: runs the launcher headless (--settings-dir, --print-command) on a few saved-settings
combinations and checks the game's command line it would run."""
import os, subprocess, sys, shutil

exe, work = sys.argv[1], sys.argv[2]
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)

def run(name, game_section):
    d = os.path.join(work, name)
    os.makedirs(d)
    with open(os.path.join(d, "settings.ini"), "w") as f:
        f.write("[Game]\nGameFolder=%s\n%s" % (os.path.join(work, "gamedir"), game_section))
    out = os.path.join(d, "cmd.txt")
    subprocess.run([exe, "--settings-dir", d, "--print-command", out], timeout=60)
    if not os.path.exists(out):
        sys.exit("%s: no output" % name)
    return open(out, encoding="utf-8").read().split("\n")[:-1]

fails = []
def check(name, args, present=(), absent=(), pairs=()):
    for a in present:
        if a not in args: fails.append("%s: missing %s in %s" % (name, a, args))
    for a in absent:
        if a in args: fails.append("%s: unexpected %s" % (name, a))
    for k, v in pairs:
        if k not in args or os.path.normcase(os.path.normpath(args[args.index(k) + 1])) != os.path.normcase(os.path.normpath(v)): fails.append("%s: %s %s expected" % (name, k, v))

a = run("defaults", "")
check("defaults", a, absent=["--classic", "--render-scale", "--fps", "--no-wide", "--smooth-shading", "--true-night", "--fullscreen"],
      pairs=[("--game-dir", os.path.join(work, "gamedir")), ("--frame-ticks", "7")])
if not a[0].lower().endswith("f117-enhanced.exe"): fails.append("defaults: program %s" % a[0])
a = run("scale4", "RenderScale=4\nWide=0\nSmoothShading=1\nTrueNight=1\nFullscreen=1\n")
check("scale4", a, present=["--no-wide", "--smooth-shading", "--true-night", "--fullscreen"], absent=["--classic"], pairs=[("--render-scale", "4")])
a = run("classic", "RenderScale=3\nWide=0\nSmoothShading=1\nTrueNight=1\nFrameRate=3\nClassic=1\n")
check("classic", a, present=["--classic"], absent=["--render-scale", "--fps", "--no-wide", "--smooth-shading", "--true-night"])
a = run("fps_monitor", "FrameRate=0\n")
check("fps_monitor", a, absent=["--fps"])
a = run("fps_60", "FrameRate=1\n")
check("fps_60", a, absent=["--classic"], pairs=[("--fps", "60")])
a = run("fps_30", "FrameRate=2\n")
check("fps_30", a, absent=["--classic"], pairs=[("--fps", "30")])
a = run("fps_off", "FrameRate=3\n")
check("fps_off", a, absent=["--classic"], pairs=[("--fps", "off")])
a = run("classic_fps60", "FrameRate=1\nClassic=1\n")
check("classic_fps60", a, present=["--classic"], absent=["--fps"])
a = run("auto_night", "RenderScale=0\nTrueNight=1\nFrameTicks=0\n")
check("auto_night", a, present=["--true-night"], absent=["--render-scale", "--no-wide"], pairs=[("--frame-ticks", "0")])
if fails:
    sys.exit("\n".join(fails))
print("launcher_args OK")
