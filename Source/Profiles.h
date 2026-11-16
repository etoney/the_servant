#pragma once
// Profiles: the sound The Servant shapes a track toward.
//
// A profile is a folder of songs. Its name is the folder's name, and it's built by measuring the
// songs in it (one is enough; more make it more reliable):
//
//   macOS    ~/Library/Application Support/Castle Danger/Profiles/
//   Windows  %APPDATA%\Castle Danger\Profiles\
//       Parts/<name>/   isolated parts (a guitar stem, a bass DI...)   -> The Servant
//       Mixes/<name>/   finished, mixed songs                          -> The Master
//
// Each song is measured once; the numbers are kept in a cache file in the folder
// (.castle-danger-cache.json), so adding a song only measures the new one. No audio is copied
// anywhere. One starter profile is built in so the plugin works out of the box.
#include <map>
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "ServantDsp.h"

namespace svt
{
struct Profile
{
    juce::String name, notes;
    int songs = 0;                 // songs measured (0 = empty folder)
    bool builtIn = false;
    juce::String signature;        // changes when the folder's songs change
    float target[kBands] {}, tol[kBands] {};
    float resTol = 3.0f, hpf = 80.0f, lpf = 12000.0f, maxBoost = 6.0f, maxCut = 6.0f;

    bool usable() const { return builtIn || songs > 0; }

    //---- built-in profiles: JSON with a "curve" of [hz, target dB, tolerance dB] points
    static bool fromJson (const juce::var& v, Profile& p)
    {
        if (v.getProperty ("format", {}).toString() != "castle-danger-profile") return false;
        p.name  = v.getProperty ("name", {}).toString();
        p.notes = v.getProperty ("notes", "").toString();
        auto* curve = v.getProperty ("curve", {}).getArray();
        if (p.name.isEmpty() || curve == nullptr || curve->size() < 2) return false;

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
        p.builtIn = true;
        return true;
    }
    static bool fromString (const juce::String& json, Profile& p)
    {
        juce::var v;
        if (juce::JSON::parse (json, v).failed()) return false;
        return fromJson (v, p);
    }

    //---- a profile from measured songs (each a normalised 1/6-octave curve)
    static Profile fromSongs (const juce::String& name, const std::vector<std::array<float, kBands>>& curves)
    {
        Profile p;
        p.name = name;
        p.songs = (int) curves.size();
        if (curves.empty()) return p;
        const size_t n = curves.size();
        std::vector<float> col (n);
        for (int k = 0; k < kBands; ++k)
        {
            for (size_t i = 0; i < n; ++i) col[i] = curves[i][(size_t) k];
            std::sort (col.begin(), col.end());
            auto pct = [&col, n] (float q) { const float x = q * (float) (n - 1); const size_t i = (size_t) x; const float t = x - (float) i;
                                             return i + 1 < n ? col[i] + t * (col[i + 1] - col[i]) : col[i]; };
            p.target[k] = pct (0.5f);
            // one song can't show how much variation is normal: use a fixed allowance until there are more
            if (n == 1)      p.tol[k] = 2.5f;
            else if (n == 2) p.tol[k] = std::max (2.5f, std::abs (col[1] - col[0]) * 0.5f + 1.0f);
            else             p.tol[k] = juce::jlimit (1.5f, 6.0f, (pct (0.75f) - pct (0.25f)) * 0.5f + 1.0f);
        }
        normalise (p.target);
        for (int k = 0; k < kBands; ++k) if (p.target[k] < -12.0f) p.tol[k] = std::max (p.tol[k], 4.0f);   // the edges are less certain

        // clean-up filters: where the references fall 18 dB below their loudest mid band
        float peak = -100.0f;
        for (int k = 0; k < kBands; ++k) if (bandHz (k) >= 99.0f && bandHz (k) <= 8100.0f) peak = std::max (peak, p.target[k]);
        int lo = 0, hi = kBands - 1;
        while (lo < kBands - 1 && p.target[lo] <= peak - 18.0f) ++lo;
        while (hi > 0 && p.target[hi] <= peak - 18.0f) --hi;
        p.hpf = juce::jlimit (20.0f, 400.0f, bandHz (lo) * 0.7f);
        p.lpf = juce::jlimit (2000.0f, 20000.0f, bandHz (hi) * 1.4f);
        p.notes = "Measured from " + juce::String (n) + (n == 1 ? " song." : " songs.");
        return p;
    }

    //---- saved inside a session, so a project still opens right if the folder is gone
    juce::String toSessionString() const
    {
        juce::String s;
        s << name << "\n" << (builtIn ? -1 : songs) << "\n" << resTol << " " << hpf << " " << lpf << " " << maxBoost << " " << maxCut << "\n";
        for (int k = 0; k < kBands; ++k) s << juce::String (target[k], 3) << " " << juce::String (tol[k], 3) << (k + 1 < kBands ? " " : "");
        return s;
    }
    static bool fromSessionString (const juce::String& s, Profile& p)
    {
        auto lines = juce::StringArray::fromLines (s);
        if (lines.size() < 4) return false;
        auto nums = juce::StringArray::fromTokens (lines[2], " ", "");
        auto curve = juce::StringArray::fromTokens (lines[3], " ", "");
        if (nums.size() < 5 || curve.size() != kBands * 2) return false;
        p.name = lines[0]; p.songs = lines[1].getIntValue();
        p.resTol = nums[0].getFloatValue(); p.hpf = nums[1].getFloatValue(); p.lpf = nums[2].getFloatValue();
        p.maxBoost = nums[3].getFloatValue(); p.maxCut = nums[4].getFloatValue();
        for (int k = 0; k < kBands; ++k) { p.target[k] = curve[k * 2].getFloatValue(); p.tol[k] = curve[k * 2 + 1].getFloatValue(); }
        p.builtIn = p.songs < 0;
        return ! p.name.isEmpty();
    }
};

//==============================================================================
// Measures one audio file the same way the plugin listens (gated long-term spectrum -> 1/6-octave shape)
inline bool measureSong (juce::AudioFormatManager& fm, const juce::File& f, std::array<float, kBands>& curve, float& seconds,
                         const std::function<bool()>& shouldStop = {})
{
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr || reader->sampleRate < 8000.0 || reader->lengthInSamples <= 0) return false;
    Analyzer a;
    a.prepare (reader->sampleRate);
    a.start();
    const int block = 65536, nch = (int) std::min<unsigned int> (2, reader->numChannels);
    juce::AudioBuffer<float> buf (nch, block);
    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += block)
    {
        if (shouldStop && shouldStop()) return false;
        const int n = (int) std::min<juce::int64> (block, reader->lengthInSamples - pos);
        reader->read (&buf, 0, n, pos, true, nch > 1);
        a.push (buf.getReadPointer (0), nch > 1 ? buf.getReadPointer (1) : nullptr, n);
    }
    std::vector<float> pow, env;
    const int frames = a.getAverage (pow, env);
    if (frames < 8) return false;   // under ~1.5 s of audio above -55 dBFS
    bandCurve (pow, reader->sampleRate, curve.data());
    seconds = a.learnedSeconds();
    return true;
}

//==============================================================================
// Watches Profiles/Parts on a background thread: every folder is a profile. New or changed songs
// are measured (once; the numbers are cached in the folder), and the profile is rebuilt.
class ProfileFolders : private juce::Thread
{
public:
    static juce::File& rootOverride() { static juce::File f; return f; }   // tests point this somewhere else
    static juce::File root()
    {
        if (rootOverride() != juce::File()) return rootOverride();
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Castle Danger").getChildFile ("Profiles");
    }
    static juce::File partsFolder() { return root().getChildFile ("Parts"); }
    static juce::File mixesFolder() { return root().getChildFile ("Mixes"); }

    ProfileFolders() : juce::Thread ("Servant profile folders")
    {
        formats.registerBasicFormats();
    }
    ~ProfileFolders() override { stopThread (4000); }

    void start() { ensureFolders(); startThread (juce::Thread::Priority::low); }

    // creates the folders (and a note explaining them) the first time
    static void ensureFolders()
    {
        partsFolder().createDirectory();
        mixesFolder().createDirectory();
        auto note = root().getChildFile ("READ ME.txt");
        if (! note.existsAsFile())
            note.replaceWithText (
                "CASTLE DANGER PROFILES\n\n"
                "Every folder in here is a profile. The folder's name is the profile's name.\n"
                "Drop songs into it (WAV, AIFF, FLAC, MP3, OGG). One song is enough; more make it more reliable.\n\n"
                "Parts\\<name>\\   isolated parts for THE SERVANT: just the rhythm guitar, just the bass...\n"
                "                (from multitracks, or split out of a finished song with a stem-separation tool)\n"
                "Mixes\\<name>\\   finished, mixed songs for THE MASTER\n\n"
                "Each song is measured once. The numbers are kept in .castle-danger-cache.json in the folder.\n");
    }

    // synchronous scan (used by the thread, and directly by the tests)
    void scanNow()
    {
        const juce::ScopedLock scanning (scanLock);
        std::map<juce::String, Profile> found;
        for (auto& dir : partsFolder().findChildFiles (juce::File::findDirectories, false))
        {
            if (threadShouldExit()) return;
            const auto songs = songsIn (dir);
            juce::String sig;
            for (auto& s : songs) sig << s.getFileName() << ":" << s.getSize() << ":" << s.getLastModificationTime().toMilliseconds() << ";";
            {
                const juce::ScopedLock sl (lock);
                auto it = profiles.find (dir.getFileName());
                if (it != profiles.end() && it->second.signature == sig) { found[dir.getFileName()] = it->second; continue; }
            }
            auto p = build (dir, songs);
            p.signature = sig;
            found[dir.getFileName()] = p;
        }
        bool changed = false;
        {
            const juce::ScopedLock sl (lock);
            if (found.size() != profiles.size()) changed = true;
            else for (auto& [k, v] : found) { auto it = profiles.find (k); if (it == profiles.end() || it->second.signature != v.signature) changed = true; }
            if (changed) profiles = std::move (found);
            measuring = {};
        }
        if (changed) ++version;
    }

    std::vector<Profile> getProfiles() const
    {
        const juce::ScopedLock sl (lock);
        std::vector<Profile> v;
        for (auto& [k, p] : profiles) v.push_back (p);
        return v;
    }
    juce::String getMeasuring() const { const juce::ScopedLock sl (lock); return measuring; }
    std::atomic<int> version { 0 };
    std::atomic<int> songsMeasured { 0 };   // counts real measurements (not cache hits)

    juce::AudioFormatManager& getFormats() { return formats; }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            scanNow();
            wait (2000);
        }
    }

    juce::Array<juce::File> songsIn (const juce::File& dir) const
    {
        juce::Array<juce::File> out;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false))
            if (! f.getFileName().startsWithChar ('.') && formats.findFormatForFileExtension (f.getFileExtension()) != nullptr)
                out.add (f);
        struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()); } } cmp;
        out.sort (cmp);
        return out;
    }

    Profile build (const juce::File& dir, const juce::Array<juce::File>& songs)
    {
        auto cacheFile = dir.getChildFile (".castle-danger-cache.json");
        juce::var cache = juce::JSON::parse (cacheFile);
        juce::Array<juce::var> keep;
        std::vector<std::array<float, kBands>> curves;
        bool cacheChanged = false;
        int done = 0;
        for (auto& f : songs)
        {
            if (threadShouldExit()) break;
            const juce::String key = f.getFileName() + ":" + juce::String (f.getSize()) + ":" + juce::String (f.getLastModificationTime().toMilliseconds());
            std::array<float, kBands> curve {};
            bool have = false;
            if (auto* arr = cache.getProperty ("songs", {}).getArray())
                for (auto& s : *arr)
                    if (s.getProperty ("key", {}).toString() == key)
                        if (auto* c = s.getProperty ("curve", {}).getArray(); c != nullptr && c->size() == kBands)
                        {
                            for (int k = 0; k < kBands; ++k) curve[(size_t) k] = (float) (double) (*c)[k];
                            keep.add (s); have = true; break;
                        }
            if (! have)
            {
                {
                    const juce::ScopedLock sl (lock);
                    measuring = dir.getFileName() + ": " + juce::String (done + 1) + " OF " + juce::String (songs.size());
                }
                float secs = 0;
                if (measureSong (formats, f, curve, secs, [this] { return threadShouldExit(); }))
                {
                    ++songsMeasured;
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("key", key);
                    o->setProperty ("file", f.getFileName());
                    o->setProperty ("seconds", secs);
                    juce::Array<juce::var> c;
                    for (auto v : curve) c.add (juce::roundToInt (v * 100.0f) / 100.0);
                    o->setProperty ("curve", c);
                    keep.add (juce::var (o));
                    have = true;
                }
                cacheChanged = true;
            }
            if (have) curves.push_back (curve);
            ++done;
        }
        if (auto* arr = cache.getProperty ("songs", {}).getArray(); arr != nullptr && arr->size() != keep.size()) cacheChanged = true;
        if (cacheChanged && ! threadShouldExit())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("format", "castle-danger-measurements");
            o->setProperty ("version", 1);
            o->setProperty ("songs", keep);
            cacheFile.replaceWithText (juce::JSON::toString (juce::var (o)));
        }
        return Profile::fromSongs (dir.getFileName(), curves);
    }

    juce::AudioFormatManager formats;
    juce::CriticalSection lock, scanLock;
    std::map<juce::String, Profile> profiles;
    juce::String measuring;
};

//==============================================================================
class ProfileLibrary
{
public:
    void set (std::vector<Profile> builtIns, const std::vector<Profile>& folders)
    {
        profiles = std::move (builtIns);
        for (auto& p : folders)
        {
            bool replaced = false;
            for (auto& q : profiles) if (q.name == p.name) { q = p; replaced = true; }
            if (! replaced) profiles.push_back (p);
        }
        std::stable_sort (profiles.begin(), profiles.end(), [] (const Profile& a, const Profile& b)
                          { return a.builtIn != b.builtIn ? ! a.builtIn : a.name.compareNatural (b.name) < 0; });
    }
    const Profile* find (const juce::String& name) const
    {
        for (auto& p : profiles) if (p.name == name) return &p;
        return nullptr;
    }
    const std::vector<Profile>& all() const { return profiles; }

private:
    std::vector<Profile> profiles;
};
} // namespace svt
