#!/usr/bin/env python3
"""
Build a Castle Danger genre profile (used by The Servant and The Master) from reference tracks.

Feed it isolated parts of one kind from well-mixed songs in one genre: for example every rhythm
guitar track from your metal multitracks (Cambridge-MT "Mixing Secrets" sessions, your own
sessions), and/or rhythm-guitar stems split from finished records with stem separation (Demucs).
Only the measurements go into the profile. No audio is stored.

    python3 tools/build_profile.py --genre Metal --source "Rhythm Guitar" \
        --out profiles/metal-rhythm-guitar.json  refs/metal/rhythm-guitar/*.wav

It measures every file exactly the way the plugin does (8192-point FFT, 50% overlap, Hann window,
frames quieter than -55 dBFS ignored, 1/6-octave bands, shape normalised to the 100 Hz - 8 kHz
average), then stores the median shape and how much the references disagree (the tolerance).

Needs Python 3 with numpy and scipy. WAV files only (16/24/32-bit or float).
"""
import argparse, json, sys
import numpy as np
from scipy.io import wavfile

FFT = 8192
HOP = FFT // 2
GATE = 3.2e-6                     # mean square of a -55 dBFS frame
BANDS = [1000.0 * 2.0 ** ((k - 30) / 6.0) for k in range(55)]


def read_mono(path):
    sr, x = wavfile.read(path)
    x = np.asarray(x)
    if x.dtype.kind == "i":
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max)
    elif x.dtype.kind == "u":
        x = (x.astype(np.float64) - 128.0) / 128.0
    else:
        x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    return sr, x


def ltas(x):
    win = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(FFT) / FFT)
    acc = np.zeros(FFT // 2 + 1)
    frames = 0
    for start in range(0, len(x) - FFT + 1, HOP):
        f = x[start:start + FFT]
        if np.mean(f * f) < GATE:
            continue
        acc += np.abs(np.fft.rfft(f * win)) ** 2
        frames += 1
    return (acc / frames if frames else None), frames


def band_curve(pw, sr):
    bin_hz = sr / FFT
    nb = len(pw)
    out = []
    for f in BANDS:
        lo, hi = f * 2 ** (-1 / 12), f * 2 ** (1 / 12)
        i0 = max(1, int(np.ceil(lo / bin_hz)))
        idx = [i for i in range(i0, nb) if i * bin_hz < hi]
        if idx:
            s = pw[idx].sum()
        else:
            xb = f / bin_hz
            i = min(max(1, int(xb)), nb - 2)
            t = min(max(xb - i, 0.0), 1.0)
            s = ((1 - t) * pw[i] + t * pw[i + 1]) * (hi - lo) / bin_hz
        out.append(10 * np.log10(max(s, 1e-20)))
    out = np.array(out)
    mid = [k for k, f in enumerate(BANDS) if 99 <= f <= 8100]
    return out - out[mid].mean()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--genre", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hpf", type=float, default=None, help="clean-up high-pass in Hz (default: worked out from the references)")
    ap.add_argument("--lpf", type=float, default=None, help="clean-up low-pass in Hz (default: worked out from the references)")
    ap.add_argument("--resonance-tolerance", type=float, default=3.0)
    ap.add_argument("--max-boost", type=float, default=6.0)
    ap.add_argument("--max-cut", type=float, default=6.0)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    curves = []
    for path in a.files:
        try:
            sr, x = read_mono(path)
        except Exception as e:
            print(f"skipped {path}: {e}", file=sys.stderr)
            continue
        pw, frames = ltas(x)
        if pw is None or frames < 8:
            print(f"skipped {path}: not enough audio above -55 dBFS", file=sys.stderr)
            continue
        curves.append(band_curve(pw, sr))
        print(f"measured {path}  ({frames * HOP / sr:.1f} s)")
    if not curves:
        sys.exit("no usable reference files")

    c = np.array(curves)
    median = np.median(c, axis=0)
    mid = [k for k, f in enumerate(BANDS) if 99 <= f <= 8100]
    median -= median[mid].mean()
    if len(curves) >= 3:
        q75, q25 = np.percentile(c, [75, 25], axis=0)
        tol = np.clip((q75 - q25) / 2 + 1.0, 1.5, 6.0)
    else:
        tol = np.full(len(BANDS), 2.5)   # too few references to measure the spread
    # extremes are less certain: widen the tolerance where the curve falls away
    tol = np.where(median < -12, np.maximum(tol, 4.0), tol)

    peak = median[mid].max()
    def edge(side):
        ks = range(len(BANDS)) if side == "low" else range(len(BANDS) - 1, -1, -1)
        for k in ks:
            if median[k] > peak - 18:
                return BANDS[k]
        return BANDS[0] if side == "low" else BANDS[-1]
    hpf = a.hpf if a.hpf else float(np.clip(edge("low") * 0.7, 20, 400))
    lpf = a.lpf if a.lpf else float(np.clip(edge("high") * 1.4, 2000, 20000))

    profile = {
        "format": "castle-danger-profile",
        "version": 1,
        "genre": a.genre,
        "source": a.source,
        "status": "measured",
        "notes": f"Built by tools/build_profile.py from {len(curves)} reference file(s).",
        "curve": [[round(f, 2), round(float(m), 2), round(float(t), 2)] for f, m, t in zip(BANDS, median, tol)],
        "resonance_tolerance_db": a.resonance_tolerance,
        "hpf_hz": round(hpf, 1),
        "lpf_hz": round(lpf, 1),
        "max_boost_db": a.max_boost,
        "max_cut_db": a.max_cut,
    }
    with open(a.out, "w") as f:
        json.dump(profile, f, indent=2)
    print(f"wrote {a.out}: {a.genre} / {a.source}, {len(curves)} references, HPF {hpf:.0f} Hz, LPF {lpf:.0f} Hz")


if __name__ == "__main__":
    main()
