# The Servant

A free sound-matching track assistant (VST3 / AU / Standalone) by **Castle Danger Music Company**.

Put it on a track, pick a profile, hit **ANALYZE** and play the song. When playback stops, The Servant compares the track with the profile and sets everything for you:

- **CLEAN-UP:** high-pass and low-pass from the profile
- **RESONANCE:** finds up to 12 frequencies that ring and ducks only those, only while they ring (dynamic, 5 ms look-ahead)
- **TONE EQ:** up to six broad bands that move the track's shape toward the profile's target

**MIX LINK (v0.3):** with The Master on the master bus, each Servant reports its track's 10-band energy over Castle Danger Link; The Master shares the mix's tone, compression and level fixes out to the tracks that cause them (ROLE, COMPRESSOR and TRIM controls; see `controls.txt`).

Every decision lands on a knob you can override (it turns gold), with LOCK per module, AMOUNT (0-150 %), MATCH (level-matched output) and COMPARE (the dry track, latency aligned). It's built for mixing: process, print the track, move The Servant to the next one.

**v0.2.0 prototype:** custom profiles made from your own songs, plus a built-in starter. See [the design spec](https://claude.ai/artifact/P89G9jVnUdQXUVgspNSJ4n) for the roadmap (vocals and the de-esser come in v2).

See `controls.txt` for every control and `readme.txt` for installing.

## Profiles: a folder of songs

A profile is a folder of songs; its name is the profile's name. One song is enough. With three or more, the tolerance comes from how much the songs actually vary.

```
~/Library/Application Support/Castle Danger/Profiles/      (Windows: %APPDATA%\Castle Danger\Profiles\)
    Parts/<name>/    isolated parts (a guitar stem, a bass DI...)  -> The Servant
    Mixes/<name>/    finished, mixed songs                         -> The Master
```

**NEW** in the plugin makes a folder and opens it; **FOLDER** opens the selected one. A background thread watches `Parts/`, measures new songs exactly the way the plugin listens (gated long-term spectrum, 1/6-octave shape), and caches the numbers in `.castle-danger-cache.json` in each folder. Only measurements are stored, never audio. The selected profile is also saved in the session, so a project still opens correctly if the folder is gone. WAV, AIFF, FLAC, OGG and MP3 are read on both platforms (plus whatever Core Audio reads on macOS).

The built-in `profiles/metal-rhythm-guitar.json` is a hand-tuned starter so the plugin works before any profile is made.

## Building

Pushing to `main` runs GitHub Actions, which builds macOS (VST3 + AU + Standalone, universal) and Windows (VST3 + Standalone), then:

- uploads the builds as workflow artifacts, and
- pushes the release zips to the `downloads-macos` / `downloads-windows` branches.

A Linux job runs the headless checks (`test/snapshot.cpp`). They cover profiles: the built-in starter; a folder with one song becoming a profile; the measurement cache; adding a song; empty folders; NEW; and a session keeping its profile after the folder is deleted. They also cover analysis: a profile is made from a clean test riff, then the same riff is made boxy and ringing. The plugin must find the planted 900 Hz and 2.5 kHz rings without flagging the riff's harmonics, cut the boxy low-mids and land close to the clean one. They also cover transparency with everything off, COMPARE alignment, MATCH, AMOUNT, silence, and state save/load (including user overrides). It also renders the editor to PNGs (the `gui-snapshots` artifact).

Local build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Servant_VST3
```

Built with JUCE 8.0.15 (same as DREAD, Possessed, BuzzKill, Chop Shop and Dominatrix).
