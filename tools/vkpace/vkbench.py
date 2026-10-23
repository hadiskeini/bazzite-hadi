#!/usr/bin/env python3
"""Compare sessions (compositors, settings) on the same replay section, from vkpace logs.

usage: vkbench.py mark LOG LABEL [--seconds 60] [--lead 15]   count down, then record one section
       vkbench.py compare LOG...                               one column per log

`mark` needs the game running with VKPACE_LOG=LOG and the replay paused at the section start. Three clicks
count down, a bell means unpause now, a chime means the section is over. It writes LOG.phases."""
import argparse, json, os, statistics, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vkreport as vr
from cpuab import cue, sleep_until, sample_freq

SKIP_NS = 1e9  # reaction time after the bell


def recent_max_fps(log):
    with open(log, 'rb') as fh:
        fh.seek(max(0, os.path.getsize(log) - 262144))
        a = [l.split() for l in fh.read().decode(errors='replace').splitlines() if l.startswith('A ')]
    return {x[4] for x in a[-200:]}


def mark(a):
    size = os.path.getsize(a.log)
    time.sleep(1)
    if os.path.getsize(a.log) == size:
        sys.exit(f'{a.log} does not grow: is the game running with VKPACE_LOG set?')
    caps = recent_max_fps(a.log)
    if caps - {'0'}:
        print(f'WARNING: the game limiter is on (maxFPS {",".join(sorted(caps))}); type fps_max 0 in the console', flush=True)
    import threading
    freq, stop = [], threading.Event()
    th = threading.Thread(target=sample_freq, args=(a.log, stop, freq), daemon=True); th.start()
    go = time.monotonic_ns() + int(a.lead * 1e9)
    print(f'{a.label}: unpause on the bell in {a.lead:.0f} s, section {a.seconds:.0f} s', flush=True)
    try:
        for k in (3, 2, 1):
            sleep_until(go - k * 10**9); cue('audio-volume-change')
        sleep_until(go); cue('bell')
        sleep_until(go + int(a.seconds * 1e9)); cue('complete')
    finally:
        end = time.monotonic_ns(); stop.set(); th.join()
        env = {k: os.environ.get(k, '') for k in ('XDG_CURRENT_DESKTOP', 'XDG_SESSION_DESKTOP', 'WAYLAND_DISPLAY')}
        with open(a.log + '.phases', 'w') as fh:
            json.dump({'phases': [{'profile': a.label, 'start': go, 'end': end}], 'freq_mhz': freq, 'env': env}, fh)
        print(f'done; {a.log}.phases')


def summary(log):
    meta = json.load(open(log + '.phases'))
    ph = meta['phases'][0]; t0, t1 = ph['start'] + SKIP_NS, ph['end']
    cfg, S, Q, F = vr.parse(log)
    T = int(cfg.get('T', '0') or 0) or int(1e9 / 141)
    fr = [f for f in vr.main_frames(S) if t0 <= f['start'] < t1]
    start = {f['frame']: f['start'] for f in vr.main_frames(S)}
    q = {i: frm for i, _, frm in Q}
    ids = sorted(F)
    iv, lat = [], []
    for i0, i1 in zip(ids, ids[1:]):
        s = start.get(q.get(i1), -1)
        if not (t0 <= s < t1):
            continue
        lat.append(F[i1] - s)
        if i1 == i0 + 1 and 0 < F[i1] - F[i0] < 100e6:
            iv.append(F[i1] - F[i0])
    sec = (t1 - t0) / 1e9
    P = 1e9 / 64
    _, phi = vr.fit_phase(fr, P) if fr else (0, 0)
    tick = sorted(f['work'] for f, x in zip(fr, vr.tick_flags(fr, P, phi)) if x)
    mhz = [m for t, m in meta['freq_mhz'] if t0 <= t < t1]
    caps = {l.split()[4] for l in open(log) if l.startswith('A ') and t0 <= int(l.split()[1]) < t1}
    ms = lambda v, p: sorted(v)[int(p * (len(v) - 1))] / 1e6 if v else float('nan')
    return ph['profile'], [
        ('section seconds', f'{sec:.0f}'),
        ('game limiter maxFPS', ','.join(sorted(caps)) or '-'),
        ('flips per second', f'{len(iv) / sec:.1f}'),
        ('flip interval p50 / p99 ms', f'{ms(iv, .5):.2f} / {ms(iv, .99):.2f}'),
        ('flip interval sd ms', f'{statistics.pstdev(iv) / 1e6:.2f}' if iv else '-'),
        ('stretched flips > 7.5 ms /s', f'{sum(d > T + 0.41e6 for d in iv) / sec:.1f}'),
        ('hitches > 10 ms /s', f'{sum(d > 10e6 for d in iv) / sec:.2f}'),
        ('hitches > 20 ms (count)', f'{sum(d > 20e6 for d in iv)}'),
        ('frame start -> photon p50 / p90 ms', f'{ms(lat, .5):.2f} / {ms(lat, .9):.2f}'),
        ('tick-frame work p50 / p90 ms', f'{ms(tick, .5):.2f} / {ms(tick, .9):.2f}'),
        ('main-thread clock median MHz', f'{statistics.median(mhz):.0f}' if mhz else '-'),
    ]


def compare(a):
    cols = [summary(l) for l in a.logs]
    w = max(len(k) for k, _ in cols[0][1]) + 2
    print(' ' * w + ''.join(f'{c[0]:>16s}' for c in cols))
    for i, (k, _) in enumerate(cols[0][1]):
        print(f'{k:<{w}}' + ''.join(f'{c[1][i][1]:>16s}' for c in cols))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('mark'); m.add_argument('log'); m.add_argument('label')
    m.add_argument('--seconds', type=float, default=60); m.add_argument('--lead', type=float, default=15)
    sub.add_parser('compare').add_argument('logs', nargs='+')
    a = ap.parse_args()
    mark(a) if a.cmd == 'mark' else compare(a)
