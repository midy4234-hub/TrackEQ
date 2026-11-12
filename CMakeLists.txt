cmake_minimum_required(VERSION 3.22)

# この 2 つは project() より前に書かないと効かない
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures")

project(TrackEQ VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PluginLab (共通の見た目 LabLook.h と検証ハーネス LabTest.h)
set(LAB_DIR "/Users/midy9969nect/Desktop/Claude Code/PluginLab" CACHE PATH "PluginLab")
set(LAB_COMMON "${LAB_DIR}/juce/common")

# 共有の JUCE (../_deps/JUCE) があればそれを使う。無ければ取得する
set(_juce_shared "${CMAKE_CURRENT_SOURCE_DIR}/../_deps/JUCE")
if(EXISTS "${_juce_shared}/modules/juce_core/juce_core.h")
    add_subdirectory("${_juce_shared}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# 検証用ビルドなどでインストールしたくないときは -DLAB_INSTALL=OFF
option(LAB_INSTALL "Copy the plugin to ~/Library/Audio/Plug-Ins after build" ON)

juce_add_plugin(TrackEQ
    PRODUCT_NAME "TrackEQ"
    COMPANY_NAME "MIDy"
    BUNDLE_ID "com.midy.trackeq"
    PLUGIN_MANUFACTURER_CODE "MIDy"
    PLUGIN_CODE "Trck"
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES "Fx"
    COPY_PLUGIN_AFTER_BUILD ${LAB_INSTALL})

target_sources(TrackEQ PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_include_directories(TrackEQ PRIVATE "${LAB_COMMON}")

target_compile_definitions(TrackEQ PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(TrackEQ
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)

# 検証用オフラインドライバ (lab.py test / render / snapshot が使う)
option(LAB_BUILD_TESTS "Build the offline test driver" OFF)
if(LAB_BUILD_TESTS)
    juce_add_console_app(TrackEQTest PRODUCT_NAME "TrackEQTest")
    target_sources(TrackEQTest PRIVATE tests/TestMain.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(TrackEQTest PRIVATE "${LAB_COMMON}")
    target_compile_definitions(TrackEQTest PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="TrackEQ")
    target_link_libraries(TrackEQTest
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
