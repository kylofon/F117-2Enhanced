"""S4.3 profile: the cost of a display refresh by stage (snapshot, render, compose, the whole refresh; take / present when a window shows them).

The real-path chain of enh_fps_jerk (the home airbase, F117_TEST_POSE: level banked flight at 3000 ft from VGAME frame 60), headless on the virtual clock,
--fps-test 4 (4 refreshes per frame: the same refreshes in every run), --render-scale 4 --wide, in the first cockpit view (extended), for each window
size and with / without --smooth-shading --true-night (night palette, F117_TEST_NIGHT=2). Prints the display-stages line of each run (ms avg / p95).

usage: profile.py EXE GAME WORK [WxH ...] [--frames N] [--extra "ARGS"]
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'e24'))
import sandbox_hash as SH

S54 = os.path.join(HERE, '..', 's54')
S58 = os.path.join(HERE, '..', 's58')


def run(exe, game, work, name, window, n, extra, night, frames):
    d = os.path.join(work, name)
    g = SH.fresh_game(game, os.path.join(d, 'game'))
    env = dict(os.environ, F117_KEYS='1:31,2:03,11:39', F117_SHELL_SCRIPT=os.path.join(S54, 'shell_chain_libya.txt'), F117_VGAME_FRAMES=str(frames),
               F117_END_SCRIPT=os.path.join(S58, 'end_chain.txt'), F117_VCLOCK='1', F117_VGAME_SELECT='50', F117_VGAME_SITE='0', F117_TEST_POSE='60',
               F117_WINDOW=window, F117_ENH_ARGS='--render-scale %d --wide %s' % (n, extra))
    env.pop('F117_CLASSIC', None)
    if os.environ.get('PROFILE_REALTIME'):      # real time (no virtual clock): the display clock paces the refreshes
        env.pop('F117_VCLOCK', None)
    else:                                       # per refresh picture hash (an optimisation must leave every one equal; not in real time: it costs ~10 ms)
        env['F117_DISP_HASH'] = os.path.join(d, 'disp_hash.txt')
    if night:
        env['F117_TEST_NIGHT'] = '2'
    else:
        env.pop('F117_TEST_NIGHT', None)
    cmd = [exe, '--headless', '--game-dir', g]
    if os.environ.get('PROFILE_WINDOW'):        # a real window (--scale S --aspect 1:1, e.g. '6'): the present stage is measured too; no sound
        cmd = [exe, '--scale', os.environ['PROFILE_WINDOW'], '--aspect', '1:1', '--game-dir', g]
        env['SDL_AUDIO_DRIVER'] = 'dummy'
    if os.environ.get('PROFILE_WRAP'):          # a sampling profiler that runs the command (work/e43/sampler.exe OUT cmd...)
        cmd = [os.environ['PROFILE_WRAP'], os.path.join(d, 'samples.txt')] + cmd
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1800)
    open(os.path.join(d, 'stdout.txt'), 'w').write(p.stdout)
    st = [l for l in p.stdout.splitlines() if l.startswith('display')]
    return st


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        return 2
    exe, game, work = (os.path.abspath(x) for x in a[:3])
    rest = a[3:]
    frames, extra, wins, modes = 200, '--fps-test 4', [], ('day', 'night')
    i = 0
    while i < len(rest):
        if rest[i] == '--frames':
            frames = int(rest[i + 1]); i += 2
        elif rest[i] == '--extra':
            extra = rest[i + 1]; i += 2
        elif rest[i] == '--mode':
            modes = tuple(rest[i + 1].split(',')); i += 2
        else:
            wins.append(rest[i]); i += 1
    wins = wins or ['1920x1080', '2560x1440']
    for w in wins:
        for m in modes:
            night = m == 'night'
            ex = extra + (' --smooth-shading --true-night' if night else '')
            st = run(exe, game, work, '%s_%s' % (w, m), w, 4, ex, night, frames)
            print('%s %s:' % (w, m))
            for l in st:
                print('  ' + l)
    return 0


if __name__ == '__main__':
    sys.exit(main())
