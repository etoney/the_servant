#pragma once
// Genre profiles: one JSON file per genre + source pair ("Metal / Rhythm Guitar").
// The same files and folder are read by The Master, so a profile made for one works in both.
//
//   built in:  compiled into the plugin (profiles/*.json)
//   user:      macOS   ~/Library/Application Support/Castle Danger/Profiles/*.json
//              Windows %APPDATA%\Castle Danger\Profiles\*.json
//
// A user file with the same genre + source replaces the built-in one.
#include <juce_core/juce_core.h>
#include "ServantDsp.h"

namespace svt
{
struct Profile
{
    juce::String genre, source, status, notes;
    float target[kBands] {}, tol[kBands] {};
    float resTol = 3.0f, hpf = 80.0f, lpf = 12000.0f, maxBoost = 6.0f, maxCut = 6.0f;
    bool fromUserFolder = false;

    juce::String key() const { return genre + " / " + source; }

    // curve = [[hz, target dB, tolerance dB], ...], interpolated in log frequency onto the 1/6-octave bands
    static bool fromJson (const juce::var& v, Profile& p)
    {
        if (v.getProperty ("format", {}).toString() != "castle-danger-profile") return false;
        p.genre  = v.getProperty ("genre", {}).toString();
        p.source = v.getProperty ("source", {}).toString();
        p.status = v.getProperty ("status", "").toString();
        p.notes  = v.getProperty ("notes", "").toString();
        auto* curve = v.getProperty ("curve", {}).getArray();
        if (p.genre.isEmpty() || p.source.isEmpty() || curve == nullptr || curve->size() < 2) return false;

        std::vector<float> lf, tg, tl;
        for (auto& pt : *curve)
        {
            auto* a = pt.getArray();
            if (a == nullptr || a->size() < 2) return false;
            const float hz = (float) (double) (*a)[0];
            if (hz <= 0.0f) return false;
            lf.push_back (std::log2 (hz));
            tg.push_back ((float) (double) (*a)[1]);
            tl.push_back (a->size() > 2 ? (float) (double) (*a)[2] : 3.0f);
        }
        for (size_t i = 1; i < lf.size(); ++i) if (lf[i] <= lf[i - 1]) return false;

        for (int k = 0; k < kBands; ++k)
        {
            const float x = std::log2 (bandHz (k));
            size_t j = 0;
            while (j + 2 < lf.size() && x > lf[j + 1]) ++j;
            const float t = juce::jlimit (0.0f, 1.0f, (x - lf[j]) / (lf[j + 1] - lf[j]));
            p.target[k] = tg[j] + (tg[j + 1] - tg[j]) * t;
            p.tol[k]    = std::max (0.5f, tl[j] + (tl[j + 1] - tl[j]) * t);
        }
        normalise (p.target);

        p.resTol   = (float) (double) v.getProperty ("resonance_tolerance_db", 3.0);
        p.hpf      = juce::jlimit (20.0f, 400.0f, (float) (double) v.getProperty ("hpf_hz", 80.0));
        p.lpf      = juce::jlimit (2000.0f, 20000.0f, (float) (double) v.getProperty ("lpf_hz", 20000.0));
        p.maxBoost = juce::jlimit (0.0f, 12.0f, (float) (double) v.getProperty ("max_boost_db", 6.0));
        p.maxCut   = juce::jlimit (0.0f, 12.0f, (float) (double) v.getProperty ("max_cut_db", 6.0));
        return true;
    }

    static bool fromString (const juce::String& json, Profile& p)
    {
        juce::var v;
        if (juce::JSON::parse (json, v).failed()) return false;
        return fromJson (v, p);
    }
};

class ProfileLibrary
{
public:
    static juce::File userFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Castle Danger").getChildFile ("Profiles");
    }

    // builtIn = the JSON texts compiled into the plugin
    void load (const juce::StringArray& builtIn, bool includeUserFolder = true)
    {
        profiles.clear();
        for (auto& s : builtIn)
        {
            Profile p;
            if (Profile::fromString (s, p)) add (p);
        }
        if (includeUserFolder)
            for (auto& f : userFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
            {
                Profile p;
                if (Profile::fromString (f.loadFileAsString(), p)) { p.fromUserFolder = true; add (p); }
            }
        std::sort (profiles.begin(), profiles.end(), [] (const Profile& a, const Profile& b)
                   { return a.genre != b.genre ? a.genre < b.genre : a.source < b.source; });
    }

    void add (const Profile& p)
    {
        for (auto& q : profiles)
            if (q.genre == p.genre && q.source == p.source) { q = p; return; }
        profiles.push_back (p);
    }

    juce::StringArray genres() const
    {
        juce::StringArray g;
        for (auto& p : profiles) g.addIfNotAlreadyThere (p.genre);
        return g;
    }
    juce::StringArray sources (const juce::String& genre) const
    {
        juce::StringArray s;
        for (auto& p : profiles) if (p.genre == genre) s.addIfNotAlreadyThere (p.source);
        return s;
    }
    const Profile* find (const juce::String& genre, const juce::String& source) const
    {
        for (auto& p : profiles) if (p.genre == genre && p.source == source) return &p;
        return nullptr;
    }
    const std::vector<Profile>& all() const { return profiles; }

private:
    std::vector<Profile> profiles;
};
} // namespace svt
