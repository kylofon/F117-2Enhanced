"""S4.3: one render per display refresh (ctest `enh_one_render`).

S4.1 built a refresh of an extended view from two passes (the wide pass for the 3D picture, then the main pass for the page, the HUD and the MFDs). S4.3 renders
it once (the wide pass composes its own picture) and, at alpha 1 in a view that is not extended, shows the canonical frame's own render without any pass.
`--fps-check` builds every refresh both ways (the S4.1 way first) and compares the two pictures pixel by pixel.

Runs (headless, virtual clock: every run is the same machine run; --fps-test 4 --fps-check, --render-scale 2):
  sweep      the real-path view sweep of r3d_hr_wide_realpath (the home airbase, every view: extended, the second cockpit view, the frame-only views), 16:9 window
  night      the airbase in banked flight (F117_TEST_POSE) with --smooth-shading --true-night and the night palette, 16:9 window
  reuse      the same banked flight with --no-wide (canvas = frame): the 4th refresh of a frame (alpha 1) is the canonical render, compared with a pass;
             the left view from 62 s (S4.6: in the cockpit views the map and the scope MFDs are redrawn per refresh at sub-pixel places, so a refresh at
             alpha 1 is a pass there, not the canonical render; they are compared in the other runs)
  reuse_n    the same at night with smooth shading
  each: 0 refreshes that differ, every refresh compared, 0 forbidden calls; reuse runs: a quarter of the refreshes reused.
Then the rate fallback, in real time (display clock, --fps 60): with F117_TEST_REFRESH_MS=20 (every pass 20 ms slower, as on a slow machine) most refreshes
must come at every 2nd display period (30 Hz) and the rate must not flap (few switches).

usage: python one_render.py <f117 executable> <game dir> <work dir>      exit 77 without the game files
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'e24'))
sys.path.insert(0, os.path.join(HERE, '..', 'e34'))
import sandbox_hash as SH
import wide_realpath as WR

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
S54 = os.path.join(HERE, '..', 's54')
S58 = os.path.join(HERE, '..', 's58')


def run(exe, game, work, name, window, extra, keys='1:31,2:03,11:39', frames=200, select='50', pose='60', night=False, vclock=True, env_extra=None):
    d = os.path.join(work, name)
    g = SH.fresh_game(game, os.path.join(d, 'game'))
    env = dict(os.environ, F117_KEYS=keys, F117_SHELL_SCRIPT=os.path.join(S54, 'shell_chain_libya.txt'), F117_VGAME_FRAMES=str(frames),
               F117_END_SCRIPT=os.path.join(S58, 'end_chain.txt'), F117_VCLOCK='1', F117_VGAME_SELECT=select, F117_VGAME_SITE='0',
               F117_WINDOW=window, F117_ENH_ARGS='--render-scale 2 --wide ' + extra)
    for k in ('F117_CLASSIC', 'F117_TEST_NIGHT', 'F117_TEST_POSE', 'F117_TEST_REFRESH_MS'):
        env.pop(k, None)
    if pose:
        env['F117_TEST_POSE'] = pose
    if night:
        env['F117_TEST_NIGHT'] = '2'
    if not vclock:
        env.pop('F117_VCLOCK', None)
    env.update(env_extra or {})
    p = subprocess.run([exe, '--headless', '--game-dir', g], env=env, capture_output=True, text=True, timeout=1200)
    open(os.path.join(d, 'stdout.txt'), 'w').write(p.stdout)
    open(os.path.join(d, 'stderr.txt'), 'w').write(p.stderr)
    return p.stdout


REUSE_KEYS = '1:31,2:03,11:39,62:2a+32'   # S4.6: the left view (no MFDs; the map and the scope are redrawn per refresh in the cockpit views)


def field(out, pat):
    m = re.search(pat, out)
    return [int(x) for x in m.groups()] if m else None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    exe, game, work = (os.path.abspath(a) for a in sys.argv[1:4])
    if not os.path.isfile(os.path.join(game, 'END.EXE')) or not os.path.isfile(os.path.join(ROOT, 'work', 'unpacked', 'START.EXE')):
        print('no game files')
        return 77
    os.makedirs(work, exist_ok=True)
    bad, lines = [], []
    runs = [
        ('sweep', '1280x720', '--fps-test 4 --fps-check', dict(keys=WR.KEYS, frames=WR.FRAMES, select=str(WR.SELECT), pose='')),
        ('night', '1280x720', '--fps-test 4 --fps-check --smooth-shading --true-night', dict(night=True)),
        ('reuse', '1280x720', '--fps-test 4 --fps-check --no-wide', dict(keys=REUSE_KEYS)),
        ('reuse_n', '1280x720', '--fps-test 4 --fps-check --no-wide --smooth-shading --true-night', dict(night=True, keys=REUSE_KEYS)),
    ]
    for name, win, extra, kw in runs:
        out = run(exe, game, work, name, win, extra, **kw)
        chk = field(out, r'fps-check: (\d+) refreshes compared, (\d+) differ, (\d+) pixels differ')
        disp = field(out, r'display: (\d+) refreshes in (\d+) stream frames')
        reuse = field(out, r'display-reuse: (\d+) refreshes')
        viol = field(out, r'violations (\d+)')
        if not chk or not disp:
            bad.append('%s: no fps-check / display line (see %s)' % (name, os.path.join(work, name, 'stdout.txt')))
            continue
        if chk[1] or chk[2]:
            bad.append('%s: %d of %d refreshes differ from the two-pass picture (%d pixels)' % (name, chk[1], chk[0], chk[2]))
        if chk[0] != disp[0] or chk[0] < 100:
            bad.append('%s: %d refreshes compared of %d' % (name, chk[0], disp[0]))
        if viol and viol[0]:
            bad.append('%s: %d forbidden calls in a pass' % (name, viol[0]))
        if name.startswith('reuse') and (not reuse or reuse[0] * 5 < disp[0]):
            bad.append('%s: only %s of %d refreshes reused the canonical render' % (name, reuse and reuse[0], disp[0]))
        lines.append('%s %d refreshes identical%s' % (name, chk[0], ' (%d reused)' % reuse[0] if reuse and reuse[0] else ''))
    out = run(exe, game, work, 'fallback', '1280x720', '--fps 60', vclock=False, env_extra={'F117_TEST_REFRESH_MS': '20'})
    rate = field(out, r'display-rate: refreshes at every display period (\d+), every 2nd (\d+), every 3rd or later (\d+); rate switches (\d+)')
    if not rate:
        bad.append('fallback: no display-rate line')
    else:
        tot = rate[0] + rate[1] + rate[2]
        if not tot or rate[1] < 0.8 * tot or rate[3] > 4:
            bad.append('fallback: refreshes at every period %d, every 2nd %d, slower %d, switches %d (want mostly every 2nd, few switches)' % tuple(rate))
        lines.append('fallback (+20 ms a pass, --fps 60): %d of %d refreshes at 30 Hz, %d switches' % (rate[1], tot, rate[3]))
    if bad:
        print('enh_one_render FAILED:\n  ' + '\n  '.join(bad))
        return 1
    print('enh_one_render OK: ' + '; '.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main())
