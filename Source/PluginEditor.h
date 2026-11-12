#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include "PluginProcessor.h"
#include "LabLook.h"

namespace trackeq
{
    inline juce::Colour bandColour (int b)
    {
        static const juce::Colour c[numBands] = { juce::Colour (0xffff6b6b), juce::Colour (0xffffc857), juce::Colour (0xff7bd88f),
                                                  juce::Colour (0xff6ec8ff), juce::Colour (0xffc79bff) };
        return c[juce::jlimit (0, numBands - 1, b)];
    }
}

// スペクトルの上に各バンドの点を置いて動かす画面
//   点をドラッグ: 横 = Freq、縦 = Gain        スクロール: Q        ピンチ / 範囲の端をドラッグ: Track
//   しきい値の横線をドラッグ: Threshold      点をダブルクリック: オフ    空いた所をダブルクリック: 空きバンドをそこに置く
class GraphView  : public juce::Component
{
public:
    explicit GraphView (TrackEQAudioProcessor& p) : proc (p) {}

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    void update();   // タイマーから

    std::function<void (int)> onSelect;

    // テスト用にも使う座標
    float xOf (float f) const { return (float) getWidth() * std::log (f / fMin) / std::log (fMax / fMin); }
    float fOf (float x) const { return fMin * std::pow (fMax / fMin, x / (float) getWidth()); }
    float yGain (float db) const { return (float) plotH() * (0.5f - db / (2.0f * gainSpan)); }
    float gainOf (float y) const { return (0.5f - y / (float) plotH()) * 2.0f * gainSpan; }
    float ySpec (float db) const { return (float) plotH() * (1.0f - (juce::jlimit (dbMin, dbMax, db) - dbMin) / (dbMax - dbMin)); }
    float specOf (float y) const { return dbMin + (1.0f - y / (float) plotH()) * (dbMax - dbMin); }

private:
    enum Part { none, node, edgeLo, edgeHi, thresh };
    struct Hit { Part part = none; int band = -1; };

    static constexpr float fMin = 30.0f, fMax = 20000.0f, dbMin = -90.0f, dbMax = 0.0f, gainSpan = 33.0f;
    int plotH() const { return getHeight() - 16; }
    float param (int b, const char* n) const { return proc.apvts.getRawParameterValue (TrackEQAudioProcessor::bandId (b, n))->load(); }
    juce::RangedAudioParameter* rap (int b, const char* n) const { return proc.apvts.getParameter (TrackEQAudioProcessor::bandId (b, n)); }
    void set (int b, const char* n, float v) const { auto* p = rap (b, n); p->setValueNotifyingHost (p->convertTo0to1 (v)); }
    void setOnce (int b, const char* n, float v) const { auto* p = rap (b, n); p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (v)); p->endChangeGesture(); }
    bool isOn (int b) const { return param (b, "on") > 0.5f; }
    bool relative() const { return proc.apvts.getRawParameterValue ("relative")->load() > 0.5f; }
    Hit hitTest2 (juce::Point<float>) const;
    int bandNear (juce::Point<float>) const;   // スクロール・ピンチの相手
    void select (int b) { proc.selectedBand = b; if (onSelect) onSelect (b); }

    TrackEQAudioProcessor& proc;
    std::vector<float> shown;
    Hit drag;
    juce::Point<float> dragOffset;
};

class TrackEQAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit TrackEQAudioProcessorEditor (TrackEQAudioProcessor&);
    ~TrackEQAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    GraphView graph;

private:
    void timerCallback() override;
    void bind (int band);

    TrackEQAudioProcessor& proc;
    lab::Look look;

    int current = -1;
    std::array<juce::TextButton, trackeq::numBands> tabs;
    lab::Toggle on, listen, relative, bypass;
    lab::NumberDrag freq, gain, q, track;
    lab::Knob thresh, ratio, attack, release;
    juce::Rectangle<int> panel, meter, numLabel[4], tabArea;
    float shownGain = 0.0f, shownHz = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackEQAudioProcessorEditor)
};
