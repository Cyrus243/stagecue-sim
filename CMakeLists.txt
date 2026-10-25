cmake_minimum_required(VERSION 3.22)
project(StageCueSoundcardSim VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE)
    message(FATAL_ERROR "StageCueSoundcardSim builds on Linux (ALSA snd-aloop) and macOS (CoreAudio) only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SOUNDCARD_SIM_BUILD_TESTS "Build the simulator's unit tests" ${PROJECT_IS_TOP_LEVEL})

# When built inside stagecue-backend, reuse its CPM setup and LIB_DIR.
if(NOT DEFINED LIB_DIR)
    set(LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)
endif()
if(NOT COMMAND CPMAddPackage)
    include(cmake/cpm.cmake)
endif()

# Single-header audio I/O (ALSA, CoreAudio), decoding (WAV/FLAC/MP3) and resampling (public domain / MIT-0).
CPMAddPackage(
    NAME miniaudio
    GITHUB_REPOSITORY mackron/miniaudio
    GIT_TAG 0.11.25
    SOURCE_DIR ${LIB_DIR}/miniaudio
    DOWNLOAD_ONLY YES
)

if(NOT TARGET ftxui::component)
    CPMAddPackage(
        NAME ftxui
        GITHUB_REPOSITORY ArthurSonzogni/FTXUI
        GIT_TAG v6.1.9
        VERSION 6.1.9
        SOURCE_DIR ${LIB_DIR}/ftxui
        OPTIONS
            "FTXUI_BUILD_EXAMPLES OFF"
            "FTXUI_BUILD_DOCS OFF"
            "FTXUI_BUILD_TESTS OFF"
            "FTXUI_ENABLE_INSTALL OFF"
            "FTXUI_QUIET ON"
    )
endif()

find_package(Threads REQUIRED)

# Everything except the UI and main(), shared by the app and the tests.
add_library(soundcard_sim_core STATIC
        src/AudioIo.cpp
        src/LoopbackStatus.cpp
        src/Mixer.cpp
        src/SimConfig.cpp
        src/SimEngine.cpp
        src/SimSetup.cpp
        src/Sources.cpp
        src/StatusPoller.cpp
        src/StemStream.cpp
        src/miniaudio.c
)
target_include_directories(soundcard_sim_core PUBLIC src)
target_include_directories(soundcard_sim_core SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_link_libraries(soundcard_sim_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Platform seam: CoreAudio on macOS, ALSA on Linux. miniaudio links them directly
# (MA_NO_RUNTIME_LINKING in src/miniaudio.c) instead of dlopen()ing them.
if(APPLE)
    target_sources(soundcard_sim_core PRIVATE
            src/platform/CoreAudioDevices.cpp
            src/platform/PlatformMac.cpp
    )
    target_link_libraries(soundcard_sim_core PUBLIC
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework CoreFoundation"
    )
else()
    find_package(ALSA REQUIRED)
    target_sources(soundcard_sim_core PRIVATE src/platform/PlatformLinux.cpp)
    target_link_libraries(soundcard_sim_core PUBLIC ALSA::ALSA m)
endif()

add_executable(StageCueSoundcardSim
        src/main.cpp
        src/Ui.cpp
)
target_link_libraries(StageCueSoundcardSim
        PRIVATE
        soundcard_sim_core
        ftxui::screen
        ftxui::dom
        ftxui::component
)

if(SOUNDCARD_SIM_BUILD_TESTS)
    if(NOT TARGET GTest::gtest_main AND NOT TARGET gtest_main)
        CPMAddPackage(
            NAME googletest
            GITHUB_REPOSITORY google/googletest
            VERSION 1.17.0
            SOURCE_DIR ${LIB_DIR}/googletest
            OPTIONS "INSTALL_GTEST OFF" "gtest_force_shared_crt ON"
        )
    endif()

    enable_testing()
    add_executable(SoundcardSimTest
            tests/SimConfigTest.cpp
            tests/SourcesTest.cpp
            tests/MetersTest.cpp
            tests/LoopbackStatusTest.cpp
            tests/EventLogTest.cpp
            tests/MixerTest.cpp
            tests/SourceSlotsTest.cpp
            tests/SimSetupTest.cpp
            tests/StemStreamTest.cpp
    )
    target_link_libraries(SoundcardSimTest PRIVATE soundcard_sim_core gtest_main)
    include(GoogleTest)
    gtest_discover_tests(SoundcardSimTest)
endif()
