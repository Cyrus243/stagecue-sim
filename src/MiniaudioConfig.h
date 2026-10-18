#pragma once

// One miniaudio configuration for every translation unit that includes it:
// only the backends this tool uses, no high-level engine, linked (not dlopen()ed).
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#if defined(__APPLE__)
    #define MA_ENABLE_COREAUDIO
#else
    #define MA_ENABLE_ALSA
#endif
#define MA_NO_RUNTIME_LINKING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MA_NO_GENERATION

#include <miniaudio.h>
