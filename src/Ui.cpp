#include "Ui.h"

#include "LoopbackStatus.h"
#include "Meters.h"
#include "SimEngine.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace sim
{
    namespace
    {
        using namespace ftxui;
        using Clock = std::chrono::steady_clock;

        constexpr float kSignalThresholdDb = -60.0f;
        constexpr auto kSignalHold = std::chrono::seconds(1);
        constexpr auto kRefreshInterval = std::chrono::milliseconds(33);   // ~30 Hz, like LevelMeterBroadcaster

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

        std::string fixed(float value, int precision)
        {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(precision) << value;
            return stream.str();
        }

        std::string formatDb(float db) { return db <= kMeterFloorDb ? "  -inf" : fixed(db, 1); }

        std::string formatFrequency(float hz)
        {
            return hz >= 1000.0f ? fixed(hz / 1000.0f, 2) + " kHz" : fixed(hz, 0) + " Hz";
        }

        Element meterBar(float db, int width)
        {
            const float fraction = std::clamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
            const Color barColor = db > -6.0f ? Color::Red : db > -18.0f ? Color::Yellow : Color::Green;
            return hbox({
                gauge(fraction) | color(barColor) | size(WIDTH, EQUAL, width),
                text(formatDb(db) + " dB") | size(WIDTH, EQUAL, 10),
            });
        }

        Element badge(bool ok, const std::string& okText, const std::string& badText)
        {
            return ok ? text(" " + okText + " ") | bgcolor(Color::Green) | color(Color::Black)
                      : text(" " + badText + " ") | bgcolor(Color::Red) | color(Color::White);
        }

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

                auto inputsSlider = Slider("", &requestedInputs, 1, kMaxChannels, 1);
                auto outputsSlider = Slider("", &requestedOutputs, 1, kMaxChannels, 1);
                auto applyButton = Button("Apply / restart", [this] { applyIoConfig(); }, ButtonOption::Ascii());
                auto muteCheckbox = Checkbox("mute main out", &monitorMuted);
                auto quitButton = Button("Quit", screen.ExitLoopClosure(), ButtonOption::Ascii());

                auto settings = Container::Horizontal({ inputsSlider, outputsSlider, applyButton, muteCheckbox, quitButton });
                auto settingsView = Renderer(settings, [=, this] {
                    return hbox({
                        text(" Inputs "), inputsSlider->Render() | size(WIDTH, EQUAL, 18),
                        text(" " + std::to_string(requestedInputs)) | size(WIDTH, EQUAL, 4),
                        text("  Outputs "), outputsSlider->Render() | size(WIDTH, EQUAL, 18),
                        text(" " + std::to_string(requestedOutputs)) | size(WIDTH, EQUAL, 4),
                        text("  "), applyButton->Render(),
                        text("  "), muteCheckbox->Render(),
                        filler(),
                        quitButton->Render(), text(" "),
                    });
                });

                auto body = Container::Horizontal({ inputsContainer, outputsContainer });
                auto root = Container::Vertical({ body, settingsView });

                auto view = Renderer(root, [=, this] {
                    syncWithEngine();
                    return vbox({
                        header(),
                        separator(),
                        hbox({
                            vbox({
                                text(" Inputs (sim → PluginHost)") | bold,
                                inputsContainer->Render() | vscroll_indicator | yframe,
                            }) | flex,
                            separator(),
                            vbox({
                                text(" Outputs (PluginHost → sim)") | bold,
                                outputsContainer->Render() | vscroll_indicator | yframe,
                            }) | size(WIDTH, GREATER_THAN, 61),
                        }) | flex,
                        separator(),
                        settingsView->Render(),
                        footer(),
                    }) | border;
                });

                return CatchEvent(view, [this](const Event& event) {
                    if (event == Event::Escape)
                    {
                        screen.Exit();
                        return true;
                    }
                    return false;
                });
            }

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
                auto kindToggle = Toggle(&kKindLabels, &r->kind);
                auto frequency = Slider("", &r->frequencyPosition, 0, 100, 1);
                auto level = Slider("", &r->levelDb, -60.0f, 0.0f, 1.0f);

                InputOption pathOption;
                pathOption.multiline = false;
                pathOption.placeholder = "path/to/file.wav (Enter to load)";
                pathOption.on_enter = [this, r] { loadFile(*r); };
                auto path = Input(&r->path, pathOption);

                auto container = Container::Horizontal({
                    kindToggle,
                    Maybe(frequency, [r] { return r->kind == kSine; }),
                    Maybe(path, [r] { return r->kind == kFile; }),
                    Maybe(level, [r] { return r->kind != kSilence; }),
                });

                row->component = Renderer(container, [=] {
                    Element params = text("") | size(WIDTH, EQUAL, 52);
                    if (r->kind == kSine)
                        params = hbox({
                            frequency->Render() | size(WIDTH, EQUAL, 16),
                            text(" " + formatFrequency(r->frequencyHz)) | size(WIDTH, EQUAL, 11),
                            text(" lvl "), level->Render() | size(WIDTH, EQUAL, 12),
                            text(" " + fixed(r->levelDb, 0) + " dB") | size(WIDTH, EQUAL, 8),
                        });
                    else if (r->kind == kFile)
                        params = hbox({
                            path->Render() | size(WIDTH, EQUAL, 27),
                            text(" lvl "), level->Render() | size(WIDTH, EQUAL, 12),
                            text(" " + fixed(r->levelDb, 0) + " dB") | size(WIDTH, EQUAL, 8),
                        });

                    auto statusLine = text("      " + r->status) | dim;
                    if (r->statusIsError)
                        statusLine = text("      " + r->status) | color(Color::Red);

                    return vbox({
                        hbox({
                            text(" In " + std::to_string(r->index + 1)) | bold | size(WIDTH, EQUAL, 7),
                            kindToggle->Render() | size(WIDTH, EQUAL, 16),
                            params,
                            text(" "),
                            meterBar(r->meterDb, 10),
                        }),
                        statusLine,
                    });
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
                        row.path = spec.filePath;

                row.appliedKind = row.kind;
            }

            std::unique_ptr<OutputRow> makeOutputRow(int index)
            {
                auto row = std::make_unique<OutputRow>();
                row->index = index;
                row->monitor = engine.isMonitorEnabled(index);

                auto* r = row.get();
                auto monitor = Checkbox("main out", &r->monitor);
                row->component = Renderer(monitor, [=] {
                    const auto now = Clock::now();
                    const bool active = r->lastSignal && now - *r->lastSignal < kSignalHold;

                    std::string ago = "never";
                    if (r->lastSignal)
                    {
                        const auto seconds = std::chrono::duration<float>(now - *r->lastSignal).count();
                        ago = active ? "now" : fixed(seconds, 0) + "s ago";
                    }

                    auto signal = active ? text(" ● SIGNAL ") | bgcolor(Color::Green) | color(Color::Black) | bold
                                         : text(" ○ idle   ") | dim;
                    auto side = text(r->index % 2 == 0 ? "L" : "R") | dim;

                    return hbox({
                        text(" Out " + std::to_string(r->index + 1)) | bold | size(WIDTH, EQUAL, 8),
                        meterBar(r->meterDb, 10),
                        signal,
                        text(" " + ago) | dim | size(WIDTH, EQUAL, 9),
                        monitor->Render(), text(" "), side,
                    });
                });
                return row;
            }

            // ------------------------------------------------------- actions

            void loadFile(InputRow& row)
            {
                std::string error;
                auto file = FileSource::load(row.path, engine.getConfig().sampleRate, row.levelDb, error);
                if (!file)
                {
                    row.status = error;
                    row.statusIsError = true;
                    return;
                }

                row.file = std::move(file);
                row.kind = kFile;
                row.appliedKind = kFile;
                engine.setInputSource(row.index, row.file);
                row.status = row.file->describe();
                row.statusIsError = false;
            }

            void applyIoConfig()
            {
                const auto error = engine.reconfigure(requestedInputs, requestedOutputs);
                requestedInputs = engine.getNumInputs();
                requestedOutputs = engine.getNumOutputs();
                populateRows();

                if (error.empty())
                    flash = "Restarted with " + std::to_string(requestedInputs) + " in / "
                            + std::to_string(requestedOutputs) + " out - re-select the device in PluginHost.";
                else
                    flash = error;
                flashIsError = !error.empty();
            }

            /** Pushes UI edits to the engine and pulls meters out of it. Runs every frame. */
            void syncWithEngine()
            {
                const auto now = Clock::now();
                const double interval = lastSync ? std::chrono::duration<double>(now - *lastSync).count() : 0.0;
                lastSync = now;

                engine.setMonitorMuted(monitorMuted);

                for (int i = 0; i < engine.getNumInputs(); ++i)
                {
                    auto& row = *inputRows[static_cast<std::size_t>(i)];
                    applyInputRow(row);
                    row.meterDb = row.meter.update(engine.consumeInputPeak(i), interval);
                }

                for (int i = 0; i < engine.getNumOutputs(); ++i)
                {
                    auto& row = *outputRows[static_cast<std::size_t>(i)];
                    engine.setMonitorEnabled(i, row.monitor);

                    const float peak = engine.consumeOutputPeak(i);
                    if (gainToDb(peak) > kSignalThresholdDb)
                        row.lastSignal = now;
                    row.meterDb = row.meter.update(peak, interval);
                }

                if (now - lastStatusPoll > std::chrono::milliseconds(250))
                {
                    loopback = queryLoopbackStatus(engine.getConfig().cardId);
                    lastStatusPoll = now;
                }
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

            // -------------------------------------------------------- chrome

            Element substream(const std::string& label, const SubstreamStatus& status, int expectedChannels)
            {
                if (!status.open)
                    return hbox({ text(label), badge(false, "", "not connected") });

                std::string details = status.state;
                if (status.channels > 0)
                    details += " · " + std::to_string(status.channels) + " ch · " + std::to_string(status.rate) + " Hz";

                auto element = hbox({ text(label), badge(true, "connected", ""), text(" " + details) | dim });
                if (status.channels > 0 && status.channels != expectedChannels)
                    element = hbox({ element, text("  (sim has " + std::to_string(expectedChannels) + " ch)") | color(Color::Yellow) });
                return element;
            }

            Element header()
            {
                const auto& config = engine.getConfig();
                const auto lastError = engine.getLastError();
                const auto monitorError = engine.getMonitorError();

                Elements lines;
                lines.push_back(hbox({
                    text(" StageCue Soundcard Sim ") | bold | inverted,
                    text("  card "), badge(loopback.cardPresent, config.cardId, config.cardId + " missing"),
                    text("  engine "), badge(engine.isRunning(), "running", "stopped"),
                    text("  " + std::to_string(config.sampleRate) + " Hz · " + std::to_string(config.periodFrames)
                         + " frames" + (engine.isRunning() ? " · " + engine.getLoopbackFormat() : "")) | dim,
                }));

                if (!loopback.cardPresent)
                    lines.push_back(text(" Loopback card not found - run ./setup-loopback.sh (sudo modprobe snd-aloop id="
                                         + config.cardId + " pcm_substreams=1)") | color(Color::Yellow));

                lines.push_back(hbox({
                    text(" In PluginHost select "), text("StageCue Soundcard Sim") | bold,
                    text(" as input and output device") ,
                    text("  (ALSA alias stagecue_sim → " + appPcmName(config) + ")") | dim,
                }));

                lines.push_back(hbox({
                    substream(" PluginHost input:  ", loopback.appCapture, engine.getNumInputs()),
                }));
                lines.push_back(hbox({
                    substream(" PluginHost output: ", loopback.appPlayback, engine.getNumOutputs()),
                }));

                lines.push_back(hbox({
                    text(" main out "), text(config.monitorDevice) | bold, text("  "),
                    engine.isRunning() ? badge(engine.isMonitorOpen() && monitorError.empty(), "ok", "unavailable")
                                       : text("(engine stopped)") | dim,
                    text("   xruns: loopback " + std::to_string(engine.getLoopbackXruns())
                         + " · main out " + std::to_string(engine.getMonitorXruns())
                         + " · main-out dropouts " + std::to_string(engine.getMonitorDropouts())) | dim,
                }));

                if (!lastError.empty())
                    lines.push_back(paragraph(" " + lastError) | color(Color::Red));
                if (!monitorError.empty())
                    lines.push_back(text(" main out: " + monitorError) | color(Color::Yellow));

                return vbox(std::move(lines));
            }

            Element footer()
            {
                Elements lines;
                if (!flash.empty())
                    lines.push_back(paragraph(" " + flash) | color(flashIsError ? Color::Red : Color::Green));
                lines.push_back(text(" Tab/arrows: move · ←/→: adjust · Enter: load file · Esc or Ctrl+C: quit") | dim);
                return vbox(std::move(lines));
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
            std::string flash;
            bool flashIsError = false;

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

        std::atomic<bool> ticking { true };
        std::thread ticker([&] {
            while (ticking.load())
            {
                std::this_thread::sleep_for(kRefreshInterval);
                screen.PostEvent(Event::Custom);
            }
        });

        screen.Loop(component);

        ticking = false;
        ticker.join();
        return 0;
    }
}
