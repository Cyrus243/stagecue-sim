#include "SimConfig.h"
#include "SimEngine.h"
#include "Ui.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace
{
    volatile std::sig_atomic_t quitRequested = 0;

    void requestQuit(int) { quitRequested = 1; }

    std::string onOff(bool value) { return value ? "connected" : "-"; }

    /** No terminal UI: runs the engine and prints one status line every 5 s until
        SIGINT/SIGTERM. Meant for soak tests (watch the RSS column stay flat). */
    int runHeadless(sim::SimEngine& engine)
    {
        using namespace std::chrono;

        std::signal(SIGINT, requestQuit);
        std::signal(SIGTERM, requestQuit);

        if (const auto error = engine.getLastError(); !error.empty())
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::cout << "running headless: " << engine.getNumInputs() << " in / " << engine.getNumOutputs() << " out @ "
                  << engine.getConfig().sampleRate << " Hz, format " << engine.getLoopbackFormat()
                  << " (Ctrl+C to quit)" << std::endl;

        const auto start = steady_clock::now();
        auto nextReport = start;
        float inputPeak = 0.0f, outputPeak = 0.0f;   // loudest channel since the last line
        while (quitRequested == 0)
        {
            std::this_thread::sleep_for(milliseconds(200));
            engine.housekeeping();
            // Drain the meters like the UI would, so peaks don't just sit there.
            for (int i = 0; i < engine.getNumInputs(); ++i)
                inputPeak = std::max(inputPeak, engine.consumeInputPeak(i));
            for (int i = 0; i < engine.getNumOutputs(); ++i)
                outputPeak = std::max(outputPeak, engine.consumeOutputPeak(i));

            const auto now = steady_clock::now();
            if (now < nextReport)
                continue;
            nextReport = now + seconds(5);

            const auto link = engine.getLinkStatus();
            std::cout << "[" << duration_cast<seconds>(now - start).count() << "s] "
                      << (engine.isStalled() ? "STALLED" : engine.isRunning() ? "running" : "stopped")
                      << "  xruns " << engine.getLoopbackXruns() << "  main-out xruns " << engine.getMonitorXruns()
                      << "  dropouts " << engine.getMonitorDropouts()
                      << "  peak in " << sim::gainToDb(inputPeak) << " dB out " << sim::gainToDb(outputPeak) << " dB"
                      << "  link in " << onOff(link.appCapture.open) << " out " << onOff(link.appPlayback.open)
                      << "  rss " << sim::platform::residentMemoryBytes() / (1024 * 1024) << " MB" << std::endl;
            inputPeak = outputPeak = 0.0f;

            if (!engine.isRunning())
            {
                std::cerr << "error: " << engine.getLastError() << "\n";
                return 1;
            }
        }
        return 0;
    }
}

int main(int argc, char** argv)
{
    const auto parsed = sim::parseArgs(argc, argv);
    if (!parsed.config)
    {
        (parsed.exitCode == 0 ? std::cout : std::cerr) << parsed.message;
        return parsed.exitCode;
    }

    sim::platform::init();
    sim::SimEngine engine(*parsed.config);

    for (const auto& spec : parsed.config->sources)
    {
        switch (spec.kind)
        {
            case sim::SourceSpec::Kind::Silence:
                engine.setInputSource(spec.input, nullptr);
                break;

            case sim::SourceSpec::Kind::Sine:
                engine.setInputSource(spec.input, std::make_shared<sim::SineSource>(
                                                      parsed.config->sampleRate, spec.frequencyHz, spec.levelDb));
                break;

            case sim::SourceSpec::Kind::File:
            {
                std::string error;
                auto file = sim::FileSource::load(spec.filePath, parsed.config->sampleRate, spec.levelDb, error);
                if (!file)
                {
                    std::cerr << "--source " << spec.input + 1 << ": " << error << "\n";
                    return 2;
                }
                engine.setInputSource(spec.input, std::move(file));
                break;
            }
        }
    }

    engine.setPlaying(parsed.config->startPlaying);

    // A failed start is shown in the UI header; the user can fix it and press Apply.
    (void) engine.start();

    const int exitCode = parsed.config->headless ? runHeadless(engine) : sim::runUi(engine);
    engine.stop();
    return exitCode;
}
