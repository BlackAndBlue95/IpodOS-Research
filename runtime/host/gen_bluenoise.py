#!/usr/bin/env python3
"""16x16 blue-noise threshold tile for RGB565 dithering (dither565.c), by void-and-cluster
(Ulichney 1993) on a torus, Gaussian sigma 1.5.
  gen_bluenoise.py > ../bluenoise.h"""
import math, random

N, SIGMA = 16, 1.5
K = [[math.exp(-(min(dx, N - dx) ** 2 + min(dy, N - dy) ** 2) / (2 * SIGMA * SIGMA)) for dx in range(N)] for dy in range(N)]


def energy(bits):
    e = [[0.0] * N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            if bits[y][x]:
                for j in range(N):
                    for i in range(N): e[j][i] += K[(j - y) % N][(i - x) % N]
    return e


def extreme(bits, want, pick):
    e = energy(bits)
    return pick(((e[y][x], y, x) for y in range(N) for x in range(N) if bits[y][x] == want))[1:]


random.seed(1)
bits = [[0] * N for _ in range(N)]
for y, x in random.sample([(y, x) for y in range(N) for x in range(N)], N * N // 10): bits[y][x] = 1
while True:                                   # initial pattern: move the tightest cluster into the largest void
    cy, cx = extreme(bits, 1, max); bits[cy][cx] = 0
    vy, vx = extreme(bits, 0, min)
    if (vy, vx) == (cy, cx): bits[cy][cx] = 1; break
    bits[vy][vx] = 1
ones = sum(map(sum, bits))
rank = [[0] * N for _ in range(N)]
b = [r[:] for r in bits]
for r in range(ones - 1, -1, -1):             # phase 1: remove clusters
    y, x = extreme(b, 1, max); b[y][x] = 0; rank[y][x] = r
b = [r[:] for r in bits]
for r in range(ones, N * N):                  # phases 2 and 3: fill voids
    y, x = extreme(b, 0, min); b[y][x] = 1; rank[y][x] = r
print('/* 16x16 blue-noise thresholds 0..255 (host/gen_bluenoise.py) */')
print('const uint8_t bluenoise[256] = {')
for y in range(N): print('    ' + ', '.join('%3d' % rank[y][x] for x in range(N)) + ',')
print('};')
