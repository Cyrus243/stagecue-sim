#include "SimConfig.h"
#include "SimEngine.h"
#include "Ui.h"

#include <iostream>

int main(int argc, char** argv)
{
    const auto parsed = sim::parseArgs(argc, argv);
    if (!parsed.config)
    {
        (parsed.exitCode == 0 ? std::cout : std::cerr) << parsed.message;
        return parsed.exitCode;
    }

    sim::AlsaPcm::silenceLibraryErrors();
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

    // A failed start is shown in the UI header; the user can fix it and press Apply.
    (void) engine.start();

    const int exitCode = sim::runUi(engine);
    engine.stop();
    return exitCode;
}
