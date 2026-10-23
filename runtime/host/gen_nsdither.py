#!/usr/bin/env python3
"""Designs flac/nsdither.h's 9-tap noise shapers (worst critical band vs the ISO 226 threshold, total
noise at most +12 dB over TPDF) and compares them with published ones. Needs numpy and scipy.
  gen_nsdither.py        prints the comparison and the Q12 tables"""
import numpy as np
from scipy.linalg import solve_toeplitz
from scipy.optimize import minimize

FI = [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500,
      3150, 4000, 5000, 6300, 8000, 10000, 12500, 14000, 16000, 18000, 20000]
TI = [78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
      -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3, 18.4, 40.2, 73.2, 110]
CB = [0, 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300,
      6400, 7700, 9500, 12000, 15500, 20000]
SPL_FS = 110.0          # a full-scale sine at 110 dB SPL: loud headphone listening of a quiet passage
TAPS, MAXTOT = 9, 12.0
N = 4096
PUB = {  # Lipshitz/Wannamaker filters as published for 44.1 kHz (SoX's tables)
    'Lipshitz 5 (E)': [2.033, -2.165, 1.959, -1.590, 0.6149],
    'Wannamaker 9 F': [2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847],
    'Wannamaker 9 mod-E': [1.662, -1.263, 0.4827, -0.2913, 0.1268, -0.1124, 0.03252, -0.01265, -0.03524],
    'Wannamaker 9 imp-E': [2.847, -4.685, 6.214, -7.184, 6.639, -5.032, 3.263, -1.632, 0.4191],
}

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

def ntf2(h):
    """|NTF|^2 of the loop (NTF = 1 - sum h_k z^-k) on N bins over 0..fs/2"""
    return np.abs(np.fft.rfft(np.concatenate([[1.0], -np.asarray(h, float)]), 2 * N)[:N]) ** 2

def bands(fs):
    f = (np.arange(N) + 0.5) * fs / 2 / N
    out = []
    for lo, hi in zip(CB[:-1], CB[1:]):
        s = np.where((f >= lo) & (f < hi))[0]
        if len(s): out.append((s, thr(np.sqrt(lo * hi) if lo else 50)))
    return f, out

def ntr(h, fs, B):
    """per critical band: TPDF (1/4 LSB^2 white) through the NTF, dB over the threshold"""
    dens = 0.25 / (fs / 2) * ntf2(h) * fs / 2 / N
    return np.array([10*np.log10(np.sum(dens[s]) / (32767.0**2 / 2)) + SPL_FS - t for s, t in B])

def lpc_start(fs, cap):
    """min-phase fit of the threshold shape, capped at +cap dB: a starting point"""
    n = 1 << 14
    f = np.arange(n + 1) * fs / 2 / n
    t = np.minimum(np.clip(thr(f) - thr(3150.0), -60, None), cap)
    p = 10 ** (-t / 10)
    r = np.fft.ifft(np.concatenate([p, p[-2:0:-1]])).real[:TAPS + 1]
    return solve_toeplitz(r[:TAPS], r[1:TAPS + 1])

def design(fs):
    f, B = bands(fs)
    obj = lambda h: 0.3 * np.log(np.sum(np.exp(ntr(h, fs, B) / 0.3)))
    cons = [{'type': 'ineq', 'fun': lambda h: MAXTOT - 10*np.log10(1 + np.sum(h * h))}]
    rng = np.random.default_rng(1)
    starts = [lpc_start(fs, c) for c in (20, 25, 30, 35, 40)] + [np.array(PUB['Wannamaker 9 F'])]
    best = None
    for k in range(24):
        h0 = starts[k] if k < len(starts) else best + rng.normal(0, 0.2, TAPS)
        r = minimize(obj, h0, method='SLSQP', constraints=cons, options={'maxiter': 400, 'ftol': 1e-7})
        if 10*np.log10(1 + np.sum(r.x**2)) > MAXTOT + 0.05: continue
        if best is None or ntr(r.x, fs, B).max() < ntr(best, fs, B).max(): best = r.x
    return np.round(best * 4096).astype(int)

def report(name, h, fs):
    f, B = bands(fs)
    m2 = ntf2(h)
    w = lambda wf: 10*np.log10(np.sum(m2[f < 20000] * 10**(wf(f[f < 20000])/10)) / np.sum(10**(wf(f[f < 20000])/10)))
    b15 = (f >= 1000) & (f < 5000)
    v = ntr(h, fs, B)
    print(f'{name:20s} NTRmax {v.max():+5.1f}  total {10*np.log10(m2.mean()):+5.1f}  1-5k {10*np.log10(m2[b15].mean()):+6.1f}  '
          f'468 {w(r468):+5.1f}  A {w(aw):+5.1f}  peak {10*np.log10(m2.max()):4.1f}  sum|NTF| {1 + np.sum(np.abs(h)):4.1f}')

if __name__ == '__main__':
    for fs in (44100, 48000):
        print(f'--- {fs} Hz: dB re flat TPDF; NTRmax = worst critical band over threshold (absolute, FS = {SPL_FS:.0f} dB SPL)')
        report('flat TPDF', [], fs)
        for k, h in PUB.items(): report(k, h, fs)
        q = design(fs)
        report('minimax 9 (Q12)', q / 4096, fs)
        print('    static const int16_t ns_h%d[NS_TAPS] = { %s };' % (fs // 1000, ', '.join(str(x) for x in q)))
