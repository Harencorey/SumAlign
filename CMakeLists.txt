cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED SA_VERSION)
    set(SA_VERSION 0.1.0)   # CI passes 0.1.<build number>
endif()
project(SumAlign VERSION ${SA_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)  # universal binary
endif()

option(SA_BUILD_TESTS "Build the tests" ON)

# JUCE: use a local checkout if you pass -DJUCE_DIR=/path/to/JUCE, otherwise download 8.0.15.
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.15 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(SA_FORMATS AU VST3 Standalone)
    set(SA_COPY_AFTER_BUILD TRUE)   # installs into ~/Library/Audio/Plug-Ins/
else()
    set(SA_FORMATS VST3 Standalone)
    set(SA_COPY_AFTER_BUILD FALSE)
endif()

juce_add_plugin(SumAlign
    COMPANY_NAME "Norka DSP"
    BUNDLE_ID com.norkadsp.sumalign
    PLUGIN_MANUFACTURER_CODE Nrka
    PLUGIN_CODE SmAl              # keep this forever once it's been used in sessions
    FORMATS ${SA_FORMATS}
    PRODUCT_NAME "SumAlign"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Tools
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "SumAlign processes the live audio from your audio interface."
    COPY_PLUGIN_AFTER_BUILD ${SA_COPY_AFTER_BUILD})

target_sources(SumAlign PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(SumAlign PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SumAlign
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Tests --------------------------------------------------------------------
if(SA_BUILD_TESTS)
    enable_testing()
    # Engine tests: plain C++, no JUCE.
    add_executable(AlignTests tests/test_engine.cpp)
    target_include_directories(AlignTests PRIVATE Source)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(AlignTests PRIVATE -Wno-mismatched-new-delete)
    endif()
    add_test(NAME engine COMMAND AlignTests)

    # ALIGN analysis tests: plain C++, no JUCE.
    add_executable(AnalysisTests tests/test_analysis.cpp)
    target_include_directories(AnalysisTests PRIVATE Source)
    find_package(Threads REQUIRED)
    target_link_libraries(AnalysisTests PRIVATE Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(AnalysisTests PRIVATE -Wno-mismatched-new-delete)
    endif()
    add_test(NAME analysis COMMAND AnalysisTests)

    # Whole-session ALIGN tests (network estimator, solver, hub): plain C++.
    add_executable(NetworkTests tests/test_network.cpp)
    target_include_directories(NetworkTests PRIVATE Source)
    target_link_libraries(NetworkTests PRIVATE Threads::Threads)
    add_test(NAME network COMMAND NetworkTests)

    # Plug-in level tests: the real processor, driven like a host drives it.
    juce_add_console_app(AlignProcessorTests PRODUCT_NAME "AlignProcessorTests")
    target_sources(AlignProcessorTests PRIVATE
        tests/test_processor.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(AlignProcessorTests PRIVATE Source)
    target_compile_definitions(AlignProcessorTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="SumAlign" JucePlugin_VersionString="${SA_VERSION}")
    target_link_libraries(AlignProcessorTests PRIVATE
        juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
    add_test(NAME processor COMMAND AlignProcessorTests)
endif()

# ---- Tools --------------------------------------------------------------------
option(SA_BUILD_TOOLS "Build the UI snapshot tool" OFF)
if(SA_BUILD_TOOLS)
    juce_add_console_app(AlignSnapshot PRODUCT_NAME "AlignSnapshot")
    target_sources(AlignSnapshot PRIVATE tools/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(AlignSnapshot PRIVATE Source)
    target_compile_definitions(AlignSnapshot PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="SumAlign" JucePlugin_VersionString="${SA_VERSION}")
    target_link_libraries(AlignSnapshot PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
endif()
