"""S2.4: the sandbox hash test (ctest `enh_sandbox_hash`, DESIGN.md 4.2 / 5.3).

The scripted chain of `debrief_chain_sea` (START: a new pilot, a strike over Libya -> VGAME: 40 frames of flight, a dive into the sea, the
real crash -> END: killed in action -> START saves the roster -> quit) runs twice on the virtual clock (F117_VCLOCK=1, so both runs are the
same machine run tick for tick), with the enhancements on:
  base  --sandbox-hash FILE                      (no display pass)
  test  --sandbox-hash FILE --sandbox-test K     (K sandboxed display passes in every VGAME frame, rendered from the frame's pre-render image
                                                 with a test view: another heading and position per pass, sandbox.c test_view)
  classic --classic --sandbox-hash FILE          (the faithful path: everything but the canvas hash must be the same as base)
  wide  --sandbox-hash FILE --wide, window 1280 x 720 (S3.4: a canvas wider than the frame, so every frame of the first cockpit view gets a wide
                                                 display pass with the culling widened to the canvas: everything but the canvas hash must be the same as base)
Per VGAME frame the port writes a hash of the sim state the previous step left: the whole DGROUP, the comm block, the LCG 926E, the sound
driver's memory and the OPL register stream so far, the whole mem[], the VGA model (vram + DAC) and the canvas (shadow plane and layers,
so a leak only the hr picture would show is caught too). Every frame must be identical, and the final comm block and ROSTER.FIL too; the test run
must have run K passes per frame with no forbidden call (host_pump, OPL write, sound poll, file access) inside a pass.

usage: python sandbox_hash.py <f117 executable> <game dir> <work dir> [K]
exit 77 (ctest SKIP) without the game files.
"""
import os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 's54'))
S54 = os.path.join(HERE, '..', 's54')
S58 = os.path.join(HERE, '..', 's58')


def fresh_game(game, dst):
    import shell_emu
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for f in os.listdir(game):
        p = os.path.join(game, f)
        if os.path.isfile(p) and f.upper() != 'ROSTER.FIL':
            shutil.copy2(p, os.path.join(dst, f))
    open(os.path.join(dst, 'ROSTER.FIL'), 'wb').write(shell_emu.make_roster())
    return dst


def run(exe, game, work, name, extra, window=None):
    d = os.path.join(work, name)
    g = fresh_game(game, os.path.join(d, 'game'))
    hf = os.path.join(d, 'hash.txt')
    env = dict(os.environ, F117_KEYS='1:31,2:03,11:39', F117_SHELL_SCRIPT=os.path.join(S54, 'shell_chain_libya.txt'), F117_VGAME_DIVE='40',
               F117_VGAME_FRAMES='400', F117_END_SCRIPT=os.path.join(S58, 'end_chain.txt'), F117_VCLOCK='1',
               F117_ENH_ARGS=('--sandbox-hash %s %s' % (hf, extra)).strip())
    env.pop('F117_CLASSIC', None)
    env.pop('F117_WINDOW', None)
    if window:
        env['F117_WINDOW'] = window                     # S3.4: the window a headless run pretends to have (a wide canvas)
    t = time.time()
    p = subprocess.run([exe, '--headless', '--trace', '--game-dir', g], env=env, capture_output=True, text=True, timeout=900)
    secs = time.time() - t
    open(os.path.join(d, 'stdout.txt'), 'w').write(p.stdout)
    open(os.path.join(d, 'stderr.txt'), 'w').write(p.stderr)
    want = [r'EXEC START\.EXE', r'START\.EXE exit code 0[CD]h', r'EXEC VGAME\.EXE', r'VGAME\.EXE exit code 81h', r'EXEC END\.EXE',
            r'END\.EXE exit code 23h', r'EXEC START\.EXE', r'START\.EXE exit code 00h', r'quit: START returned 0']
    pos = 0
    for w in want:
        m = re.compile(w).search(p.stdout, pos)
        if not m:
            print('enh_sandbox_hash FAILED (%s run): "%s" not found after offset %d\n%s\n%s' % (name, w, pos, p.stdout[-2000:], p.stderr[-800:]))
            return None
        pos = m.end()
    if not os.path.isfile(hf):
        print('enh_sandbox_hash FAILED (%s run): no hash file' % name)
        return None
    lines = open(hf).read().splitlines()
    frames = [l for l in lines if l[:1].isdigit()]
    final = [l for l in lines if l.startswith('final ')]
    stats = [l for l in lines if l.startswith('sandbox: ')]
    roster = open(os.path.join(g, 'ROSTER.FIL'), 'rb').read()
    return dict(frames=frames, final=final[0] if final else '', stats=stats[0] if stats else '', roster=roster, secs=secs)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    exe, game, work = (os.path.abspath(a) for a in sys.argv[1:4])
    k = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    if not os.path.isfile(os.path.join(game, 'END.EXE')) or not os.path.isfile(os.path.join(ROOT, 'work', 'unpacked', 'START.EXE')):
        print('no game files')
        return 77
    os.makedirs(work, exist_ok=True)
    a = run(exe, game, work, 'base', '--fps off')                   # S4.1: no display refresh in these runs (sim_interp_equal tests the stream)
    if a is None:
        return 1
    b = run(exe, game, work, 'test', '--sandbox-test %d --fps off' % k)
    if b is None:
        return 1
    fa, fb = a['frames'], b['frames']
    if len(fa) < 20:
        print('enh_sandbox_hash FAILED: only %d VGAME frames hashed' % len(fa))
        return 1
    for i in range(max(len(fa), len(fb))):
        la = fa[i] if i < len(fa) else '(none)'
        lb = fb[i] if i < len(fb) else '(none)'
        if la != lb:
            pa, pb = la.split(), lb.split()
            diff = [x.split('=')[0] for x, y in zip(pa, pb) if x != y]
            print('enh_sandbox_hash FAILED: VGAME frame %d differs (%s)\n  base %s\n  test %s' % (i, ', '.join(diff) or 'length', la, lb))
            return 1
    fin_a = re.sub(r' passes=\d+ violations=\d+', '', a['final'])
    fin_b = re.sub(r' passes=\d+ violations=\d+', '', b['final'])
    if not fin_a or fin_a != fin_b:
        print('enh_sandbox_hash FAILED: the final comm block / OPL stream differ\n  base %s\n  test %s' % (a['final'], b['final']))
        return 1
    if a['roster'] != b['roster']:
        print('enh_sandbox_hash FAILED: ROSTER.FIL differs')
        return 1
    c = run(exe, game, work, 'classic', '--classic')               # and the faithful path: the same sim (all but the canvas hash)
    if c is None:
        return 1
    strip = lambda l: re.sub(r' canvas=\w+', '', l)
    for i, (la, lc) in enumerate(zip(fa, c['frames'])):
        if strip(la) != strip(lc):
            print('enh_sandbox_hash FAILED: VGAME frame %d differs between --classic and the enhanced run\n  enh     %s\n  classic %s' % (i, la, lc))
            return 1
    if len(c['frames']) != len(fa) or re.sub(r' passes=\d+ violations=\d+', '', c['final']) != fin_a or c['roster'] != a['roster']:
        print('enh_sandbox_hash FAILED: --classic ends differently (%d frames, %s)' % (len(c['frames']), c['final']))
        return 1
    w = run(exe, game, work, 'wide', '--wide --fps off', window='1280x720')   # S3.4: the wide display passes leave the sim as it is
    if w is None:
        return 1
    for i, (la, lw_) in enumerate(zip(fa, w['frames'])):
        if strip(la) != strip(lw_):
            pa, pw = la.split(), lw_.split()
            diff = [x.split('=')[0] for x, y in zip(pa, pw) if x != y]
            print('enh_sandbox_hash FAILED: VGAME frame %d differs between the --wide run and the base run (%s)\n  base %s\n  wide %s' % (i, ', '.join(diff), la, lw_))
            return 1
    mw = re.search(r'passes=(\d+) violations=(\d+)', w['final'])
    wpasses, wviol = int(mw.group(1)), int(mw.group(2))
    if len(w['frames']) != len(fa) or re.sub(r' passes=\d+ violations=\d+', '', w['final']) != fin_a or w['roster'] != a['roster'] or wviol or wpasses < (len(fa) * 3) // 4:
        print('enh_sandbox_hash FAILED: the --wide run ends differently or ran too few wide passes (%d frames, %d passes, %d forbidden calls, %s)' % (len(w['frames']), wpasses, wviol, w['final']))
        return 1
    m = re.search(r'passes=(\d+) violations=(\d+)', b['final'])
    passes, viol = int(m.group(1)), int(m.group(2))
    if viol or passes < k * (len(fb) - 1):
        print('enh_sandbox_hash FAILED: %d passes (want >= %d), %d forbidden calls inside a pass\n  %s' % (passes, k * (len(fb) - 1), viol, b['stats']))
        return 1
    opl = re.search(r'opl=(\d+):', fa[-1]).group(1)
    print('enh_sandbox_hash OK: %d VGAME frames identical with %d display passes per frame (%d passes, %s OPL writes by then) and in --classic, '
          'final comm block and roster identical; %s; base %.0f s, test %.0f s, classic %.0f s; --wide (canvas 1280 x 600): %d wide display passes, the same %d frames '
          '(all but the canvas hash), 0 forbidden calls, %.0f s' % (len(fa), k, passes, opl, b['stats'], a['secs'], b['secs'], c['secs'], wpasses, len(w['frames']), w['secs']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
