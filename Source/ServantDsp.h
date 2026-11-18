#pragma once
// The Servant: analysis, decisions and the processing chain.
//
//   Analyzer  listens to the whole track (long-term average spectrum, gated so silence and bleed
//             don't count)
//   decide()  compares it with a profile and picks the corrections:
//               - up to 12 resonances for the dynamic suppressor
//               - up to 6 broad tone-EQ bands that move the track toward the target curve
//   Engine    clean-up (HPF / LPF) -> resonance suppressor (5 ms lookahead) -> tone EQ -> MATCH
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

namespace svt
{
//==============================================================================
// 1/6-octave analysis bands, 31.25 Hz .. 16 kHz
constexpr int kBands = 55;
constexpr int kTone = 6;
constexpr int kMaxRes = 12;
constexpr int kFftOrder = 13;
constexpr int kFftSize = 1 << kFftOrder;
constexpr int kHop = kFftSize / 2;

inline float bandHz (int k) { return 1000.0f * std::pow (2.0f, (float) (k - 30) / 6.0f); }

// curves are compared by shape, not level: shift so the 100 Hz - 8 kHz average is 0 dB
inline void normalise (float* curve)
{
    double s = 0; int n = 0;
    for (int k = 0; k < kBands; ++k)
        if (bandHz (k) >= 99.0f && bandHz (k) <= 8100.0f) { s += curve[k]; ++n; }
    const float m = (float) (s / std::max (1, n));
    for (int k = 0; k < kBands; ++k) curve[k] -= m;
}

inline float toDb (double p) { return (float) (10.0 * std::log10 (std::max (p, 1e-20))); }

//==============================================================================
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] {}, z2[2] {};

    inline float process (float x, int ch) noexcept
    {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }
    void bypass() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    // RBJ cookbook
    void peak (double sr, double f, double q, double db)
    {
        f = juce::jlimit (10.0, sr * 0.45, f);
        const double A = std::pow (10.0, db / 40.0), w = juce::MathConstants<double>::twoPi * f / sr;
        const double al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void highpass (double sr, double f, double q)
    {
        f = juce::jlimit (10.0, sr * 0.45, f);
        const double w = juce::MathConstants<double>::twoPi * f / sr, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void lowpass (double sr, double f, double q)
    {
        f = juce::jlimit (10.0, sr * 0.45, f);
        const double w = juce::MathConstants<double>::twoPi * f / sr, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void bandpass (double sr, double f, double q)   // 0 dB at the centre
    {
        f = juce::jlimit (10.0, sr * 0.45, f);
        const double w = juce::MathConstants<double>::twoPi * f / sr, al = std::sin (w) / (2.0 * q), c = std::cos (w);
        set (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }

    double magnitudeDb (double f, double sr) const
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr;
        const std::complex<double> z1c = std::polar (1.0, -w), z2c = std::polar (1.0, -2.0 * w);
        const auto num = (double) b0 + (double) b1 * z1c + (double) b2 * z2c;
        const auto den = 1.0 + (double) a1 * z1c + (double) a2 * z2c;
        return 20.0 * std::log10 (std::max (1e-12, std::abs (num) / std::abs (den)));
    }
};

//==============================================================================
// Listens while the song plays. Audio thread pushes; the message thread reads the average.
class Analyzer
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        fft = std::make_unique<juce::dsp::FFT> (kFftOrder);
        window.resize (kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kFftSize);
        fifo.assign (kFftSize, 0.0f);
        work.assign (kFftSize * 2, 0.0f);
        envW = std::max (2, (int) std::round (80.0 / (sr / kFftSize)));
        mx.assign (kFftSize / 2 + 1, 0.0f);
        cum.assign (kFftSize / 2 + 2, 0.0);
        const juce::SpinLock::ScopedLockType sl (lock);
        acc.assign (kFftSize / 2 + 1, 0.0);
        accEnv.assign (kFftSize / 2 + 1, 0.0);
        frames = 0; fill = 0;
    }

    void start()
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::fill (acc.begin(), acc.end(), 0.0);
        std::fill (accEnv.begin(), accEnv.end(), 0.0);
        frames = 0; fill = 0; heardSamples = 0;
        learning = true;
    }
    void stop() { learning = false; }

    // audio thread
    void push (const float* l, const float* r, int n) noexcept
    {
        if (! learning.load() || fifo.empty()) return;
        heardSamples += n;
        for (int i = 0; i < n; ++i)
        {
            fifo[(size_t) fill++] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            if (fill == kFftSize)
            {
                frame();
                std::memmove (fifo.data(), fifo.data() + kHop, sizeof (float) * (size_t) (kFftSize - kHop));
                fill = kFftSize - kHop;
            }
        }
    }

    // message thread: average power per FFT bin over the frames that passed the gate, and the
    // average of each frame's upper envelope (harmonic lines filled in, so only real resonances stick out)
    int getAverage (std::vector<float>& out, std::vector<float>& outEnv) const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        out.assign (acc.size(), 0.0f);
        outEnv.assign (acc.size(), 0.0f);
        if (frames == 0) return 0;
        for (size_t i = 0; i < acc.size(); ++i) { out[i] = (float) (acc[i] / frames); outEnv[i] = (float) (accEnv[i] / frames); }
        return frames;
    }
    float learnedSeconds() const { return (float) (frames.load() * kHop) / (float) sr; }
    float heardSeconds() const   { return (float) heardSamples.load() / (float) sr; }
    double getSampleRate() const { return sr; }

    std::atomic<bool> learning { false };

private:
    void frame() noexcept
    {
        double e = 0;
        for (int i = 0; i < kFftSize; ++i) e += (double) fifo[(size_t) i] * fifo[(size_t) i];
        if (e / kFftSize < 3.2e-6) return;           // gate: quieter than -55 dBFS RMS doesn't count
        for (int i = 0; i < kFftSize; ++i) work[(size_t) i] = fifo[(size_t) i] * window[(size_t) i];
        std::fill (work.begin() + kFftSize, work.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (work.data(), true);
        const int nb = kFftSize / 2 + 1;
        for (int i = 0; i < nb; ++i) work[(size_t) i] *= work[(size_t) i];
        // upper envelope: max over +-80 Hz, then averaged over +-80 Hz
        for (int i = 0; i < nb; ++i)
        {
            float m = 0;
            for (int j = std::max (0, i - envW); j <= std::min (nb - 1, i + envW); ++j) m = std::max (m, work[(size_t) j]);
            mx[(size_t) i] = m;
        }
        cum[0] = 0;
        for (int i = 0; i < nb; ++i) cum[(size_t) i + 1] = cum[(size_t) i] + mx[(size_t) i];
        const juce::SpinLock::ScopedTryLockType tl (lock);
        if (! tl.isLocked()) return;
        for (int i = 0; i < nb; ++i)
        {
            const int lo = std::max (0, i - envW), hi = std::min (nb - 1, i + envW);
            acc[(size_t) i] += (double) work[(size_t) i];
            accEnv[(size_t) i] += (cum[(size_t) hi + 1] - cum[(size_t) lo]) / (hi - lo + 1);
        }
        ++frames;
    }

    double sr = 48000.0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, fifo, work, mx;
    std::vector<double> acc, accEnv, cum;
    int fill = 0, envW = 14;
    std::atomic<int> frames { 0 };
    std::atomic<int64_t> heardSamples { 0 };
    juce::SpinLock lock;
};

//==============================================================================
struct ToneBand { float f = 1000, g = 0, q = 1; };
// dRef = the narrow/broad detector ratio the track would show here with no resonance, measured
// from its own smoothed spectrum, for each sharpness in kRefQ (interpolated in between)
constexpr int kRefQs = 7;
constexpr float kRefQ[kRefQs] = { 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 11.0f, 16.0f };
struct ResBand
{
    float f = 1000, prom = 0;
    float dRef[kRefQs] {};
    float refFor (float q) const
    {
        if (q <= kRefQ[0]) return dRef[0];
        for (int i = 1; i < kRefQs; ++i)
            if (q <= kRefQ[i])
            {
                const float t = std::log (q / kRefQ[i - 1]) / std::log (kRefQ[i] / kRefQ[i - 1]);
                return dRef[i - 1] + t * (dRef[i] - dRef[i - 1]);
            }
        return dRef[kRefQs - 1];
    }
};

struct Analysis
{
    bool valid = false;
    float seconds = 0;
    float measured[kBands] {};          // the track's shape (normalised dB)
    float excessBefore = 0, excessAfter = 0;   // RMS dB outside the profile's tolerance, before / after the tone EQ
    int nTone = 0;
    ToneBand tone[kTone];
    int nRes = 0;
    ResBand res[kMaxRes];
};

// Band energies of an averaged power spectrum (one value per FFT bin, bin i = i * sr / kFftSize)
inline void bandCurve (const std::vector<float>& pow, double sr, float* out)
{
    const double binHz = sr / kFftSize;
    const int nb = (int) pow.size();
    for (int k = 0; k < kBands; ++k)
    {
        const double f = bandHz (k), lo = f * std::pow (2.0, -1.0 / 12.0), hi = f * std::pow (2.0, 1.0 / 12.0);
        double s = 0; int c = 0;
        for (int i = std::max (1, (int) std::ceil (lo / binHz)); i < nb && (double) i * binHz < hi; ++i) { s += pow[(size_t) i]; ++c; }
        if (c == 0)   // band narrower than a bin: interpolate the density
        {
            const double x = f / binHz; const int i0 = juce::jlimit (1, nb - 2, (int) x);
            const double t = juce::jlimit (0.0, 1.0, x - i0);
            s = ((1 - t) * pow[(size_t) i0] + t * pow[(size_t) i0 + 1]) * (hi - lo) / binHz;
        }
        out[k] = toDb (s);
    }
    normalise (out);
}

// The bands the tone EQ is allowed to care about, and how much of each band's difference to correct
template <typename ProfileT>
inline void excessCurve (const float* curve, const ProfileT& p, float* ex)
{
    for (int k = 0; k < kBands; ++k)
    {
        const float f = bandHz (k);
        const bool inUse = f >= p.hpf * 1.3f && f <= std::min (p.lpf / 1.3f, 14000.0f) && p.target[k] > -24.0f;
        // soft knee: within half the tolerance it's left alone; past the full tolerance the whole
        // difference counts (so a real problem gets corrected all the way, not just to the edge)
        const float e = curve[k] - p.target[k];
        const float w = juce::jlimit (0.0f, 1.0f, (std::abs (e) - 0.5f * p.tol[k]) / (0.5f * p.tol[k]));
        ex[k] = inUse ? e * w : 0.0f;
    }
}

inline float rmsOf (const float* x)
{
    double s = 0; for (int k = 0; k < kBands; ++k) s += (double) x[k] * x[k];
    return (float) std::sqrt (s / kBands);
}

template <typename ProfileT>
inline Analysis decide (const std::vector<float>& pow, const std::vector<float>& env, double sr, int frames, const ProfileT& p)
{
    Analysis a;
    if (frames <= 0 || pow.size() < (size_t) (kFftSize / 2) || env.size() != pow.size()) return a;
    a.valid = true;
    a.seconds = (float) (frames * kHop) / (float) sr;
    bandCurve (pow, sr, a.measured);

    //---- tone EQ: greedy fit of broad peaking bands to the smoothed excess
    float ex[kBands], r[kBands];
    excessCurve (a.measured, p, ex);
    a.excessBefore = rmsOf (ex);
    for (int k = 0; k < kBands; ++k)   // ~1/2-octave smoothing: the tone EQ makes broad moves only
    {
        double s = 0, w = 0;
        for (int j = -3; j <= 3; ++j)
            if (k + j >= 0 && k + j < kBands) { const double wt = 4 - std::abs (j); s += wt * ex[k + j]; w += wt; }
        r[k] = (float) (s / w);
    }
    for (int it = 0; it < kTone; ++it)
    {
        int km = 0;
        for (int k = 1; k < kBands; ++k) if (std::abs (r[k]) > std::abs (r[km])) km = k;
        if (std::abs (r[km]) < 1.0f) break;
        // width of the lobe: neighbours with the same sign and at least half the peak
        int lo = km, hi = km;
        while (lo > 0 && r[lo - 1] * r[km] > 0 && std::abs (r[lo - 1]) >= 0.5f * std::abs (r[km])) --lo;
        while (hi < kBands - 1 && r[hi + 1] * r[km] > 0 && std::abs (r[hi + 1]) >= 0.5f * std::abs (r[km])) ++hi;
        const double oct = std::max (1.0 / 3.0, (double) (hi - lo + 1) / 6.0);
        const double q = juce::jlimit (0.5, 3.0, std::sqrt (std::pow (2.0, oct)) / (std::pow (2.0, oct) - 1.0));
        const float fc = std::sqrt (bandHz (lo) * bandHz (hi));
        const float g = juce::jlimit (-p.maxCut, p.maxBoost, -r[km] * 0.9f);
        if (std::abs (g) < 0.5f) break;
        Biquad bq; bq.peak (sr, fc, q, g);
        for (int k = 0; k < kBands; ++k) r[k] += (float) bq.magnitudeDb (bandHz (k), sr);
        a.tone[a.nTone++] = { fc, g, (float) q };
    }
    std::sort (a.tone, a.tone + a.nTone, [] (const ToneBand& x, const ToneBand& y) { return x.f < y.f; });
    {
        float after[kBands];
        for (int k = 0; k < kBands; ++k)
        {
            after[k] = a.measured[k];
            for (int i = 0; i < a.nTone; ++i) { Biquad bq; bq.peak (sr, a.tone[i].f, a.tone[i].q, a.tone[i].g); after[k] += (float) bq.magnitudeDb (bandHz (k), sr); }
        }
        normalise (after);
        float ex2[kBands]; excessCurve (after, p, ex2);
        a.excessAfter = rmsOf (ex2);
    }

    //---- resonances: bumps standing out of the 1-octave-smoothed envelope spectrum
    //     (the envelope fills in the riff's harmonic lines, so shared notes don't look like resonances)
    const double binHz = sr / kFftSize;
    const int nb = (int) env.size();
    const int w = std::max (2, (int) std::round (80.0 / binHz));
    std::vector<float> db ((size_t) nb), ref ((size_t) nb);
    std::vector<double> refPow ((size_t) nb, 0.0);   // the real spectrum, 1-octave smoothed: what the detectors will see without the ring
    {
        std::vector<double> cum ((size_t) nb + 1, 0.0), cumP ((size_t) nb + 1, 0.0);
        for (int i = 0; i < nb; ++i)
        {
            cum[(size_t) i + 1] = cum[(size_t) i] + env[(size_t) i];
            cumP[(size_t) i + 1] = cumP[(size_t) i] + pow[(size_t) i];
            db[(size_t) i] = toDb (env[(size_t) i]);
        }
        for (int i = 1; i < nb; ++i)
        {
            const double f = i * binHz;
            const int lo = std::max (1, (int) (f * std::pow (2.0, -0.5) / binHz));
            const int hi = std::min (nb - 1, std::max (lo + 1, (int) (f * std::pow (2.0, 0.5) / binHz)));
            ref[(size_t) i] = toDb ((cum[(size_t) hi + 1] - cum[(size_t) lo]) / (hi - lo + 1));
            // ...leaving out the middle third-octave, so the ring itself doesn't raise its own reference
            const int clo = std::max (lo, (int) (f * std::pow (2.0, -1.0 / 6.0) / binHz)), chi = std::min (hi, (int) (f * std::pow (2.0, 1.0 / 6.0) / binHz));
            const double outer = (cumP[(size_t) hi + 1] - cumP[(size_t) lo]) - (cumP[(size_t) chi + 1] - cumP[(size_t) clo]);
            const int nOuter = (hi - lo + 1) - (chi - clo + 1);
            refPow[(size_t) i] = nOuter > 0 ? outer / nOuter : (cumP[(size_t) hi + 1] - cumP[(size_t) lo]) / (hi - lo + 1);
        }
    }
    std::vector<ResBand> cand;
    const float fMax = std::min (8000.0f, p.lpf / 1.2f);
    for (int i = std::max (w + 1, (int) (200.0 / binHz)); i < nb - w - 1 && (float) (i * binHz) < fMax; ++i)
    {
        const float prom = db[(size_t) i] - ref[(size_t) i];
        if (prom < 0.6f * p.resTol) continue;   // the envelope smears narrow rings, so its threshold sits lower
        bool isMax = true;
        for (int j = -w; j <= w && isMax; ++j) if (j != 0 && db[(size_t) (i + j)] > db[(size_t) i]) isMax = false;
        if (! isMax) continue;
        const float y0 = db[(size_t) i - 1], y1 = db[(size_t) i], y2 = db[(size_t) i + 1];
        const float den = y0 - 2 * y1 + y2;
        const float d = std::abs (den) > 1e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (y0 - y2) / den) : 0.0f;
        ResBand rb; rb.f = (float) ((i + d) * binHz); rb.prom = prom;
        // detector reference from the smoothed spectrum (what this spot would look like without the ring)
        for (int qi = 0; qi < kRefQs; ++qi)
        {
            Biquad nbq, bbq; nbq.bandpass (sr, rb.f, kRefQ[qi]); bbq.bandpass (sr, rb.f, 0.7);
            double sn = 0, sb = 0;
            const int j0 = std::max (1, (int) (rb.f / 16.0f / binHz)), j1 = std::min (nb - 1, (int) (rb.f * 4.0f / binHz));
            for (int j = j0; j <= j1; ++j)
            {
                const double fj = j * binHz;
                // detectors are two band-passes in series (steep skirts keep the low end out)
                sn += refPow[(size_t) j] * std::pow (10.0, 2.0 * nbq.magnitudeDb (fj, sr) / 10.0);
                sb += refPow[(size_t) j] * std::pow (10.0, 2.0 * bbq.magnitudeDb (fj, sr) / 10.0);
            }
            rb.dRef[qi] = toDb (sn / std::max (sb, 1e-30));
        }
        cand.push_back (rb);
    }
    std::sort (cand.begin(), cand.end(), [] (const ResBand& x, const ResBand& y) { return x.prom > y.prom; });
    for (auto& c : cand)
    {
        bool clash = false;
        for (int i = 0; i < a.nRes; ++i) if (std::abs (std::log2 (c.f / a.res[i].f)) < 0.25f) clash = true;
        if (! clash && a.nRes < kMaxRes) a.res[a.nRes++] = c;
    }
    std::sort (a.res, a.res + a.nRes, [] (const ResBand& x, const ResBand& y) { return x.f < y.f; });
    return a;
}

//==============================================================================
// What the track sends to the mix, in The Master's 10 bands (20-60, 60-120, 120-250, 250-500,
// 500-1k, 1-2k, 2-4k, 4-8k, 8-12k, 12-20k Hz): mean power per band since the last reset.
// Reported over Castle Danger Link so The Master can tell which tracks make each part of the mix.
class BandMeter
{
public:
    static constexpr int kBands = 10;
    void prepare (double sampleRate)
    {
        static const float edges[kBands + 1] = { 20, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 12000, 20000 };
        for (int b = 0; b < kBands; ++b)
        {
            hp[b].design (true, sampleRate, edges[b]);
            lp[b].design (false, sampleRate, std::min ((double) edges[b + 1], sampleRate * 0.45));
        }
        if (sampleRate != preparedRate) resetRequested.store (true);   // hosts re-prepare often: keep what was measured
        preparedRate = sampleRate;
    }
    void requestReset() { resetRequested.store (true); }
    // the clean-up filters just changed: rescale what was measured so far as if they'd always been
    // like this (message thread; dB per band)
    void requestScale (const std::array<float, kBands>& db)
    {
        for (int b = 0; b < kBands; ++b)
        {
            const double s = std::pow (10.0, db[(size_t) b] / 10.0);
            pendingScale[(size_t) b].store (s);
            out[(size_t) b].store (out[(size_t) b].load() * s);
        }
        scaleRequested.store (true);
    }

    // audio thread (the processed, trimmed output)
    void process (const float* L, const float* R, int n) noexcept
    {
        if (resetRequested.exchange (false))
        {
            for (auto& v : acc) v = 0;
            total = 0; count = 0; peak = 0; active = 0;
            for (int b = 0; b < kBands; ++b) { hp[b].reset(); lp[b].reset(); }
            scaleRequested.store (false);
        }
        if (scaleRequested.exchange (false))
        {
            double before = 0, after = 0;
            for (int b = 0; b < kBands; ++b) { before += acc[(size_t) b]; acc[(size_t) b] *= pendingScale[(size_t) b].load(); after += acc[(size_t) b]; }
            if (before > 0) total *= after / before;
        }
        double blockPow = 0;
        for (int i = 0; i < n; ++i)
        {
            const float x = 0.5f * (L[i] + (R != nullptr ? R[i] : L[i]));
            for (int b = 0; b < kBands; ++b) { const double y = lp[b].process (hp[b].process (x)); acc[(size_t) b] += y * y; }
            total += (double) x * x;
            blockPow += (double) x * x;
            peak = std::max (peak, std::abs (x));
        }
        count += n;
        if (blockPow / std::max (1, n) > 1e-8) active += n;   // -80 dBFS: the part is playing
        for (int b = 0; b < kBands; ++b) out[(size_t) b].store (count > 0 ? acc[(size_t) b] / (double) count : 0.0);
        outTotal.store (count > 0 ? total / (double) count : 0.0);
        outPeak.store (peak);
        outSeconds.store ((double) active / std::max (1.0, sr));
    }
    void setRate (double r) { sr = r; }

    // message thread
    std::array<std::atomic<double>, kBands> out {};
    std::atomic<double> outTotal { 0 }, outSeconds { 0 };
    std::atomic<float> outPeak { 0 };

private:
    struct Bq
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void design (bool high, double rate, double f)
        {
            const double w = juce::MathConstants<double>::twoPi * f / rate, c = std::cos (w), al = std::sin (w) / (2.0 * 0.7071), a0 = 1 + al;
            if (high) { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = (1 + c) / 2 / a0; }
            else      { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0;  b2 = (1 - c) / 2 / a0; }
            a1 = -2 * c / a0; a2 = (1 - al) / a0;
        }
        void reset() { z1 = z2 = 0; }
        inline double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    std::array<Bq, kBands> hp, lp;
    std::array<double, kBands> acc {};
    double total = 0, sr = 48000.0, preparedRate = 0;
    juce::int64 count = 0, active = 0;
    float peak = 0;
    std::atomic<bool> resetRequested { true }, scaleRequested { false };
    std::array<std::atomic<double>, kBands> pendingScale {};
};

struct Params
{
    float amount = 1.0f;
    bool cleanOn = true;  float hpf = 80, lpf = 12000;
    bool resOn = true;    float resDepth = 0.6f, resQ = 10.0f, resSpeed = 0.5f, resTol = 3.0f;
    bool toneOn = true;   ToneBand tone[kTone];
    bool match = true, compare = false;
    bool compOn = false; float comp = 0.0f;   // 0..1: program-dependent compressor (set by The Master, or by hand)
    int compStyle = 3;                        // 0 PUNCH, 1 SMOOTH, 2 TIGHT, 3 GLUE (resolved from the ROLE when on AUTO)
    float trimDb = 0.0f;                      // output trim AFTER match, so it really changes the track's level in the mix
    bool deessOn = false; float deess = 0.4f, deessHz = 6500.0f;   // split-band de-esser: amount 0..1, split frequency
    bool transOn = false; float transAtk = 0.0f, transSus = 0.0f;  // transient shaper: -1..+1 each (more / less attack, longer / shorter tails)
};

// How hard the track's hits stick out of its own body, for The Master: every 10 ms, the most the fast
// envelope rose over the slow one (dB). Steady pads sit near 0; a snare or a picked note jumps 10-20 dB.
struct TransStats
{
    static constexpr int kBins = 80;   // 0 .. 40 dB in 0.5 dB steps
    std::array<std::atomic<int>, kBins> hist {};
    std::atomic<bool> resetRequested { false };
    void add (float d) { ++hist[(size_t) juce::jlimit (0, kBins - 1, (int) (d * 2.0f))]; }
    // message thread: { p95, seconds }
    void read (float& p95, float& seconds) const
    {
        int total = 0;
        for (auto& h : hist) total += h.load();
        seconds = total * 0.01f;
        p95 = 0.0f;
        if (total == 0) return;
        const int need = (int) std::ceil (0.95 * total);
        int acc = 0;
        for (int b = 0; b < kBins; ++b) { acc += hist[(size_t) b].load(); if (acc >= std::max (1, need)) { p95 = (b + 0.5f) * 0.5f; return; } }
        p95 = 40.0f;
    }
};

// How sibilant the track is, for The Master: every 10 ms, how far the energy above the de-esser's
// split stands out from the whole signal (dB). Vowels sit far below 0; an "s" comes up near 0.
struct SibStats
{
    static constexpr int kBins = 100;   // -45 .. +5 dB in 0.5 dB steps
    std::array<std::atomic<int>, kBins> hist {};
    std::atomic<bool> resetRequested { false };
    void add (float rDb) { ++hist[(size_t) juce::jlimit (0, kBins - 1, (int) ((rDb + 45.0f) * 2.0f))]; }
    // message thread: { p50, p95, events per minute, seconds }
    void read (float& p50, float& p95, float& perMin, float& seconds) const
    {
        int total = 0, events = 0;
        for (int b = 0; b < kBins; ++b) { total += hist[(size_t) b].load(); if (b >= (int) ((-6.0f + 45.0f) * 2.0f)) events += hist[(size_t) b].load(); }
        seconds = total * 0.01f;
        p50 = p95 = -45.0f; perMin = 0.0f;
        if (total == 0) return;
        auto pct = [&] (double q) { int need = (int) std::ceil (q * total), acc = 0; for (int b = 0; b < kBins; ++b) { acc += hist[(size_t) b].load(); if (acc >= std::max (1, need)) return -45.0f + (b + 0.5f) * 0.5f; } return 5.0f; };
        p50 = pct (0.5); p95 = pct (0.95);
        // an "s" lasts ~100 ms = ~10 windows
        perMin = seconds > 0 ? (events / 10.0f) / (seconds / 60.0f) : 0.0f;
    }
};

// NOISE: what this track does when nobody is playing it. Every second of the raw input (before The
// Servant touches it) is checked: the quietest real (not digital-silence) seconds give the noise floor,
// and a quiet second where 50 or 60 Hz and its harmonics stand out is hum. The Master reads this to put
// BuzzKill on exactly the tracks that need it.
class NoiseMeter
{
public:
    void prepare (double rate) { sr = rate; len = std::max (1, (int) std::lround (sr)); buf.assign ((size_t) len, 0.0f); fill = 0; }
    std::atomic<bool> resetRequested { false };

    // message thread: judged against how loud the track got over the whole pass, so a count-in
    // before anything was played still counts as quiet
    struct Result { float floorDb = -120, loudDb = -120, humDb = 0; int humHz = 0, humSec = 0, quietSec = 0, seconds = 0; };
    Result read() const
    {
        Result r;
        const int n = std::min (count.load(), kMax);
        r.seconds = n;
        for (int i = 0; i < n; ++i) r.loudDb = std::max (r.loudDb, sec[(size_t) i].db);
        for (int i = 0; i < n; ++i)
        {
            const auto& s = sec[(size_t) i];
            r.floorDb = i == 0 ? s.db : std::min (r.floorDb, s.db);
            if (s.db < -42.0f && s.db < r.loudDb - 20.0f)
            {
                ++r.quietSec;
                if (s.humHz != 0) { ++r.humSec; if (s.humR > r.humDb) { r.humDb = s.humR; r.humHz = s.humHz; } }
            }
        }
        return r;
    }

    void push (const float* l, const float* r, int n) noexcept
    {
        if (resetRequested.exchange (false)) { count = 0; fill = 0; }
        if (buf.empty()) return;
        for (int i = 0; i < n; ++i)
        {
            buf[(size_t) fill] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            if (++fill >= len) { second(); fill = 0; }
        }
    }

private:
    static double goertzel (const float* x, int n, double f, double rate)
    {
        const double c = 2.0 * std::cos (juce::MathConstants<double>::twoPi * f / rate);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < n; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        return std::max (1e-30, s1 * s1 + s2 * s2 - c * s1 * s2);
    }
    double ratio (double f) const
    {
        const float* x = buf.data();
        const double p = goertzel (x, len, f, sr);
        const double nb = 0.25 * (goertzel (x, len, f - 9.0, sr) + goertzel (x, len, f - 5.0, sr) + goertzel (x, len, f + 5.0, sr) + goertzel (x, len, f + 9.0, sr));
        return 10.0 * std::log10 (p / nb);
    }
    void second() noexcept
    {
        double p = 0;
        for (int i = 0; i < len; ++i) p += (double) buf[(size_t) i] * buf[(size_t) i];
        const float db = (float) (10.0 * std::log10 (std::max (1e-20, p / len)));
        if (db < -110.0f) return;                    // digital silence (a gate, an empty part): not noise
        const int i = count.load();
        if (i >= kMax) return;
        Sec s; s.db = db;
        if (db < -42.0f)                             // could be a quiet second: is there hum in it?
        {
            const double r50 = ratio (50.0), r60 = ratio (60.0);
            const bool is60 = r60 >= r50;
            const double f0 = is60 ? 60.0 : 50.0, rr = is60 ? r60 : r50;
            const double h = std::max (ratio (2.0 * f0), ratio (3.0 * f0));
            if (rr > 12.0 && h > 6.0) { s.humR = (float) rr; s.humHz = is60 ? 60 : 50; }
        }
        sec[(size_t) i] = s;
        count = i + 1;
    }
    struct Sec { float db = -120, humR = 0; int humHz = 0; };
    static constexpr int kMax = 1800;                // 30 minutes
    std::array<Sec, kMax> sec {};
    std::atomic<int> count { 0 };
    double sr = 48000.0;
    std::vector<float> buf;
    int len = 48000, fill = 0;
};

// The compressor's character. Same AMOUNT knob; the style decides how it reacts.
struct CompStyle { float atkMs, relMs, ratioBase, ratioAmt, kneeDb, rmsMs; bool lookahead; float thrOff; const char* name; };
inline const CompStyle& compStyle (int i)
{
    static const CompStyle s[4] = {
        { 15.0f,  80.0f, 3.0f, 2.0f, 0.0f,  0.0f, false,  8.0f, "PUNCH"  },   // lets each hit's crack through, then holds the body
        {  3.0f, 250.0f, 2.0f, 2.0f, 6.0f, 10.0f, true,  10.0f, "SMOOTH" },   // vocal levelling: catches early, lets go slowly
        {  6.0f, 150.0f, 3.0f, 3.0f, 3.0f,  5.0f, true,  10.0f, "TIGHT"  },   // every note at the same level (bass)
        { 25.0f, 300.0f, 1.5f, 1.5f, 6.0f, 30.0f, false, 10.0f, "GLUE"   } }; // gentle, slow, low ratio
    return s[juce::jlimit (0, 3, i)];
}
// AUTO: the style for a ROLE (Lead Vocal, Rhythm Guitar, Bass, Kick, Snare, Drums, Keys / Pads, Other,
// Lead Guitar, Backing Vocals): SMOOTH for leads and voices, TIGHT for bass, PUNCH for drums, GLUE otherwise
inline int compStyleForRole (int role)
{
    switch (role) { case 0: case 8: case 9: return 1; case 2: return 2; case 3: case 4: case 5: return 0; default: return 3; }
}

class Engine
{
public:
    Analyzer analyzer;

    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        analyzer.prepare (sr);
        noise.prepare (sr);
        look = juce::roundToInt (0.005 * sr);
        dl.assign ((size_t) (look + 1) * 2, 0.0f);
        dd.assign ((size_t) (look + 1) * 2, 0.0f);
        dpos = 0;
        for (auto* b : { &hp1, &hp2, &lp }) b->reset();
        for (auto& b : tone) b.reset();
        for (int i = 0; i < kMaxRes; ++i) { narrow[i].reset(); broad[i].reset(); cut[i].reset(); cut[i].bypass(); envN[i] = envB[i] = 1e-9f; grSm[i] = 0; }
        inPow = outPow = 1e-6f; matchGain = 1.0f;
        compEnv = 0; compLvl = 1e-4f; compGr = 0; compRms = 0; trimGain = 1.0f;
        compDelay.assign ((size_t) (look + 1), 1.0f); compDpos = 0;
        deEnvS = deEnvF = 1e-9f; deGr = 0; deWinMax = -100; deWinN = 0; deWinAct = false; deHzCached = -1; deHp.reset(); deLp.reset(); deLp2.reset();
        tsF = tsS = tsA = tsB = 1e-9f; tsGain = 1.0f; tsWinMax = 0; tsWinN = 0; tsWinAct = false;
        cached = {}; cachedValid = false; resVersionSeen = -1;
        juce::ignoreUnused (maxBlock);
    }
    int getLatency() const { return look; }

    // message thread: the resonances to work on
    void setResonances (const ResBand* r, int n)
    {
        const juce::SpinLock::ScopedLockType sl (resLock);
        nResPending = std::min (n, kMaxRes);
        for (int i = 0; i < nResPending; ++i) resPending[i] = r[i];
        ++resVersion;
    }
    std::array<std::atomic<bool>, kMaxRes> muted {};
    std::array<std::atomic<float>, kMaxRes> grDb {};   // live gain reduction per resonance, for the display
    std::atomic<float> inPeak { 0 }, outPeak { 0 }, matchDb { 0 };
    SibStats sib;
    std::atomic<float> deGrDb { 0 };   // live de-esser gain reduction (read + reset by the editor)
    TransStats trans;
    NoiseMeter noise;
    std::atomic<float> tsDb { 0 };     // live transient-shaper move, dB, biggest |move| since the editor last read it (signed)

    void process (juce::AudioBuffer<float>& buf, const Params& p) noexcept
    {
        const int n = buf.getNumSamples(), nch = std::min (2, buf.getNumChannels());
        if (n == 0 || nch == 0) return;
        float* L = buf.getWritePointer (0);
        float* R = nch > 1 ? buf.getWritePointer (1) : nullptr;
        analyzer.push (L, R, n);
        noise.push (L, R, n);
        pickUpResonances();
        updateStatic (p);

        const float spd = juce::jlimit (0.0f, 1.0f, p.resSpeed);
        const float atkMs = 20.0f * std::pow (0.05f, spd), relMs = 250.0f * std::pow (0.1f, spd);
        const float aA = 1.0f - std::exp (-1.0f / (0.001f * atkMs * (float) sr));
        const float aR = 1.0f - std::exp (-1.0f / (0.001f * relMs * (float) sr));
        const float depth = p.resDepth * p.amount;
        const float matchA = 1.0f - std::exp (-1.0f / (1.5f * (float) sr));
        const float smA = 1.0f - std::exp (-1.0f / (0.05f * (float) sr));
        // compressor: threshold rides the track's own level (3 s), so COMP works the same on a quiet or a hot track
        const float cAmt = p.compOn ? juce::jlimit (0.0f, 1.0f, p.comp) * std::min (1.0f, p.amount) : 0.0f;
        const auto& cs = compStyle (p.compStyle);
        const float cRatio = cs.ratioBase + cs.ratioAmt * cAmt;
        const float cEnvA = std::exp (-1.0f / (0.06f * (float) sr));    // peak hold decay
        const float cAtk = 1.0f - std::exp (-1.0f / (0.001f * cs.atkMs * (float) sr)), cRel = 1.0f - std::exp (-1.0f / (0.001f * cs.relMs * (float) sr));
        const float cRmsA = cs.rmsMs > 0 ? 1.0f - std::exp (-1.0f / (0.001f * cs.rmsMs * (float) sr)) : 0.0f;
        const float cLvlA = 1.0f - std::exp (-1.0f / (3.0f * (float) sr));
        const float trimTarget = juce::Decibels::decibelsToGain (juce::jlimit (-24.0f, 12.0f, p.trimDb));
        // de-esser: the energy above the split vs the whole signal; an "s" pushes it up toward 0 dB
        const float dAmt = p.deessOn ? juce::jlimit (0.0f, 1.0f, p.deess) * std::min (1.0f, p.amount) : 0.0f;
        const float deThr = -6.0f - 10.0f * dAmt, deMax = 4.0f + 10.0f * dAmt;
        const float deAtkS = 1.0f - std::exp (-1.0f / (0.0005f * (float) sr)), deRelS = 1.0f - std::exp (-1.0f / (0.04f * (float) sr));
        const float deAtkF = 1.0f - std::exp (-1.0f / (0.005f * (float) sr)), deRelF = 1.0f - std::exp (-1.0f / (0.06f * (float) sr));
        const float grAtk = 1.0f - std::exp (-1.0f / (0.001f * (float) sr)), grRel = 1.0f - std::exp (-1.0f / (0.05f * (float) sr));
        const int deWin = std::max (1, (int) (0.01 * sr));
        if (sib.resetRequested.exchange (false)) { for (auto& h : sib.hist) h = 0; deWinMax = -100; deWinN = 0; deWinAct = false; }
        if (trans.resetRequested.exchange (false)) { for (auto& h : trans.hist) h = 0; tsWinMax = 0; tsWinN = 0; tsWinAct = false; }
        // transient shaper: ATTACK compares a fast envelope with a slow-attack one (the hit sticks out of the body),
        // SUSTAIN compares a slow-release envelope with a fast-release one (the tail hangs on after the hit)
        const float tAtk = p.transOn ? juce::jlimit (-1.0f, 1.0f, p.transAtk) * std::min (1.0f, p.amount) : 0.0f;
        const float tSus = p.transOn ? juce::jlimit (-1.0f, 1.0f, p.transSus) * std::min (1.0f, p.amount) : 0.0f;
        auto coef = [this] (float ms) { return 1.0f - std::exp (-1.0f / (0.001f * ms * (float) sr)); };
        const float tFa = coef (0.2f), tSa = coef (15.0f), tR1 = coef (60.0f), tAa = coef (0.5f), tAr = coef (25.0f), tBr = coef (300.0f);
        const float tsSm = coef (0.5f);
        float tsPeak = 0;
        float ip = 0, op = 0;

        for (int s0 = 0; s0 < n; s0 += 16)
        {
            const int s1 = std::min (n, s0 + 16);
            // detectors + suppressor coefficients for this little block
            for (int s = s0; s < s1; ++s)
            {
                float x[2] = { L[s], R != nullptr ? R[s] : L[s] };
                ip = std::max (ip, std::max (std::abs (x[0]), std::abs (x[1])));
                const float dry0 = x[0], dry1 = x[1];
                inPow += matchA * (0.5f * (dry0 * dry0 + dry1 * dry1) - inPow);
                // compressor detector on the incoming audio: the audio itself comes out 5 ms later,
                // so the gain is already down when a hit arrives (look-ahead, no transient overshoot)
                if (cAmt > 0.0f || compGr > 0.01f)
                {
                    const float ms = 0.5f * (dry0 * dry0 + dry1 * dry1);
                    const float pk = std::max (std::abs (dry0), std::abs (dry1));
                    compEnv = std::max (pk * pk, compEnv * cEnvA);
                    compRms += cRmsA * (ms - compRms);
                    if (ms > 1e-8f) compLvl += cLvlA * (ms - compLvl);
                    const float envDb = 10.0f * std::log10 ((cs.rmsMs > 0 ? compRms * 2.0f : compEnv) + 1e-12f);   // RMS of a sine reads 3 dB under its peak
                    const float thr = 10.0f * std::log10 (compLvl + 1e-12f) + cs.thrOff - 10.0f * cAmt;
                    float over = envDb - thr;
                    if (cs.kneeDb > 0.0f && std::abs (over) < cs.kneeDb * 0.5f) over = (over + cs.kneeDb * 0.5f) * (over + cs.kneeDb * 0.5f) / (2.0f * cs.kneeDb);
                    const float want = cAmt > 0.0f ? std::max (0.0f, over) * (1.0f - 1.0f / cRatio) : 0.0f;
                    compGr += (want > compGr ? cAtk : cRel) * (want - compGr);
                    float gnow = juce::Decibels::decibelsToGain (-compGr);
                    if (! cs.lookahead)
                    {   // no head start: the gain lines up with the audio, so the attack of each hit gets through
                        compDelay[(size_t) compDpos] = gnow;
                        compDpos = (compDpos + 1) % (look + 1);
                        gnow = compDelay[(size_t) compDpos];
                    }
                    compBuf[s - s0] = gnow;
                }
                else compBuf[s - s0] = 1.0f;
                if (p.cleanOn)
                    for (int c = 0; c < nch; ++c)
                    {
                        float v = x[c];
                        if (hpfOn) v = hp2.process (hp1.process (v, c), c);
                        if (lpfOn) v = lp.process (v, c);
                        x[c] = v;
                    }
                const float det = nch > 1 ? 0.5f * (x[0] + x[1]) : x[0];
                {   // de-esser detector (always runs: it also measures how sibilant the track is)
                    const float hs = deHp.process (det, 0), sp = hs * hs, fp = det * det;
                    deEnvS += (sp > deEnvS ? deAtkS : deRelS) * (sp - deEnvS);
                    deEnvF += (fp > deEnvF ? deAtkF : deRelF) * (fp - deEnvF);
                    const bool active = deEnvF > 1e-7f;   // -70 dBFS: something is playing
                    const float rDb = active ? 10.0f * std::log10 ((deEnvS + 1e-12f) / (deEnvF + 1e-12f)) : -100.0f;
                    if (active) { deWinMax = std::max (deWinMax, rDb); deWinAct = true; }
                    if (++deWinN >= deWin) { if (deWinAct) sib.add (deWinMax); deWinMax = -100; deWinN = 0; deWinAct = false; }
                    const float want = dAmt > 0.0f && active ? juce::jlimit (0.0f, deMax, (rDb - deThr) * (0.5f + dAmt)) : 0.0f;
                    deGr += (want > deGr ? grAtk : grRel) * (want - deGr);
                    deBuf[s - s0] = juce::Decibels::decibelsToGain (-deGr);
                }
                for (int i = 0; i < nRes; ++i)
                {
                    const float a = narrow[i].process (narrow[i].process (det, 0), 1), b = broad[i].process (broad[i].process (det, 0), 1);
                    envN[i] += (a * a > envN[i] ? aA : aR) * (a * a - envN[i]);
                    envB[i] += (b * b > envB[i] ? aA : aR) * (b * b - envB[i]);
                }
                // lookahead: the signal is processed 5 ms late, so the detectors see trouble coming
                const size_t w = (size_t) dpos * 2;
                dl[w] = x[0]; dl[w + 1] = x[1]; dd[w] = dry0; dd[w + 1] = dry1;
                dpos = (dpos + 1) % (look + 1);
                const size_t rd = (size_t) dpos * 2;
                L[s] = dl[rd]; if (R != nullptr) R[s] = dl[rd + 1];
                dryOut[0][s - s0] = dd[rd]; dryOut[1][s - s0] = dd[rd + 1];
            }
            for (int i = 0; i < nRes; ++i)
            {
                const float d = 10.0f * std::log10 ((envN[i] + 1e-12f) / (envB[i] + 1e-12f));
                const float over = d - dRef[i] - 0.25f * p.resTol;
                float gr = (p.resOn && ! muted[(size_t) i].load()) ? juce::jlimit (0.0f, 12.0f, 2.0f * depth * std::max (0.0f, over)) : 0.0f;
                grSm[i] += 0.35f * (gr - grSm[i]);
                grDb[(size_t) i].store (grSm[i]);
                if (grSm[i] > 0.05f) cut[i].peak (sr, res[i].f, p.resQ, -grSm[i]); else cut[i].bypass();
            }
            // apply suppressor + tone EQ + match on the delayed audio
            for (int s = s0; s < s1; ++s)
            {
                for (int c = 0; c < nch; ++c)
                {
                    float* ptr = c == 0 ? L : R;
                    float v = ptr[s];
                    for (int i = 0; i < nRes; ++i) if (grSm[i] > 0.05f) v = cut[i].process (v, c);
                    if (p.toneOn) for (int i = 0; i < kTone; ++i) if (toneActive[i]) v = tone[i].process (v, c);
                    {   // split at the de-esser frequency: low + high = the signal exactly; only the high part is turned down
                        const float lo = deLp2.process (deLp.process (v, c), c);
                        if (deBuf[s - s0] < 0.999f) v = lo + deBuf[s - s0] * (v - lo);
                    }
                    ptr[s] = v;
                }
                if (compBuf[s - s0] < 0.999f) { L[s] *= compBuf[s - s0]; if (R != nullptr) R[s] *= compBuf[s - s0]; }
                {   // transient shaper (its detector always runs: it also measures how hard the track's hits stick out)
                    const float m = R != nullptr ? 0.5f * (std::abs (L[s]) + std::abs (R[s])) : std::abs (L[s]);
                    tsF += (m > tsF ? tFa : tR1) * (m - tsF);
                    tsS += (m > tsS ? tSa : tR1) * (m - tsS);
                    tsA += (m > tsA ? tAa : tAr) * (m - tsA);
                    tsB += (m > tsB ? tAa : tBr) * (m - tsB);
                    const bool active = tsS > 3e-4f;   // ~ -70 dBFS
                    const float atkDb = active ? juce::jlimit (0.0f, 30.0f, 20.0f * std::log10 ((tsF + 1e-9f) / (tsS + 1e-9f))) : 0.0f;
                    const float susDb = active ? juce::jlimit (0.0f, 20.0f, 20.0f * std::log10 ((tsB + 1e-9f) / (tsA + 1e-9f))) : 0.0f;
                    if (active) { tsWinMax = std::max (tsWinMax, atkDb); tsWinAct = true; }
                    if (++tsWinN >= deWin) { if (tsWinAct) trans.add (tsWinMax); tsWinMax = 0; tsWinN = 0; tsWinAct = false; }
                    const float want = juce::jlimit (-12.0f, 12.0f, tAtk * 0.8f * std::max (0.0f, atkDb - 2.0f))   // 2 dB dead zone: a held note's ripple isn't a hit
                                      + juce::jlimit (-12.0f, 12.0f, tSus * 0.6f * susDb);
                    const float g = juce::Decibels::decibelsToGain (want);
                    tsGain += tsSm * (g - tsGain);
                    if (std::abs (want) > std::abs (tsPeak)) tsPeak = want;
                    if (std::abs (tsGain - 1.0f) > 1e-4f) { L[s] *= tsGain; if (R != nullptr) R[s] *= tsGain; }
                }
                const float l = L[s], r = R != nullptr ? R[s] : l;
                outPow += matchA * (0.5f * (l * l + r * r) - outPow);
                const float target = p.match && inPow > 1e-7f ? juce::jlimit (0.25f, 4.0f, std::sqrt (inPow / std::max (outPow, 1e-9f))) : 1.0f;
                matchGain += smA * (target - matchGain);
                if (p.compare)
                {
                    L[s] = dryOut[0][s - s0]; if (R != nullptr) R[s] = dryOut[1][s - s0];
                }
                else
                {
                    trimGain += smA * (trimTarget - trimGain);
                    L[s] *= matchGain * trimGain; if (R != nullptr) R[s] *= matchGain * trimGain;
                }
                op = std::max (op, std::max (std::abs (L[s]), R != nullptr ? std::abs (R[s]) : 0.0f));
            }
        }
        for (int c = 2; c < buf.getNumChannels(); ++c) buf.clear (c, 0, n);
        inPeak.store (std::max (inPeak.load(), ip));
        outPeak.store (std::max (outPeak.load(), op));
        matchDb.store (20.0f * std::log10 (matchGain));
        compGrDb.store (std::max (compGrDb.load(), compGr));
        deGrDb.store (std::max (deGrDb.load(), deGr));
        if (std::abs (tsPeak) > std::abs (tsDb.load())) tsDb.store (tsPeak);
    }
    std::atomic<float> compGrDb { 0 };   // live compressor gain reduction (read + reset by the editor)

private:
    void pickUpResonances() noexcept
    {
        if (resVersion.load() == resVersionSeen) return;
        const juce::SpinLock::ScopedTryLockType tl (resLock);
        if (! tl.isLocked()) return;
        resVersionSeen = resVersion.load();
        nRes = nResPending;
        for (int i = 0; i < nRes; ++i)
        {
            res[i] = resPending[i];
            narrow[i].reset(); broad[i].reset(); cut[i].reset(); cut[i].bypass();
            envN[i] = envB[i] = 1e-9f; grSm[i] = 0;
        }
        for (int i = nRes; i < kMaxRes; ++i) grDb[(size_t) i].store (0.0f);
        cachedQ = -1;
    }

    void updateStatic (const Params& p) noexcept
    {
        if (p.deessHz != deHzCached)
        {
            deHzCached = p.deessHz;
            deHp.highpass (sr, p.deessHz, 0.7071);
            deLp.lowpass (sr, p.deessHz, 0.7071); deLp2.lowpass (sr, p.deessHz, 0.7071);   // 24 dB/oct split: the low end stays out of it
        }
        if (cachedQ != p.resQ)
        {
            cachedQ = p.resQ;
            for (int i = 0; i < nRes; ++i)
            {
                narrow[i].bandpass (sr, res[i].f, p.resQ); broad[i].bandpass (sr, res[i].f, 0.7);
                dRef[i] = res[i].refFor (p.resQ);
            }
        }
        const bool same = cachedValid && cached.hpf == p.hpf && cached.lpf == p.lpf && cached.amount == p.amount;
        bool toneSame = same;
        for (int i = 0; i < kTone && toneSame; ++i)
            toneSame = cached.tone[i].f == p.tone[i].f && cached.tone[i].g == p.tone[i].g && cached.tone[i].q == p.tone[i].q;
        if (toneSame) return;
        cached = p; cachedValid = true;
        hpfOn = p.hpf > 20.5f;
        hp1.highpass (sr, p.hpf, 0.5412); hp2.highpass (sr, p.hpf, 1.3066);
        lpfOn = p.lpf < 19900.0f;
        lp.lowpass (sr, p.lpf, 0.7071);
        for (int i = 0; i < kTone; ++i)
        {
            const float g = p.tone[i].g * p.amount;
            toneActive[i] = std::abs (g) > 0.01f;
            tone[i].peak (sr, p.tone[i].f, p.tone[i].q, g);
        }
    }

    double sr = 48000.0;
    int look = 240, dpos = 0;
    std::vector<float> dl, dd;
    float dryOut[2][16] {};
    Biquad hp1, hp2, lp, tone[kTone], narrow[kMaxRes], broad[kMaxRes], cut[kMaxRes], deHp, deLp, deLp2;
    float deEnvS = 1e-9f, deEnvF = 1e-9f, deGr = 0, deWinMax = -100, deHzCached = -1, deBuf[16] {};
    int deWinN = 0; bool deWinAct = false;
    float tsF = 1e-9f, tsS = 1e-9f, tsA = 1e-9f, tsB = 1e-9f, tsGain = 1.0f, tsWinMax = 0;
    int tsWinN = 0; bool tsWinAct = false;
    bool hpfOn = true, lpfOn = true, toneActive[kTone] {};
    float envN[kMaxRes] {}, envB[kMaxRes] {}, grSm[kMaxRes] {}, dRef[kMaxRes] {};
    float inPow = 1e-6f, outPow = 1e-6f, matchGain = 1.0f;
    float compEnv = 0, compLvl = 1e-4f, compGr = 0, compRms = 0, trimGain = 1.0f;
    std::vector<float> compDelay = std::vector<float> (241, 1.0f);
    int compDpos = 0;
    float compBuf[16] {};
    Params cached; bool cachedValid = false; float cachedQ = -1;

    ResBand res[kMaxRes], resPending[kMaxRes];
    int nRes = 0, nResPending = 0;
    std::atomic<int> resVersion { 0 };
    int resVersionSeen = -1;
    juce::SpinLock resLock;
};
} // namespace svt
