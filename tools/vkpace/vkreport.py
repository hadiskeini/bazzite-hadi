#!/usr/bin/env python3
"""Compact report for one vkpace log (VKPACE_LOG): heavy share, budget overrun, stretched flips,
and the 64 Hz tick periodicity of main-thread work.

usage: vkreport.py LOG [--tick-hz 64] [--speedup 0.09] [--stretch-ms 7.5]

Frame work = input exit (S t_out) to the next input entry (S t_in) on the main thread. This is
wall time, so it includes any wait of the main thread on the render thread, not only CPU work.
Only 1-s windows that look like gameplay are used: at least 90 % of the cap's frame count and
not dominated by work pinned at the period (menus and loading screens behave like that)."""
import argparse, math, sys
from collections import defaultdict


def parse(path):
    cfg, S, Q, F = {}, [], [], {}
    render_frame, last_p = None, None
    with open(path) as fh:
        for ln in fh:
            c = ln[:1]
            if c == 'S':
                p = ln.split()
                S.append((int(p[1]), int(p[2]), int(p[3]), int(p[5]), int(p[6])))
            elif c == 'A':
                p = ln.split()
                if p[5] == '1':
                    render_frame = int(p[6])
            elif c == 'P':
                last_p = int(ln.split()[1])
            elif c == 'Q':
                Q.append((int(ln.split()[1]), last_p, render_frame))
            elif c == 'F':
                p = ln.split()
                for st in p[4:]:
                    k, _, v = st.partition(':')
                    if k == '4' and v != '0':
                        F[int(p[1])] = int(v)
            elif c == 'C' and not cfg:
                cfg = dict(kv.split('=', 1) for kv in ln.split()[1:] if '=' in kv)
    return cfg, S, Q, F


def main_frames(S):
    out = []
    for a, b in zip(S, S[1:]):
        if b[4] != a[4] + 1:
            continue
        work = b[0] - a[2]
        if 0 < work < 50e6:
            out.append({'frame': a[4], 'start': a[2], 'work': work})
    return out


def play_only(fr, T):
    t0 = fr[0]['start']; win = defaultdict(list)
    for f in fr:
        win[int((f['start'] - t0) // 1e9)].append(f)
    keep, secs = [], 0
    for k in sorted(win):
        w = win[k]
        if len(w) < 0.9e9 / T or sum(abs(f['work'] - T) < 0.25e6 for f in w) > 0.5 * len(w):
            continue
        for f in w:
            f['win'] = k
        keep.extend(w); secs += 1
    return keep, secs


def tick_flags(fr, P, phi):
    """1 for the first frame that starts after a tick at phi + k*P."""
    fl = [0] * len(fr)
    for i in range(1, len(fr)):
        a, b = fr[i - 1]['start'], fr[i]['start']
        if b - a < 3 * P and math.floor((b - phi) / P) > math.floor((a - phi) / P):
            fl[i] = 1
    return fl


def contrast(fr, P, phi):
    fl = tick_flags(fr, P, phi); s1 = n1 = s0 = n0 = 0
    for f, x in zip(fr, fl):
        if x: s1 += f['work']; n1 += 1
        else: s0 += f['work']; n0 += 1
    return (s1 / n1 - s0 / n0) if n1 and n0 else 0.0


def fit_phase(fr, P):
    coarse = max(((contrast(fr, P, k * P / 128), k * P / 128) for k in range(128)))
    step = P / 128 / 8
    return max(((contrast(fr, P, coarse[1] + j * step), coarse[1] + j * step) for j in range(-8, 9)))


def auc(pos, neg):
    allv = sorted([(v, 1) for v in pos] + [(v, 0) for v in neg])
    rs = sum(i + 1 for i, (_, l) in enumerate(allv) if l); n1, n0 = len(pos), len(neg)
    return (rs - n1 * (n1 + 1) / 2) / (n1 * n0) if n1 and n0 else float('nan')


def pct(v, ps):
    v = sorted(v)
    return ' '.join(f'p{p * 100:g}={v[min(len(v) - 1, int(p * len(v)))] / 1e6:.2f}' for p in ps) if v else '-'


def share(v, cond):
    return sum(1 for x in v if cond(x)) / len(v) if v else float('nan')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('log')
    ap.add_argument('--tick-hz', type=float, default=64.0)
    ap.add_argument('--speedup', type=float, default=0.09, help='fractional cut of tick-frame work to model')
    ap.add_argument('--stretch-ms', type=float, help='default: period + 0.41 ms (7.50 ms at 141 fps)')
    a = ap.parse_args()
    cfg, S, Q, F = parse(a.log)
    T = int(cfg.get('T', '0') or 0)
    if not T:
        T = int(1e9 / 141); print(f'no PACE_FPS in log; using T={T / 1e6:.3f} ms for flips')
    if a.stretch_ms is None:
        a.stretch_ms = round(T / 1e6 + 0.41, 2)
    print(f'log {a.log}\nperiod T={T / 1e6:.3f} ms ({1e9 / T:.1f} fps), driver={cfg.get("driver", "?")}')

    fr = main_frames(S)
    play, secs = play_only(fr, T) if fr else ([], 0)
    play_frames = {f['frame']: f['win'] for f in play}
    tickset = set()
    if play:
        P = 1e9 / a.tick_hz
        c, phi = fit_phase(play, P)
        fl = tick_flags(play, P, phi)
        tickset = {f['frame'] for f, x in zip(play, fl) if x}
        pos = [f['work'] for f, x in zip(play, fl) if x]; neg = [f['work'] for f, x in zip(play, fl) if not x]
        H = 0.93 * T
        print(f'\n== main thread ({len(play)} gameplay frames, {secs} s of {len(fr) / (1e9 / T):.0f} s logged)')
        print(f'tick model {a.tick_hz:g} Hz: phase {phi / 1e6:.2f} ms, tick frames {len(pos) / len(play):.1%} '
              f'(expected {a.tick_hz * T / 1e9:.1%}), mean work tick {sum(pos) / len(pos) / 1e6:.2f} vs other '
              f'{sum(neg) / len(neg) / 1e6:.2f} ms, AUC {auc(pos, neg):.3f}')
        ctl = [(hz, contrast(play, 1e9 / hz, fit_phase(play, 1e9 / hz)[1])) for hz in
               (a.tick_hz - 0.1, a.tick_hz - 0.01, a.tick_hz + 0.01, a.tick_hz + 0.1)]
        print('periodicity check, contrast at neighbour frequencies (must collapse if the tick is real): '
              + ', '.join(f'{hz:g} Hz {d / 1e6:.2f}' for hz, d in ctl) + f' vs {c / 1e6:.2f} ms')
        win = defaultdict(list)
        for f in play:
            win[math.floor((f['start'] - phi) / P)].append(f['work'])
        full = [v for v in win.values() if len(v) >= 2]
        print(f'tick windows with a frame > {H / 1e6:.2f} ms: {share(full, lambda v: max(v) > H):.1%}; '
              f'heaviest frame is the first after the tick: {share(full, lambda v: v.index(max(v)) == 0):.1%}')
        hv = [x for f, x in zip(play, fl) if f['work'] > H]
        print(f'threshold heavy (> {H / 1e6:.2f} ms): {len(hv) / len(play):.1%} of frames; '
              f'{sum(hv) / max(1, len(hv)):.1%} of them are tick frames; {share(pos, lambda w: w > H):.1%} of tick frames pass it')
        print(f'tick-frame work: {pct(pos, (.1, .5, .9, .99))} ms; other: {pct(neg, (.1, .5, .9, .99))} ms')
        over = [w - T for w in pos if w > T]
        print(f'tick frames over budget: {len(over) / len(pos):.1%} (other frames {share(neg, lambda w: w > T):.1%}); '
              f'overrun {pct(over, (.5, .9, .99))} ms; over by >1 ms {share(pos, lambda w: w > T + 1e6):.1%}, '
              f'>2 ms {share(pos, lambda w: w > T + 2e6):.1%}')
        k = 1 - a.speedup
        print(f'model: tick-frame work x{k:.2f} -> within budget {share(pos, lambda w: w * k <= T):.1%} '
              f'(now {share(pos, lambda w: w <= T):.1%}); other frames {share(neg, lambda w: w * k <= T):.1%} '
              f'(now {share(neg, lambda w: w <= T):.1%}); over-budget tick frames '
              f'{share(pos, lambda w: w > T) * len(pos) / secs:.1f}/s -> {share(pos, lambda w: w * k > T) * len(pos) / secs:.1f}/s')
    elif S:
        print('no gameplay windows found')

    # flips: first-pixel-out interval of consecutive present ids
    q = {i: fr_ for i, _, fr_ in Q}
    ids = sorted(F)
    iv = []
    for i0, i1 in zip(ids, ids[1:]):
        if i1 != i0 + 1:
            continue
        d = F[i1] - F[i0]
        if not (0 < d < 100e6):
            continue
        frm = q.get(i1)
        if play_frames and frm not in play_frames:
            continue
        iv.append((F[i1], d, frm))
    if not iv:
        print('\nno flip timing in log'); return
    dur = sum(d for _, d, _ in iv) / 1e9
    lim = a.stretch_ms * 1e6
    st = [d for _, d, _ in iv if d > lim]
    print(f'\n== flips ({len(iv)} intervals, {dur:.0f} s{" of gameplay" if play_frames else ""}): mean {dur / len(iv) * 1e3:.3f} ms '
          f'= {len(iv) / dur:.1f} fps')
    print(f'interval {pct([d for _, d, _ in iv], (.01, .5, .9, .99, .999))} ms')
    print(f'stretched > {a.stretch_ms} ms: {len(st) / dur:.1f}/s ({len(st) / len(iv):.1%}); size {pct(st, (.5, .9, .99))} ms; '
          + ', '.join(f'>{x} ms {sum(d > x * 1e6 for d in st) / dur:.2f}/s' for x in (8.5, 10, round(2 * T / 1e6, 2))))
    if tickset:
        own = [frm in tickset for _, d, frm in iv if d > lim and frm in play_frames]
        print(f'stretched flips whose frame is a tick frame: {sum(own) / max(1, len(own)):.1%}')
    sec = defaultdict(int)
    for t, _, frm in iv:
        sec[play_frames[frm] if play_frames else int(t // 1e9)] += 1
    full = sorted(sec.values())[1:-1] or sorted(sec.values())
    print(f'flips per 1-s window: min {full[0]}, p5 {full[int(.05 * len(full))]}, median {full[len(full) // 2]}')
    worst = float('inf'); j = 0
    for i in range(len(iv)):
        if i and iv[i][0] - iv[i - 1][0] != iv[i][1]:
            j = i
        while iv[i][0] - iv[j][0] > 100e6:
            j += 1
        span = iv[i][0] - iv[j][0] + iv[j][1]
        if span >= 100e6:
            worst = min(worst, (i - j + 1) / (span / 1e9))
    print(f'worst 100-ms window: {worst:.0f} fps')


if __name__ == '__main__':
    main()
