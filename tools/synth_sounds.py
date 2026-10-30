#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the dcss-3ds authors
"""Synthesize the four clips that replace sounds of unknown origin.

thud, fall, falldie and paper5 in the crawl-sounds pack have no recorded
source, so the port generates its own instead (numpy only, no samples).
The output clips are released as CC0 (see SOUND-CREDITS.txt).

Format matches the rest of source/3ds/sounds/: raw PCM, signed 16-bit
little-endian, mono, 22050 Hz.

Usage: synth_sounds.py [output_dir]   (default: source/3ds/sounds)
"""
import os
import sys

import numpy as np

RATE = 22050


def t_axis(dur):
    return np.arange(int(RATE * dur)) / RATE


def onepole_lowpass(x, cutoff):
    a = np.exp(-2.0 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1.0 - a) * v + a * acc
        y[i] = acc
    return y


def bandpass(x, lo, hi):
    """Crude FFT band-pass (fine for short noise clips)."""
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(len(x), 1.0 / RATE)
    spec[(freqs < lo) | (freqs > hi)] = 0
    return np.fft.irfft(spec, len(x))


def thump(dur, f0, f1, decay, noise, rng):
    """Low sine that drops in pitch, plus a short noise transient."""
    t = t_axis(dur)
    freq = f1 + (f0 - f1) * np.exp(-t * 25.0)
    phase = 2 * np.pi * np.cumsum(freq) / RATE
    body = np.sin(phase) * np.exp(-t * decay)
    click = onepole_lowpass(rng.standard_normal(len(t)), 900.0) * np.exp(-t * 90.0)
    return body + noise * click


def whoosh(dur, rng):
    """Falling whoosh: noise through a band that sweeps downward, swelling in."""
    n = int(RATE * dur)
    noise = rng.standard_normal(n)
    out = np.zeros(n)
    seg = 512
    for start in range(0, n, seg // 2):
        end = min(start + seg, n)
        frac = start / n
        centre = 2600 * (1 - frac) + 350 * frac
        chunk = bandpass(noise[start:end] * np.hanning(end - start), centre * 0.6, centre * 1.4)
        out[start:end] += chunk
    t = t_axis(dur)
    env = np.minimum(t / (dur * 0.7), 1.0) ** 1.5
    return out * env


def normalize(x, peak_db=-1.5):
    x = x - np.mean(x)
    peak = np.max(np.abs(x)) or 1.0
    return x / peak * (10 ** (peak_db / 20))


def fade(x, fin=0.003, fout=0.02):
    n_in, n_out = int(RATE * fin), int(RATE * fout)
    x = x.copy()
    if n_in:
        x[:n_in] *= np.linspace(0, 1, n_in)
    if n_out:
        x[-n_out:] *= np.linspace(1, 0, n_out)
    return x


def make_thud(rng):
    return normalize(fade(thump(0.37, 140, 55, 14.0, 0.5, rng)))


def make_fall(rng):
    w = whoosh(0.45, rng) * 0.6
    th = thump(0.16, 120, 50, 22.0, 0.6, rng)
    out = np.concatenate([w, th])
    return normalize(fade(out))


def make_falldie(rng):
    # longer whoosh, a heavy body hit, then a smaller bounce and a settle
    w = whoosh(0.6, rng) * 0.5
    hit = thump(0.45, 110, 40, 9.0, 0.8, rng)
    bounce = thump(0.35, 130, 50, 16.0, 0.4, rng) * 0.35
    out = np.concatenate([w, hit])
    off = len(w) + int(RATE * 0.25)
    end = min(off + len(bounce), len(out))
    out[off:end] += bounce[: end - off]
    tail = np.zeros(int(RATE * 1.395) - len(out))
    return normalize(fade(np.concatenate([out, tail]), fout=0.05))


def make_paper(rng):
    # a few overlapping crinkles: bursts of high-passed noise with random
    # short envelopes, like a scroll being unrolled
    dur = 0.81
    n = int(RATE * dur)
    out = np.zeros(n)
    for _ in range(26):
        start = int(rng.uniform(0, dur - 0.08) * RATE)
        length = int(rng.uniform(0.015, 0.07) * RATE)
        burst = rng.standard_normal(length)
        env = np.exp(-np.linspace(0, rng.uniform(3, 8), length)) * rng.uniform(0.3, 1.0)
        out[start:start + length] += burst * env
    out = bandpass(out, 1800, 9000)
    t = t_axis(dur)
    swell = 0.5 + 0.5 * np.sin(np.pi * t / dur)
    return normalize(fade(out * swell, fout=0.05), peak_db=-3.0)


CLIPS = {
    "thud": make_thud,
    "fall": make_fall,
    "falldie": make_falldie,
    "paper5": make_paper,
}


def write_pcm(path, x):
    data = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    with open(path, "wb") as f:
        f.write(data.tobytes())


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for seed, (name, fn) in enumerate(CLIPS.items(), start=1):
        rng = np.random.default_rng(seed * 7919)
        path = os.path.join(out_dir, name + ".pcm")
        x = fn(rng)
        write_pcm(path, x)
        print("  %-12s %5.2f s" % (name + ".pcm", len(x) / RATE))


if __name__ == "__main__":
    default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "source", "3ds", "sounds")
    main(sys.argv[1] if len(sys.argv) > 1 else default)
