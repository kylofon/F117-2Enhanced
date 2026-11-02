"""S3.5: smooth distance shading on the golden scenes (ctest `r3d_hr_smooth`, DESIGN.md 2.11).

  python smooth_scenes.py <f117-enhanced.exe> <Game dir> <work dir> <golden work dir> [N] [--png DIR]

The render3d golden scenes (port/notes/s56/r3d_golden.py, cached references) run at --render-scale N with --hr-dump and F117_HRD_RGB=1 (the canvas as composed
from the live DAC, DIR/<id>.rgb), once without and once with --smooth-shading (the golden 1x pages must stay byte for byte both times), and once more each on a
wide canvas (--wide-test, 16:9-like); then once at N = 1 with the option. Per scene, option on against option off:
  index       the layer's index, written and stamp bits (bits 0..9) are identical; only bits 10..15 (the blend code) may differ;
  untouched   every canvas pixel whose layer word has no code is identical in RGB (HUD-free 3D scenes: the sky, objects, the art);
  between     every shown pixel with a code is exactly the mix of its colour and the neighbour entry of its ramp (computed here from the dumped DAC), so between the two;
  bands       vertically adjacent pixels of the same sky / ground band ramp: the continuous position (band + code / 64) changes by >= STEP_MAX between two rows in
              at most STEP_SHARE of the pairs where the option-off picture changes band (the steps are gone), and along every column a run of band pixels is monotonic.
N = 1 with the option: the index of every layer pixel equals the option-off one (the picture differs only in shaded colours).
With --png DIR: DIR/scene_<id>_{off,on}.png of a few scenes and DIR/contact_scenes.png. Exit 77 = no game files.
"""
import os, shutil, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'e23'))
from hrd import Hrd
GOLDEN = os.path.join(HERE, '..', 's56', 'r3d_golden.py')

STEP_MAX = 0.75          # a step of 3/4 of a band or more between two adjacent rows
STEP_SHARE = 0.05        # ... in at most 5 % of the pairs where the original steps
MONO_TOL = 2             # 2/64 of a band: the rounding of the code
PNG_SCENES = ('runway0', 'high0', 'low_d3_0', 'close3', 'view_chase', 'tv0')


def golden(exe, game, work, gwork, args):
    for fn in ('refs.bin', 'stamp.txt'):
        src = os.path.join(gwork, fn)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(work, fn))
    env = dict(os.environ)
    env.pop('F117_CLASSIC', None)
    env['F117_ENH_ARGS'] = '--no-fine-terrain ' + args      # the fine terrain moves the hr terrain off the 1x page
    env['F117_HRD_RGB'] = '1'
    return subprocess.run([sys.executable, GOLDEN, exe, game, work], env=env, capture_output=True, text=True, timeout=1800)


def run_port(exe, game, work, args):
    env = dict(os.environ)
    env.pop('F117_CLASSIC', None)
    env['F117_ENH_ARGS'] = '--no-fine-terrain ' + args      # the fine terrain moves the hr terrain off the 1x page
    env['F117_HRD_RGB'] = '1'
    return subprocess.run([exe, '--render-test', work, '--game-dir', game], env=env, capture_output=True, text=True, timeout=1800)


def read_rgb(path):
    b = open(path, 'rb').read()
    lw, lh = (int(v) for v in np.frombuffer(b, '<i4', 2))
    return np.frombuffer(b, '<u4', lw * lh, 8).reshape(lh, lw)


def pal8(h):
    p = h.palette().astype(np.uint32)
    return (p[:, 0] << 16) | (p[:, 1] << 8) | p[:, 2]


def mix(pal, lay):
    """the compositor's l3_rgb in numpy: (rgb, neighbour rgb, has code)"""
    idx = (lay & 0xFF).astype(np.int32)
    code = (lay >> 10).astype(np.int32)
    d = code - 32
    nb = np.where(d > 0, idx + 1, idx - 1) & 0xFF
    ok = (code != 0) & (((idx ^ nb) & 0xF0) == 0)
    w = np.abs(d).astype(np.uint32)
    a, b = pal[idx], pal[nb]
    out = np.zeros(lay.shape, np.uint32)
    for sh in (0, 8, 16):
        ca, cb = (a >> sh) & 255, (b >> sh) & 255
        out |= (((ca * (64 - w) + cb * w + 32) >> 6) & 255) << sh
    return np.where(ok, out, a), a, b, ok


def band_stats(lay, shown):
    """vertical pairs of the same band ramp: (pairs, off steps, on steps >= STEP_MAX, non-monotonic runs, runs)"""
    idx = (lay & 0xFF).astype(np.int32)
    ramp = np.where((idx >= 0x80) & (idx <= 0x8F), 1, np.where((idx >= 0x70) & (idx <= 0x7F), 2, 0))
    ramp[~shown] = 0
    code = (lay >> 10).astype(np.int32)
    pos = (idx & 15) * 64 + np.where(code != 0, code - 32, 0)
    same = (ramp[:-1] != 0) & (ramp[:-1] == ramp[1:])
    dpos = np.abs(pos[1:] - pos[:-1])
    doff = (idx[1:] & 15) != (idx[:-1] & 15)
    pairs = int(same.sum())
    off_steps = int((same & doff).sum())
    on_steps = int((same & (dpos >= STEP_MAX * 64)).sum())
    bad = runs = 0
    for x in range(lay.shape[1]):
        col, r, p = same[:, x], ramp[:, x], pos[:, x]
        y = 0
        n = len(col)
        while y < n:
            if not col[y]:
                y += 1
                continue
            y0 = y
            while y < n and col[y]:
                y += 1
            seq = np.diff(p[y0:y + 1])
            if len(seq) >= 3:
                runs += 1
                if (seq > MONO_TOL).any() and (seq < -MONO_TOL).any():
                    bad += 1
    return pairs, off_steps, on_steps, bad, runs


def compare(doff, don, tot, worst):
    files = sorted(f for f in os.listdir(don) if f.endswith('.hrd'))
    for f in files:
        sid = f[:-4]
        if not os.path.isfile(os.path.join(doff, f)) or not os.path.isfile(os.path.join(don, sid + '.rgb')):
            tot['missing'] += 1
            continue
        a, b = Hrd(os.path.join(doff, f)), Hrd(os.path.join(don, f))
        tot['scenes'] += 1
        la, lb = a.full_layer(), b.full_layer()
        if la.shape != lb.shape or (la & 0xFC00).any() or ((la & 0x3FF) != (lb & 0x3FF)).any():
            tot['index_bad'] += 1
            worst.append('%s: the layer index differs' % sid)
            continue
        ra, rb = read_rgb(os.path.join(doff, sid + '.rgb')), read_rgb(os.path.join(don, sid + '.rgb'))
        code = (lb & 0xFC00) != 0
        tot['coded'] += int(code.sum())
        diff = ra != rb
        tot['untouched_bad'] += int((diff & ~code).sum())
        pal = pal8(b)
        exp, ca, cb, ok = mix(pal, lb)
        shown = ((lb & 0x100) != 0) & (ra == ca)                      # the canvas shows this layer pixel (not art over it)
        sc = shown & code
        tot['shown_coded'] += int(sc.sum())
        tot['between_bad'] += int((sc & (rb != exp)).sum())
        for sh in (0, 8, 16):
            x, lo_, hi_ = (rb >> sh) & 255, np.minimum((ca >> sh) & 255, (cb >> sh) & 255), np.maximum((ca >> sh) & 255, (cb >> sh) & 255)
            tot['outside'] += int((sc & ok & ((x < lo_) | (x > hi_))).sum())
        idx = lb & 0xFF
        tot['coded_obj'] += int((sc & (idx >= 0xA0)).sum())
        tot['coded_band'] += int((sc & (idx >= 0x60) & (idx <= 0x8F)).sum())
        p, so, sn, bad, runs = band_stats(lb, shown)
        for k, v in zip(('pairs', 'off_steps', 'on_steps', 'mono_bad', 'runs'), (p, so, sn, bad, runs)):
            tot[k] += v
        if tot['untouched_bad'] or tot['between_bad']:
            if len(worst) < 6:
                worst.append('%s: untouched %d, between %d' % (sid, int((diff & ~code).sum()), int((sc & (rb != exp)).sum())))


def new_tot():
    return dict(scenes=0, missing=0, index_bad=0, coded=0, untouched_bad=0, shown_coded=0, between_bad=0, outside=0, coded_obj=0, coded_band=0,
                pairs=0, off_steps=0, on_steps=0, mono_bad=0, runs=0)


def verdict(name, tot, worst):
    print('r3d-smooth %s: %d scenes; index bits 0..9 equal in %d; untouched pixels differing %d; %d shown pixels with a code (%d bands / ground faces 60h-8Fh, %d ramps A0h-FFh: objects, water), '
          'not the mix %d, outside the two colours %d; band pairs %d: option off steps %d, option on steps >= %.2f band %d (%.2f %%); columns %d runs, %d not monotonic' % (
              name, tot['scenes'], tot['scenes'] - tot['index_bad'], tot['untouched_bad'], tot['shown_coded'], tot['coded_band'], tot['coded_obj'],
              tot['between_bad'], tot['outside'], tot['pairs'], tot['off_steps'], STEP_MAX, tot['on_steps'], 100.0 * tot['on_steps'] / max(1, tot['off_steps']),
              tot['runs'], tot['mono_bad']))
    for w in worst[:6]:
        print('  ' + w)
    bad = 0
    if tot['scenes'] < 50 or tot['missing']:
        print('  too few scenes / missing dumps'); bad += 1
    if tot['index_bad'] or tot['untouched_bad'] or tot['between_bad'] or tot['outside']:
        bad += 1
    if tot['coded_band'] < 10000 or tot['coded_obj'] < 100 or tot['off_steps'] < 1000:
        print('  too little to judge'); bad += 1
    if tot['on_steps'] > STEP_SHARE * tot['off_steps'] or tot['mono_bad'] > 0.001 * max(1, tot['runs']):
        print('  the bands still step'); bad += 1
    return bad


def write_pngs(doff, don, out, n):
    from PIL import Image, ImageDraw
    os.makedirs(out, exist_ok=True)
    tiles = []
    for sid in PNG_SCENES:
        pa, pb = os.path.join(doff, sid + '.rgb'), os.path.join(don, sid + '.rgb')
        if not (os.path.isfile(pa) and os.path.isfile(pb)):
            continue
        row = []
        for tag, p in (('off', pa), ('on', pb)):
            im = read_rgb(p)
            rgb = np.stack([(im >> 16) & 255, (im >> 8) & 255, im & 255], 2).astype(np.uint8)
            img = Image.fromarray(rgb)
            img = img.resize((img.width, int(img.height * 1.2)), Image.NEAREST)
            img.save(os.path.join(out, 'scene_%s_%s.png' % (sid, tag)))
            row.append((img, '%s  smooth %s' % (sid, tag)))
        tiles.append(row)
    if not tiles:
        return
    w = max(t[0].width for r in tiles for t in r)
    h = max(t[0].height for r in tiles for t in r) + 18
    sheet = Image.new('RGB', (2 * w + 8, h * len(tiles)), (40, 40, 48))
    for i, r in enumerate(tiles):
        for j, (img, cap) in enumerate(r):
            sheet.paste(img, (j * (w + 8), i * h + 18))
            ImageDraw.Draw(sheet).text((j * (w + 8) + 4, i * h + 3), cap, fill=(235, 235, 235))
    sheet.save(os.path.join(out, 'contact_scenes.png'))
    print('r3d-smooth: %s' % os.path.join(out, 'contact_scenes.png'))


def main():
    argv = sys.argv[1:]
    png = None
    if '--png' in argv:
        i = argv.index('--png'); png = argv[i + 1]; del argv[i:i + 2]
    if len(argv) < 4:
        print(__doc__)
        return 2
    exe, game, work, gwork = (os.path.abspath(a) for a in argv[:4])
    n = int(argv[4]) if len(argv) > 4 else 2
    os.makedirs(work, exist_ok=True)
    dirs = {}
    for tag, extra in (('off', ''), ('on', '--smooth-shading')):
        d = os.path.join(work, tag)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        r = golden(exe, game, work, gwork, ('--hr-dump %s --render-scale %d %s' % (d.replace('\\', '/'), n, extra)).strip())
        if r.returncode == 77:
            print('r3d-smooth: no game files (skipped)')
            return 77
        if r.returncode != 0:
            print('r3d-smooth: the golden scenes differ (%s):' % tag, r.stdout[-800:], r.stderr[-400:])
            return 1
        dirs[tag] = d
    bad = 0
    tot, worst = new_tot(), []
    compare(dirs['off'], dirs['on'], tot, worst)
    bad += verdict('N=%d' % n, tot, worst)
    if png:
        write_pngs(dirs['off'], dirs['on'], png, n)
    cw, ch = 480 * n, 240 * n                                         # 16:9-like (the frame 320 x 200 at N, 80 px each side, 40 on top at 1x)
    for tag, extra in (('woff', ''), ('won', '--smooth-shading')):
        d = os.path.join(work, tag)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        p = run_port(exe, game, work, ('--hr-dump %s --render-scale %d --wide-test %dx%d %s' % (d.replace('\\', '/'), n, cw, ch, extra)).strip())
        if p.returncode != 0:
            print('r3d-smooth: the wide run failed (%s)' % tag, p.stdout[-600:], p.stderr[-600:])
            return 1
        dirs[tag] = d
    tot, worst = new_tot(), []
    compare(dirs['woff'], dirs['won'], tot, worst)
    bad += verdict('wide %dx%d N=%d' % (cw, ch, n), tot, worst)
    if png:
        sub = os.path.join(png, 'wide')
        write_pngs(dirs['woff'], dirs['won'], sub, n)
    d1 = os.path.join(work, 'n1')
    shutil.rmtree(d1, ignore_errors=True)
    os.makedirs(d1)
    r = golden(exe, game, work, gwork, '--hr-dump %s --render-scale 1 --smooth-shading' % d1.replace('\\', '/'))
    if r.returncode != 0:
        print('r3d-smooth: the golden scenes differ (N = 1):', r.stdout[-800:], r.stderr[-400:])
        return 1
    same = differ = coded = 0
    for f in sorted(f for f in os.listdir(d1) if f.endswith('.hrd')):
        h = Hrd(os.path.join(d1, f))
        img, mask = h.compose3d()
        m = mask == 1
        differ += int((img[m] != h.page[m]).sum()) if h.n == 1 else 0
        same += int(m.sum())
        coded += int(((h.full_layer() & 0xFC00) != 0).sum())
    print('r3d-smooth N=1 with the option: %d hr layer pixels, %d with an index other than the 1x page, %d with a code' % (same, differ, coded))
    if differ or same == 0 or coded == 0:
        bad += 1
    for k in ('off', 'on', 'woff', 'won', 'n1'):
        shutil.rmtree(os.path.join(work, k), ignore_errors=True)
    print('r3d-smooth %s' % ('OK' if not bad else 'FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
