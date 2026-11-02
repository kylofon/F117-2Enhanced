"""S2.2: the hr 3D back end on the render3d golden scenes (ctests `r3d_hr_n1`, `r3d_hr_n234`).

  python r3d_hr.py <f117-enhanced.exe> <Game dir> <work dir> <golden work dir> N [N ...]

Runs port/notes/s56/r3d_golden.py with the enhancements ON (F117_CLASSIC removed) and `--hr-check --render-scale N` (through F117_ENH_ARGS),
so every scenario must still give the faithful pages, DGROUP, DAC and comm block byte for byte (the hr hooks are invisible to the 1x
renderer and the sim), and the port writes per scenario how the hr 3D layer compares with the 1x page (DIR/hr_check.txt): for every 1x pixel
of a 3D pass window whose last write was 3D, its N x N layer block. A layer pixel "agrees" when what the compositor shows there (the layer
value, or the 1x value when the layer is not written) equals the 1x pixel.
  N = 1: the layer must be written and equal for every such pixel (identity, rule R1).
  N > 1: edges legitimately move by up to one original pixel (sub-pixel vertices, the 1x rasteriser's inclusive right / bottom ends; a
         nearly horizontal edge can step at another x), so the tolerance is split: overall agreement >= 90 % (measured 2026-10-19: 95.7 %,
         94.3 %, 94.0 % at N = 2, 3, 4), and in "interior" blocks (the 1x pixel and its 8 neighbours have one value, i.e. away from every
         edge) >= 99.5 % overall (measured 99.98 / 99.96 / 99.94 %) and >= 99 % in every scenario with at least 2000 interior layer pixels
         (worst measured 99.37 %, the low-detail horizon). Small TV / probe windows of a few dozen pixels are reported, not judged.
S3.2 adds the TV camera and map MFD scenes (tvclose / tvfire / tvflir / tvsmoke / tvsimple_flir, map, mapfp, mapbg): the window must still be shown from the 3D layer at the end
(`w3d` of `wpx`, >= 97 %), two passes where the close view draws two, the TV sprites (fire puffs, smoke) drawn at N x, the FLIR window in the green ramp 98h-9Fh (`tramp` of `tpx`).
S3.1 adds the whole frame: the work shadow and layers composed over the page of the last pass (sprites, far dots, tracers, target boxes, the keyed art of every view
mode included), counted per 1x pixel's N x N block (`fpx`, `fdiffer`; "interior" blocks `finner`, `finner_differ`). N = 1: no pixel may differ in any scenario.
N > 1: interior blocks agree >= 99.5 % overall (and >= 99 % in every scenario with >= 2000 interior layer pixels; measured 99.96 % at N = 4, worst scenario
view_cockpit 99.6 %); every 1x footprint pixel of the vector emitters (`vtot`) has an hr line pixel of its colour within one original pixel (`vhit`, 100 %). The
scenes with sprites (`sprites`, `sprite_px`), far dots, tracers / boxes (`lines`, `line_px`) must have been exercised (the view_* scenes of r3d_golden.py).
The references come from the golden test's cache (<golden work dir>/refs.bin, stamp.txt); without them r3d_golden.py rebuilds them. Exit 77 = no game files.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, '..', 's56', 'r3d_golden.py')
OVERALL, INNER, INNER_SCN, MIN_PX = 0.90, 0.995, 0.99, 2000


def parse(path):
    rows = []
    for line in open(path):
        f = line.split()
        if not f:
            continue
        d = {'id': f[0]}
        for kv in f[1:]:
            k, v = kv.split('=')
            d[k] = int(v)
        rows.append(d)
    return rows


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 2
    exe, game, work, gwork = sys.argv[1:5]
    ns = [int(a) for a in sys.argv[5:]]
    os.makedirs(work, exist_ok=True)
    for fn in ('refs.bin', 'stamp.txt'):
        src = os.path.join(gwork, fn)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(work, fn))
    bad = 0
    for n in ns:
        out = os.path.join(work, 'hr_check.txt')
        if os.path.exists(out):
            os.remove(out)
        env = dict(os.environ)
        env.pop('F117_CLASSIC', None)
        env['F117_ENH_ARGS'] = '--no-fine-terrain --hr-check --render-scale %d' % n      # the fine terrain moves the hr terrain off the 1x page
        r = subprocess.run([sys.executable, GOLDEN, exe, game, work], env=env, capture_output=True, text=True, timeout=1800)
        if r.returncode == 77:
            print('r3d-hr: no game files (skipped)')
            return 77
        last = (r.stdout.strip().splitlines() or [''])[-1]
        if r.returncode != 0:
            print('r3d-hr N=%d: the golden scenes differ with the enhancements on:' % n, r.stdout[-800:], r.stderr[-400:])
            bad += 1
            continue
        if not os.path.exists(out):
            print('r3d-hr N=%d: no hr_check.txt' % n)
            bad += 1
            continue
        rows = parse(out)
        tot = {k: sum(d.get(k, 0) for d in rows) for k in ('checked', 'hr', 'stamped', 'agree', 'differ', 'empty', 'inner', 'inner_differ', 'fpx', 'fdiffer', 'finner', 'finner_differ', 'vtot', 'vhit', 'vthin', 'vstray',
                                                       'sprites', 'sprite_px', 'dots', 'lines', 'line_px', 'wpx', 'w3d', 'tpx', 'tramp')}
        px = tot['agree'] + tot['differ']
        agree = tot['agree'] / px if px else 0.0
        inner = 1 - tot['inner_differ'] / tot['inner'] if tot['inner'] else 1.0
        print('r3d-hr N=%d: %d scenarios, golden identical; %d 3D pixels checked, layer pixels: %d hr, %d stamped, %d empty; agreement %.2f %%, interior %.3f %%'
              % (n, len(rows), tot['checked'], tot['hr'], tot['stamped'], tot['empty'], 100 * agree, 100 * inner))
        if tot['checked'] == 0:
            print('  nothing checked')
            bad += 1
            continue
        print('  frame: %d layer px, %d differ, interior %d / %d differ; objects: %d sprites (%d px), %d dots, %d lines (%d px); vector footprints %d, %d within 1 px'
              % (tot['fpx'], tot['fdiffer'], tot['finner'], tot['finner_differ'], tot['sprites'], tot['sprite_px'], tot['dots'], tot['lines'], tot['line_px'], tot['vtot'], tot['vhit']))
        # S3.2: the TV camera window (two passes in one window, sprites, the FLIR tint) and the map MFD window (ortho terrain, the backup page 2 round trip): at the end of
        # the scenario the window is still shown from the 3D layer (a 3D record equal to the page) except the labels, the tint covered it, and the passes were really two
        tvm = [d for d in rows if d['id'].startswith(('tv', 'map'))]
        if len(tvm) < 35 or sum(d.get('sprites', 0) for d in tvm) < 60:
            print('  the TV / map scenes did not run (%d scenes, %d sprites)' % (len(tvm), sum(d.get('sprites', 0) for d in tvm)))
            bad += 1
        for d in tvm:
            if d['wpx'] == 0 or d['w3d'] < 0.97 * d['wpx']:
                print('  scenario %s: only %d of %d window pixels are shown from the 3D layer' % (d['id'], d['w3d'], d['wpx']))
                bad += 1
            if d['id'].startswith(('tvclose', 'tvfire', 'tvflir')) and d['passes'] != 2:
                print('  scenario %s: %d passes (the close view of an air target draws two)' % (d['id'], d['passes']))
                bad += 1
            if d['id'].startswith(('tvflir', 'tvsimple')) and (d['tpx'] == 0 or d['tramp'] < 0.95 * d['tpx']):
                print('  scenario %s: the FLIR window is not in the green ramp (%d of %d px)' % (d['id'], d['tramp'], d['tpx']))
                bad += 1
            if d['id'].startswith('tvfire') and d['sprites'] < 4 or d['id'].startswith('tvsmoke') and d['sprites'] < 16:
                print('  scenario %s: %d TV sprites at N x' % (d['id'], d['sprites']))
                bad += 1
        # S3.3: the HUD / MFD scenes (hud_*: tapes with tick strips, frame, reticle, pitch ladder, ILS, scope, map lines and arcs, the ADI polygon): the vector lines are thin at N > 1
        # (`vthin`: footprint blocks that show less than the line colour everywhere; the ADI polygon only along its edge), show only the line colour or what it replaced (`vstray`),
        # and at N = 1 the frame is the 1x frame (checked below for every scenario)
        huds = [d for d in rows if d['id'].startswith('hud_')]
        if len(huds) < 12 or sum(d['lines'] for d in huds) < 200 or sum(d['vtot'] for d in huds) < 2500:
            print('  the HUD / MFD scenes (hud_*) did not exercise the vector lines: %d scenes, %d lines, %d footprints' % (len(huds), sum(d['lines'] for d in huds), sum(d['vtot'] for d in huds)))
            bad += 1
        for d in huds:
            if d['vstray']:
                print('  scenario %s: %d vector footprint blocks show a colour that is neither the line nor what it replaced' % (d['id'], d['vstray']))
                bad += 1
            if n > 1 and d['vtot'] and (d['vthin'] < 20 if d['id'].startswith('hud_adi') else d['vthin'] < 0.95 * d['vtot']):
                print('  scenario %s: only %d of %d vector footprint blocks are thin at N = %d' % (d['id'], d['vthin'], d['vtot'], n))
                bad += 1
        views = [d for d in rows if d['id'].startswith('view_')]
        if len(views) < 13 or tot['sprites'] < 50 or tot['dots'] < 8 or tot['lines'] < 50 or tot['vtot'] < 300:
            print('  the object scenes (view_*) did not exercise sprites / dots / tracers / boxes: %d scenes, %d sprites, %d dots, %d lines, %d footprints'
                  % (len(views), tot['sprites'], tot['dots'], tot['lines'], tot['vtot']))
            bad += 1
        if tot['vhit'] != tot['vtot']:
            print('  %d of %d vector footprint pixels have no hr line pixel within one original pixel' % (tot['vtot'] - tot['vhit'], tot['vtot']))
            bad += 1
        if n == 1:
            for d in rows:
                if d['fdiffer']:
                    print('  scenario %s: %d frame pixels differ from the 1x page' % (d['id'], d['fdiffer']))
                    bad += 1
        else:
            if tot['finner'] and 1 - tot['finner_differ'] / tot['finner'] < INNER:
                print('  frame interior agreement below %.1f %%' % (100 * INNER))
                bad += 1
            for d in rows:
                if d['finner'] >= MIN_PX and 1 - d['finner_differ'] / d['finner'] < INNER_SCN:
                    print('  scenario %s: frame interior agreement %.2f %% below %.0f %%' % (d['id'], 100 - 100 * d['finner_differ'] / d['finner'], 100 * INNER_SCN))
                    bad += 1
        if n == 1:
            if tot['differ'] or tot['empty']:
                bad += 1
                for d in rows:
                    if d['differ'] or d['empty']:
                        print('  scenario %s: %d differ, %d empty' % (d['id'], d['differ'], d['empty']))
            continue
        worst = sorted([d for d in rows if d['checked']], key=lambda d: d['agree'] / max(1, d['agree'] + d['differ']))
        for d in worst[:3]:
            print('  lowest: %s %.2f %% of %d layer px (%d checked)' % (d['id'], 100 * d['agree'] / max(1, d['agree'] + d['differ']), d['agree'] + d['differ'], d['checked']))
        if agree < OVERALL:
            print('  overall agreement below %.0f %%' % (100 * OVERALL))
            bad += 1
        if inner < INNER:
            print('  interior agreement below %.1f %%' % (100 * INNER))
            bad += 1
        for d in rows:
            if d['inner'] >= MIN_PX and 1 - d['inner_differ'] / d['inner'] < INNER_SCN:
                print('  scenario %s: interior agreement %.2f %% below %.0f %%' % (d['id'], 100 - 100 * d['inner_differ'] / d['inner'], 100 * INNER_SCN))
                bad += 1
    print('r3d-hr %s' % ('OK' if not bad else 'FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
