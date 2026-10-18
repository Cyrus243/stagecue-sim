# Soundcard sim: load a folder, save and recall setups

## Context

The soundcard sim (`modules/stagecue-backend/tools/soundcard-sim`, its own repo `stagecue-sim`) feeds Tessel test audio. Today each input row has its own path box: you type or paste a path and press Enter, once per input. With 16 WAV stems that is slow, and it gets worse as the input count grows (the sim tops out at 32, the loopback driver's limit). There is also no way to keep a setup, so every session starts over.

What we want: point the sim at a folder once and have it fill the inputs, then save that setup under a name and recall it later from a list inside the sim.

## Design (agreed in chat)

**Load folder.** The Control panel gets a `Folder` path box. Enter scans the folder for WAV/FLAC/MP3 files (hidden files and macOS `._*` files skipped), sorts them by name, and loads them onto In 1, In 2, … using the existing background loader (`loadFile` / `finishLoad` in `src/Ui.cpp`). To change the order you rename the files (e.g. `01 Kick.wav`). If there are more files than 32 inputs, the extras are skipped and the log says how many.

**Save setup.** A `Save as` box in the Control panel. Enter writes a plain text file with one line per input, using the same syntax as `--source` (`1=file:/abs/path/Kick.wav:-6`, `2=sine:1000:-12`, `3=silence`), plus `#` comments. You can reorder inputs by editing the file. File paths are stored as absolute paths.

**Recall setup.** A `Setups` list in the Control panel shows every saved setup by name. Enter on a name reloads every input to match. A file that's missing shows an error on its own row, and the other rows still load.

### My calls (check these)

- **Input count:** when a folder or setup needs more inputs than are running, the sim grows the input count itself (`engine.reconfigure`, capped at 32) and logs "re-select the device in Tessel". It never shrinks the count. Outputs stay as they are. (I asked about this but didn't get an answer.)
- Inputs the folder or setup doesn't cover are set to Off, so the result matches the folder or setup exactly.
- **Natural sort**, so `2 Snare` comes before `10 Tom` even without zero-padding. Matching is case-insensitive.
- Folder load and recall **stop and rewind the transport**, so all the stems are ready before you press Play (this matches how the transport behaves at startup).
- A folder load keeps each row's current level. A recalled setup applies the levels it saved.
- Setups live in `$XDG_CONFIG_HOME/stagecue-sim/setups/`, falling back to `~/.config/stagecue-sim/setups/`, on both Linux and macOS, as `<name>.setup`. Names are limited to letters, digits, space, `-` and `_`. Saving under an existing name overwrites it, and the log says so.
- **Non-goals:** a `--setup NAME` command-line flag (easy to add later on top of the same code), a file browser, drag-and-drop, reordering inside the sim, and any change to Tessel itself.

## Implementation

All changes are in this repo, on branch `as/sim-setups`, built TDD-style.


1. **New `src/SimSetup.h/.cpp`** (pure logic, added to `soundcard_sim_core` in `CMakeLists.txt`):
   - `listAudioFiles(dir, error) -> std::vector<std::filesystem::path>`: filters by extension, skips hidden files, natural sort.
   - `struct Setup { std::vector<SourceSpec> sources; }`, plus `formatSetup(const Setup&)` and `parseSetup(text, error)`. Each line goes through the existing `parseSourceSpec` (`src/SimConfig.cpp:90`). Blank lines and `#` lines are ignored, and the error names the bad line.
   - `setupsDirectory()`, `listSetups(dir)` (sorted names), `saveSetup(dir, name, setup, error)` (creates the directory), `loadSetup(dir, name, error)`, `isValidSetupName`.
   - Path expansion: move `resolvePath` out of the anonymous namespace in `src/Sources.cpp:18` into a shared header so folder paths get `~/` expansion too.
   - Add a `formatSourceSpec(const SourceSpec&)` next to `parseSourceSpec` so saving and parsing stay symmetric.

2. **Tests: `tests/SimSetupTest.cpp`** (added to `SoundcardSimTest`). Cover the round trip format→parse, comments and blank lines, a bad line's error, natural order and the extension/hidden-file filter (in a temporary directory), name validation, save then list then load in a temporary `XDG_CONFIG_HOME`, and overwriting.

3. **UI (`src/Ui.cpp`)**:
   - Control panel: add `Folder` and `Save as` `Input`s and a `Setups` `Menu`, inserted into the `controls` container and the `controlView` renderer (around `src/Ui.cpp:146`).
   - `applySources(const std::vector<SourceSpec>&)`: the shared core of folder load and recall. It grows the input count if needed (the same path as `applyIoConfig` → `populateRows`), stops and rewinds the transport, then for each row either sets Off, sets a sine (reusing the logic `syncWithEngine` already applies around `src/Ui.cpp:560`), or sets `row.path`/`row.levelDb` and calls `loadFile`. A row with a load already pending gets its new request queued, not dropped (today `loadFile` returns early on a pending load).
   - `saveCurrentSetup()`: builds a `Setup` from `inputRows` (the kind, the loaded file's resolved path, the frequency, the level), saves it, refreshes the list, and logs the result.
   - Update the footer key help and the README (the Run section, plus a new "Setups" subsection).

4. **Propagate**: commit and push in the sim, bump the `tools/soundcard-sim` pointer in stagecue-backend and push, then bump `modules/stagecue-backend` in the frontend (following the CLAUDE.md submodule rule). Don't commit until asked.

## Verification

- `cmake --build tools/soundcard-sim/build && tools/soundcard-sim/build/SoundcardSimTest`: everything green, including the new `SimSetup*` tests.
- Manual: `./gradlew runAll -PsimArgs="--inputs 8"`. Load a folder of 16 stems: the input count grows to 16, the log asks you to re-select the device, the rows load in name order, and after reselecting the device in Tessel, Play gives 16 meters moving. Save it as `band`, quit, restart the sim, pick `band` from the list: same files, same levels, same order. Move two lines in `~/.config/stagecue-sim/setups/band.setup`, recall it again, and the order follows the file. Delete one WAV and recall: that row shows an error and the rest still play.
