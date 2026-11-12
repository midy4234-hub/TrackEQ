#pragma once

// PluginLab 共通の見た目 (ChordRes / Transist4 と同じ「Ableton 純正寄り」)
//
//   lab::Look          … LookAndFeel。つまみ・ボタン・スライダーの文字
//   lab::Knob          … ラベル (上) + つまみ + 値 (下) の組。setUp() で APVTS につなぐ
//   lab::NumberDrag    … 数字そのものを上下ドラッグする入力 (クロスオーバーの周波数など)
//   lab::Toggle        … オン/オフのボタン (APVTS の bool)
//   lab::Switch        … 択一ボタン列 (APVTS の choice / int)
//   lab::drawPanel()   … タイトル付きのパネル
//   lab::drawMeterV/H  … ピークメーター (dB)。0 dBFS 超は赤
//
// つまみのプロパティ: "bipolar" = true で 0 から弧を描く (±のあるパラメータ)
// UI の文字は ASCII だけにする (JUCE 既定の書体に日本語が無く文字化けする)

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

namespace lab
{
    namespace col
    {
        const juce::Colour bg          { 0xff2e2e2e };
        const juce::Colour panel       { 0xff363636 };
        const juce::Colour panelStroke { 0xff404040 };
        const juce::Colour text        { 0xffd2d2d2 };
        const juce::Colour dim         { 0xff8c8c8c };
        const juce::Colour lcd         { 0xff1f1f1f };
        const juce::Colour lcdGrid     { 0xff333333 };
        const juce::Colour blue        { 0xff6ec8ff };   // 波形・表示のデータ
        const juce::Colour accent      { 0xffffa400 };   // 値の弧・オンのボタン
        const juce::Colour meter       { 0xff8ee06b };
        const juce::Colour over        { 0xffff5a5a };
        const juce::Colour track       { 0xff555555 };
        const juce::Colour switchOff   { 0xff2a2a2a };
        const juce::Colour onText      { 0xff1e1e1e };
    }

    class Look  : public juce::LookAndFeel_V4
    {
    public:
        Look()
        {
            setColour (juce::Slider::textBoxTextColourId, col::text);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxHighlightColourId, col::accent.withAlpha (0.4f));
            setColour (juce::Label::textColourId, col::text);
            setColour (juce::TextEditor::backgroundColourId, col::lcd);
            setColour (juce::TextEditor::textColourId, col::text);
            setColour (juce::TextEditor::focusedOutlineColourId, col::accent);
            setColour (juce::CaretComponent::caretColourId, col::accent);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider& s) override
        {
            const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat();

            // 数字だけの入力 (NumberDrag)
            if (s.getProperties()["kind"].toString() == "number")
            {
                const bool active = s.isMouseOverOrDragging();
                g.setColour (active ? col::switchOff.brighter (0.15f) : col::switchOff);
                g.fillRoundedRectangle (bounds.reduced (0.5f), 2.0f);
                g.setColour (active ? col::accent : col::panelStroke.brighter (0.1f));
                g.drawRoundedRectangle (bounds.reduced (0.5f), 2.0f, 1.0f);
                g.setColour (col::text);
                g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
                auto text = s.getProperties()["display"].toString();
                if (text.isEmpty())
                    text = s.getTextFromValue (s.getValue());
                g.drawText (text, bounds, juce::Justification::centred);
                return;
            }

            const float r = juce::jmin (16.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 3.0f);
            const auto c = bounds.getCentre();
            const float angle = startAngle + pos * (endAngle - startAngle);

            float origin = startAngle;
            if ((bool) s.getProperties()["bipolar"])
                origin = startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle);

            juce::Path trackArc;
            trackArc.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, endAngle, true);
            g.setColour (col::track);
            g.strokePath (trackArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

            if (std::abs (angle - origin) > 0.001f)
            {
                juce::Path valueArc;
                valueArc.addCentredArc (c.x, c.y, r, r, 0.0f, juce::jmin (origin, angle), juce::jmax (origin, angle), true);
                g.setColour (col::accent);
                g.strokePath (valueArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            }

            const juce::Point<float> tip (c.x + r * std::sin (angle), c.y - r * std::cos (angle));
            g.setColour (col::text);
            g.drawLine ({ c, tip }, 1.5f);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                   bool hover, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            const bool on = b.getToggleState();
            auto fill = on ? col::accent : col::switchOff;
            if (hover && ! on) fill = fill.brighter (0.15f);
            if (down) fill = fill.darker (0.1f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, 2.0f);
            if (! on)
            {
                g.setColour (col::panelStroke.brighter (0.1f));
                g.drawRoundedRectangle (r, 2.0f, 1.0f);
            }
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool hover, bool) override
        {
            const bool on = b.getToggleState();
            g.setColour (on ? col::onText : (hover ? col::text : col::dim));
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
        }

        juce::Font getLabelFont (juce::Label&) override { return juce::FontOptions (10.5f); }

        juce::Label* createSliderTextBox (juce::Slider& s) override
        {
            auto* l = LookAndFeel_V4::createSliderTextBox (s);
            l->setFont (juce::FontOptions (10.5f));
            l->setColour (juce::Label::textColourId, col::text);
            l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
            return l;
        }
    };

    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;

    inline void resetOnDoubleClick (juce::Slider& s, juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        if (auto* param = apvts.getParameter (id))
            s.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
    }

    // ラベル (上) + つまみ + 値 (下)
    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttach> attach;

        void setUp (juce::Component& parent, juce::AudioProcessorValueTreeState& apvts,
                    const juce::String& paramId, const juce::String& title, bool bipolar = false)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 14);
            slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            slider.setColour (juce::Slider::textBoxTextColourId, col::text);
            slider.getProperties().set ("bipolar", bipolar);
            parent.addAndMakeVisible (slider);

            label.setText (title, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setColour (juce::Label::textColourId, col::dim);
            parent.addAndMakeVisible (label);

            attach = std::make_unique<SliderAttach> (apvts, paramId, slider);
            resetOnDoubleClick (slider, apvts, paramId);
        }

        // cell の上 14px にラベル、残り (最大 78px) につまみと値
        void setBounds (juce::Rectangle<int> cell)
        {
            cell = cell.withSizeKeepingCentre (cell.getWidth(), juce::jmin (cell.getHeight(), 92));
            label.setBounds (cell.removeFromTop (14));
            slider.setBounds (cell.reduced (2, 0));
        }
    };

    // 数字を上下ドラッグで動かす入力。pixelsForFullRange で感度 (既定 480px で全域)
    struct NumberDrag
    {
        juce::Slider slider;
        std::unique_ptr<SliderAttach> attach;

        void setUp (juce::Component& parent, juce::AudioProcessorValueTreeState& apvts,
                    const juce::String& paramId, int pixelsForFullRange = 480)
        {
            slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setMouseDragSensitivity (pixelsForFullRange);
            slider.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            slider.getProperties().set ("kind", "number");
            parent.addAndMakeVisible (slider);
            attach = std::make_unique<SliderAttach> (apvts, paramId, slider);
            resetOnDoubleClick (slider, apvts, paramId);
        }

        // 表示文字を差し替えたいとき (実効値など)。空ならパラメータの文字
        void setDisplay (const juce::String& text)
        {
            if (slider.getProperties()["display"].toString() != text)
            {
                slider.getProperties().set ("display", text);
                slider.repaint();
            }
        }
    };

    struct Toggle
    {
        juce::TextButton button;
        std::unique_ptr<ButtonAttach> attach;

        void setUp (juce::Component& parent, juce::AudioProcessorValueTreeState& apvts,
                    const juce::String& paramId, const juce::String& text)
        {
            button.setButtonText (text);
            button.setClickingTogglesState (true);
            parent.addAndMakeVisible (button);
            attach = std::make_unique<ButtonAttach> (apvts, paramId, button);
        }
    };

    // 択一ボタン列。items = { {"表示", 値}, ... }
    struct Switch
    {
        std::vector<std::unique_ptr<juce::TextButton>> buttons;
        std::vector<int> values;
        std::unique_ptr<juce::ParameterAttachment> attach;

        void setUp (juce::Component& parent, juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                    std::initializer_list<std::pair<const char*, int>> items)
        {
            for (auto& [text, value] : items)
            {
                auto b = std::make_unique<juce::TextButton> (text);
                const int v = value;
                b->onClick = [this, v] { attach->setValueAsCompleteGesture ((float) v); };
                parent.addAndMakeVisible (*b);
                buttons.push_back (std::move (b));
                values.push_back (value);
            }
            attach = std::make_unique<juce::ParameterAttachment> (*apvts.getParameter (paramId),
                [this] (float v)
                {
                    for (size_t i = 0; i < buttons.size(); ++i)
                        buttons[i]->setToggleState (juce::roundToInt (v) == values[i], juce::dontSendNotification);
                });
            attach->sendInitialUpdate();
        }

        void setBounds (juce::Rectangle<int> r)
        {
            const int n = (int) buttons.size();
            for (int i = 0; i < n; ++i)
            {
                const int x0 = r.getX() + r.getWidth() * i / n, x1 = r.getX() + r.getWidth() * (i + 1) / n;
                buttons[(size_t) i]->setBounds (x0, r.getY(), x1 - x0, r.getHeight());
            }
        }
    };

    inline void drawTitle (juce::Graphics& g, const juce::String& name, int x = 16)
    {
        g.setColour (col::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (name, x, 6, 300, 24, juce::Justification::centredLeft);
    }

    // タイトル付きパネル。titleInLeftColumn > 0 なら、左端の列に縦中央でタイトル (横帯型のレイアウト用)
    inline void drawPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, int titleInLeftColumn = 0)
    {
        g.setColour (col::panel);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
        g.setColour (col::panelStroke);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        if (titleInLeftColumn > 0)
            g.drawText (title, r.getX() + 10, r.getY(), titleInLeftColumn - 10, r.getHeight(), juce::Justification::centredLeft);
        else
            g.drawText (title, r.getX() + 10, r.getY() + 6, 200, 18, juce::Justification::centredLeft);
    }

    inline void drawLcd (juce::Graphics& g, juce::Rectangle<int> r)
    {
        g.setColour (col::lcd);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
    }

    // ピークメーター。値は dB。上がるときは即座、下がるときは呼ぶ側で減衰させる (fallDb を参照)
    constexpr float meterMinDb = -48.0f, meterMaxDb = 12.0f;

    inline float fallDb (float shownDb, float newPeakLinear, float dbPerFrame = 1.5f)
    {
        return juce::jmax (juce::Decibels::gainToDecibels (newPeakLinear, -100.0f), shownDb - dbPerFrame);
    }

    inline void drawMeterV (juce::Graphics& g, juce::Rectangle<float> bar, float db)
    {
        auto toY = [&] (float d) { return bar.getBottom() - juce::jlimit (0.0f, 1.0f, (d - meterMinDb) / (meterMaxDb - meterMinDb)) * bar.getHeight(); };
        g.setColour (col::lcdGrid);
        g.fillRect (bar);
        const float y = toY (db), zeroY = toY (0.0f);
        if (y < bar.getBottom())
        {
            g.setColour (col::meter);
            g.fillRect (bar.withTop (juce::jmax (y, zeroY)));
            if (y < zeroY)
            {
                g.setColour (col::over);
                g.fillRect (bar.withTop (y).withBottom (zeroY));
            }
        }
    }

    inline void drawMeterH (juce::Graphics& g, juce::Rectangle<float> bar, float db)
    {
        auto toX = [&] (float d) { return bar.getX() + juce::jlimit (0.0f, 1.0f, (d - meterMinDb) / (meterMaxDb - meterMinDb)) * bar.getWidth(); };
        g.setColour (col::lcdGrid);
        g.fillRect (bar);
        const float x = toX (db), zeroX = toX (0.0f);
        if (x > bar.getX())
        {
            g.setColour (col::meter);
            g.fillRect (bar.withRight (juce::jmin (x, zeroX)));
            if (x > zeroX)
            {
                g.setColour (col::over);
                g.fillRect (bar.withLeft (zeroX).withRight (x));
            }
        }
    }
}
