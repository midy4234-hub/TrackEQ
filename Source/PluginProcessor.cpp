#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::String gainText (float v, int) { return juce::String::formatted ("%+.1f dB", (double) v); }
    juce::String dbText (float v, int)  { return juce::String::formatted ("%.1f dB", (double) v); }
    juce::String hzText (float v, int)  { return v >= 1000.0f ? juce::String::formatted ("%.2f kHz", (double) v / 1000.0) : juce::String::formatted ("%.0f Hz", (double) v); }
    juce::String stText (float v, int)  { return v <= 0.0f ? juce::String ("Off") : juce::String::formatted ("%.1f st", (double) v); }
    juce::String qText (float v, int)   { return juce::String::formatted ("%.1f", (double) v); }
    juce::String ratioText (float v, int) { return juce::String::formatted ("%.1f : 1", (double) v); }
    juce::String msText (float v, int)  { return v < 10.0f ? juce::String::formatted ("%.1f ms", (double) v) : juce::String::formatted ("%.0f ms", (double) v); }

    juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        return { lo, hi,
                 [] (float a, float b, float t) { return a * std::pow (b / a, t); },
                 [] (float a, float b, float v) { return std::log (v / a) / std::log (b / a); },
                 [] (float a, float b, float v) { return juce::jlimit (a, b, v); } };
    }
}

TrackEQAudioProcessor::TrackEQAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TrackEQ", createLayout())
{
    for (int b = 0; b < trackeq::numBands; ++b)
    {
        auto get = [&] (const char* n) { return apvts.getRawParameterValue (bandId (b, n)); };
        bp[(size_t) b] = { get ("on"), get ("listen"), get ("freq"), get ("track"), get ("q"),
                           get ("thresh"), get ("ratio"), get ("gain"), get ("attack"), get ("release") };
    }
    pRel    = apvts.getRawParameterValue ("relative");
    pBypass = apvts.getRawParameterValue ("bypass");
}

juce::AudioProcessorValueTreeState::ParameterLayout TrackEQAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const float defFreq[trackeq::numBands] = { 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f };

    for (int b = 0; b < trackeq::numBands; ++b)
    {
        const String n = "B" + String (b + 1) + " ";
        auto f = [&] (const char* id, const char* name, NormalisableRange<float> r, float def, String (*text) (float, int))
        {
            layout.add (std::make_unique<AudioParameterFloat> (ParameterID { bandId (b, id), 1 }, n + name, r, def,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (text)));
        };
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { bandId (b, "on"), 1 }, n + "On", false));
        f ("freq",    "Freq",      logRange (40.0f, 16000.0f), defFreq[b], hzText);
        f ("track",   "Track",     NormalisableRange<float> (0.0f, 12.0f, 0.1f), 3.0f, stText);
        f ("q",       "Q",         logRange (1.0f, 40.0f), 8.0f, qText);
        f ("thresh",  "Threshold", NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -24.0f, dbText);
        f ("ratio",   "Ratio",     logRange (1.0f, 20.0f), 4.0f, ratioText);
        f ("gain",    "Gain",      NormalisableRange<float> (-30.0f, 30.0f, 0.1f), -12.0f, gainText);
        f ("attack",  "Attack",    logRange (0.1f, 200.0f), 5.0f, msText);
        f ("release", "Release",   logRange (5.0f, 2000.0f), 100.0f, msText);
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { bandId (b, "listen"), 1 }, n + "Listen", false));
    }
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "relative", 1 }, "Relative", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

bool TrackEQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void TrackEQAudioProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate, juce::jmax (1, getTotalNumInputChannels()));
    setLatencySamples (engine.latency());
}

void TrackEQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numCh = juce::jmin (trackeq::maxChannels, getTotalNumInputChannels());
    for (int ch = numCh; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, n);
    if (numCh < 1 || n < 1) return;

    std::array<trackeq::BandParams, trackeq::numBands> prm;
    for (int b = 0; b < trackeq::numBands; ++b)
    {
        const auto& s = bp[(size_t) b];
        auto& d = prm[(size_t) b];
        d.on = s.on->load() > 0.5f;
        d.listen = s.listen->load() > 0.5f;
        d.freq = s.freq->load();
        d.track = s.track->load();
        d.q = s.q->load();
        d.thresh = s.thresh->load();
        d.ratio = s.ratio->load();
        d.gain = s.gain->load();
        d.attackMs = s.attack->load();
        d.releaseMs = s.release->load();
    }

    float* io[trackeq::maxChannels] = { buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr };
    engine.process (io, n, prm, pRel->load() > 0.5f, pBypass->load() > 0.5f);
}

juce::AudioProcessorEditor* TrackEQAudioProcessor::createEditor()
{
    return new TrackEQAudioProcessorEditor (*this);
}

void TrackEQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void TrackEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TrackEQAudioProcessor();
}
