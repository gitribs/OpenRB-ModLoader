# OpenRB Modloader

A JUCE VST3 / AU / standalone instrument that wraps the ReBirth TR-808 and TR-909 from
[OpenRB-338](https://github.com/pingw33n/openrb-338). The DSP is a copy of `orb-engine` in
`third_party/openrb-338`; this project does not rewrite it. Mods are `.rbm` kits, not `.rbs`
songs. Panel JPEGs inside a mod are unused.

The machines always run at 44100 Hz in 64-sample blocks, the same grid the original uses. Host
audio at any other rate is resampled on the way out. The host playhead drives the sequencers
(pattern mode only). In the standalone app the tempo counter free-runs.

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

## Buses

| Bus | Channels | Notes |
|-----|----------|--------|
| Output | stereo | mix of every instrument that is not Disabled |
| 808 BD … 808 CY | mono | 11 TR-808 instruments, mix order |
| 909 BD … 909 RC | mono | 11 TR-909 instruments, mix order |

Aux buses start disabled. **Disable** next to each sequencer omits the selected instrument
from the stereo mix (Accent has no output). Host aux routing is independent of that.
Tempo follows the host playhead.

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
