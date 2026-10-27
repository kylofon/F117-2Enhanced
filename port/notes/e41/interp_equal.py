"""S4.1: the display stream leaves the sim alone, and the camera it shows moves evenly (ctests `sim_interp_equal`, `enh_fps_jerk`; DESIGN.md 4.8).

sim_interp_equal: the scripted chain of `debrief_chain_sea` (START: a new pilot, a strike over Libya -> VGAME: 40 frames of flight, a dive into the sea, the real
crash -> END -> START saves the roster -> quit) on the virtual clock (F117_VCLOCK=1: every run is the same machine run tick for tick), window 1280 x 720 (a wide
canvas, so a refresh renders the wide pass and the main pass), with --sandbox-hash (per VGAME frame a hash of DGROUP, comm block, LCG, sound driver, OPL stream,
mem[], VGA and the canvas):
  off     --fps off                       (no display refresh: the S3.4 path, one wide pass per frame)
  test    --fps-test 4                    (4 refreshes per frame at alpha 1/4 .. 1, the own aircraft and camera interpolated)
  f60     --fps 60                        (refreshes timed by the display clock: their number and alphas differ from run to run)
  vsync   --fps vsync                     (the default)
  night   --fps-test 3 --true-night --smooth-shading with the night palette (F117_TEST_NIGHT=2), against the same with --fps off
Every frame line (canvas included), the final comm block, the OPL stream and ROSTER.FIL must be identical to `off`, with no forbidden call in a pass and the
refreshes really run.

enh_fps_jerk: the real-path chain (the airbase, a site selected) with the test pose (F117_TEST_POSE: level banked flight at 3000 ft, 380 kt from frame 60) and the
view sweep of r3d_hr_wide_realpath (cockpit, second cockpit, left, right, rear, front, chase, delayed, side, target views, cockpit), window 1280 x 720, N = 2,
--fps-test 4 --fps-trace: per refresh the camera view_frame built (B288 / B752 / B75C, E44C / C07C / B770). Per frame that interpolates, in the views whose camera
follows the own aircraft only (0, 0x41..0x44, 0x85..0x87): the 4 steps of each camera component from the previous frame's last refresh (the canonical frame
k-1) are even (each within 2 + 3 % of the frame's whole move of the mean) and none goes backward; the frames that hold (start, view change, jump) show one
picture (all their refreshes equal). Then a real-time run (no virtual clock) of the same with --fps 60: alpha never decreases inside a frame and stays in 0..1, and
the cockpit camera never steps backward, also across a sim step.

usage: python interp_equal.py <f117 executable> <game dir> <work dir> [equal|jerk]          exit 77 without the game files
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'e24'))
sys.path.insert(0, os.path.join(HERE, '..', 'e32'))
sys.path.insert(0, os.path.join(HERE, '..', 'e34'))
import sandbox_hash as SH

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
WINDOW = '1280x720'


def chain(exe, game, work, name, extra, env_extra=None):
    """the debrief_chain_sea chain (sandbox_hash.py run) in a wide window, with extra env"""
    os.environ['F117_WINDOW'] = WINDOW
    for k, v in (env_extra or {}).items():
        os.environ[k] = v
    try:
        return SH.run(exe, game, work, name, extra, window=WINDOW)
    finally:
        for k in (env_extra or {}):
            os.environ.pop(k, None)
        os.environ.pop('F117_WINDOW', None)


def display_line(d):
    for l in open(os.path.join(d, 'hash.txt')):
        if l.startswith('display: '):
            return l.strip()
    return ''


def compare(a, b, what):
    fa, fb = a['frames'], b['frames']
    if len(fa) != len(fb):
        return '%s: %d frames, want %d' % (what, len(fb), len(fa))
    for i, (la, lb) in enumerate(zip(fa, fb)):
        if la != lb:
            diff = [x.split('=')[0] for x, y in zip(la.split(), lb.split()) if x != y]
            return '%s: VGAME frame %d differs (%s)\n  off  %s\n  test %s' % (what, i, ', '.join(diff), la, lb)
    fin = lambda r: re.sub(r' passes=\d+ violations=\d+', '', r['final'])
    if fin(a) != fin(b) or a['roster'] != b['roster']:
        return '%s: the final comm block / OPL stream / roster differ\n  off  %s\n  test %s' % (what, a['final'], b['final'])
    v = int(re.search(r'violations=(\d+)', b['final']).group(1))
    if v:
        return '%s: %d forbidden calls inside a pass' % (what, v)
    return None


def equal(exe, game, work):
    off = chain(exe, game, work, 'off', '--fps off')
    if off is None:
        return 1
    if len(off['frames']) < 20:
        print('sim_interp_equal FAILED: only %d VGAME frames' % len(off['frames']))
        return 1
    out = []
    for name, extra in (('test', '--fps-test 4'), ('f60', '--fps 60'), ('vsync', '--fps vsync')):
        r = chain(exe, game, work, name, extra)
        if r is None:
            return 1
        e = compare(off, r, name)
        dl = display_line(os.path.join(work, name))
        m = re.search(r'display: (\d+) refreshes in (\d+) stream frames \((\d+) interpolated\)', dl)
        if not e and not m:
            e = '%s: no display refresh ran' % name
        if not e and name == 'test' and int(m.group(1)) < 4 * int(m.group(2)):
            e = 'test: %s refreshes in %s stream frames, want 4 each' % (m.group(1), m.group(2))
        if not e and int(m.group(3)) < 10:
            e = '%s: only %s interpolated refreshes' % (name, m.group(3))
        if e:
            print('sim_interp_equal FAILED: ' + e)
            return 1
        out.append('%s %s/%s/%s (%.0f s)' % (name, m.group(1), m.group(2), m.group(3), r['secs']))
    night = {'F117_TEST_NIGHT': '2'}
    noff = chain(exe, game, work, 'night_off', '--fps off --true-night --smooth-shading', night)
    non = chain(exe, game, work, 'night_on', '--fps-test 3 --true-night --smooth-shading', night)
    if noff is None or non is None:
        return 1
    e = compare(noff, non, 'night')
    if e:
        print('sim_interp_equal FAILED: ' + e)
        return 1
    print('sim_interp_equal OK: %d VGAME frames, every frame hash (DGROUP, comm, LCG, sound, OPL, mem, VGA, canvas), the final comm block, OPL stream and roster '
          'identical to --fps off, 0 forbidden calls; refreshes / stream frames / interpolated: %s; night + smooth shading --fps-test 3 identical to its --fps off run '
          '(%d frames); %s' % (len(off['frames']), ', '.join(out), len(noff['frames']), display_line(os.path.join(work, 'test'))))
    return 0


# ---------------------------------------------------------------- the jerk trace

OWN_VIEWS = {0x00, 0x41, 0x42, 0x43, 0x44, 0x85, 0x86, 0x87}   # the camera follows the own aircraft only (the others also follow objects / the pose ring: S4.2)
TRACE_RE = re.compile(r'^(\d+) (\d+) ([\d.]+) ([\d.]+) (\d) (\S+) view=(\w+) cam=(-?\d+),(-?\d+),(\d+) ang=(\d+),(\d+),(\d+)')


def read_trace(path):
    frames = {}
    for l in open(path):
        m = TRACE_RE.match(l)
        if not m:
            continue
        g = m.groups()
        r = dict(frame=int(g[0]), j=int(g[1]), alpha=float(g[2]), ms=float(g[3]), lerp=g[4] == '1', why=g[5], view=int(g[6], 16),
                 cam=(int(g[7]), int(g[8]), int(g[9])), ang=(int(g[10]), int(g[11]), int(g[12])))
        frames.setdefault(r['frame'], []).append(r)
    return frames


def comps(r):
    return list(r['cam']) + list(r['ang'])


def delta(a, b, i):
    d = b - a
    if i >= 3:                                   # an angle: the shortest 16-bit arc
        d = (d + 0x8000) % 0x10000 - 0x8000
    return d


def jerk_check(frames, k):
    stats = dict(lerp=0, hold=0, holds={}, checked=0, uneven=0, back=0, worst=0.0, other_views=0, hold_bad=0, steps=0)
    bad = []
    keys = sorted(frames)
    for n, f in enumerate(keys):
        rs = sorted(frames[f], key=lambda r: r['j'])
        if not rs[0]['lerp']:
            stats['hold'] += 1
            stats['holds'][rs[0]['why']] = stats['holds'].get(rs[0]['why'], 0) + 1
            if any(comps(r) != comps(rs[0]) for r in rs):
                stats['hold_bad'] += 1
                bad.append('frame %d holds (%s) but its refreshes differ' % (f, rs[0]['why']))
            continue
        stats['lerp'] += 1
        if rs[0]['view'] not in OWN_VIEWS:
            stats['other_views'] += 1
            continue
        if n == 0 or keys[n - 1] != f - 1 or len(rs) != k:
            continue
        prev = sorted(frames[f - 1], key=lambda r: r['j'])[-1]   # the canonical frame k-1 (its last refresh, alpha 1)
        if prev['alpha'] != 1.0 or prev['view'] != rs[0]['view']:
            continue
        seq = [prev] + rs
        stats['checked'] += 1
        for i in range(6):
            steps = [delta(comps(seq[j])[i], comps(seq[j + 1])[i], i) for j in range(k)]
            total = sum(steps)
            mean = total / k
            tol = 2 + 0.03 * abs(total)
            dev = max(abs(s - mean) for s in steps)
            stats['steps'] += k
            if abs(total) >= 40:
                stats['worst'] = max(stats['worst'], dev / abs(mean) if mean else 0)
            if dev > tol:
                stats['uneven'] += 1
                bad.append('frame %d component %d: steps %s (mean %.1f) uneven' % (f, i, steps, mean))
            if total and any(s * total < 0 and abs(s) > 2 for s in steps):
                stats['back'] += 1
                bad.append('frame %d component %d: a backward step %s' % (f, i, steps))
    return stats, bad


def jerk_run(exe, game, work, name, extra, vclock=True):
    import real_path as RP
    import wide_realpath as WR
    os.environ['F117_WINDOW'] = WINDOW
    os.environ['SEL'] = str(WR.SELECT)
    os.environ['F117_TEST_POSE'] = '60'
    tr = os.path.join(work, name, 'trace.txt')
    os.makedirs(os.path.dirname(tr), exist_ok=True)
    if os.path.exists(tr):
        os.remove(tr)
    t = time.time()
    try:
        if vclock:
            p, _ = RP.run(exe, game, work, name, 2, WR.KEYS, '', frames=WR.FRAMES, extra='--wide --fps-trace %s %s' % (tr.replace(os.sep, '/'), extra))
        else:                                    # real time: the same command line without the virtual clock
            d = os.path.join(work, name)
            g = SH.fresh_game(game, os.path.join(d, 'game'))
            env = dict(os.environ, F117_KEYS=WR.KEYS, F117_SHELL_SCRIPT=os.path.join(HERE, '..', 's54', 'shell_chain_libya.txt'), F117_VGAME_FRAMES='300',
                       F117_END_SCRIPT=os.path.join(HERE, '..', 's58', 'end_chain.txt'), F117_VGAME_SELECT=str(WR.SELECT), F117_VGAME_SITE='0',
                       F117_ENH_ARGS='--render-scale 2 --wide --fps-trace %s %s' % (tr.replace(os.sep, '/'), extra))
            env.pop('F117_CLASSIC', None)
            env.pop('F117_VCLOCK', None)
            p = subprocess.run([exe, '--headless', '--trace', '--game-dir', g], env=env, capture_output=True, text=True, timeout=900)
            open(os.path.join(d, 'stdout.txt'), 'w').write(p.stdout)
    finally:
        for k in ('F117_WINDOW', 'SEL', 'F117_TEST_POSE'):
            os.environ.pop(k, None)
    return p, tr, time.time() - t


def jerk(exe, game, work):
    k = 4
    p, tr, secs = jerk_run(exe, game, work, 'jerk', '--fps-test %d' % k)
    if p.returncode != 0 or not os.path.isfile(tr):
        print('enh_fps_jerk FAILED: the run ended with %d (%s)' % (p.returncode, p.stderr[-400:]))
        return 1
    frames = read_trace(tr)
    st, bad = jerk_check(frames, k)
    views = sorted({r['view'] for rs in frames.values() for r in rs})
    ok = not bad and st['checked'] >= 150 and all(w in st['holds'] for w in ('start', 'view', 'jump')) and len(views) >= 10
    print('enh_fps_jerk (virtual clock, %d refreshes a frame at alpha 1/%d..1): %d frames traced (%d interpolated, %d holding: %s), views %s; %d own-camera frames checked '
          '(%d steps of 6 components from the canonical frame k-1): uneven %d, backward %d, worst deviation %.1f %% of the mean step (moves >= 40); holding frames with differing '
          'refreshes %d; %.0f s%s' % (k, k, len(frames), st['lerp'], st['hold'], ', '.join('%s %d' % kv for kv in sorted(st['holds'].items())),
                                     ' '.join('%x' % v for v in views), st['checked'], st['steps'], st['uneven'], st['back'], 100 * st['worst'], st['hold_bad'], secs,
                                     '' if ok else '  BAD'))
    for b in bad[:10]:
        print('  ' + b)
    if not ok:
        print('enh_fps_jerk FAILED')
        return 1
    # real time: alpha from the display clock
    p, tr, secs = jerk_run(exe, game, work, 'jerk_rt', '--fps 60', vclock=False)
    if p.returncode != 0 or not os.path.isfile(tr):
        print('enh_fps_jerk FAILED: the real-time run ended with %d (%s)' % (p.returncode, p.stderr[-400:]))
        return 1
    frames = read_trace(tr)
    keys = sorted(frames)
    adec = arange = back = boundary = checked = 0
    per_frame = []
    last = None
    for f in keys:
        rs = sorted(frames[f], key=lambda r: r['j'])
        per_frame.append(len(rs))
        for a, b in zip(rs, rs[1:]):
            if b['alpha'] < a['alpha']:
                adec += 1
        arange += sum(1 for r in rs if not 0 <= r['alpha'] <= 1)
        for r in rs:
            if last is not None and r['view'] == 0 and last['view'] == 0 and r['lerp'] and last['frame'] >= f - 1:
                checked += 1
                for i in (0, 1, 3):                   # x, y and heading of the cockpit camera in the banked turn
                    d = delta(comps(last)[i], comps(r)[i], i)
                    want = delta(comps(sorted(frames[f - 1], key=lambda q: q['j'])[-1])[i] if f - 1 in frames else comps(last)[i], comps(rs[-1])[i], i)
                    if d * want < 0 and abs(d) > 2:
                        back += 1
                        if r['j'] == 1:
                            boundary += 1
            last = r
    ok = not adec and not arange and not back and checked >= 100
    print('enh_fps_jerk real time (--fps 60, display clock): %d frames, %.1f refreshes a frame on average (%d..%d), alpha decreasing inside a frame %d, out of 0..1 %d; '
          'cockpit camera refreshes checked %d, backward steps %d (at a sim step %d); %.0f s%s' % (len(keys), sum(per_frame) / max(1, len(per_frame)), min(per_frame or [0]),
                                                                                          max(per_frame or [0]), adec, arange, checked, back, boundary, secs, '' if ok else '  BAD'))
    if not ok:
        print('enh_fps_jerk FAILED')
        return 1
    print('enh_fps_jerk OK')
    return 0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    exe, game, work = (os.path.abspath(a) for a in sys.argv[1:4])
    mode = sys.argv[4] if len(sys.argv) > 4 else 'equal'
    if not os.path.isfile(os.path.join(game, 'END.EXE')) or not os.path.isfile(os.path.join(ROOT, 'work', 'unpacked', 'START.EXE')):
        print('no game files')
        return 77
    os.makedirs(work, exist_ok=True)
    return equal(exe, game, work) if mode == 'equal' else jerk(exe, game, work)


if __name__ == '__main__':
    sys.exit(main())
