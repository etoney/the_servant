#pragma once
// The Servant is run by The Master: there are no part profiles any more.
//
//   - TONE (the 6 broad EQ bands), COMPRESSOR and TRIM come from The Master, decided from the
//     whole mix against the genre (the MIX profile).
//   - CLEAN-UP and RESONANCES are decided here, from what this track actually plays plus its ROLE:
//     where its useful range starts and ends, and what rings out of it.
#include <cstring>
#include "ServantDsp.h"

namespace svt
{
constexpr int kNumRoles = 10;
// what decide() needs; with no boost / cut allowance it makes no tone moves (The Master does those)
struct RoleProfile
{
    float target[kBands] {}, tol[kBands] {};
    float resTol = 3.0f, hpf = 80.0f, lpf = 20000.0f, maxBoost = 0.0f, maxCut = 0.0f;
    RoleProfile() { for (auto& t : tol) t = 3.0f; }
};

// ROLE order = the "role" parameter = the menu: vocals, guitars, bass, drums, keys, other.
struct RoleRules
{
    const char* name;
    float hpfMin, hpfMax;   // the high-pass lands in here, from where the track's content starts
    float lpfMax, lpfMin;   // 20000 = no low-pass for this role; else it lands in here, from where the content ends
    float resTol;           // how far a ring may stand out before it's ducked (dB)
};

inline const RoleRules& roleRules (int role)
{
    static const RoleRules r[] = {
        { "Lead Vocal",  90.0f, 180.0f, 20000.0f, 20000.0f, 3.0f },
        { "Backing Vocals", 100.0f, 200.0f, 20000.0f, 20000.0f, 3.0f },   // a little more low cut than the lead
        { "Lead Guitar", 70.0f, 160.0f, 14000.0f, 10000.0f, 3.0f },       // keeps the low E; trims fizz above the notes
        { "Rhythm Guitar", 70.0f, 160.0f, 12000.0f,  8000.0f, 3.0f },
        { "Bass",        30.0f,  45.0f, 20000.0f, 20000.0f, 4.0f },
        { "Kick",        30.0f,  40.0f, 20000.0f, 20000.0f, 4.5f },
        { "Snare",       70.0f, 140.0f, 20000.0f, 20000.0f, 3.0f },
        { "Drums",       30.0f,  40.0f, 20000.0f, 20000.0f, 4.5f },
        { "Keys / Pads", 40.0f, 120.0f, 20000.0f, 20000.0f, 3.5f },
        { "Other",       30.0f,  80.0f, 20000.0f, 20000.0f, 3.5f } };
    return r[juce::jlimit (0, kNumRoles - 1, role)];
}

// ROLE from the DAW's track name ("Lead Vox", "BGV 2", "Kick In", "Rythm GTR1 L", "Bass DI", "OH L"...).
// -1 when the name says nothing about the part. Checked in this order: backing vocals before vocals,
// kick before bass ("bass drum"), lead guitar before rhythm guitar.
inline int roleFromNameWords (const juce::StringArray& w);

inline int roleFromName (const juce::String& raw)
{
    // words: camelCase, letters/digits and punctuation split apart ("14_ElecGtr04DI" -> "elec gtr 04 di")
    juce::String n;
    juce::juce_wchar prev = 0;
    for (auto c : raw)
    {
        const bool alpha = juce::CharacterFunctions::isLetter (c), digit = juce::CharacterFunctions::isDigit (c);
        if (! alpha && ! digit) { n << ' '; prev = 0; continue; }
        if (prev != 0 && (juce::CharacterFunctions::isLetter (prev) != alpha
                          || (juce::CharacterFunctions::isLowerCase (prev) && juce::CharacterFunctions::isUpperCase (c)))) n << ' ';
        n << juce::CharacterFunctions::toLowerCase (c); prev = c;
    }
    juce::StringArray w; w.addTokens (n, " ", ""); w.removeEmptyStrings();
    if (w.isEmpty()) return -1;
    const int r = roleFromNameWords (w);
    if (r >= 0) return r;
    // run-together words ("drumsroom", "lvox", "bassgtr"): split out the part names and try again
    static const char* stems[] = { "overhead", "backing", "cymbal", "guitar", "vocal", "snare", "drum", "kick", "bass", "synth", "piano",
                                   "keys", "room", "vox", "gtr", "tom", "hat", "pad" };
    juce::StringArray x;
    for (auto t : w)
    {
        for (auto* st : stems)
            if (t.length() > (int) std::strlen (st) && t.contains (st)) { t = t.replace (st, juce::String (" ") + st + " "); break; }
        x.addTokens (t, " ", "");
    }
    x.removeEmptyStrings();
    if (x.size() > w.size()) { if (x.contains ("l") && x.contains ("vox")) x.add ("lv"); if (x.contains ("b") && x.contains ("vox")) x.add ("bv"); return roleFromNameWords (x); }
    return -1;
}

inline int roleFromNameWords (const juce::StringArray& w)
{
    auto has = [&] (std::initializer_list<const char*> words) { for (auto* x : words) if (w.contains (x)) return true; return false; };
    auto starts = [&] (std::initializer_list<const char*> pre) { for (auto& t : w) for (auto* x : pre) if (t.startsWith (x)) return true; return false; };
    const bool voice = has ({ "vox", "vocal", "vocals", "voc", "vo", "vx", "voice", "singer", "sing", "lv", "lvox", "ldvox", "leadvox", "rap", "scream", "screams", "harmony", "harmonies", "harm", "choir", "adlib", "adlibs" }) || starts ({ "vocal", "vox" });
    if (has ({ "bv", "bvs", "bgv", "bgvs", "bkg", "bvox", "choir", "harmony", "harmonies", "harm", "adlib", "adlibs", "gang" })
        || (voice && has ({ "backing", "back", "bk", "bg", "dbl", "double", "doubles", "oohs", "aahs" }))) return 1;
    if (voice) return 0;
    if (has ({ "kick", "kik", "kck", "bd", "kd" }) || (has ({ "bass" }) && has ({ "drum" }))) return 5;
    if (has ({ "snare", "snr", "sn", "sd" })) return 6;
    if (has ({ "drum", "drums", "kit", "oh", "ohs", "overhead", "overheads", "room", "rooms", "tom", "toms", "hat", "hats", "hh", "hihat",
              "cymbal", "cymbals", "ride", "crash", "perc", "percussion", "shaker", "tamb", "loop", "beat", "beats" })) return 7;
    if (has ({ "bass", "bs", "bas", "sub", "808" })) return 4;
    const bool guitar = has ({ "gtr", "gtrs", "guitar", "guitars", "gt", "git", "gtar", "elec", "acoustic", "ac", "dist" }) || starts ({ "guitar" });
    const bool lead = has ({ "lead", "ld", "solo", "melody", "mel" });
    if (guitar) return lead ? 2 : 3;
    if (has ({ "keys", "key", "kb", "piano", "pno", "synth", "synths", "syn", "pad", "pads", "organ", "rhodes", "wurli", "strings", "str", "string", "mellotron" })) return 8;
    if (has ({ "solo" })) return 2;                                                // "Solo": a guitar solo (a bare "Lead" could be a voice: left alone)
    if (has ({ "rhythm", "rythm", "rhy", "rhyth", "riff", "chug", "chugs" })) return 3;
    return -1;
}

struct Cleanup { float hpf = 80.0f, lpf = 20000.0f, resTol = 3.0f; juce::String why; };

// measured: the track's shape (dB per 1/6-octave band; only the shape matters)
inline Cleanup decideCleanup (const float* measured, int role)
{
    const auto& rr = roleRules (role);
    Cleanup c;
    c.resTol = rr.resTol;
    // 1/2-octave smoothing, then measure against the track's loudest region
    float sm[kBands], top = -1000.0f;
    for (int k = 0; k < kBands; ++k)
    {
        double sum = 0; int n = 0;
        for (int j = std::max (0, k - 1); j <= std::min (kBands - 1, k + 1); ++j) { sum += std::pow (10.0, measured[j] / 10.0); ++n; }
        sm[k] = (float) (10.0 * std::log10 (std::max (1e-30, sum / n)));
        top = std::max (top, sm[k]);
    }
    // where the content starts: the lowest band within 30 dB of the loudest
    int k0 = 0;
    while (k0 < kBands - 1 && sm[k0] < top - 30.0f) ++k0;
    const float start = bandHz (k0);
    c.hpf = std::round (juce::jlimit (rr.hpfMin, rr.hpfMax, start * 0.6f));
    c.why << "HIGH-PASS " << juce::roundToInt (c.hpf) << " Hz: the track's content starts around " << juce::roundToInt (start) << " Hz";
    if (rr.lpfMax < 19900.0f)
    {
        // where it ends: the highest band within 40 dB of the loudest
        int k1 = kBands - 1;
        while (k1 > 0 && sm[k1] < top - 40.0f) --k1;
        const float end = bandHz (k1);
        c.lpf = std::round (juce::jlimit (rr.lpfMin, rr.lpfMax, end * 1.5f) / 100.0f) * 100.0f;
        c.why << "; LOW-PASS " << juce::String (c.lpf / 1000.0f, 1) << " kHz: above " << juce::String (end / 1000.0f, 1) << " kHz it's fizz, not " << rr.name;
    }
    else c.why << "; no low-pass for a " << juce::String (rr.name).toLowerCase() << " part";
    return c;
}

// how much a filter change moves each of The Master's 10 bands (dB, power-averaged over the band)
inline std::array<float, 10> cleanupBandChange (double sr, float hpfOld, float lpfOld, float hpfNew, float lpfNew)
{
    static const float edges[11] = { 20, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 12000, 20000 };
    auto response = [sr] (float hpf, float lpf, double f)
    {
        double db = 0;
        if (hpf > 20.5f) { Biquad a, b; a.highpass (sr, hpf, 0.5412); b.highpass (sr, hpf, 1.3066); db += a.magnitudeDb (f, sr) + b.magnitudeDb (f, sr); }
        if (lpf < 19900.0f) { Biquad l; l.lowpass (sr, lpf, 0.7071); db += l.magnitudeDb (f, sr); }
        return db;
    };
    std::array<float, 10> out {};
    for (int b = 0; b < 10; ++b)
    {
        double pOld = 0, pNew = 0;
        for (int i = 0; i < 16; ++i)
        {
            const double f = std::min (sr * 0.45, edges[b] * std::pow ((double) edges[b + 1] / edges[b], (i + 0.5) / 16.0));
            pOld += std::pow (10.0, response (hpfOld, lpfOld, f) / 10.0);
            pNew += std::pow (10.0, response (hpfNew, lpfNew, f) / 10.0);
        }
        out[(size_t) b] = (float) (10.0 * std::log10 (std::max (1e-12, pNew) / std::max (1e-12, pOld)));
    }
    return out;
}
} // namespace svt
