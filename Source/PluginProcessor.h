#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "Engine.h"

class TrackEQAudioProcessor  : public juce::AudioProcessor
{
public:
    TrackEQAudioProcessor();
    ~TrackEQAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::String bandId (int band, const char* name) { return "b" + juce::String (band + 1) + "_" + name; }

    juce::AudioProcessorValueTreeState apvts;
    trackeq::Engine engine;
    std::atomic<double> currentSampleRate { 48000.0 };
    std::atomic<int> selectedBand { 0 };   // 画面で選んでいるバンド (保存しない)

private:
    struct BandPtrs { std::atomic<float> *on, *listen, *freq, *track, *q, *thresh, *ratio, *gain, *attack, *release; };
    std::array<BandPtrs, trackeq::numBands> bp {};
    std::atomic<float>* pRel = nullptr;
    std::atomic<float>* pBypass = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackEQAudioProcessor)
};
