# Folder Load and Setups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load a folder of stems onto the sim's inputs in one go, and save/recall named input setups from a list in the sim.

**Architecture:** Pure logic (folder scan, natural sort, setup text format, setup storage) lives in a new `SimSetup` unit in `soundcard_sim_core` and is unit-tested. `Ui.cpp` gets one shared `applySources()` that folder load and setup recall both feed, built on the existing background loader. The setup file format is the `--source` spec syntax, one line per input.

**Tech Stack:** C++20, FTXUI 6.1.9, miniaudio, GoogleTest, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-10-17-folder-load-and-setups-design.md`

## Global Constraints

- Max inputs: `kMaxChannels` (32). Grow the input count when needed, never shrink. Outputs unchanged.
- Audio extensions: `.wav`, `.flac`, `.mp3`, case-insensitive. Skip names starting with `.` (covers `._*`) and anything that isn't a regular file.
- Setup files: `<setupsDirectory()>/<name>.setup`; `setupsDirectory()` = `$XDG_CONFIG_HOME/stagecue-sim/setups` if set and non-empty, else `$HOME/.config/stagecue-sim/setups` (Linux and macOS alike).
- Setup names: 1–64 chars of `[A-Za-z0-9 _-]`, not starting or ending with a space.
- File paths in setups are absolute (via `resolvePath`).
- Folder load and recall stop the transport and rewind to 0:00.
- Inputs not covered by the folder/setup are set to Off.
- No commits without the user's go-ahead (repo CLAUDE.md). Commit steps give the message to use once allowed.
- Build/test: `cmake --build build && ./build/SoundcardSimTest` from `tools/soundcard-sim`.

## Review Focus

1. Paths with spaces, `:` and non-ASCII (`/tmp/My Stems/a:b ü.wav`) must round-trip through a setup file exactly → Task 1 and Task 3 tests.
2. Real stem folders hold junk: `.WAV` uppercase, `._Kick.wav`, `.DS_Store`, `notes.txt`, a subfolder named `old.wav` → Task 2 test.
3. Recall or folder load while earlier loads are still decoding must not let a stale result overwrite the new choice → Task 4 manual check (UI, no unit harness).
4. A hand-edited setup with the same input twice is an error naming the line, not a silent last-wins → Task 3 test.
5. First run: no setups directory yet → empty list, no error; a folder path that doesn't exist → error message → Task 2 and Task 3 tests.

---

### Task 1: `formatSourceSpec` and a shared `resolvePath`

**Files:**
- Modify: `src/SimConfig.h`, `src/SimConfig.cpp` (next to `parseSourceSpec`)
- Modify: `src/Sources.h`, `src/Sources.cpp:16-26` (move `resolvePath` out of the anonymous namespace)
- Test: `tests/SimConfigTest.cpp`

**Interfaces:**
- Produces: `std::string sim::formatSourceSpec(const SourceSpec&)` in `SimConfig.h`; `std::filesystem::path sim::resolvePath(const std::string&)` declared in `Sources.h`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(SimConfig, FormatsSourceSpecsThatParseBack)
{
    std::string error;
    for (const char* text : { "1=silence", "2=sine:440:-6", "3=file:/tmp/My Stems/a:b ü.wav:-12" })
    {
        const auto spec = parseSourceSpec(text, error);
        ASSERT_TRUE(spec) << error;
        EXPECT_EQ(formatSourceSpec(*spec), text);
    }
}

TEST(SimConfig, FormattedFileSpecAlwaysCarriesALevel)
{
    // A path ending in ":12" would otherwise read back as a level.
    SourceSpec spec;
    spec.input = 0;
    spec.kind = SourceSpec::Kind::File;
    spec.filePath = "/tmp/take:12";
    spec.levelDb = -12.0f;
    std::string error;
    const auto parsed = parseSourceSpec(formatSourceSpec(spec), error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->filePath, "/tmp/take:12");
}
```

- [ ] **Step 2: Run, expect a compile failure** (`formatSourceSpec` undeclared).

- [ ] **Step 3: Implement.** Format is `N=silence`, `N=sine:HZ:DB`, `N=file:PATH:DB` with 1-based N. Numbers: shortest form that reads back (integers without decimals, e.g. `440`, `-6`; otherwise up to 2 decimals, trailing zeros trimmed). Move `resolvePath` unchanged into the `sim` namespace and declare it in `Sources.h`.

- [ ] **Step 4: Run `./build/SoundcardSimTest`, expect all PASS.**

- [ ] **Step 5: Commit** — `feat: format source specs so setups can be written back`

---

### Task 2: Folder scan with natural sort

**Files:**
- Create: `src/SimSetup.h`, `src/SimSetup.cpp` (add to `soundcard_sim_core` in `CMakeLists.txt`)
- Test: `tests/SimSetupTest.cpp` (add to `SoundcardSimTest` in `CMakeLists.txt`)

**Interfaces:**
- Consumes: `resolvePath` (Task 1).
- Produces: `bool sim::naturalLess(const std::string& a, const std::string& b)`; `std::vector<std::filesystem::path> sim::listAudioFiles(const std::string& folder, std::string& error)` — resolves `~/`, returns paths sorted by `naturalLess` on the filename; on a missing/unreadable folder returns empty and sets `error` to `"no such folder: <path>"` / the filesystem message.

- [ ] **Step 1: Write the failing tests.** Use a fixture that creates a fresh directory under `std::filesystem::temp_directory_path()` and removes it in `TearDown`.

```cpp
TEST(SimSetup, NaturalOrderComparesNumbersByValueAndIgnoresCase)
{
    EXPECT_TRUE(naturalLess("2 Snare.wav", "10 Tom.wav"));
    EXPECT_TRUE(naturalLess("01 kick.wav", "02 Snare.wav"));
    EXPECT_TRUE(naturalLess("bass.wav", "Drums.wav"));
    EXPECT_FALSE(naturalLess("10 Tom.wav", "2 Snare.wav"));
}

TEST_F(TempDirTest, ListsOnlyAudioFilesInNaturalOrder)
{
    for (const char* name : { "10 Tom.WAV", "2 Snare.flac", "1 Kick.wav", "3 Gtr.mp3",
                              "._1 Kick.wav", ".DS_Store", "notes.txt" })
        touch(dir / name);
    std::filesystem::create_directory(dir / "old.wav");

    std::string error;
    const auto files = listAudioFiles(dir.string(), error);
    EXPECT_TRUE(error.empty()) << error;
    std::vector<std::string> names;
    for (const auto& f : files) names.push_back(f.filename().string());
    EXPECT_EQ(names, (std::vector<std::string>{ "1 Kick.wav", "2 Snare.flac", "3 Gtr.mp3", "10 Tom.WAV" }));
    EXPECT_TRUE(files.front().is_absolute());
}

TEST(SimSetup, MissingFolderIsAnError)
{
    std::string error;
    EXPECT_TRUE(listAudioFiles("/definitely/not/here", error).empty());
    EXPECT_FALSE(error.empty());
}
```

- [ ] **Step 2: Run, expect compile failure.**

- [ ] **Step 3: Implement.** `naturalLess`: walk both strings; when both sides are at a digit, compare the digit runs by value (strip leading zeros, then by length, then lexically), ties broken by the shorter run first; otherwise compare `std::tolower` chars. Full tie → plain `a < b` so the order is total. `listAudioFiles` uses `std::filesystem::directory_iterator` with `std::error_code` overloads (no throws).

- [ ] **Step 4: Run tests, expect PASS.**

- [ ] **Step 5: Commit** — `feat: scan a folder for stems in natural name order`

---

### Task 3: Setup format and storage

**Files:**
- Modify: `src/SimSetup.h`, `src/SimSetup.cpp`
- Test: `tests/SimSetupTest.cpp`

**Interfaces:**
- Consumes: `parseSourceSpec`, `formatSourceSpec` (Task 1).
- Produces (all in `namespace sim`):
  - `struct Setup { std::vector<SourceSpec> sources; };`
  - `std::string formatSetup(const Setup&)` — first line `# StageCue soundcard sim setup`, then one `formatSourceSpec` line per source in input order, trailing newline.
  - `std::optional<Setup> parseSetup(const std::string& text, std::string& error)` — skips blank and `#` lines (after trimming), trims `\r`; errors are `"line N: <parser error>"` and `"line N: input K appears twice"`.
  - `bool isValidSetupName(const std::string&)`
  - `std::filesystem::path setupsDirectory()`
  - `std::vector<std::string> listSetups(const std::filesystem::path& dir)` — names without `.setup`, sorted by `naturalLess`; missing dir → empty.
  - `bool saveSetup(const std::filesystem::path& dir, const std::string& name, const Setup&, std::string& error)` — creates `dir`, writes via a temp file + rename so a crash never leaves half a file.
  - `std::optional<Setup> loadSetup(const std::filesystem::path& dir, const std::string& name, std::string& error)`

- [ ] **Step 1: Write the failing tests** (reuse the temp-dir fixture from Task 2):

```cpp
TEST(SimSetup, FormatAndParseRoundTrip)
{
    std::string error;
    Setup setup;
    for (const char* t : { "1=file:/tmp/My Stems/a:b ü.wav:-6", "2=sine:1000:-12", "3=silence" })
        setup.sources.push_back(*parseSourceSpec(t, error));
    const auto parsed = parseSetup(formatSetup(setup), error);
    ASSERT_TRUE(parsed) << error;
    ASSERT_EQ(parsed->sources.size(), 3u);
    EXPECT_EQ(parsed->sources[0].filePath, "/tmp/My Stems/a:b ü.wav");
    EXPECT_FLOAT_EQ(parsed->sources[1].frequencyHz, 1000.0f);
}

TEST(SimSetup, ParseSkipsCommentsAndBlankLinesAndNamesBadLines)
{
    std::string error;
    EXPECT_TRUE(parseSetup("# hi\n\n  \n2=sine\r\n", error));
    EXPECT_FALSE(parseSetup("# hi\n1=sine\n1=noise\n", error));
    EXPECT_NE(error.find("line 3"), std::string::npos);
    EXPECT_FALSE(parseSetup("1=sine\n1=silence\n", error));
    EXPECT_NE(error.find("line 2"), std::string::npos);
    EXPECT_NE(error.find("twice"), std::string::npos);
}

TEST(SimSetup, ValidatesNames)
{
    EXPECT_TRUE(isValidSetupName("band"));
    EXPECT_TRUE(isValidSetupName("Live Set_2-b"));
    for (const char* bad : { "", " band", "band ", "../x", "a/b", "a.setup", "x:y" })
        EXPECT_FALSE(isValidSetupName(bad)) << bad;
}

TEST_F(TempDirTest, SaveListLoadAndOverwrite)
{
    const auto setups = dir / "nested" / "setups";
    EXPECT_TRUE(listSetups(setups).empty());          // first run: no folder yet

    std::string error;
    Setup a; a.sources.push_back(*parseSourceSpec("1=sine:440:-6", error));
    Setup b; b.sources.push_back(*parseSourceSpec("1=silence", error));
    ASSERT_TRUE(saveSetup(setups, "band", a, error)) << error;
    ASSERT_TRUE(saveSetup(setups, "10 songs", a, error)) << error;
    ASSERT_TRUE(saveSetup(setups, "2 songs", a, error)) << error;
    EXPECT_EQ(listSetups(setups), (std::vector<std::string>{ "2 songs", "10 songs", "band" }));

    ASSERT_TRUE(saveSetup(setups, "band", b, error)) << error;   // overwrite
    const auto loaded = loadSetup(setups, "band", error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->sources[0].kind, SourceSpec::Kind::Silence);
    EXPECT_FALSE(saveSetup(setups, "../evil", a, error));
}

TEST_F(TempDirTest, SetupsDirectoryFollowsXdgConfigHome)
{
    ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    EXPECT_EQ(setupsDirectory(), dir / "stagecue-sim" / "setups");
    ::setenv("XDG_CONFIG_HOME", "", 1);
    EXPECT_EQ(setupsDirectory(), std::filesystem::path(std::getenv("HOME")) / ".config" / "stagecue-sim" / "setups");
    ::unsetenv("XDG_CONFIG_HOME");
}
```

(Restore any pre-existing `XDG_CONFIG_HOME` in the fixture.)

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement** to the interfaces above.
- [ ] **Step 4: Run tests, expect PASS.**
- [ ] **Step 5: Commit** — `feat: save and load named input setups`

---

### Task 4: `applySources` and the Folder box

**Files:**
- Modify: `src/Ui.cpp` (`InputRow`, `loadFile`/`finishLoad` at ~341-378, `applyIoConfig`, `build()` controls at ~146-175)

**Interfaces:**
- Consumes: `listAudioFiles` (Task 2), `SourceSpec`.
- Produces (private `SimUi` members, used by Task 5): `void applySources(const std::vector<SourceSpec>& sources, const std::string& what)`; `InputRow::loadedPath` (resolved path of the file currently playing, empty if none).

No unit harness exists for `Ui.cpp`; each step is checked by building and running the sim.

- [ ] **Step 1: Make loads replaceable.** Add to `InputRow`: `std::uint64_t loadGeneration = 0; bool loadQueued = false; std::string loadedPath;`. `LoadResult` gains `std::uint64_t generation; std::string path;` (the resolved path). `loadFile(row)` bumps `loadGeneration`; if a load is pending it sets `loadQueued = true` and status `"queued " + row.path`, else starts the async as today, capturing the generation. `finishLoad` discards a result whose generation is stale and then, if `loadQueued`, clears it and starts the queued load. On success it sets `row.loadedPath = result.path`. (Never assign over a pending `std::future` from `std::async`: its destructor blocks the UI until the decode ends.)

- [ ] **Step 2: Extract `bool growInputs(int needed)`** from `applyIoConfig`: if `needed > engine.getNumInputs()`, call `engine.reconfigure(std::min(needed, kMaxChannels), engine.getNumOutputs())`, update `requestedInputs`, `populateRows()`, log `"Restarted with N in / M out - re-select the device in Tessel"` (also use "Tessel" in `applyIoConfig`'s message). Returns false and logs on error.

- [ ] **Step 3: Implement `applySources(sources, what)`**. `what` is used in the log (`"Loaded folder Stems: 16 files"`, `"Recalled setup band"`).
  1. `growInputs(max input index + 1)`.
  2. `engine.setPlaying(false); engine.restart();`
  3. For every row: find its spec (none → Silence).
     - Silence: `kind = kSilence`, bump `loadGeneration`, `loadQueued = false`.
     - Sine: `kind = kSine`, `frequencyHz`, `frequencyPosition = appliedFrequencyPosition = frequencyToPosition(hz)`, `levelDb`; bump generation, clear queue.
     - File: `kind = appliedKind = kFile`, `file.reset()`, `loadedPath.clear()`, `engine.setInputSource(i, nullptr)`, `path = spec.filePath`, `levelDb = spec.levelDb`, `loadFile(row)`.
  `applyInputRow` on the next frame does the rest for Off/Sine.

- [ ] **Step 4: Folder box.** Add `std::string folderPath;` and an `Input(&folderPath, inputOption("folder of stems (Enter to load)"))` whose `on_enter` calls `loadFolder()`: `listAudioFiles`; error → log Error; zero files → log Warn `"No WAV/FLAC/MP3 files in <folder>"`; else build specs `i=file:path` with each row's current `levelDb` (row default −12 dB for rows not yet created), cap at `kMaxChannels`, log Warn `"Skipped K files: the sim has at most 32 inputs"` when capped, then `applySources`. Render it in `controlView` as `countRow`-style `"Folder   "` + box (width 30).

- [ ] **Step 5: Build and check by hand.** `cmake --build build`, then `./build/StageCueSoundcardSim --inputs 4`, load a folder of 6+ stems: inputs grow to 6, rows fill in name order, transport stopped at 0:00. Load a second folder straight away while the first is still decoding: every row ends up with the second folder's file (Review Focus 3). Load a folder with fewer files: leftover rows go Off.

- [ ] **Step 6: Commit** — `feat: load a folder of stems onto the inputs`

---

### Task 5: Save and recall in the Control panel, docs

**Files:**
- Modify: `src/Ui.cpp`, `README.md`

**Interfaces:**
- Consumes: `applySources`, `InputRow::loadedPath` (Task 4); `Setup`, `formatSetup`, `saveSetup`, `loadSetup`, `listSetups`, `setupsDirectory`, `isValidSetupName` (Task 3).

- [ ] **Step 1: Save box.** `std::string saveName;` + `Input` (`"setup name (Enter to save)"`) → `saveCurrentSetup()`: reject invalid names (log Error with the allowed characters); build `Setup` from the first `engine.getNumInputs()` rows: Silence, Sine (`frequencyHz`, `levelDb`), File only if `loadedPath` non-empty (else saved as silence, and log Warn `"In N: no file loaded, saved as Off"`). Log Ok `"Saved setup band"` or `"Overwrote setup band"` (check `listSetups` first), then refresh the list and clear `saveName`.

- [ ] **Step 2: Setups list.** `std::vector<std::string> setupNames; int selectedSetup = 0;` filled from `listSetups(setupsDirectory())` at `build()` and after each save. A `Menu(&setupNames, &selectedSetup)` with `on_enter` → `recallSetup(name)`: `loadSetup` (error → log Error), else `applySources(setup.sources, "setup " + name)`. Render under a `"Setups"` label, at most 6 rows tall with `frame | vscroll_indicator`; empty list shows muted `"none saved yet"`. Show `setupsDirectory()` muted under it so users know where to edit files.

- [ ] **Step 3: Footer and README.** Footer: `" Tab/arrows: move · ←/→: adjust · Enter: load file / folder, save or recall a setup · Esc or Ctrl+C: quit"`. README: add a "Setups" subsection under Run (folder load + natural order + rename to reorder, save/recall, file location and format, inputs grow but never shrink, re-select the device in Tessel after growing) and mention it in the Keys line.

- [ ] **Step 4: Build, run tests, check by hand.** `cmake --build build && ./build/SoundcardSimTest` all PASS. Then the spec's manual run: load 16 stems → save `band` → quit → restart → recall `band` (same files, levels, order); swap two lines in `band.setup` and recall (order follows the file); rename one WAV and recall (that row shows an error, others play).

- [ ] **Step 5: Commit** — `feat: save and recall input setups from the control panel`

---

### Task 6: Propagate (only when the user asks)

- [ ] Push `as/sim-setups` in the sim repo and merge per the user's flow.
- [ ] In stagecue-backend: bump `tools/soundcard-sim`, commit `chore: bump soundcard sim (folder load, setups)`, push.
- [ ] In the frontend: bump `modules/stagecue-backend`, commit, push. `./gradlew runAll` still builds the sim.
