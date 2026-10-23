#!/usr/bin/env python3
"""Measures the 24-to-16 requantiser (flac/nsdither.h) through dither_test: noise spectra of TPDF and
shaped dither on silence, 1 kHz at -60/-90 dBFS, a full-scale sweep and overloads. Needs numpy, scipy, matplotlib.
  dither_test.py BIN_DIR OUT_DIR    (BIN_DIR holds dither_test)"""
import os, subprocess, sys
import numpy as np
from scipy.signal import welch
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

BIN, OUT = sys.argv[1], sys.argv[2]
FS_SINE = 32767.0 ** 2 / 2
os.makedirs(OUT, exist_ok=True)

# ISO 226:2003 threshold to 12.5 kHz, ISO 389-7 above
FI = [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500,
      3150, 4000, 5000, 6300, 8000, 10000, 12500, 14000, 16000, 18000, 20000]
TI = [78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
      -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3, 18.4, 40.2, 73.2, 110]
CB = [0, 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300,
      6400, 7700, 9500, 12000, 15500, 20000]
def thr(f): return np.interp(np.log(np.maximum(f, 20)), np.log(FI), TI)

def r468(f):
    f = np.maximum(f, 1.0)
    h1 = -4.737338981378384e-24*f**6 + 2.043828333606125e-15*f**4 - 1.363894795463638e-7*f**2 + 1
    h2 = 1.306612257412824e-19*f**5 - 2.118150887518656e-11*f**3 + 5.559488023498642e-4*f
    return 18.2 + 20*np.log10(1.246332637532143e-4*f/np.sqrt(h1**2 + h2**2))

def aw(f):
    f2 = np.maximum(f, 1.0) ** 2
    ra = 12194**2*f2*f2/((f2+20.6**2)*np.sqrt((f2+107.7**2)*(f2+737.9**2))*(f2+12194**2))
    return 20*np.log10(ra) + 2.0

H = {44100: [6838, -7824, 7702, -7405, 4481, -2432, 398, 559, -729],
     48000: [7646, -8825, 7797, -5305, 711, 2035, -3292, 2631, -1200]}   # as in nsdither.h

def run(mode, fs, x, binary='dither_test'):
    """x: float LSB, stereo (n, 2) -> int16 output, Q8 input actually used"""
    q = np.round(x * 256).astype(np.int32)
    if binary == 'noclamp':    # the same loop with an unbounded error state, channel 0 only
        h, rng, e = H[fs], np.random.default_rng(7), [0] * 9
        d = rng.integers(0, 256, (len(q), 2)).sum(1) - 255
        y = np.zeros(len(q))
        for n, v in enumerate(q[:, 0].tolist()):
            v -= (sum(a * b for a, b in zip(h, e)) + 2048) >> 12
            o = min(max((v + int(d[n]) + 128) >> 8, -32768), 32767)
            e = [(o << 8) - v] + e[:8]
            y[n] = o
        return np.stack([y, y], 1), q / 256.0
    pi, po = os.path.join(OUT, 'in.q8'), os.path.join(OUT, 'out.s16')
    q.tofile(pi)
    subprocess.run([os.path.join(BIN, binary), mode, str(fs), pi, po], check=True)
    y = np.fromfile(po, np.int16).reshape(-1, 2).astype(float)
    os.remove(pi); os.remove(po)
    return y, q / 256.0

def psd(e, fs):
    f, p = welch(e, fs, window='hann', nperseg=8192, axis=0)
    return f, p.mean(axis=1) if p.ndim > 1 else p   # LSB^2/Hz, both channels averaged

def band(f, p, lo, hi, w=None):
    s = (f >= lo) & (f < hi)
    df = f[1] - f[0]
    return np.sum(p[s] * (10 ** (w(f[s]) / 10) if w else 1)) * df

def stats(f, p):
    tot = band(f, p, 0, f[-1] + 1)
    b15 = band(f, p, 1000, 5000)
    ntr = [10*np.log10(band(f, p, lo, hi) / FS_SINE) + 110 - thr(np.sqrt(lo*hi) if lo else 50) for lo, hi in zip(CB[:-1], CB[1:])]
    return dict(total=10*np.log10(tot/FS_SINE), b15=10*np.log10(b15/FS_SINE),
                A=10*np.log10(band(f, p, 20, 20000, aw)/FS_SINE), w468=10*np.log10(band(f, p, 20, 20000, r468)/FS_SINE),
                ntr=max(ntr), ntr_band=CB[int(np.argmax(ntr))])

def tone(fs, n, dbfs, f0=1000.0):
    t = np.arange(n) / fs
    s = 32767 * 10 ** (dbfs / 20) * np.sin(2*np.pi*f0*t)
    return np.stack([s, s], 1)

def spur(f, p, skip=()):
    """highest bin over the median floor around it (20 Hz..20 kHz), skipping the tone bins"""
    from scipy.ndimage import median_filter
    m = median_filter(p, 31)
    ok = (f > 20) & (f < 20000)
    for s in skip: ok &= np.abs(f - s) > 100
    r = p[ok] / m[ok]
    k = np.argmax(r)
    return 10*np.log10(r[k]), f[ok][k]

lines = []
def log(s=''):
    print(s); lines.append(s)

N = 1 << 19
res = {}
for fs in (44100, 48000):
    log(f'== {fs} Hz (noise = output - input, dBFS re a full-scale sine; NTR: worst critical band vs threshold, FS = 110 dB SPL)')
    for name, x in [('silence', np.zeros((N, 2))), ('1k -60', tone(fs, N, -60)), ('1k -90', tone(fs, N, -90))]:
        for mode in ('tpdf', 'shaped'):
            y, xi = run(mode, fs, x)
            e = y - xi
            f, p = psd(e, fs)
            st = stats(f, p)
            fo, po = psd(y, fs)
            sp, spf = spur(fo, po, skip=[1000 * k for k in range(1, 21)] if name != 'silence' else ())
            c = np.corrcoef(e[:, 0], e[:, 1])[0, 1]
            res[(fs, name, mode)] = (f, p, fo, po)
            log(f'{name:8s} {mode:6s} total {st["total"]:7.2f}  1-5k {st["b15"]:7.2f}  A {st["A"]:7.2f}  468 {st["w468"]:7.2f}  '
                f'NTRmax {st["ntr"]:+5.1f} @{st["ntr_band"]}  spur {sp:4.1f} dB @{spf:.0f}  L/R corr {c:+.3f}')
        if name == '1k -90':
            for mode in ('tpdf', 'shaped'):
                fo, po = res[(fs, name, mode)][2:]
                h = [10*np.log10(band(fo, po, k*1000-15, k*1000+15)/FS_SINE) for k in (1, 2, 3)]
                fl = 10*np.log10(np.median(po[(fo > 1500) & (fo < 2500)]) * 30 / FS_SINE)
                log(f'   {mode:6s} -90 dBFS tone: fundamental {h[0]:.1f}, H2 {h[1]:.1f}, H3 {h[2]:.1f}, floor in 30 Hz {fl:.1f} dBFS')
    # noise modulation: DC at 32 offsets across one LSB, 2^17 samples each
    offs = np.arange(32) / 32 + 0.003
    for mode in ('tpdf', 'shaped'):
        pw, mu = [], []
        for o in offs:
            y, xi = run(mode, fs, np.full((1 << 17, 2), o))
            e = (y - xi)[4096:]
            pw.append(10*np.log10(np.mean((e - e.mean()) ** 2))); mu.append(e.mean())
        pw = np.array(pw)
        log(f'DC at 32 sub-LSB offsets {mode:6s}: noise power {pw.min():+.2f}..{pw.max():+.2f} dB re LSB^2 (spread {pw.max()-pw.min():.2f} dB), '
            f'mean error {min(mu):+.3f}..{max(mu):+.3f} LSB')
    # full-scale sweep 20 Hz..20 kHz and overloads, then silence: clipping recovery
    T = 4 * fs
    t = np.arange(T) / fs
    sweep = 32767.0 * np.sin(2*np.pi*20*(T/fs)/np.log(1000)*(np.exp(t/(T/fs)*np.log(1000)) - 1))
    over = 2 * 32767.0 * np.sin(2*np.pi*1000*t[: fs])
    dc = np.full(fs // 2, 32767.6)
    sil = np.zeros(fs)
    seq = np.concatenate([sweep, sil, over, sil, dc, -dc - 1, sil])
    marks = np.cumsum([0, T, fs, fs, fs, fs // 2, fs // 2, fs])
    for binary in ('dither_test', 'noclamp'):
        y, xi = run('shaped', fs, np.stack([seq, seq], 1), binary)
        ideal = np.clip(np.round(xi), -32768, 32767)
        d = np.abs(y[:, 0] - ideal[:, 0])
        sweep_dev = d[: T].max()
        steady = np.sqrt(np.mean((y[marks[1]:marks[2], 0]) ** 2))
        after = [y[m:m + int(0.05 * fs), 0] for m in (marks[1], marks[3], marks[6])]
        rms_after = [np.sqrt(np.mean(a ** 2)) for a in after]
        settle = []
        for m in (marks[3], marks[6]):
            seg = np.abs(y[m:m + fs, 0])
            big = np.where(seg > 20)[0]
            settle.append(0 if len(big) == 0 else big[-1] + 1)
        # held over full scale: distance from the rail after 100 samples, and any periodic pattern in it
        dv = np.concatenate([32767 - y[marks[4] + 100:marks[5], 0], y[marks[5] + 100:marks[6], 0] + 32768])
        dcdev = dv.max()
        ac = 0.0
        if dv.std() > 0:
            z = dv - dv.mean()
            ac = max(abs(np.dot(z[:-k], z[k:])) / np.dot(z, z) for k in range(16, 2000))   # past the 9-tap memory
        burst = max(np.abs(y[m:m + 64, 0]).max() for m in (marks[3], marks[6]))
        log(f'clip test {"clamped" if binary == "dither_test" else "no clamp"}: sweep max |out - clip(in)| {sweep_dev:.0f} LSB; '
            f'held over full scale: off the rail by <= {dcdev:.0f} LSB, autocorr at lags 16..2000 <= {ac:.2f}; '
            f'silence after sweep/overload/DC: rms {rms_after[0]:.2f}/{rms_after[1]:.2f}/{rms_after[2]:.2f} LSB (steady {steady:.2f}), '
            f'first 64 samples peak {burst:.0f} LSB, last |out|>20 LSB at {settle[0]}/{settle[1]}')
        res[(fs, 'clip', binary)] = y[:, 0], xi[:, 0], marks
    log()

open(os.path.join(OUT, 'dither_report.txt'), 'w').write('\n'.join(lines) + '\n')

# figures
fig, ax = plt.subplots(2, 2, figsize=(13, 8.5))
for i, fs in enumerate((44100, 48000)):
    a = ax[0][i]
    for mode, col in (('tpdf', '#888888'), ('shaped', '#1f6fd1')):
        f, p = res[(fs, 'silence', mode)][:2]
        # per-Hz density as dB SPL at 110 dB SPL full scale, critical-band-ish smoothing not applied
        a.semilogx(f[1:], 10*np.log10(p[1:] / FS_SINE) + 110, color=col, lw=1, label=f'{mode} noise (dB SPL/Hz)')
    ff = np.logspace(np.log10(20), np.log10(fs / 2), 400)
    bw = np.interp(ff, [0, 500, 20000, 24000], [100, 100, 0.2 * 20000, 0.2 * 24000])
    a.semilogx(ff, thr(ff) - 10*np.log10(np.maximum(bw, 100)), 'k--', lw=1, label='hearing threshold / critical bandwidth')
    a.set_xlim(20, fs / 2); a.set_ylim(-60, 30); a.grid(True, which='both', alpha=.3)
    a.set_title(f'Dither noise on silence, {fs} Hz (full scale = 110 dB SPL)'); a.set_xlabel('Hz'); a.legend(fontsize=8)
    a = ax[1][i]
    for mode, col in (('tpdf', '#888888'), ('shaped', '#1f6fd1')):
        fo, po = res[(fs, '1k -90', mode)][2:]
        a.semilogx(fo[1:], 10*np.log10(po[1:] * (fo[1] - fo[0]) / FS_SINE), color=col, lw=1, label=f'{mode}')
    a.set_xlim(20, fs / 2); a.set_ylim(-150, -80); a.grid(True, which='both', alpha=.3)
    a.set_title(f'Output spectrum, 1 kHz at -90 dBFS, {fs} Hz (dBFS per {fs/8192:.1f} Hz bin)'); a.set_xlabel('Hz'); a.legend(fontsize=8)
fig.tight_layout(); fig.savefig(os.path.join(OUT, 'dither_spectra.png'), dpi=110)

fig, ax = plt.subplots(1, 2, figsize=(13, 3.8))
for a, binary in zip(ax, ('dither_test', 'noclamp')):
    y, xi, m = res[(44100, 'clip', binary)]
    s0, s1 = m[2] - 200, m[3] + 400
    a.plot(np.arange(s0, s1) - m[3], y[s0:s1] - np.clip(np.round(xi[s0:s1]), -32768, 32767), lw=.6)
    a.set_title(('error clamped' if binary == 'dither_test' else 'no clamp') + ': out - clip(in), +6 dBFS sine then silence')
    a.set_xlabel('samples from the end of the overload'); a.set_ylabel('LSB'); a.grid(alpha=.3)
fig.tight_layout(); fig.savefig(os.path.join(OUT, 'dither_clip.png'), dpi=110)
