# OpenRB Modloader

A JUCE VST3 / AU / standalone instrument that wraps the ReBirth TR-808 and TR-909 from
[OpenRB-338](https://github.com/pingw33n/openrb-338). The DSP is a copy of `orb-engine` in
`third_party/openrb-338`; this project does not rewrite it. Mods are `.rbm` kits.
Panel JPEGs inside a mod are unused.

The header open/save controls read and write authentic ReBirth 2.x `.rbs` songs. Load
applies every 808 and 909 pattern and knob from the file and selects the named mod.
Song-mode pattern sequences and 808/909 automation are kept. 303s still do not sound.
Save writes a 4.2 song with the current drums and the sequencer tracks.

The machines always run at 44100 Hz in 64-sample blocks, the same grid the original uses. Host
audio at any other rate is resampled on the way out. Pattern mode follows the host playhead.
In a DAW, song mode follows host play, stop, and position; tempo follows the DAW. Opening the
transport strip enters song mode. Standalone keeps the transport visible and switches pattern/song
with the PATTERN and SONG radios.

This plugin is GPL-3.0-only. JUCE is pulled under its AGPLv3. The engine copy is the
`orb-engine`, `rbs`, and `rbm` crates, under the same license. Copyright (C) 2026 the
OpenRB-338 authors. OpenRB-338 itself is not modified.

## Layout

```
third_party/openrb-338/crates/orb-engine   # copied engine (per-instrument process split)
third_party/openrb-338/crates/rbs           # song-format crate the engine links
third_party/openrb-338/crates/rbm           # .rbm kit loader
third_party/openrb-338/assets/Standard.rbm  # built-in kit
```

## Mods

Default scan folder:

```
/Library/Application Support/Propellerhead/Rebirth RB-338/Mods/
```

Override with `ORB_MOD_DIR`, or pick a folder in Settings (gear icon). Item 0 is always
**Standard ReBirth** (the embedded kit), so the plugin works with an empty Mods folder.

- Recrypted `.rbm` files load with the embedded key window (no DAT).
- Original `.rbm` files need `RB20FUL.DAT` from a ReBirth 2.x install, or a 2048-byte-sector
  ISO of that CD (the usual ReBirth 2.x disc image). Settings **File Locations** accepts
  `*.DAT` and `*.iso`. `ORB_RB20FUL` may point at either. The plugin finds `RB20FUL.DAT` in
  the image and reads only two 2 KB pages; the 128 MB file is not extracted or copied.
  If you scroll to a kit that needs the DAT/ISO and it is unset or the previous file was
  moved, Settings opens and asks you to locate it.
- The chosen DAT/ISO path and mods folder are kept while the plugin instance is alive
  (changing mods reuses them), stored in the project as `datPath` and `modsFolder` (restored
  on reload), and remembered in `~/Library/Application Support/OpenRB/Modloader.keypath` and
  `Modloader.modsfolder` so a new instance in any project starts with the last paths. The
  files themselves still have to exist at those locations. `ORB_MOD_DIR` still wins over the
  remembered mods folder.
- A mod that cannot be descrambled stays listed and keeps the previous kit sounding. Status:
  `needs DAT/ISO` or `bad key`.

Up/down steps through the scanned list in byte order of filenames, the same as OpenRB-338.
Loading a kit swaps samples and keeps patterns, knobs, and step data.

## Songs

The song name sits to the right of the mod picker. It shows the open `.rbs` file, or
**unsaved** until a song is loaded or saved. The folder icon opens a ReBirth 2.x song;
the floppy disk saves one. Settings stays to the right of those controls.

Open uses the vendored `rbs` reader (file versions 4.0, 4.1, and 4.2, the ReBirth 2.x
format) and the same 808 tom-tune conversion ReBirth applies to pre-4.2 files. The 808
and 909 devices are applied: all 32 patterns per machine, pattern lengths and shuffle
flags, drum knobs, GLOB tempo/loop/song-mode, and the nine song tracks. 808 and 909
pattern-select and knob automation play; 303/fx tracks are stored and written back but
do not sound. The GLOB mod name is matched against the scanned kit list (item 0 is
Standard ReBirth). Distortion, PCF, and Delay remain a recall snapshot.

A song saved in song mode opens the 909 fold arrow. In a DAW that also opens the transport
strip (Record, bar, loop start/length); the host drives play, stop, and locate. Closing the
strip returns to pattern mode without erasing the arrangement. Standalone always shows the
transport (Play, Stop, rewind, forward, Record, bar, loop, tempo) and PATTERN/SONG radios.
Record in song mode is ReBirth's: pattern buttons and knobs write TRAKs; the step grid still
edits the pattern bank. Right-click Edit can initialize the song or the current loop from the
pattern-mode state (with confirmation).

The header control next to the MIDI drag handle **prints** 808/909 song-mode TRAKs into the
DAW as named plugin-parameter automation (`808 Pattern`, `808 BD Decay`, and so on). Arm it,
set the instrument track to Latch or Write (not Touch), play from bar 1 once, then stop.
Only controllers that move after tick 0 are written. Turn Print off or stop the host to end
the gestures. Then switch to **PATTERN** so the DAW owns playback; leaving song mode on
lets TRAK chase fight the new lanes. Pattern-select lanes should be stepped/square, not
linear ramps. Logic’s AU wrapper is the safer Logic path. Ableton Live is the host most
likely to ignore plugin-originated gestures. Standalone has no Print control. Host loop
off during the take; a looped pass rewrites the same points. 303, mixer, and effects
TRAKs are not printed.

Save writes a canonical 4.2 `.rbs` that ReBirth 2.x can open. 808 and 909 knobs and
patterns are the current panel. Tracks come from the sequencer. 303s stay as they were
in the last loaded file, or constructor defaults on a new song.

## Buses

| Bus | Channels | Notes |
|-----|----------|--------|
| Output | stereo | mix of every instrument that is not Disabled (1/2) |
| 808 Mix | stereo | 808 mixer channel, after the volume fader (3/4) |
| 909 Mix | stereo | 909 mixer channel, after the volume fader (5/6) |
| 808 BD … 808 CY | mono | 11 TR-808 instruments, mix order |
| 909 BD … 909 RC | mono | 11 TR-909 instruments, mix order |

808 Mix and 909 Mix are additional main stereo outs and start enabled. Drum-slot auxes start
disabled. **MUTE** next to each sequencer omits the selected instrument from the stereo mix
and from that machine's Mix bus (Accent has no output). Header **MUTE** (focused machine)
omits that machine from Output 1/2 and leaves its Mix bus and slot auxes alone. Host aux
routing is independent of that. Instrument auxes are independent of the mixer faders.
Pattern-mode tempo follows the host playhead. In a DAW, song mode follows the host
transport; standalone uses the transport strip.

## Build

Rust 1.99.1 (see `rust-toolchain.toml`) and CMake 3.24+. macOS builds are universal
(`arm64` + `x86_64`). The first build will ask `rustup` for `aarch64-apple-darwin` if that
target is missing.

```sh
./scripts/build.sh
```

or

```sh
cargo test
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

VST3, AU, and the standalone app are copied into `output/` after the build. Nothing is copied
into the system plug-in folders.

```
output/VST3/OpenRB Modloader.vst3
output/AU/OpenRB Modloader.component
output/Standalone/OpenRB Modloader.app
```

Those copies are ad-hoc signed and are fine on this Mac. To share a zip that Gatekeeper will
load on other machines (Intel and Apple Silicon), install a Developer ID Application
certificate and a notary profile, then:

```sh
./scripts/package-macos.sh
```

That rebuilds, signs with the Developer ID, notarizes, and writes
`output/OpenRB-Modloader-macOS-vVERSION.zip`. Run `./scripts/package-macos.sh --help` for the
one-time `notarytool store-credentials` setup. `--skip-build` packages the current `output/`.
