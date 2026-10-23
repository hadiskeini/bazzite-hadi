#!/usr/bin/env python3
"""A/B test of the power profile against Deadlock's tick-frame work, from one vkpace log.

usage: cpuab.py run LOG [--phase 3] [--rounds 30]   switch balanced/performance every PHASE s, beep at start and end
       cpuab.py replay LOG [--order BPPB] [--section 20] [--gap 15]
                                                    one replay section per pass, after a warm-up pass
       cpuab.py report LOG                          compare the phases, paired with their neighbours

Short phases let each balanced phase meet a performance phase over nearly the same game scene, so the test
can run during live action instead of in a repeatable but quiet scene. `replay` plays the same replay section
in every pass instead: in each gap, seek back and pause; three clicks count down, a bell means unpause now,
a chime means the pass is over. The warm-up pass loads shaders and assets and is not compared.

`run` needs the game running with VKPACE_LOG=LOG. It writes LOG.phases and always restores the
profile that was active before. Phase times are CLOCK_MONOTONIC, the clock of the vkpace log."""
import argparse, json, os, statistics, subprocess, sys, threading, time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vkreport as vr

PP = ['busctl', '--system', 'net.hadess.PowerProfiles', '/net/hadess/PowerProfiles', 'net.hadess.PowerProfiles', 'ActiveProfile']
SKIP_NS = 1e9  # tuned applies a profile in 0.1-0.3 s


def get_profile():
    return subprocess.run(['busctl', '--system', 'get-property', *PP[2:]], capture_output=True, text=True, check=True).stdout.split()[1].strip('"')


def set_profile(p):
    subprocess.run(['busctl', '--system', 'set-property', *PP[2:], 's', p], check=True)


SOUNDS = '/usr/share/sounds/freedesktop/stereo/'


def cue(name):
    subprocess.Popen(['paplay', SOUNDS + name + '.oga'], stderr=subprocess.DEVNULL)


def sleep_until(t_ns):
    time.sleep(max(0, (t_ns - time.monotonic_ns()) / 1e9))


def beep(n=1):
    for _ in range(n):
        subprocess.run(['paplay', '/usr/share/sounds/freedesktop/stereo/bell.oga'], stderr=subprocess.DEVNULL)


def main_tid(log):
    with open(log, 'rb') as fh:
        fh.seek(max(0, os.path.getsize(log) - 65536))
        lines = [l for l in fh.read().decode(errors='replace').splitlines() if l.startswith('S ')]
    return int(lines[-1].split()[4]) if lines else None


def sample_freq(log, stop, out):
    """The clock of the CPU that the game's main thread runs on, at 10 Hz."""
    tid = None
    while not stop.is_set():
        try:
            tid = tid or main_tid(log)
            with open(f'/proc/{tid}/stat') as fh:
                cpu = int(fh.read().rsplit(')', 1)[1].split()[36])
            with open(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq') as fh:
                out.append((time.monotonic_ns(), int(fh.read()) // 1000))
        except (OSError, ValueError, TypeError, IndexError):
            tid = None
        stop.wait(0.1)


def run(a):
    size = os.path.getsize(a.log)
    time.sleep(1)
    if os.path.getsize(a.log) == size:
        sys.exit(f'{a.log} does not grow: is the game running with VKPACE_LOG set?')
    before = get_profile()
    order = ['balanced', 'performance'] * a.rounds
    phases, freq, stop = [], [], threading.Event()
    th = threading.Thread(target=sample_freq, args=(a.log, stop, freq), daemon=True); th.start()
    beep()
    try:
        for p in order:
            set_profile(p)
            t0 = time.monotonic_ns()
            print(f'{p:12s} {a.phase} s', flush=True)
            time.sleep(a.phase)
            phases.append({'profile': p, 'start': t0, 'end': time.monotonic_ns()})
    finally:
        set_profile(before); stop.set(); th.join()
        beep(2)
        with open(a.log + '.phases', 'w') as fh:
            json.dump({'phases': phases, 'freq_mhz': freq}, fh)
        print(f'done, profile restored to {before}; phases in {a.log}.phases')


def replay(a):
    size = os.path.getsize(a.log)
    time.sleep(1)
    if os.path.getsize(a.log) == size:
        sys.exit(f'{a.log} does not grow: is the game running with VKPACE_LOG set?')
    before = get_profile()
    names = {'B': 'balanced', 'P': 'performance'}
    order = ['warmup'] + [names[c] for c in a.order.upper()]
    phases, freq, stop = [], [], threading.Event()
    th = threading.Thread(target=sample_freq, args=(a.log, stop, freq), daemon=True); th.start()
    try:
        for i, p in enumerate(order):
            set_profile('balanced' if p == 'warmup' else p)
            go = time.monotonic_ns() + int(a.gap * 1e9)
            print(f'pass {i} ({p}): seek back and pause; unpause on the bell in {a.gap:.0f} s', flush=True)
            for k in (3, 2, 1):
                sleep_until(go - k * 10**9); cue('audio-volume-change')
            sleep_until(go); cue('bell')
            sleep_until(go + int(a.section * 1e9)); cue('complete')
            phases.append({'profile': p, 'start': go, 'end': time.monotonic_ns()})
    finally:
        set_profile(before); stop.set(); th.join()
        time.sleep(1); beep(2)
        with open(a.log + '.phases', 'w') as fh:
            json.dump({'phases': phases, 'freq_mhz': freq}, fh)
        print(f'done, profile restored to {before}; phases in {a.log}.phases')


def by_pass(fr, fl, p):
    return [f['work'] for f, x in zip(fr, fl) if x and p['start'] + SKIP_NS <= f['start'] < p['end']]


def report(a):
    with open(a.log + '.phases') as fh:
        meta = json.load(fh)
    cfg, S, Q, F = vr.parse(a.log)
    T = int(cfg.get('T', '0') or 0) or int(1e9 / 141)
    fr, _ = vr.play_only(vr.main_frames(S), T)  # drops menus and loading screens
    P = 1e9 / 64
    _, phi = vr.fit_phase(fr, P)
    fl = vr.tick_flags(fr, P, phi)

    def phase_of(t):
        for p in meta['phases']:
            if p['start'] + SKIP_NS <= t < p['end']:
                return p['profile']

    tick, other, start = defaultdict(list), defaultdict(list), {}
    for f, x in zip(fr, fl):
        start[f['frame']] = f['start']
        p = phase_of(f['start'])
        if p:
            (tick if x else other)[p].append(f['work'])
    q = {i: frm for i, _, frm in Q}
    ids = sorted(F)
    flips, flip_times = defaultdict(list), []
    for i0, i1 in zip(ids, ids[1:]):
        d = F[i1] - F[i0]
        p = phase_of(start.get(q.get(i1), -1))
        if i1 == i0 + 1 and 0 < d < 100e6 and p:
            flips[p].append(d)
            flip_times.append((d, start[q[i1]]))
    mhz = defaultdict(list)
    for t, m in meta['freq_mhz']:
        p = phase_of(t)
        if p:
            mhz[p].append(m)

    secs = defaultdict(float)
    for p in meta['phases']:
        secs[p['profile']] += (p['end'] - p['start'] - SKIP_NS) / 1e9
    print(f'T={T / 1e6:.3f} ms, tick phase {phi / 1e6:.2f} ms, first {SKIP_NS / 1e9:.0f} s of each phase dropped')
    for p in ('balanced', 'performance'):
        tk, ot, fl_ = sorted(tick[p]), sorted(other[p]), flips[p]
        if not tk:
            print(f'{p}: no frames'); continue
        print(f'\n== {p} ({secs[p]:.0f} s, {len(tk)} tick frames)')
        print(f'main-thread clock: median {statistics.median(mhz[p]) if mhz[p] else float("nan"):.0f} MHz')
        print(f'tick-frame work: mean {statistics.fmean(tk) / 1e6:.2f}  {vr.pct(tk, (.1, .5, .9, .99))} ms')
        print(f'other-frame work: mean {statistics.fmean(ot) / 1e6:.2f}  {vr.pct(ot, (.5, .9))} ms')
        print(f'tick frames over budget: {vr.share(tk, lambda w: w > T):.1%}')
        if fl_:
            print(f'flips > {T / 1e6 + 0.41:.2f} ms: {sum(d > T + 0.41e6 for d in fl_) / secs[p]:.1f}/s, '
                  f'> 10 ms: {sum(d > 10e6 for d in fl_) / secs[p]:.2f}/s')

    if len(meta['phases']) <= 10:
        print('\npass  profile      tick p50   p90  over budget  flips>7.5/s  clock')
        for i, p in enumerate(meta['phases']):
            w = sorted(by_pass(fr, fl, p))
            if not w:
                continue
            m = [x for t, x in meta['freq_mhz'] if p['start'] + SKIP_NS <= t < p['end']]
            fp = [d for d, t in flip_times if p['start'] + SKIP_NS <= t < p['end']]
            sec = (p['end'] - p['start'] - SKIP_NS) / 1e9
            print(f'{i:4d}  {p["profile"]:11s} {statistics.median(w) / 1e6:8.2f} {w[int(.9 * len(w))] / 1e6:5.2f}'
                  f' {vr.share(w, lambda x: x > T):11.1%} {sum(d > T + 0.41e6 for d in fp) / sec:12.1f}'
                  f'  {statistics.median(m) if m else float("nan"):5.0f}')

    # One pair = a balanced phase and the performance phase right after it, so both saw nearly the same scene.
    by_phase = defaultdict(list)
    for f, x in zip(fr, fl):
        if x:
            for i, p in enumerate(meta['phases']):
                if p['start'] + SKIP_NS <= f['start'] < p['end']:
                    by_phase[i].append(f['work'])
    pairs = []
    idx = [i for i, p in enumerate(meta['phases']) if p['profile'] in ('balanced', 'performance')]
    for i, j in zip(idx[::2], idx[1::2]):
        if meta['phases'][i]['profile'] == 'performance':
            i, j = j, i
        b, q = by_phase.get(i), by_phase.get(j)
        if b and q and len(b) > 20 and len(q) > 20:
            pairs.append((statistics.median(b), statistics.median(q), statistics.fmean(vr.share(v, lambda w: w > T) for v in (b, q))))
    if len(pairs) >= 3:
        d = [q - b for b, q, _ in pairs]
        se = statistics.stdev(d) / len(d) ** 0.5
        print(f'\n== paired ({len(pairs)} pairs): performance minus balanced, tick-frame median work: '
              f'{statistics.fmean(d) / 1e6:+.2f} ms (+-{2 * se / 1e6:.2f}), faster in {sum(x < 0 for x in d)} of {len(d)} pairs')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run'); r.add_argument('log'); r.add_argument('--phase', type=float, default=3); r.add_argument('--rounds', type=int, default=30)
    r = sub.add_parser('replay'); r.add_argument('log'); r.add_argument('--order', default='BPPB')
    r.add_argument('--section', type=float, default=20); r.add_argument('--gap', type=float, default=15)
    sub.add_parser('report').add_argument('log')
    a = ap.parse_args()
    {'run': run, 'replay': replay, 'report': report}[a.cmd](a)
