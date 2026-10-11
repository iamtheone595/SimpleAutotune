cmake_minimum_required(VERSION 3.22)

# Build for both Apple Silicon and Intel so it loads whichever way Ableton runs
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(SimpleAutotune VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(SimpleAutotune
    COMPANY_NAME "DIY"
    PLUGIN_MANUFACTURER_CODE Diyx
    PLUGIN_CODE Satn
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Simple Autotune"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Simple Autotune needs audio input to process your voice."
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(SimpleAutotune PRIVATE Source/Autotune.cpp)

target_compile_definitions(SimpleAutotune PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SimpleAutotune PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
