cmake_minimum_required(VERSION 3.22)
project(StageCueSoundcardSim VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "StageCueSoundcardSim relies on ALSA snd-aloop and only builds on Linux")
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

# Single-header WAV/AIFF/FLAC decoders (public domain / MIT-0).
CPMAddPackage(
    NAME dr_libs
    GITHUB_REPOSITORY mackron/dr_libs
    GIT_TAG dfe8377631000664666519fdb83da193fd8037f4
    SOURCE_DIR ${LIB_DIR}/dr_libs
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

find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

# Everything except the UI and main(), shared by the app and the tests.
add_library(soundcard_sim_core STATIC
        src/AlsaPcm.cpp
        src/AudioFileDecoder.cpp
        src/LoopbackStatus.cpp
        src/SimConfig.cpp
        src/SimEngine.cpp
        src/Sources.cpp
)
target_include_directories(soundcard_sim_core PUBLIC src)
target_include_directories(soundcard_sim_core SYSTEM PRIVATE ${dr_libs_SOURCE_DIR})
target_link_libraries(soundcard_sim_core
        PUBLIC
        ALSA::ALSA
        Threads::Threads
)

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
    )
    target_link_libraries(SoundcardSimTest PRIVATE soundcard_sim_core gtest_main)
    include(GoogleTest)
    gtest_discover_tests(SoundcardSimTest)
endif()
