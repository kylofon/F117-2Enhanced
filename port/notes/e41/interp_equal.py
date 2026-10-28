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
--fps-test 4 --fps-trace: per refresh the camera view_frame built (B288 / B752 / B75C, E44C / C07C / B770). Per frame that interpolates, in every view (S4.1: the
own-aircraft views only; S4.2 adds the delayed view's pose ring and the target views' objects): the 4 steps of each camera component from the previous frame's last refresh (the canonical frame
k-1) are even (each within 2 + 3 % of the frame's whole move of the mean) and none goes backward; the frames that hold (start, view change, jump) show one
picture (all their refreshes equal). S4.2, the per-object-class jerk trace: per interpolating object (units, projectiles, tracers, smoke, wreck, spinners, the
pose ring, the flight-path marker offset) and frame the 4 values its refreshes showed step evenly from its F_{k-1} value, never backward, and end on F_k
(the classes this chain has must appear; tracers / smoke / the wreck are covered by ctest enh_interp_rules). Then a real-time run (no virtual clock) of the same with --fps 60: alpha never decreases inside a frame and stays in 0..1, and
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


def objects_line(d):
    for l in open(os.path.join(d, 'hash.txt')):
        if l.startswith('display-objects: '):
            return l.strip()
    return ''


def objects_lerped(line, cls):
    m = re.search(r' %s (\d+)/(\d+)' % cls, line)
    return int(m.group(1)) if m else 0


def airbase_chain(exe, game, work, name, extra):
    """S4.2: the real-path chain of enh_fps_jerk (the airbase: taxiing traffic, the delayed and target views, projectiles) with --sandbox-hash"""
    import real_path as RP
    import wide_realpath as WR
    os.environ['F117_WINDOW'] = WINDOW
    os.environ['SEL'] = str(WR.SELECT)
    os.environ['F117_TEST_POSE'] = '60'
    d = os.path.join(work, name)
    os.makedirs(d, exist_ok=True)
    hf = os.path.join(d, 'hash.txt')
    if os.path.exists(hf):
        os.remove(hf)
    t = time.time()
    try:
        p, _ = RP.run(exe, game, work, name, 2, WR.KEYS, '', frames=WR.FRAMES, extra='--wide --sandbox-hash %s %s' % (hf.replace(os.sep, '/'), extra))
    finally:
        for k in ('F117_WINDOW', 'SEL', 'F117_TEST_POSE'):
            os.environ.pop(k, None)
    if p.returncode != 0 or not os.path.isfile(hf):
        print('sim_interp_equal FAILED: the airbase run %s ended with %d' % (name, p.returncode))
        return None
    lines = open(hf).read().splitlines()
    fin = [l for l in lines if l.startswith('final ')]
    return dict(frames=[l for l in lines if l[:1].isdigit()], final=fin[0] if fin else '', roster=b'', secs=time.time() - t)


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
        if not e and name == 'test' and objects_lerped(objects_line(os.path.join(work, name)), 'units') < 10:
            e = 'test: the object pass did not run (%s)' % objects_line(os.path.join(work, name))
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
    # S4.2: the object pass, where there are objects to interpolate (units, projectiles, the pose ring, the target views)
    aoff = airbase_chain(exe, game, work, 'airbase_off', '--fps off')
    aon = airbase_chain(exe, game, work, 'airbase_on', '--fps-test 4')
    if aoff is None or aon is None:
        return 1
    e = compare(aoff, aon, 'airbase') if len(aoff['frames']) >= 400 else 'airbase: only %d frames' % len(aoff['frames'])
    aobj = objects_line(os.path.join(work, 'airbase_on'))
    if not e and not all(objects_lerped(aobj, c) > 0 for c in ('units', 'projectiles', 'pose-ring')):
        e = 'airbase: the object pass did not interpolate units, projectiles and the pose ring (%s)' % aobj
    if e:
        print('sim_interp_equal FAILED: ' + e)
        return 1
    print('sim_interp_equal OK: %d VGAME frames, every frame hash (DGROUP, comm, LCG, sound, OPL, mem, VGA, canvas), the final comm block, OPL stream and roster '
          'identical to --fps off, 0 forbidden calls; refreshes / stream frames / interpolated: %s; night + smooth shading --fps-test 3 identical to its --fps off run '
          '(%d frames); %s; S4.2 object pass: airbase chain --fps-test 4 identical to --fps off (%d frames), %s' % (len(off['frames']), ', '.join(out), len(noff['frames']),
                                                                                   display_line(os.path.join(work, 'test')), len(aoff['frames']), aobj))
    return 0


# ---------------------------------------------------------------- the jerk trace

OWN_VIEWS = set(range(0x100))   # S4.2: every view (S4.1 checked only the own-aircraft cameras; the delayed and target views follow the pose ring / objects)
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


OBJ_RE = re.compile(r'^o (\d+) (\d+) (\w+) (\d+) v=(\S+) a=(\S+) b=(\S+)')
OBJ_CLASSES = ('unit', 'proj', 'tracer', 'smoke', 'wreck', 'spin', 'ring', 'fpm')


def obj_delta(cls, i, a, b):
    d = b - a
    if cls == 'spin' or (cls in ('unit', 'ring') and i < 2):   # 32-bit world positions; the spinners are unwrapped in the trace
        return d
    return (d + 0x8000) % 0x10000 - 0x8000                      # 16-bit cells, heights, angles: the shortest arc


def obj_jerk(path, k):
    """S4.2: the per-object-class jerk trace. Per interpolating object and frame: the k values its refreshes showed, from its F_{k-1} value (a), must step
    evenly (each within 2 + 3 % of the frame's move of the mean), never backward, and end on F_k (b)"""
    objs = {}
    for l in open(path):
        m = OBJ_RE.match(l)
        if m:
            g = m.groups()
            objs.setdefault((int(g[0]), g[2], int(g[3])), []).append((int(g[1]), [int(x) for x in g[4].split(',')], [int(x) for x in g[5].split(',')],
                                                                      [int(x) for x in g[6].split(',')]))
    st = {c: dict(frames=0, uneven=0, back=0, off=0, worst=0.0, partial=0) for c in OBJ_CLASSES}
    bad = []
    for (f, cls, slot), rows in sorted(objs.items()):
        rows.sort()
        s = st[cls]
        if len(rows) != k:
            s['partial'] += 1
            continue
        s['frames'] += 1
        a, b = rows[0][2], rows[0][3]
        if rows[-1][1] != b:
            s['off'] += 1
            bad.append('frame %d %s %d: the last refresh %s is not F_k %s' % (f, cls, slot, rows[-1][1], b))
        for i in range(4):
            seq = [a[i]] + [r[1][i] for r in rows]
            steps = [obj_delta(cls, i, seq[j], seq[j + 1]) for j in range(k)]
            total = sum(steps)
            mean = total / k
            dev = max(abs(x - mean) for x in steps)
            if abs(total) >= 40 and mean:
                s['worst'] = max(s['worst'], dev / abs(mean))
            if dev > 2 + 0.03 * abs(total):
                s['uneven'] += 1
                bad.append('frame %d %s %d component %d: steps %s uneven' % (f, cls, slot, i, steps))
            if total and any(x * total < 0 and abs(x) > 2 for x in steps):
                s['back'] += 1
                bad.append('frame %d %s %d component %d: a backward step %s' % (f, cls, slot, i, steps))
    return st, bad


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
    ok = not bad and st['checked'] >= 400 and all(w in st['holds'] for w in ('start', 'view', 'jump')) and len(views) >= 10
    print('enh_fps_jerk (virtual clock, %d refreshes a frame at alpha 1/%d..1): %d frames traced (%d interpolated, %d holding: %s), views %s; %d camera frames checked (every view) '
          '(%d steps of 6 components from the canonical frame k-1): uneven %d, backward %d, worst deviation %.1f %% of the mean step (moves >= 40); holding frames with differing '
          'refreshes %d; %.0f s%s' % (k, k, len(frames), st['lerp'], st['hold'], ', '.join('%s %d' % kv for kv in sorted(st['holds'].items())),
                                     ' '.join('%x' % v for v in views), st['checked'], st['steps'], st['uneven'], st['back'], 100 * st['worst'], st['hold_bad'], secs,
                                     '' if ok else '  BAD'))
    for b in bad[:10]:
        print('  ' + b)
    if not ok:
        print('enh_fps_jerk FAILED')
        return 1
    ost, obad = obj_jerk(tr, k)
    need = ('unit', 'proj', 'spin', 'ring', 'fpm')                 # in this chain (tracers, smoke and the wreck: ctest enh_interp_rules)
    ok = not obad and all(ost[c]['frames'] > 0 for c in need) and ost['unit']['frames'] >= 500
    print('enh_fps_jerk objects (S4.2, per class: object-frames checked / uneven / backward / not ending on F_k, worst deviation of the mean step): %s%s'
          % ('; '.join('%s %d/%d/%d/%d %.1f %%' % (c, v['frames'], v['uneven'], v['back'], v['off'], 100 * v['worst']) for c, v in ost.items()), '' if ok else '  BAD'))
    for b in obad[:10]:
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
