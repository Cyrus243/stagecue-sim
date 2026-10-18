#include "Ui.h"

#include "EventLog.h"
#include "LoopbackStatus.h"
#include "Meters.h"
#include "SimEngine.h"
#include "SimSetup.h"
#include "UiStyle.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace sim
{
    namespace
    {
        using namespace ftxui;
        using namespace style;
        using Clock = std::chrono::steady_clock;
        using Severity = EventLog::Severity;

        constexpr float kSignalThresholdDb = -60.0f;
        constexpr auto kSignalHold = std::chrono::seconds(1);
        // 20 Hz: smooth enough for meters. FTXUI repaints the whole screen every frame,
        // so each extra frame costs the terminal emulator real work.
        constexpr auto kRefreshInterval = std::chrono::milliseconds(50);
        constexpr int kWideLayoutColumns = 130;   // below this the metrics tiles are hidden
        constexpr int kTallLayoutRows = 40;       // below this the activity log is hidden
        constexpr std::size_t kLogLines = 6;
        constexpr int kMeterCells = 14;
        constexpr int kNarrowMeterCells = 8;

        enum Kind { kSilence = 0, kSine = 1, kFile = 2 };
        const std::vector<std::string> kKindLabels = { "Off", "Sine", "File" };

        // Slider position 0..100 <-> 20 Hz..20 kHz on a log scale.
        float positionToFrequency(int position) { return 20.0f * std::pow(1000.0f, static_cast<float>(position) / 100.0f); }
        int frequencyToPosition(float hz)
        {
            const float position = 100.0f * std::log(std::max(hz, 20.0f) / 20.0f) / std::log(1000.0f);
            return std::clamp(static_cast<int>(std::lround(position)), 0, 100);
        }

        /** Rounds to 3 significant digits so slider-picked frequencies read nicely. */
        float roundFrequency(float hz)
        {
            const float scale = std::pow(10.0f, std::floor(std::log10(hz)) - 2.0f);
            return std::round(hz / scale) * scale;
        }

        std::string formatFrequency(float hz)
        {
            return hz >= 1000.0f ? fixed(hz / 1000.0f, 2) + " kHz" : fixed(hz, 0) + " Hz";
        }

        /** "~/.config/..." rather than the full home path, for display. */
        std::string homeRelative(const std::filesystem::path& path)
        {
            const auto text = path.string();
            const char* home = std::getenv("HOME");
            if (home && *home && text.rfind(std::string(home) + "/", 0) == 0)
                return "~" + text.substr(std::string(home).size());
            return text;
        }

        std::string twoDigits(int value) { return (value < 9 ? "0" : "") + std::to_string(value + 1); }

        /** Result of a file decoded off the UI thread. */
        struct LoadResult
        {
            std::shared_ptr<FileSource> file;
            std::string error;
            std::string path;               // resolved, as saved in a setup
            std::uint64_t generation = 0;   // stale when the row has moved on since
        };

        struct InputRow
        {
            int index = 0;
            int kind = kSilence;
            int appliedKind = -1;
            float frequencyHz = 1000.0f;
            int frequencyPosition = frequencyToPosition(1000.0f);
            int appliedFrequencyPosition = frequencyPosition;
            float levelDb = -12.0f;
            std::string path;
            std::string status;
            bool statusIsError = false;
            std::shared_ptr<SineSource> sine;
            std::shared_ptr<FileSource> file;
            std::future<LoadResult> pendingLoad;
            std::uint64_t loadGeneration = 0;   // bumped by every new choice for this row
            bool loadQueued = false;            // load `requestedPath` once the pending one is done
            std::string requestedPath;          // what Enter / a folder / a setup asked for last
            std::shared_ptr<std::atomic<bool>> loadCancel;   // stops the decode in flight
            std::string loadedPath;             // file playing now (resolved); empty if none
            MeterBallistics meter;
            float meterDb = kMeterFloorDb;
            Component component;
        };

        struct OutputRow
        {
            int index = 0;
            bool monitor = false;
            MeterBallistics meter;
            float meterDb = kMeterFloorDb;
            std::optional<Clock::time_point> lastSignal;
            bool live = false;
            Component component;
        };

        class SimUi
        {
        public:
            SimUi(SimEngine& engineToUse, ScreenInteractive& screenToUse)
                : engine(engineToUse), screen(screenToUse),
                  requestedInputs(engine.getNumInputs()), requestedOutputs(engine.getNumOutputs())
            {
            }

            Component build()
            {
                inputsContainer = Container::Vertical({});
                outputsContainer = Container::Vertical({});
                populateRows();

                const auto& config = engine.getConfig();
                log.add(Severity::Info, "Simulator started: " + std::to_string(engine.getNumInputs()) + " in / "
                                        + std::to_string(engine.getNumOutputs()) + " out @ "
                                        + std::to_string(config.sampleRate) + " Hz, period "
                                        + std::to_string(config.periodFrames));

                auto inputsSlider = Slider(sliderOption(&requestedInputs, 1, kMaxChannels, 1));
                auto outputsSlider = Slider(sliderOption(&requestedOutputs, 1, kMaxChannels, 1));
                auto applyButton = Button("Apply / restart", [this] { applyIoConfig(); }, primaryButton());
                auto muteCheckbox = Checkbox("mute main out", &monitorMuted, checkboxOption());
                auto quitButton = Button("Quit", screen.ExitLoopClosure(), secondaryButton());
                auto playButton = Button(&playLabel, [this] { togglePlaying(); }, primaryButton());
                auto restartButton = Button("⏮ Restart", [this] { restartSong(); }, secondaryButton());
                InputOption folderOption = inputOption("folder of stems (Enter to load)");
                folderOption.on_enter = [this] { loadFolder(); };
                auto folderInput = Input(&folderPath, folderOption);
                InputOption saveOption = inputOption("setup name (Enter to save)");
                saveOption.on_enter = [this] { saveCurrentSetup(); };
                auto saveInput = Input(&saveName, saveOption);
                refreshSetups();
                auto setupsMenuOption = MenuOption::Vertical();
                setupsMenuOption.on_enter = [this] { recallSetup(); };
                setupsMenuOption.entries_option.transform = [](const EntryState& state) {
                    auto label = text((state.active ? "▸ " : "  ") + state.label);
                    if (state.focused)
                        return label | bgcolor(kAccent) | color(kBg) | bold;
                    return state.active ? label | color(kBright) : label | color(kText);
                };
                auto setupsMenu = Menu(&setupNames, &selectedSetup, setupsMenuOption);

                auto controls = Container::Vertical({
                    Container::Horizontal({ playButton, restartButton }),
                    inputsSlider, outputsSlider, muteCheckbox,
                    Container::Horizontal({ applyButton, quitButton }),
                    folderInput, saveInput, setupsMenu,
                });
                auto controlView = Renderer(controls, [=, this] {
                    auto countRow = [](const std::string& label, const Component& slider, int value) {
                        return hbox({
                            muted(label) | size(WIDTH, EQUAL, 9),
                            slider->Render() | size(WIDTH, EQUAL, 20),
                            text(" " + std::to_string(value)) | bold | color(kBright),
                        });
                    };
                    return panel("Control", vbox({
                        hbox({ playButton->Render(), text("  "), restartButton->Render() }),
                        hbox({ muted("Song     "), text(songTime()) | bold | color(kBright) }),
                        countRow("Inputs", inputsSlider, requestedInputs),
                        countRow("Outputs", outputsSlider, requestedOutputs),
                        muteCheckbox->Render(),
                        hbox({ applyButton->Render(), text("  "), quitButton->Render() }),
                        separatorLight() | color(kPanelBorder),
                        hbox({ muted("Folder   "), folderInput->Render() | size(WIDTH, EQUAL, 30) }),
                        hbox({ muted("Save as  "), saveInput->Render() | size(WIDTH, EQUAL, 30) }),
                        hbox({
                            muted("Setups   "),
                            setupNames.empty() ? muted("none saved yet (Enter recalls)")
                                               : setupsMenu->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 6),
                        }),
                        // Capped so a long path never widens the panel (and squeezes the log).
                        muted("         " + homeRelative(setupsDirectory())) | size(WIDTH, LESS_THAN, 40),
                    }));
                });

                auto body = Container::Horizontal({ inputsContainer, outputsContainer });
                auto root = Container::Vertical({ body, controlView });

                auto view = Renderer(root, [=, this] {
                    syncWithEngine();
                    // Narrow terminals drop the metrics tiles, short ones the activity log
                    // (the control panel then moves up next to the link panel).
                    const auto terminal = Terminal::Size();
                    const bool wide = terminal.dimx >= kWideLayoutColumns;
                    meterCells = wide ? kMeterCells : kNarrowMeterCells;
                    const bool tall = terminal.dimy >= kTallLayoutRows;

                    Elements top;
                    if (wide)
                        top.push_back(metricsPanel() | flex);
                    top.push_back(linkPanel() | flex);
                    if (!tall)
                        top.push_back(controlView->Render());

                    Elements layout {
                        headerBar(),
                        hbox(std::move(top)),
                        hbox({ inputsPanel() | flex, outputsPanel() }) | flex,
                    };
                    if (tall)
                        layout.push_back(hbox({ logPanel() | flex, controlView->Render() }));
                    layout.push_back(footer());

                    return vbox(std::move(layout)) | bgcolor(kBg) | color(kText);
                });

                return CatchEvent(view, [this](const Event& event) {
                    if (event == Event::Custom)
                    {
                        framePending = false;   // let the ticker post the next one
                        return false;
                    }
                    if (event == Event::Escape)
                    {
                        screen.Exit();
                        return true;
                    }
                    return false;
                });
            }

            /** Stops decodes in flight: the pending futures join them on destruction. */
            ~SimUi()
            {
                for (auto& row : inputRows)
                    stopDecode(*row);
            }

            /** Set when a redraw tick is queued; the ticker never queues a second one. */
            std::atomic<bool> framePending { false };

        private:
            // ------------------------------------------------------------ rows

            void populateRows()
            {
                inputsContainer->DetachAllChildren();
                outputsContainer->DetachAllChildren();

                while (static_cast<int>(inputRows.size()) < engine.getNumInputs())
                    inputRows.push_back(makeInputRow(static_cast<int>(inputRows.size())));
                while (static_cast<int>(outputRows.size()) < engine.getNumOutputs())
                    outputRows.push_back(makeOutputRow(static_cast<int>(outputRows.size())));

                for (int i = 0; i < engine.getNumInputs(); ++i)
                    inputsContainer->Add(inputRows[static_cast<std::size_t>(i)]->component);
                for (int i = 0; i < engine.getNumOutputs(); ++i)
                    outputsContainer->Add(outputRows[static_cast<std::size_t>(i)]->component);
            }

            std::unique_ptr<InputRow> makeInputRow(int index)
            {
                auto row = std::make_unique<InputRow>();
                row->index = index;
                adoptExistingSource(*row);

                auto* r = row.get();
                auto kindToggle = Menu(&kKindLabels, &r->kind, toggleOption());
                auto frequency = Slider(sliderOption(&r->frequencyPosition, 0, 100, 1));
                auto level = Slider(sliderOption(&r->levelDb, -60.0f, 0.0f, 1.0f));

                InputOption pathOption = inputOption("path/to/file.wav (Enter to load)");
                pathOption.on_enter = [this, r] { loadFile(*r); };
                auto path = Input(&r->path, pathOption);

                auto container = Container::Horizontal({
                    kindToggle,
                    Maybe(frequency, [r] { return r->kind == kSine; }),
                    Maybe(path, [r] { return r->kind == kFile; }),
                    Maybe(level, [r] { return r->kind != kSilence; }),
                });

                row->component = Renderer(container, [=, this] {
                    auto levelControl = [&] {
                        return hbox({
                            muted("  lvl "), level->Render() | size(WIDTH, EQUAL, 10),
                            text(" " + fixed(r->levelDb, 0) + " dB") | size(WIDTH, EQUAL, 7),
                        });
                    };

                    Element params = muted("—");
                    if (r->kind == kSine)
                        params = hbox({
                            frequency->Render() | size(WIDTH, EQUAL, 14),
                            text(" " + formatFrequency(r->frequencyHz)) | color(kBright) | size(WIDTH, EQUAL, 10),
                            levelControl(),
                        });
                    else if (r->kind == kFile)
                        params = hbox({ path->Render() | size(WIDTH, EQUAL, 25), levelControl() });

                    Elements lines;
                    lines.push_back(hbox({
                        text(twoDigits(r->index)) | bold | color(kBright) | size(WIDTH, EQUAL, 4),
                        kindToggle->Render() | size(WIDTH, EQUAL, 21),
                        params | flex,
                        text(" "),
                        segmentedMeter(r->meterDb, meterCells),
                    }));
                    if (!r->status.empty())
                        lines.push_back(hbox({ muted("    > "), text(r->status) | color(r->statusIsError ? kRed : kMuted) }));
                    return vbox(std::move(lines));
                });
                return row;
            }

            /** Mirrors a source preset on the command line into the row's UI state. */
            void adoptExistingSource(InputRow& row)
            {
                const auto source = engine.getInputSource(row.index);
                if (auto sine = std::dynamic_pointer_cast<SineSource>(source))
                {
                    row.kind = kSine;
                    row.sine = sine;
                    row.frequencyHz = sine->getFrequency();
                    row.frequencyPosition = frequencyToPosition(row.frequencyHz);
                    row.appliedFrequencyPosition = row.frequencyPosition;
                    row.levelDb = sine->getLevelDb();
                }
                else if (auto file = std::dynamic_pointer_cast<FileSource>(source))
                {
                    row.kind = kFile;
                    row.file = file;
                    row.levelDb = file->getLevelDb();
                }

                for (const auto& spec : engine.getConfig().sources)
                    if (spec.input == row.index && spec.kind == SourceSpec::Kind::File)
                    {
                        row.path = spec.filePath;
                        if (row.file)
                            row.loadedPath = row.requestedPath = resolvePath(spec.filePath).string();
                    }

                row.appliedKind = row.kind;
            }

            std::unique_ptr<OutputRow> makeOutputRow(int index)
            {
                auto row = std::make_unique<OutputRow>();
                row->index = index;
                row->monitor = engine.isMonitorEnabled(index);

                auto* r = row.get();
                auto monitor = Checkbox(std::string("main ") + (index % 2 == 0 ? "L" : "R"), &r->monitor, checkboxOption());
                row->component = Renderer(monitor, [=, this] {
                    std::string ago = "never";
                    if (r->lastSignal)
                    {
                        const auto seconds = std::chrono::duration<float>(Clock::now() - *r->lastSignal).count();
                        ago = r->live ? "now" : fixed(seconds, 0) + "s ago";
                    }

                    Elements cells {
                        text(twoDigits(r->index)) | bold | color(kBright) | size(WIDTH, EQUAL, 4),
                        segmentedMeter(r->meterDb, meterCells),
                        text("  "),
                        statusDot(r->live ? Dot::Live : Dot::Idle, r->live ? "Signal" : "Idle") | size(WIDTH, EQUAL, 9),
                    };
                    if (meterCells == kMeterCells)
                        cells.push_back(muted(ago) | size(WIDTH, EQUAL, 8));
                    cells.push_back(monitor->Render());
                    return hbox(std::move(cells));
                });
                return row;
            }

            // ------------------------------------------------------- actions

            /** Decodes on a worker thread; finishLoad() picks the result up on a later frame. */
            void loadFile(InputRow& row)
            {
                ++row.loadGeneration;
                row.requestedPath = resolvePath(row.path).string();
                row.statusIsError = false;
                // Never assign over a pending std::async future: its destructor would block the
                // UI until that decode ends. Queue instead; finishLoad() starts it.
                if (row.pendingLoad.valid())
                {
                    stopDecode(row);
                    row.loadQueued = true;
                    row.status = "queued " + row.path;
                    return;
                }
                startLoad(row);
            }

            void startLoad(InputRow& row)
            {
                row.loadQueued = false;
                row.status = "loading " + row.requestedPath + " ...";
                row.loadCancel = std::make_shared<std::atomic<bool>>(false);
                row.pendingLoad = std::async(std::launch::async,
                                             [path = row.requestedPath, rate = engine.getConfig().sampleRate,
                                              level = row.levelDb, generation = row.loadGeneration,
                                              cancel = row.loadCancel] {
                                                 LoadResult result;
                                                 result.path = path;
                                                 result.generation = generation;
                                                 result.file = FileSource::load(path, rate, level, result.error, cancel.get());
                                                 return result;
                                             });
            }

            /** Asks the decode in flight (if any) to stop early; its result is dropped anyway. */
            static void stopDecode(InputRow& row)
            {
                if (row.loadCancel)
                    row.loadCancel->store(true);
            }

            /** Drops any load in flight for the row: its result will be ignored. */
            void cancelLoad(InputRow& row)
            {
                ++row.loadGeneration;
                row.loadQueued = false;
                row.requestedPath.clear();
                stopDecode(row);
            }

            void finishLoad(InputRow& row)
            {
                if (!row.pendingLoad.valid()
                    || row.pendingLoad.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return;

                auto result = row.pendingLoad.get();
                if (row.loadQueued)
                    startLoad(row);
                if (result.generation != row.loadGeneration)
                    return;   // the row was given something else meanwhile
                if (!result.file)
                {
                    row.status = result.error;
                    row.statusIsError = true;
                    log.add(Severity::Error, "In " + std::to_string(row.index + 1) + ": " + result.error);
                    return;
                }

                row.file = std::move(result.file);
                row.loadedPath = std::move(result.path);
                row.file->setLevelDb(row.levelDb);
                row.kind = kFile;
                row.appliedKind = kFile;
                engine.setInputSource(row.index, row.file);
                row.status = row.file->describe();
                row.statusIsError = false;
                log.add(Severity::Ok, "In " + std::to_string(row.index + 1) + " loaded " + row.status);
            }

            void applyIoConfig() { reconfigure(requestedInputs, requestedOutputs); }

            bool reconfigure(int inputs, int outputs)
            {
                const auto error = engine.reconfigure(inputs, outputs);
                requestedInputs = engine.getNumInputs();
                requestedOutputs = engine.getNumOutputs();
                populateRows();

                if (error.empty())
                    log.add(Severity::Ok, "Restarted with " + std::to_string(requestedInputs) + " in / "
                                          + std::to_string(requestedOutputs) + " out - re-select the device in Tessel");
                else
                    log.add(Severity::Error, error);
                return error.empty();
            }

            /** Adds inputs (never removes any) so `needed` of them exist. */
            bool growInputs(int needed)
            {
                needed = std::min(needed, kMaxChannels);
                if (needed <= engine.getNumInputs())
                    return true;
                return reconfigure(needed, engine.getNumOutputs());
            }

            // --------------------------------------------------- folder / setups

            /** Gives every input the source `sources` names for it (Off if none): the shared core
                of loading a folder and recalling a setup. `what` names it in the log. */
            void applySources(const std::vector<SourceSpec>& sources, const std::string& what)
            {
                int needed = 0;
                for (const auto& spec : sources)
                    needed = std::max(needed, spec.input + 1);
                if (!growInputs(needed))
                    return;

                // Stopped at 0:00, so every stem is in place before Play.
                engine.setPlaying(false);
                engine.restart();

                // Rows hidden by an earlier shrink go Off too, or they would come back playing
                // their old stems when the input count grows again.
                for (int i = engine.getNumInputs(); i < static_cast<int>(inputRows.size()); ++i)
                {
                    auto& row = *inputRows[static_cast<std::size_t>(i)];
                    cancelLoad(row);
                    row.kind = row.appliedKind = kSilence;
                    row.file.reset();
                    row.loadedPath.clear();
                    row.status.clear();
                    engine.setInputSource(i, nullptr);
                }

                int files = 0;
                for (int i = 0; i < engine.getNumInputs(); ++i)
                {
                    auto& row = *inputRows[static_cast<std::size_t>(i)];
                    const auto spec = std::find_if(sources.begin(), sources.end(), [i](const auto& s) { return s.input == i; });
                    const auto kind = spec == sources.end() ? SourceSpec::Kind::Silence : spec->kind;

                    switch (kind)
                    {
                        case SourceSpec::Kind::Silence:
                            cancelLoad(row);
                            row.kind = kSilence;
                            break;

                        case SourceSpec::Kind::Sine:
                            cancelLoad(row);
                            row.kind = kSine;
                            row.frequencyHz = spec->frequencyHz;
                            row.frequencyPosition = row.appliedFrequencyPosition = frequencyToPosition(spec->frequencyHz);
                            row.levelDb = spec->levelDb;
                            break;

                        case SourceSpec::Kind::File:
                            // Silent until the new file is decoded, rather than playing the old one.
                            row.kind = row.appliedKind = kFile;
                            row.file.reset();
                            row.loadedPath.clear();
                            engine.setInputSource(i, nullptr);
                            row.path = spec->filePath;
                            row.levelDb = spec->levelDb;
                            loadFile(row);
                            ++files;
                            break;
                    }
                }
                log.add(Severity::Info, "Loading " + what + ": " + std::to_string(files) + " file"
                                        + (files == 1 ? "" : "s") + " - transport stopped at 0:00");
            }

            void loadFolder()
            {
                std::string error;
                const auto files = listAudioFiles(folderPath, error);
                if (!error.empty())
                {
                    log.add(Severity::Error, error);
                    return;
                }
                if (files.empty())
                {
                    log.add(Severity::Warn, "No WAV/FLAC/MP3 files in " + folderPath);
                    return;
                }

                const auto count = std::min(static_cast<int>(files.size()), kMaxChannels);
                std::vector<SourceSpec> sources;
                for (int i = 0; i < count; ++i)
                {
                    SourceSpec spec;
                    spec.input = i;
                    spec.kind = SourceSpec::Kind::File;
                    spec.filePath = files[static_cast<std::size_t>(i)].string();
                    if (i < static_cast<int>(inputRows.size()))   // keep the row's level
                        spec.levelDb = inputRows[static_cast<std::size_t>(i)]->levelDb;
                    sources.push_back(std::move(spec));
                }
                if (static_cast<int>(files.size()) > count)
                    log.add(Severity::Warn, "Skipped " + std::to_string(files.size() - count) + " files: the sim has at most "
                                            + std::to_string(kMaxChannels) + " inputs");

                applySources(sources, "folder " + resolvePath(folderPath).filename().string());
            }

            void refreshSetups()
            {
                setupNames = listSetups(setupsDirectory());
                selectedSetup = std::clamp(selectedSetup, 0, std::max(0, static_cast<int>(setupNames.size()) - 1));
            }

            void saveCurrentSetup()
            {
                if (!isValidSetupName(saveName))
                {
                    log.add(Severity::Error, "Setup names use letters, digits, space, - and _ (up to 64)");
                    return;
                }

                InputSetup setup;
                for (int i = 0; i < engine.getNumInputs(); ++i)
                {
                    const auto& row = *inputRows[static_cast<std::size_t>(i)];
                    SourceSpec spec;
                    spec.input = i;
                    spec.levelDb = row.levelDb;
                    if (row.kind == kSine)
                    {
                        spec.kind = SourceSpec::Kind::Sine;
                        spec.frequencyHz = row.frequencyHz;
                    }
                    else if (row.kind == kFile && (row.pendingLoad.valid() || row.loadQueued) && !row.requestedPath.empty())
                    {
                        spec.kind = SourceSpec::Kind::File;   // still decoding: save what was asked for
                        spec.filePath = row.requestedPath;
                    }
                    else if (row.kind == kFile && !row.loadedPath.empty())
                    {
                        spec.kind = SourceSpec::Kind::File;
                        spec.filePath = row.loadedPath;
                    }
                    else if (row.kind == kFile)
                        log.add(Severity::Warn, "In " + std::to_string(i + 1) + ": no file loaded, saved as Off");
                    setup.sources.push_back(std::move(spec));
                }

                const bool existed = std::find(setupNames.begin(), setupNames.end(), saveName) != setupNames.end();
                std::string error;
                if (!saveSetup(setupsDirectory(), saveName, setup, error))
                {
                    log.add(Severity::Error, error);
                    return;
                }
                log.add(Severity::Ok, (existed ? "Overwrote setup " : "Saved setup ") + saveName);
                refreshSetups();
                if (const auto it = std::find(setupNames.begin(), setupNames.end(), saveName); it != setupNames.end())
                    selectedSetup = static_cast<int>(it - setupNames.begin());
                saveName.clear();
            }

            void recallSetup()
            {
                if (selectedSetup < 0 || selectedSetup >= static_cast<int>(setupNames.size()))
                    return;
                const auto name = setupNames[static_cast<std::size_t>(selectedSetup)];
                std::string error;
                const auto setup = loadSetup(setupsDirectory(), name, error);
                if (!setup)
                {
                    log.add(Severity::Error, error);
                    return;
                }
                applySources(setup->sources, "setup " + name);
            }

            // -------------------------------------------------------- transport

            void togglePlaying()
            {
                engine.setPlaying(!engine.isPlaying());
                log.add(Severity::Info, engine.isPlaying() ? "Transport playing" : "Transport stopped");
            }

            void restartSong()
            {
                engine.restart();
                log.add(Severity::Info, "Song restarted from 0:00");
            }

            /** "m:ss.s / m:ss.s" - song position over the loop length (the longest file). */
            std::string songTime() const
            {
                const double rate = engine.getConfig().sampleRate;
                auto format = [rate](std::uint64_t frames) {
                    const auto tenths = static_cast<std::uint64_t>(static_cast<double>(frames) / rate * 10.0);
                    const auto seconds = tenths / 10 % 60;
                    return std::to_string(tenths / 600) + ":" + (seconds < 10 ? "0" : "") + std::to_string(seconds)
                           + "." + std::to_string(tenths % 10);
                };
                return format(engine.getSongPositionFrames()) + " / " + format(engine.getSongLengthFrames());
            }

            /** Pushes UI edits to the engine and pulls meters out of it. Runs every frame. */
            void syncWithEngine()
            {
                const auto now = Clock::now();
                const double interval = lastSync ? std::chrono::duration<double>(now - *lastSync).count() : 0.0;
                lastSync = now;

                engine.housekeeping();
                engine.setMonitorMuted(monitorMuted);
                playLabel = engine.isPlaying() ? "■ Stop" : "▶ Play";

                for (int i = 0; i < engine.getNumInputs(); ++i)
                {
                    auto& row = *inputRows[static_cast<std::size_t>(i)];
                    finishLoad(row);
                    applyInputRow(row);
                    row.meterDb = row.meter.update(engine.consumeInputPeak(i), interval);
                }

                liveOutputs = 0;
                for (int i = 0; i < engine.getNumOutputs(); ++i)
                {
                    auto& row = *outputRows[static_cast<std::size_t>(i)];
                    engine.setMonitorEnabled(i, row.monitor);

                    const float peak = engine.consumeOutputPeak(i);
                    if (gainToDb(peak) > kSignalThresholdDb)
                        row.lastSignal = now;
                    row.meterDb = row.meter.update(peak, interval);

                    const bool live = row.lastSignal && now - *row.lastSignal < kSignalHold;
                    if (live && !row.live)
                        log.add(Severity::Ok, "Out " + std::to_string(i + 1) + " signal detected");
                    row.live = live;
                    liveOutputs += live ? 1 : 0;
                }

                // The poller refreshes the status in the background; this only copies it.
                if (now - lastStatusPoll > std::chrono::milliseconds(250))
                {
                    const auto previous = loopback;
                    loopback = engine.getLinkStatus();
                    lastStatusPoll = now;
                    logStatusChanges(previous);
                }
            }

            /** Adds activity-log entries for whatever changed since the last status poll. */
            void logStatusChanges(const LoopbackStatus& previous)
            {
                if (loopback.cardPresent != previous.cardPresent)
                    log.add(loopback.cardPresent ? Severity::Ok : Severity::Error,
                            "Loopback " + deviceText(engine.getConfig()).device
                                + (loopback.cardPresent ? " found" : " missing"));

                logSubstreamChange("input", previous.appCapture, loopback.appCapture, engine.getNumInputs());
                logSubstreamChange("output", previous.appPlayback, loopback.appPlayback, engine.getNumOutputs());

                const bool running = engine.isRunning();
                if (running != wasRunning)
                    log.add(running ? Severity::Ok : Severity::Error, running ? "Engine running" : "Engine stopped");
                wasRunning = running;

                const bool stalled = engine.isStalled();
                if (stalled && !wasStalled)
                    log.add(Severity::Error, "Loopback stalled: no audio callbacks for over a second");
                else if (!stalled && wasStalled)
                    log.add(Severity::Ok, "Loopback running again");
                wasStalled = stalled;

                const auto loopbackXruns = engine.getLoopbackXruns();
                if (loopbackXruns > lastLoopbackXruns)
                    log.add(Severity::Error, "Loopback xrun (+" + std::to_string(loopbackXruns - lastLoopbackXruns) + ")");
                lastLoopbackXruns = loopbackXruns;

                const auto monitorXruns = engine.getMonitorXruns();
                if (monitorXruns > lastMonitorXruns)
                    log.add(Severity::Warn, "Main out xrun (+" + std::to_string(monitorXruns - lastMonitorXruns) + ")");
                lastMonitorXruns = monitorXruns;

                const auto lastError = engine.getLastError();
                if (lastError != lastEngineError && !lastError.empty())
                    log.add(Severity::Error, lastError);
                lastEngineError = lastError;

                const auto monitorError = engine.getMonitorError();
                if (monitorError != lastMonitorError && !monitorError.empty())
                    log.add(Severity::Warn, "Main out: " + monitorError);
                lastMonitorError = monitorError;
            }

            void logSubstreamChange(const std::string& direction, const SubstreamStatus& before,
                                    const SubstreamStatus& after, int expectedChannels)
            {
                if (after.open == before.open && after.channels == before.channels)
                    return;

                if (!after.open)
                {
                    log.add(Severity::Info, "PluginHost closed " + direction);
                    return;
                }

                std::string details = after.channels > 0
                                          ? " (" + std::to_string(after.channels) + " ch, " + std::to_string(after.rate) + " Hz)"
                                          : "";
                const bool mismatch = after.channels > 0 && after.channels != expectedChannels;
                log.add(mismatch ? Severity::Warn : Severity::Ok,
                        "PluginHost opened " + direction + details
                            + (mismatch ? " - sim has " + std::to_string(expectedChannels) + " ch" : ""));
            }

            void applyInputRow(InputRow& row)
            {
                // Keep an exact preset frequency (e.g. --source 1=sine:440) until the slider moves.
                if (row.frequencyPosition != row.appliedFrequencyPosition)
                {
                    row.appliedFrequencyPosition = row.frequencyPosition;
                    row.frequencyHz = roundFrequency(positionToFrequency(row.frequencyPosition));
                }
                const float frequency = row.frequencyHz;

                if (row.kind != row.appliedKind)
                {
                    if (row.appliedKind == kFile)
                        cancelLoad(row);   // or a decode finishing later switches the row back to File
                    row.appliedKind = row.kind;
                    row.statusIsError = false;

                    switch (row.kind)
                    {
                        case kSilence:
                            engine.setInputSource(row.index, nullptr);
                            row.status.clear();
                            break;

                        case kSine:
                            if (!row.sine)
                                row.sine = std::make_shared<SineSource>(engine.getConfig().sampleRate, frequency, row.levelDb);
                            engine.setInputSource(row.index, row.sine);
                            break;

                        case kFile:
                            if (row.file)
                                engine.setInputSource(row.index, row.file);
                            else
                            {
                                engine.setInputSource(row.index, nullptr);
                                row.status = "type a file path and press Enter";
                            }
                            break;
                    }
                }

                if (row.kind == kSine && row.sine)
                {
                    row.sine->setFrequency(frequency);
                    row.sine->setLevelDb(row.levelDb);
                    row.status = row.sine->describe();
                }
                else if (row.kind == kFile && row.file)
                {
                    row.file->setLevelDb(row.levelDb);
                    if (!row.statusIsError)
                        row.status = row.file->describe();
                }
            }

            // -------------------------------------------------------- panels

            Element headerBar()
            {
                const auto& config = engine.getConfig();
                const auto lastError = engine.getLastError();
                const bool running = engine.isRunning();

                Elements lines;
                lines.push_back(hbox({
                    text("STAGECUE SOUNDCARD SIM") | bold | color(kBright),
                    text("  "), tag("loopback sim"),
                    filler(),
                    muted("Card "), chip(loopback.cardPresent, "online", "missing"),
                    muted("   Engine "), engine.isStalled() ? chip(false, "", "stalled") : chip(running, "running", "stopped"),
                    muted("   Local time "), text(EventLog::clockTime()) | color(kText),
                }));
                lines.push_back(separatorLight() | color(kPanelBorder));
                lines.push_back(hbox({
                    labelValue("Card", deviceText(config).device), text("  "),
                    labelValue("Rate", std::to_string(config.sampleRate)), text("  "),
                    labelValue("Period", std::to_string(config.periodFrames)), text("  "),
                    labelValue("Format", running ? engine.getLoopbackFormat() : "-"),
                    filler(),
                }));

                if (!loopback.cardPresent)
                    lines.push_back(text(deviceText(config).missingHint) | color(kAmber));
                if (!lastError.empty())
                    lines.push_back(paragraph(lastError) | color(kRed));

                return padded(vbox(std::move(lines))) | borderStyled(LIGHT, kPanelBorder);
            }

            Element metricsPanel()
            {
                const auto xruns = engine.getLoopbackXruns() + engine.getMonitorXruns();
                return panel("Device metrics", hbox({
                    kpiTile(std::to_string(engine.getNumInputs()), "Inputs"),
                    text(" "),
                    kpiTile(std::to_string(engine.getNumOutputs()), "Outputs"),
                    text(" "),
                    kpiTile(std::to_string(liveOutputs), "Live outputs", liveOutputs > 0 ? kGreen : kBright),
                    text(" "),
                    kpiTile(std::to_string(xruns), "Xruns", xruns > 0 ? kRed : kBright),
                }));
            }

            Element substreamRow(const std::string& label, const SubstreamStatus& status, int expectedChannels)
            {
                Elements cells { muted(label) | size(WIDTH, EQUAL, 10) };
                if (!status.open)
                {
                    cells.push_back(statusDot(Dot::Idle, "NOT CONNECTED"));
                    return hbox(std::move(cells));
                }

                const bool mismatch = status.channels > 0 && status.channels != expectedChannels;
                cells.push_back(statusDot(mismatch ? Dot::Warn : Dot::Live, "CONNECTED") | size(WIDTH, EQUAL, 14));

                std::string details = status.state;
                if (status.channels > 0)
                    details += " · " + std::to_string(status.channels) + " ch · " + std::to_string(status.rate) + " Hz";
                else if (status.rate > 0)
                    details += " · " + std::to_string(status.rate) + " Hz";
                cells.push_back(muted(details));
                if (mismatch)
                    cells.push_back(text("  sim has " + std::to_string(expectedChannels) + " ch") | color(kAmber));
                return hbox(std::move(cells));
            }

            Element linkPanel()
            {
                const auto& config = engine.getConfig();
                const auto monitorError = engine.getMonitorError();

                Element mainOut = engine.isRunning()
                                      ? chip(engine.isMonitorOpen() && monitorError.empty(), "ok", "unavailable")
                                      : muted("(engine stopped)");

                const auto devices = deviceText(config);
                const auto selectLine = devices.selectInput == devices.selectOutput
                    ? hbox({ muted("Select "), text(devices.selectInput) | bold | color(kBright),
                             muted(" as input and output device") })
                    : hbox({ muted("Select "), text(devices.selectInput) | bold | color(kBright), muted(" as input, "),
                             text(devices.selectOutput) | bold | color(kBright), muted(" as output device") });

                return panel("PluginHost link", vbox({
                    selectLine,
                    muted(devices.detail),
                    substreamRow("Input", loopback.appCapture, engine.getNumInputs()),
                    substreamRow("Output", loopback.appPlayback, engine.getNumOutputs()),
                    hbox({
                        muted("Main out") | size(WIDTH, EQUAL, 10),
                        text(config.monitorDevice) | color(kText), text("  "), mainOut,
                        muted("   xruns " + std::to_string(engine.getMonitorXruns())
                              + " · dropouts " + std::to_string(engine.getMonitorDropouts())),
                    }),
                }));
            }

            Element inputsPanel()
            {
                auto header = hbox({
                    headerCell("Ch", 4), headerCell("Source", 21), headerCell("Parameters", 0),
                    headerCell("Level", meterCells + 7),
                });
                return panel("Inputs", vbox({
                    header,
                    separatorLight() | color(kPanelBorder),
                    inputsContainer->Render() | vscroll_indicator | yframe | flex,
                }), "sim → PluginHost");
            }

            Element outputsPanel()
            {
                Elements columns { headerCell("Ch", 4), headerCell("Level", meterCells + 8), headerCell("Status", 9) };
                if (meterCells == kMeterCells)
                    columns.push_back(headerCell("Last", 8));
                columns.push_back(headerCell("Monitor", 10));
                auto header = hbox(std::move(columns));
                return panel("Outputs", vbox({
                    header,
                    separatorLight() | color(kPanelBorder),
                    outputsContainer->Render() | vscroll_indicator | yframe | flex,
                }), "PluginHost → sim");
            }

            Element logPanel()
            {
                const auto& entries = log.getEntries();
                const std::size_t first = entries.size() > kLogLines ? entries.size() - kLogLines : 0;

                Elements lines;
                for (std::size_t i = first; i < entries.size(); ++i)
                {
                    const auto& entry = entries[i];
                    const Color textColor = entry.severity == Severity::Ok     ? kGreen
                                          : entry.severity == Severity::Warn  ? kAmber
                                          : entry.severity == Severity::Error ? kRed
                                                                              : kText;
                    lines.push_back(hbox({ muted("> [" + entry.time + "] "), text(entry.text) | color(textColor) }));
                }
                while (lines.size() < kLogLines)
                    lines.push_back(text(""));

                return panel("Activity log", vbox(std::move(lines)));
            }

            Element footer()
            {
                return hbox({
                    muted(" Tab/arrows: move · ←/→: adjust · Enter: load file / folder, save or recall a setup · Esc or Ctrl+C: quit"),
                    filler(),
                    text("▌") | color(kBlue), muted("Ready "),
                });
            }

            SimEngine& engine;
            ScreenInteractive& screen;

            std::vector<std::unique_ptr<InputRow>> inputRows;
            std::vector<std::unique_ptr<OutputRow>> outputRows;
            Component inputsContainer;
            Component outputsContainer;

            int requestedInputs;
            int requestedOutputs;
            bool monitorMuted = false;
            std::string folderPath;
            std::string saveName;
            std::vector<std::string> setupNames;
            int selectedSetup = 0;
            std::string playLabel = "▶ Play";
            int liveOutputs = 0;
            int meterCells = kMeterCells;

            EventLog log;
            bool wasRunning = false;
            bool wasStalled = false;
            std::uint64_t lastLoopbackXruns = 0;
            std::uint64_t lastMonitorXruns = 0;
            std::string lastEngineError;
            std::string lastMonitorError;

            LoopbackStatus loopback;
            Clock::time_point lastStatusPoll {};
            std::optional<Clock::time_point> lastSync;
        };
    }

    int runUi(SimEngine& engine)
    {
        auto screen = ScreenInteractive::Fullscreen();
        SimUi ui(engine, screen);
        auto component = ui.build();

        // At most one redraw tick is ever queued: if a frame takes longer than the
        // interval, ticks are skipped instead of piling up in FTXUI's event queue.
        std::atomic<bool> ticking { true };
        std::thread ticker([&] {
            while (ticking.load())
            {
                std::this_thread::sleep_for(kRefreshInterval);
                if (!ui.framePending.exchange(true))
                    screen.PostEvent(Event::Custom);
            }
        });

        screen.Loop(component);

        ticking = false;
        ticker.join();
        return 0;
    }
}
