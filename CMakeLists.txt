cmake_minimum_required(VERSION 3.21)
project(NeonSynth VERSION 1.11.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE, the only C++ dependency (FLAC comes inside it). Three ways, first match wins:
#   -DJUCE_DIR=<path>   a JUCE checkout of your own (CI's cached copy, an offline machine);
#   ./JUCE              a checkout inside the project tree;
#   otherwise           CMake downloads it (v1.11.1), pinned to the 9.0.0 release's commit
#                       rather than its tag, so what we build against can never move.
# The JUCE team recommends fetching it from GitHub over package managers; with one dependency
# and no transitive ones, FetchContent needs no extra tool.
set(NEONSYNTH_JUCE_VERSION "9.0.0")
set(NEONSYNTH_JUCE_COMMIT "f8f8864172464b9adf9eba6101e1f784838d1597")   # tag 9.0.0
if(NOT DEFINED JUCE_DIR AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/JUCE/CMakeLists.txt")
    set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/JUCE")
endif()
if(DEFINED JUCE_DIR)
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
    include(FetchContent)
    message(STATUS "No JUCE_DIR: fetching JUCE ${NEONSYNTH_JUCE_VERSION} (${NEONSYNTH_JUCE_COMMIT})")
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${NEONSYNTH_JUCE_COMMIT}
        GIT_SHALLOW    FALSE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# No fused multiply-add contraction. The grain loop (GrainCloud.h) has scalar, SSE2 and
# AVX2 versions that must render bit-identical audio, so a session sounds the same on
# every machine. A compiler allowed to fuse a*b+c into one FMA (one rounding instead of
# two) does it differently in each version: GCC and Clang do so by default whenever the
# target has FMA (GCC 15 on Ubuntu 26.04 does, out of the box). MSVC doesn't contract
# unless asked (/fp:contract), so it needs no flag.
add_compile_options($<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-ffp-contract=off>)

# JUCE's recommended warning flags include -Wfloat-equal. This code compares floats exactly on
# purpose (a parameter still at its default, a value that was just set, a stored flag), so that
# one warning is switched off, which leaves the rest of the warnings readable. This interface
# target must be linked AFTER juce_recommended_warning_flags: CMake puts a dependency's options
# after the target's own, so only a later-linked target can override JUCE's flag. MSVC does not
# have the warning.
add_library(neonsynth_warning_tweaks INTERFACE)
if(NOT MSVC)
    target_compile_options(neonsynth_warning_tweaks INTERFACE -Wno-float-equal)
endif()

# CI turns this on, so a new warning in the project's own code fails the build. It is off by
# default so that a newer compiler cannot stop anyone from building a release locally.
option(NEONSYNTH_WARNINGS_AS_ERRORS "Treat compiler warnings in NeonSynth's own code as errors" OFF)
if(NEONSYNTH_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(neonsynth_warning_tweaks INTERFACE /WX)
    else()
        target_compile_options(neonsynth_warning_tweaks INTERFACE -Werror)
    endif()
endif()

# The integrity job (v1.11.0, .github/workflows/integrity.yml): Clang's warnings for code that is
# never used or never reached, and for every kind of shadowing (the name clash MSVC found in
# v1.10.0 included), which GCC and MSVC do not give. Only NeonSynth's own files get them (the
# processor and the tests, and through them every header in src/): JUCE's module sources are
# compiled into the same targets and are not written for these warnings, and JUCE's headers
# count as system headers here, so they stay quiet too. With NEONSYNTH_WARNINGS_AS_ERRORS they
# are errors. The source-file properties are set at the end of this file.
option(NEONSYNTH_INTEGRITY "Clang only: extra warnings for unused and unreachable code and shadowing (the integrity job)" OFF)
if(NEONSYNTH_INTEGRITY AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NEONSYNTH_INTEGRITY needs Clang (configure with -DCMAKE_CXX_COMPILER=clang++)")
endif()
set(neonsynth_integrity_flags
    -Wunreachable-code
    -Wunused-member-function
    -Wunused-private-field
    -Wunused-template
    -Wshadow-all
    --system-header-prefix=juce_)

# Sanitizer build (off by default): AddressSanitizer and UndefinedBehaviorSanitizer for the plugin
# and every test program, so a memory error or undefined behaviour fails the tests instead of going
# unnoticed. UB stops the program (-fno-sanitize-recover) rather than printing a warning ctest would
# ignore. GCC and Clang only (MSVC's /fsanitize=address has no UBSan). Configure it with
# CMAKE_BUILD_TYPE Debug or RelWithDebInfo: Release adds -flto (juce_recommended_lto_flags), which
# makes the reports harder to read and the build much slower. The JUCE code compiled into each target
# is instrumented too, which is what ASan needs to see the buffers JUCE hands to NeonSynth's code.
# Leave NEONSYNTH_WARNINGS_AS_ERRORS off here: GCC 13's shift instrumentation makes it report
# -Wsign-conversion in shifts that are clean in a normal build (CI checks warnings there).
option(NEONSYNTH_SANITIZE "Build NeonSynth and its tests with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
add_library(neonsynth_sanitize INTERFACE)
if(NEONSYNTH_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "NEONSYNTH_SANITIZE needs GCC or Clang")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(WARNING "NEONSYNTH_SANITIZE with a Release build also uses LTO; Debug or RelWithDebInfo is recommended")
    endif()
    # Two kinds of check stay recoverable, only so that tests/sanitizer-suppressions.txt can
    # silence them in two JUCE files (UBSan ignores suppressions for unrecoverable checks); the
    # tests run with halt_on_error=1, so they still stop a test anywhere else.
    set(neonsynth_sanitize_flags
        -fsanitize=address,undefined
        -fno-sanitize-recover=undefined
        -fsanitize-recover=nonnull-attribute,shift-base
        -fno-omit-frame-pointer)
    target_compile_options(neonsynth_sanitize INTERFACE ${neonsynth_sanitize_flags})
    target_link_options(neonsynth_sanitize INTERFACE ${neonsynth_sanitize_flags})
endif()

# Link-time optimisation for the plugin (v1.11.1). On by default and always on for releases and for
# the CI job that runs the compare test and pluginval, so what ships is what is tested. Turn it off
# for local builds and the other CI jobs: it is the slowest step of the plugin's build and changes
# nothing that sounds (-ffp-contract=off and no fast-math keep the arithmetic exact either way).
# Only Release uses it (juce_recommended_lto_flags). GCC runs its final step on every core
# (-flto=auto) instead of one; MSVC's /LTCG is parallel already.
option(NEONSYNTH_LTO "Link the plugin with link-time optimisation (Release builds)" ON)
add_library(neonsynth_lto INTERFACE)
if(NEONSYNTH_LTO)
    target_link_libraries(neonsynth_lto INTERFACE juce::juce_recommended_lto_flags)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(neonsynth_lto INTERFACE $<$<CONFIG:Release>:-flto=auto>)
    endif()
endif()

juce_add_plugin(NeonSynth
    VERSION                     1.11.0
    COMPANY_NAME                "AudioDSP"
    COMPANY_WEBSITE             "https://github.com/kv244/NeonSynth"   # until a company site is decided (A12)
    BUNDLE_ID                   "com.audiodsp.neonsynth"
    PLUGIN_MANUFACTURER_CODE    AuDs
    PLUGIN_CODE                 BaSy
    FORMATS                     Standalone VST3
    PRODUCT_NAME                "NeonSynth"
    ICON_BIG                    "${CMAKE_CURRENT_LIST_DIR}/icon.png"
    ICON_SMALL                  "${CMAKE_CURRENT_LIST_DIR}/icon.png"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

juce_generate_juce_header(NeonSynth)

# The CRT theme's fonts (v1.10.0), compiled in so the window looks the same everywhere.
# Both are under the SIL Open Font Licence; fonts/OFL-*.txt ship with the installer.
juce_add_binary_data(NeonSynthFonts
    HEADER_NAME NeonFonts.h
    NAMESPACE NeonFonts
    SOURCES
        fonts/VT323-Regular.ttf
        fonts/IBMPlexMono-Regular.ttf
        fonts/IBMPlexMono-SemiBold.ttf)

target_sources(NeonSynth
    PRIVATE
        # src/dsp
        src/dsp/Oscillator.h
        src/dsp/Blep4.h
        src/dsp/Chop.h
        src/dsp/SampleReader.h
        src/dsp/GrainCloud.h
        src/dsp/CytomicSvf.h
        src/dsp/VoiceEnvelope.h
        src/dsp/Lfo.h
        src/dsp/Modulation.h
        src/dsp/Sculpt.h
        # src/sequencer
        src/sequencer/StepSequencer.h
        # src/state
        src/state/SampleData.h
        src/state/SampleLoader.h
        src/state/SampleSlot.h
        src/state/PresetManager.h
        src/state/FactoryPresets.h
        # src/gui
        src/gui/NeonSynthEditor.h
        src/gui/CrtTheme.h
        src/gui/CrtFrame.h
        src/gui/CrtLookAndFeel.h
        src/gui/NeonStyle.h
        src/gui/SignalHistory.h
        src/gui/ScopeView.h
        src/gui/SpectrumView.h
        src/gui/LevelMeter.h
        src/gui/AdsrView.h
        src/gui/VoiceLights.h
        src/gui/SampleView.h
        src/gui/ShapeView.h
        src/gui/LfoView.h
        src/gui/SequencerView.h
        src/gui/HelpOverlay.h
        src/gui/PresetBar.h
        # src/plugin
        src/plugin/NeonSynthProcessor.h
        src/plugin/NeonSynthProcessor.cpp
        src/plugin/NeonSynthVoice.h
        src/plugin/MidiLogger.h
)

target_compile_definitions(NeonSynth
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0   # never shipped as VST2; parameters use ParameterID
        JUCE_WEB_BROWSER=0   # plugin has no embedded browser
        JUCE_USE_CURL=0      # plugin has no network I/O
)

target_link_libraries(NeonSynth
    PRIVATE
        NeonSynthFonts
        juce::juce_core
        juce::juce_gui_basics
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_plugin_client
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        neonsynth_lto
        juce::juce_recommended_warning_flags
        neonsynth_warning_tweaks
        neonsynth_sanitize
)

# Sources are grouped by job (v1.11.0): src/dsp (signal processing), src/sequencer,
# src/state (samples and their hand-off), src/gui (the editor and its views) and src/plugin
# (processor, voice, MIDI log). Includes name the folder: #include "dsp/Oscillator.h".
target_include_directories(NeonSynth
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
)

# ---------------------------------------------------------------------------
# Optional tests, one executable per file in tests/ (listed in NEONSYNTH_UNIT_TESTS or
# NEONSYNTH_ENGINE_TESTS below). JUCE and the engine are compiled once for all of them (v1.11.1),
# so each new test costs only its own file:
#   cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
#   cmake --build build --config Release --target unit_tests
#   ctest --test-dir build -C Release --output-on-failure
# Each executable exits non-zero if any test fails. A new test only needs its name added to one
# of the two lists; CI runs everything ctest knows about.
# ---------------------------------------------------------------------------
option(NEONSYNTH_BUILD_TESTS "Build the unit tests and the render benchmark" OFF)

if(NEONSYNTH_BUILD_TESTS)
    enable_testing()
    add_custom_target(unit_tests)

    # v1.11.1: JUCE and the engine are each compiled once for all the test programs, as two
    # static libraries they link, instead of once per program (before: every one of the 23
    # programs compiled the JUCE modules it used, and 12 of them NeonSynthProcessor.cpp too).
    #
    #   neonsynth_test_juce   the JUCE modules the tests use, built as a console application
    #                         would build them; its JuceHeader.h is written below
    #   neonsynth_engine      the processor and everything it includes (voice, DSP, sequencer,
    #                         state, and the editor for EditorTests), plus the fonts
    #
    # The plugin keeps its own copy of both: its JUCE is configured as a plugin
    # (JucePlugin_* definitions, the wrappers), which a test program must not be.
    add_library(neonsynth_test_juce STATIC)
    target_link_libraries(neonsynth_test_juce
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            neonsynth_sanitize)
    target_compile_definitions(neonsynth_test_juce
        PUBLIC
            JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
            JUCE_STANDALONE_APPLICATION=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_MODAL_LOOPS_PERMITTED=1)          # EditorTests runs the message loop
    # The modules' include paths and JUCE_MODULE_AVAILABLE_* definitions reach the programs
    # that link this library (JUCE's modules are linked privately, so their sources compile
    # here once and nowhere else).
    target_include_directories(neonsynth_test_juce INTERFACE
        $<TARGET_PROPERTY:neonsynth_test_juce,INCLUDE_DIRECTORIES>
        ${CMAKE_CURRENT_BINARY_DIR}/neonsynth_test_juce)
    target_compile_definitions(neonsynth_test_juce INTERFACE
        $<TARGET_PROPERTY:neonsynth_test_juce,COMPILE_DEFINITIONS>)
    set_target_properties(neonsynth_test_juce PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # JuceHeader.h for the tests and the engine, as juceaide would write it for a target with
    # these modules (juce_generate_juce_header only works on juce_add_* targets).
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/neonsynth_test_juce/JuceHeader.h
"#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_events/juce_events.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_dsp/juce_dsp.h>
#if ! DONT_SET_USING_JUCE_NAMESPACE
 using namespace juce;
#endif
")

    add_library(neonsynth_engine STATIC src/plugin/NeonSynthProcessor.cpp)
    target_include_directories(neonsynth_engine PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
    target_link_libraries(neonsynth_engine
        PUBLIC
            neonsynth_test_juce
            NeonSynthFonts
        PRIVATE
            juce::juce_recommended_warning_flags
            neonsynth_warning_tweaks)
    set_target_properties(neonsynth_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Unit tests of single classes: one program per file, linking only JUCE.
    set(NEONSYNTH_UNIT_TESTS StepSequencerTests VoiceEnvelopeTests OscillatorTests ChopTests SculptTests SampleDataTests SampleReaderTests GranularTests FilterTests LfoTests ModulationTests)

    # Programs that drive the whole processor directly (no host, no Python), linking the engine:
    #   RenderBenchmark       times processBlock for a few load scenarios (not run by ctest)
    #   RealtimeSafetyTests   fails if processBlock allocates
    #   ParameterLayoutTests  fails if a parameter changed other than by being appended
    #   SessionStateTests     saving and restoring the sample with a session
    #   StateFuzzTests        damaged and hostile sessions must not crash or break the processor
    #   EditorTests           drives the editor offscreen with mouse events and paints it (on
    #                         Linux it needs an X display, e.g. xvfb-run)
    #   PitchTests, MidiModulationTests, PlayingTests, PresetTests, VoiceStealingTests, SweepTests
    set(NEONSYNTH_ENGINE_TESTS RealtimeSafetyTests ParameterLayoutTests SessionStateTests StateFuzzTests EditorTests PitchTests MidiModulationTests
        PlayingTests PresetTests VoiceStealingTests SweepTests)

    foreach(program ${NEONSYNTH_UNIT_TESTS} ${NEONSYNTH_ENGINE_TESTS} RenderBenchmark)
        add_executable(${program} tests/${program}.cpp)
        target_include_directories(${program} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
        target_compile_definitions(${program} PRIVATE
            NEONSYNTH_TESTS_DIR="${CMAKE_CURRENT_LIST_DIR}/tests")   # fixtures, the parameter layout
        target_link_libraries(${program} PRIVATE
            juce::juce_recommended_warning_flags
            neonsynth_warning_tweaks)
        # Kept where they were before v1.11.1, so the test programs land in the same place.
        set_target_properties(${program} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${program}_artefacts/$<CONFIG>)
    endforeach()

    foreach(test ${NEONSYNTH_UNIT_TESTS})
        target_link_libraries(${test} PRIVATE neonsynth_test_juce)
    endforeach()
    foreach(program ${NEONSYNTH_ENGINE_TESTS} RenderBenchmark)
        target_link_libraries(${program} PRIVATE neonsynth_engine)
    endforeach()

    foreach(program ${NEONSYNTH_UNIT_TESTS} ${NEONSYNTH_ENGINE_TESTS})
        add_dependencies(unit_tests ${program})
        add_test(NAME ${program} COMMAND ${program})
    endforeach()

    if(NEONSYNTH_SANITIZE)
        # The sanitizers' run-time options, for every test ctest runs (see NEONSYNTH_SANITIZE).
        # LeakSanitizer stays on (the default): no test leaks today, JUCE's singletons and the
        # GUI tests' system libraries included. Use-after-return and initialisation-order checks
        # are added, and UBSan halts on the checks left recoverable, except where the
        # suppressions file says otherwise.
        get_property(neonsynth_all_tests DIRECTORY PROPERTY TESTS)
        set_tests_properties(${neonsynth_all_tests} PROPERTIES ENVIRONMENT
            "ASAN_OPTIONS=strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=${CMAKE_CURRENT_LIST_DIR}/tests/sanitizer-suppressions.txt")
    endif()
endif()

if(NEONSYNTH_INTEGRITY)
    file(GLOB neonsynth_test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tests/*.cpp)
    set_source_files_properties(src/plugin/NeonSynthProcessor.cpp ${neonsynth_test_sources}
        PROPERTIES COMPILE_OPTIONS "${neonsynth_integrity_flags}")
endif()
