cmake_minimum_required(VERSION 3.21)
project(NeonSynth VERSION 1.10.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Override with -DJUCE_DIR=... if JUCE is not checked into the project tree
if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/JUCE")
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

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

juce_add_plugin(NeonSynth
    VERSION                     1.10.1
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
        NeonSynthVoice.h
        NeonSynthEditor.h
        CrtTheme.h
        CrtFrame.h
        CrtLookAndFeel.h
        NeonSynthProcessor.h
        NeonSynthProcessor.cpp
        MidiLogger.h
        Visualisers.h
        StepSequencer.h
        SequencerView.h
        HelpOverlay.h
        VoiceEnvelope.h
        Oscillator.h
        Sculpt.h
        SampleData.h
        SampleSlot.h
        SampleLoader.h
        SampleReader.h
        GrainCloud.h
        SampleView.h
        ShapeView.h
        LfoView.h
        Lfo.h
        Modulation.h
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
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
        neonsynth_warning_tweaks
        neonsynth_sanitize
)

target_include_directories(NeonSynth
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# ---------------------------------------------------------------------------
# Optional unit tests, one executable per file in tests/ (listed in
# NEONSYNTH_UNIT_TESTS below). They only need JUCE's audio modules, so they
# build in a few seconds once JUCE is compiled:
#   cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
#   cmake --build build --config Release --target unit_tests
#   ctest --test-dir build -C Release --output-on-failure
# Each executable exits non-zero if any test fails. A new test only needs its
# name added to NEONSYNTH_UNIT_TESTS; CI runs everything ctest knows about.
# ---------------------------------------------------------------------------
option(NEONSYNTH_BUILD_TESTS "Build the unit tests and the render benchmark" OFF)

if(NEONSYNTH_BUILD_TESTS)
    enable_testing()
    set(NEONSYNTH_UNIT_TESTS StepSequencerTests VoiceEnvelopeTests OscillatorTests ChopTests SculptTests SampleDataTests SampleReaderTests GranularTests FilterTests LfoTests ModulationTests)
    add_custom_target(unit_tests)

    foreach(test ${NEONSYNTH_UNIT_TESTS})
        juce_add_console_app(${test} PRODUCT_NAME "${test}")
        target_sources(${test} PRIVATE tests/${test}.cpp)
        target_compile_definitions(${test} PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
        target_link_libraries(${test} PRIVATE
            juce::juce_audio_basics
            juce::juce_audio_formats
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
            neonsynth_warning_tweaks
            neonsynth_sanitize)
        add_dependencies(unit_tests ${test})
        add_test(NAME ${test} COMMAND ${test})
    endforeach()

    # FilterTests compares CytomicSvf with juce::dsp::StateVariableTPTFilter bit for bit
    target_link_libraries(FilterTests PRIVATE juce::juce_dsp)

    # Programs that drive the whole processor directly (no host, no Python):
    #   RenderBenchmark       times processBlock for a few load scenarios
    #   RealtimeSafetyTests   fails if processBlock allocates (also run by ctest)
    #   ParameterLayoutTests  fails if a parameter changed other than by being appended (ctest)
    #   SessionStateTests     saving and restoring the sample with a session (ctest)
    #   StateFuzzTests        damaged and hostile sessions must not crash or break the processor (ctest)
    #   EditorTests           drives the editor offscreen with mouse events and paints it (ctest;
    #                         on Linux it needs an X display, e.g. xvfb-run)
    foreach(program RenderBenchmark RealtimeSafetyTests ParameterLayoutTests SessionStateTests StateFuzzTests EditorTests PitchTests MidiModulationTests)
        juce_add_console_app(${program} PRODUCT_NAME "${program}")
        juce_generate_juce_header(${program})
        target_sources(${program} PRIVATE tests/${program}.cpp NeonSynthProcessor.cpp)
        target_include_directories(${program} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
        target_compile_definitions(${program} PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_MODAL_LOOPS_PERMITTED=1                          # EditorTests runs the message loop
            NEONSYNTH_TESTS_DIR="${CMAKE_CURRENT_LIST_DIR}/tests")   # ParameterLayoutTests' reference
        target_link_libraries(${program} PRIVATE
            NeonSynthFonts
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
            neonsynth_warning_tweaks
            neonsynth_sanitize)
    endforeach()

    foreach(program RealtimeSafetyTests ParameterLayoutTests SessionStateTests StateFuzzTests EditorTests PitchTests MidiModulationTests)
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
