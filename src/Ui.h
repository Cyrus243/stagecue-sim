#pragma once

namespace sim
{
    class SimEngine;

    /** Runs the FTXUI front-end until the user quits. Returns the process exit code. */
    int runUi(SimEngine& engine);
}
