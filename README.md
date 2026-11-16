# The Servant

A free genre-matching track assistant (VST3 / AU / Standalone) by **Castle Danger Music Company**.

Put it on a track, pick a genre and source, hit **ANALYZE** and play the song. When playback stops, The Servant compares the track with the genre profile and sets everything for you:

- **CLEAN-UP:** high-pass and low-pass from the profile
- **RESONANCE:** finds up to 12 frequencies that ring and ducks only those, only while they ring (dynamic, 5 ms look-ahead)
- **TONE EQ:** up to six broad bands that move the track's shape toward the genre target, staying inside the profile's tolerance

Every decision lands on a knob you can override (it turns gold), with LOCK per module, AMOUNT (0-150 %), MATCH (level-matched output) and COMPARE (the dry track, latency aligned). It's built for mixing: process, print the track, move The Servant to the next one.

**v0.1.0 prototype:** Metal / Rhythm Guitar, with a hand-tuned starter profile. See [the design spec](https://claude.ai/artifact/P89G9jVnUdQXUVgspNSJ4n) for the roadmap (guitars, bass and drums in Metal and Punk for v1; vocals and the de-esser in v2).

See `controls.txt` for every control and `readme.txt` for installing.

## Genre profiles

Profiles are JSON files (`profiles/*.json`), shared with **The Master**. User profiles go in `~/Library/Application Support/Castle Danger/Profiles/` (macOS) or `%APPDATA%\Castle Danger\Profiles\` (Windows) and replace a built-in profile with the same genre + source.

Build one from reference tracks (isolated parts from multitrack libraries, or stems split with Demucs). Only measurements go into the file, never audio:

```
python3 tools/build_profile.py --genre Metal --source "Rhythm Guitar" \
    --out profiles/metal-rhythm-guitar.json  refs/metal/rhythm-guitar/*.wav
```

It measures each file exactly the way the plugin does, and stores the median shape plus how much the references disagree (the tolerance).

## Building

Pushing to `main` runs GitHub Actions, which builds macOS (VST3 + AU + Standalone, universal) and Windows (VST3 + Standalone), then:

- uploads the builds as workflow artifacts, and
- pushes the release zips to the `downloads-macos` / `downloads-windows` branches.

A Linux job runs the headless checks (`test/snapshot.cpp`). They cover profile loading, and analysis of a boxy, ringing, distorted test riff: it must find the planted 900 Hz and 2.5 kHz rings without flagging the riff's harmonics, cut the boxy low-mids and land closer to the target. They also cover transparency with everything off, COMPARE alignment, MATCH, AMOUNT, silence, and state save/load (including user overrides). It also renders the editor to PNGs (the `gui-snapshots` artifact).

Local build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Servant_VST3
```

Built with JUCE 8.0.15 (same as DREAD, Possessed, BuzzKill, Chop Shop and Dominatrix).
