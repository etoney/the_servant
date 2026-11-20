# The Servant

A free track assistant (VST3 / AU) by **Castle Danger Music Company**, run by **The Master** on the master bus.

Put it on your tracks and set each one's **ROLE**. The Master's **RESET** starts every Servant listening; its **PRESCRIBE** makes each one decide, from its own track and role:

- **CLEAN-UP:** high-pass from where the track's content starts (within the role's range); low-pass above the fizz on rhythm parts
- **RESONANCE:** up to 12 frequencies that ring, ducked only while they ring (dynamic, 5 ms look-ahead)

The Master decides the rest from the whole mix against the genre: **TONE EQ** (6 broad bands), **COMPRESSOR** and **TRIM**, and sets them on **APPLY**. There are no part profiles any more (v0.4): the only profile is The Master's mix profile.

Every setting lands on a knob you can override (it turns gold), with LOCK per module (TONE EQ LOCK keeps The Master away from that track's tone), AMOUNT (0-150 %), MATCH (level-matched output) and COMPARE (the dry track, latency aligned).

When the clean-up changes, The Servant rescales the 10-band energy it reports, so The Master plans from the track as it will sound.

See `controls.txt` for every control and `readme.txt` for installing.

## Building

Pushing to `main` runs GitHub Actions, which builds macOS (VST3 + AU, universal) and Windows (VST3), then:

- uploads the builds as workflow artifacts, and
- pushes the release zips to the `downloads-macos` / `downloads-windows` branches.

A Linux job runs the headless checks (`test/snapshot.cpp`): the role rules; a boxy, ringing test riff where it must find the planted 900 Hz and 2.5 kHz rings, decide a clean-up inside the role's range and make no tone moves of its own; ROLE changes re-deciding; transparency with everything off, COMPARE alignment, MATCH, AMOUNT, silence; state save/load; and the link with The Master (RESET, PRESCRIBE's analyze-finish, the rescaled band energy, TONE EQ LOCK). It also renders the editor to PNGs (the `gui-snapshots` artifact).

Local build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Servant_VST3
```

Built with JUCE 8.0.15 (same as DREAD, Possessed, Dead Air, Cutthroat and Dominatrix).
