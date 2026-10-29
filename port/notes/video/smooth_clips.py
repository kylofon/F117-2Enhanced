"""Clips for the user's smoothness check (U1 S4.2 / S4.6): the same flights with the display stream (60 pictures a second) and with --fps off (the original's
10 a second), side by side, plus the smooth run at full size.

Each scenario runs twice headless on the virtual clock (the scripted chain of make_video.py: a fresh game folder, a synthetic roster, a pretended
1920 x 1080 window, --render-scale 4 --wide, the default game speed of 10 steps a second, keys scripted into VGAME). The sim is the same in both runs
(sim_interp_equal). Smooth: --fps-test 6 renders 6 refreshes per step at alpha 1/6 .. 1 and the capture aid writes each of them (enh_record_sub: no display
clock headless, so the encoder spreads them evenly over the step); off: the pictures the host presented. Every output frame is the picture the window would
show at that time (60 fps, no interpolation by the encoder).

Out (work/smooth/): <scenario>_smooth.mp4 (1920 x 1080), <scenario>_compare.mp4 (left --fps off, right smooth, each 960 x 540), report.txt: per clip the
pictures a second that move the scene and, from the --fps-trace of the smooth run, the per-object-class check of interp_equal.py (every interpolated object's
refreshes step evenly, never backward, and end on the step's position).

Scenarios (seconds of the host clock; VGAME starts at about 61 s):
  takeoff  the airbase: full throttle on the runway, chase view at lift-off, gear up, side and fly-by views (airbase traffic, ground units, the own aircraft)
  harm     the HARM attack of make_video.py: TV page on the SAM radar at Tripoli, the HARM fired at lock, weapon view to the impact and the smoke, side view
  threat   the same pose, full throttle, bay doors open, no attack, south past Tripoli: from about 98 s an enemy missile in flight; chase, side,
           cockpit, the scope (F3) and chase again

usage: python smooth_clips.py [--only SCN,...] [--keep] [--report-only]
"""
import argparse, os, re, shutil, subprocess, sys, time
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'e24'))
sys.path.insert(0, os.path.join(HERE, '..', 'e41'))
import sandbox_hash as SH
import interp_equal as IE

FPS = 60
K = 6                                             # refreshes per step (10 steps a second -> 60 pictures)
OUT_W, OUT_H = 1920, 1080
BASE = '--render-scale 4 --wide'
START_KEYS = '1:31,2:03,11:39'
POSE = '20,0x8000,600,0'
ATTACK = '61.7:07,62:35,62.5:3c,63:3c,63.5:09,64.5:1c'

SCN = {
    'takeoff': dict(frames=420, rec=(64.5, 90), env={},
                    keys='63:0dp,66:0dr,67:0b,68:2a+0d,72.5:2a+3b,74:e050p,75.5:e050r,77:07,79:2a+3d,83:2a+3c,87:2a+35'),
    'harm': dict(frames=260, rec=(62, 84), env={'F117_TEST_POSE': POSE},
                 keys=ATTACK + ',64.8:2a+3e,69.6:2a+3d,74:2a+3b,79:2a+3d'),
    'threat': dict(frames=700, rec=(95, 125), env={'F117_TEST_POSE': POSE},
                   keys='61.7:07,63.6:2a+0d,64:09,66:e050p,66.4:e050r,80:e050p,80.3:e050r,95.5:2a+3b,104:2a+3d,111:3b,114:3d,119:2a+3b'),
}
MODES = {'smooth': '--fps-test %d --fps-trace %%s' % K, 'off': '--fps off'}


def run(scn, mode, exe, game, work, record=True):
    cfg = SCN[scn]
    d = os.path.join(work, scn, mode)
    g = SH.fresh_game(game, os.path.join(d, 'game'))
    rec = os.path.join(d, 'rec')
    shutil.rmtree(rec, ignore_errors=True)
    os.makedirs(rec)
    tr = os.path.join(d, 'trace.txt')
    extra = BASE + ' ' + (MODES[mode] % tr.replace(os.sep, '/') if '%s' in MODES[mode] else MODES[mode])
    env = dict(os.environ, F117_KEYS=START_KEYS + ',' + cfg['keys'], F117_SHELL_SCRIPT=os.path.join(SH.S54, 'shell_chain_libya.txt'),
               F117_VGAME_FRAMES=str(cfg['frames']), F117_END_SCRIPT=os.path.join(SH.S58, 'end_chain.txt'), F117_VCLOCK='1',
               F117_WINDOW='%dx%d' % (OUT_W, OUT_H), F117_RECORD=rec, F117_RECORD_FROM=str(cfg['rec'][0]), F117_RECORD_TO=str(cfg['rec'][1] + 0.5),
               F117_ENH_ARGS=extra, **cfg['env'])
    if not record:
        env.pop('F117_RECORD')
    for k in ('F117_CLASSIC', 'F117_TEST_POSE', 'F117_TEST_NIGHT', 'F117_RECORD_FPS'):
        if k not in cfg['env']:
            env.pop(k, None)
    t = time.time()
    p = subprocess.run([exe, '--headless', '--trace', '--game-dir', g], env=env, capture_output=True, text=True, timeout=7200)
    open(os.path.join(d, 'stdout.txt'), 'w').write(p.stdout)
    open(os.path.join(d, 'stderr.txt'), 'w').write(p.stderr)
    n = sum(1 for _ in open(os.path.join(rec, 'frames.txt'))) if record else 0
    print('run %-8s %-6s rc %d, %d pictures, %.0f s' % (scn, mode, p.returncode, n, time.time() - t), flush=True)
    if p.returncode != 0 or 'quit: START returned 0' not in p.stdout:
        sys.exit('run %s %s did not end normally (see %s)' % (scn, mode, d))
    return rec


def timeline(rec):
    """-> [(t shown, w, h, vw, vh, offset)]: host presents as they are; --fps-test pictures (7th column) spread evenly over their step"""
    rows = [l.split() for l in open(os.path.join(rec, 'frames.txt')) if l.strip()]
    if not rows or len(rows[0]) < 7:
        return [(float(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4]), int(r[5])) for r in rows]
    groups = []
    for r in rows:
        if not groups or groups[-1][0] != r[0]:
            groups.append((r[0], []))
        groups[-1][1].append(r)
    out = []
    for i, (t, g) in enumerate(groups):
        t0 = float(t)
        dt = float(groups[i + 1][0]) - t0 if i + 1 < len(groups) else (t0 - float(groups[i - 1][0]) if i else 0.1)
        for j, r in enumerate(g):
            out.append((t0 + dt * j / len(g), int(r[1]), int(r[2]), int(r[3]), int(r[4]), int(r[5])))
    return out


def picture(raw, row, ow, oh):
    """the captured canvas as the window shows it (scaled to its presentation size), fitted into ow x oh"""
    t, w, h, vw, vh, off = row
    raw.seek(off)
    a = np.frombuffer(raw.read(4 * w * h), '<u4').reshape(h, w)
    im = Image.fromarray(np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], 2).astype(np.uint8))
    s = min(ow / vw, oh / vh)
    pw, ph = round(vw * s), round(vh * s)
    im = im.resize((round(w * s), round(h * s)), Image.BICUBIC)
    out = Image.new('RGB', (ow, oh))
    out.paste(im, ((ow - im.width) // 2, (oh - im.height) // 2))
    return out


class Shown:
    """the picture a window shows at time t (the last one presented by then), decoded once per change. changes: the pictures shown that differ from the
    one before in more than MOVED pixels (the host also presents a picture whenever a lamp or a blinking HUD symbol changes: ~126 pixels at 70 Hz)"""
    MOVED = 1000

    def __init__(self, rec, ow, oh):
        self.rows, self.raw, self.ow, self.oh = timeline(rec), open(os.path.join(rec, 'frames.raw'), 'rb'), ow, oh
        self.j, self.last, self.cur, self.changes, self.prev = 0, None, None, 0, None

    def at(self, t):
        while self.j < len(self.rows) and self.rows[self.j][0] <= t:
            self.j += 1
        k = max(0, self.j - 1)
        if k != self.last:
            self.last = k
            r = self.rows[k]
            self.raw.seek(r[5])
            a = np.frombuffer(self.raw.read(4 * r[1] * r[2]), '<u4')
            if self.prev is None or self.prev.shape != a.shape or np.count_nonzero(a != self.prev) > self.MOVED:
                self.changes += 1
            if self.prev is None or not np.array_equal(a, self.prev):
                self.cur = picture(self.raw, r, self.ow, self.oh)
            self.prev = a
        return self.cur


def ffmpeg(out, t0, t1, audio):
    cmd = ['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '%dx%d' % (OUT_W, OUT_H), '-r', str(FPS), '-i', '-',
           '-ss', '%.4f' % t0, '-t', '%.4f' % (t1 - t0), '-i', audio, '-map', '0:v', '-map', '1:a',
           '-af', 'aresample=48000,afade=t=in:d=0.05,afade=t=out:st=%.3f:d=0.05' % (t1 - t0 - 0.05),
           '-c:v', 'libx264', '-preset', 'medium', '-crf', '16', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '160k', '-movflags', '+faststart', out]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def font(size):
    for f in ('arialbd.ttf', 'arial.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def encode(scn, recs, work):
    t0, t1 = SCN[scn]['rec']
    n = int(round((t1 - t0) * FPS))
    full = Shown(recs['smooth'], OUT_W, OUT_H)
    half = {m: Shown(recs[m], OUT_W // 2, OUT_H // 2) for m in ('off', 'smooth')}
    out_full = os.path.join(work, '%s_smooth.mp4' % scn)
    out_cmp = os.path.join(work, '%s_compare.mp4' % scn)
    ff_full = ffmpeg(out_full, t0, t1, os.path.join(recs['smooth'], 'audio.wav'))
    ff_cmp = ffmpeg(out_cmp, t0, t1, os.path.join(recs['smooth'], 'audio.wav'))
    f = font(30)
    for k in range(n):
        t = t0 + k / FPS + 1e-6
        ff_full.stdin.write(full.at(t).tobytes())
        c = Image.new('RGB', (OUT_W, OUT_H))
        c.paste(half['off'].at(t), (0, OUT_H // 4))
        c.paste(half['smooth'].at(t), (OUT_W // 2, OUT_H // 4))
        dr = ImageDraw.Draw(c)
        dr.text((OUT_W // 4, OUT_H // 4 - 50), '--fps off (original, 10 pictures/s)', font=f, fill=(255, 255, 255), anchor='mm')
        dr.text((3 * OUT_W // 4, OUT_H // 4 - 50), 'Enhanced stream (60 pictures/s)', font=f, fill=(255, 255, 255), anchor='mm')
        dr.text((OUT_W // 2, OUT_H - 60), '%s   t = %.2f s' % (scn, t), font=font(24), fill=(160, 160, 160), anchor='mm')
        ff_cmp.stdin.write(c.tobytes())
    for ff, o in ((ff_full, out_full), (ff_cmp, out_cmp)):
        ff.stdin.close()
        if ff.wait() != 0:
            sys.exit('ffmpeg failed on %s' % o)
    secs = t1 - t0
    return dict(smooth=half['smooth'].changes / secs, off=half['off'].changes / secs, frames=n)


def report_obj(trace):
    st, bad = IE.obj_jerk(trace, K)
    cam, cbad = IE.jerk_check(IE.read_trace(trace), K)
    lines = ['  camera (own-aircraft views): %d frames checked, %d uneven, %d backward, worst %.1f %%' % (cam['checked'], cam['uneven'], cam['back'], 100 * cam['worst'])]
    for c, s in st.items():
        if s['frames']:
            lines.append('  %-9s %5d object-frames, %d uneven, %d backward, %d not ending on F_k, worst %.1f %%' % (c, s['frames'], s['uneven'], s['back'], s['off'], 100 * s['worst']))
    return lines, bad[:5] + cbad[:5]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(ROOT, 'f117port', 'build', 'f117-enhanced.exe'))
    ap.add_argument('--game', default=os.path.join(ROOT, 'Game'))
    ap.add_argument('--work', default=os.path.join(ROOT, 'work', 'smooth'))
    ap.add_argument('--only', default='')
    ap.add_argument('--keep', action='store_true', help='keep the raw captures (several GB per run)')
    ap.add_argument('--probe', action='store_true', help='the smooth run only, no recording: print its object counts (choosing the key scripts)')
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rep = []
    for scn in [s for s in SCN if not a.only or s in a.only.split(',')]:
        if a.probe:
            run(scn, 'smooth', a.exe, a.game, a.work, record=False)
            print([l for l in open(os.path.join(a.work, scn, 'smooth', 'stdout.txt')) if l.startswith('display-objects')][0].strip())
            continue
        recs = {m: run(scn, m, a.exe, a.game, a.work) for m in ('smooth', 'off')}
        r = encode(scn, recs, a.work)
        lines, bad = report_obj(os.path.join(a.work, scn, 'smooth', 'trace.txt'))
        rep.append('%s (%.1f-%.1f s): pictures a second that move the scene (over 1000 pixels changed): smooth %.1f, off %.1f' % (scn, SCN[scn]['rec'][0], SCN[scn]['rec'][1], r['smooth'], r['off']))
        rep += lines + ['  ! ' + b for b in bad]
        print('\n'.join(rep[-len(lines) - len(bad) - 1:]), flush=True)
        if not a.keep:
            for m in recs:
                shutil.rmtree(recs[m], ignore_errors=True)
    with open(os.path.join(a.work, 'report.txt'), 'a') as f:
        f.write(time.strftime('%Y-%m-%d %H:%M') + '\n' + '\n'.join(rep) + '\n')


if __name__ == '__main__':
    main()
