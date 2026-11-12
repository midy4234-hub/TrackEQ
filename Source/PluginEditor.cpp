#include "PluginEditor.h"

using trackeq::numBands;
using trackeq::bandColour;

namespace
{
    juce::String hz (float v) { return v >= 1000.0f ? juce::String::formatted ("%.2f kHz", (double) v / 1000.0) : juce::String::formatted ("%.0f Hz", (double) v); }

    // SVF ベルの振幅特性 (dB)。カットもブーストも同じ式
    float bellDb (float f, float fc, float q, float gainDb)
    {
        if (std::abs (gainDb) < 0.01f) return 0.0f;
        const double A = std::pow (10.0, gainDb / 40.0), k = 1.0 / (q * A), w = f / fc;
        const double re = 1.0 - w * w, n = re * re + (w * k * A * A) * (w * k * A * A), d = re * re + (w * k) * (w * k);
        return (float) (10.0 * std::log10 ((n + 1.0e-30) / (d + 1.0e-30)));
    }
}

//==================================================================== GraphView
void GraphView::update()
{
    const int w = getWidth();
    if (w <= 0) return;
    if ((int) shown.size() != w) shown.assign ((size_t) w, dbMin);
    auto& e = proc.engine;
    const int size = e.spectrumSize();
    const double binHz = e.binWidth();
    if (size >= 4)
        for (int x = 0; x < w; ++x)
        {
            const double f0 = fOf ((float) x - 0.5f), f1 = fOf ((float) x + 0.5f);
            const int k0 = juce::jlimit (1, size - 1, (int) std::floor (f0 / binHz)), k1 = juce::jlimit (k0, size - 1, (int) std::ceil (f1 / binHz));
            float a = 0.0f;
            if (k1 - k0 <= 1)
            {
                const double pos = juce::jlimit (1.0, (double) size - 2.0, 0.5 * (f0 + f1) / binHz);
                const int k = (int) pos;
                const float t = (float) (pos - k);
                a = e.spectrumAmp (k) * (1.0f - t) + e.spectrumAmp (k + 1) * t;
            }
            else
                for (int k = k0; k <= k1; ++k) a = juce::jmax (a, e.spectrumAmp (k));
            shown[(size_t) x] = juce::jmax (juce::Decibels::gainToDecibels (a, dbMin), shown[(size_t) x] - 2.0f);
        }
    repaint();
}

GraphView::Hit GraphView::hitTest2 (juce::Point<float> pos) const
{
    const int sel = proc.selectedBand.load();
    // 点 (選択中 → オン → オフ の順に優先)
    Hit best;
    float bestScore = 1.0e9f;
    for (int b = 0; b < numBands; ++b)
    {
        const float d = pos.getDistanceFrom ({ xOf (param (b, "freq")), yGain (param (b, "gain")) });
        if (d > 11.0f) continue;
        const float score = d + (b == sel ? 0.0f : (isOn (b) ? 3.0f : 6.0f));
        if (score < bestScore) { bestScore = score; best = { node, b }; }
    }
    if (best.part != none) return best;
    if (pos.y > (float) plotH()) return {};

    // 範囲の端と、しきい値の線 (オンのバンドだけ)
    for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < numBands; ++b)
        {
            if ((pass == 0) != (b == sel) || ! isOn (b)) continue;
            const float fc = param (b, "freq"), tr = param (b, "track");
            const float x0 = xOf (fc * std::pow (2.0f, -tr / 12.0f)), x1 = xOf (fc * std::pow (2.0f, tr / 12.0f));
            if (tr > 0.0f && std::abs (pos.x - x0) < 5.0f) return { edgeLo, b };
            if (tr > 0.0f && std::abs (pos.x - x1) < 5.0f) return { edgeHi, b };
            if (! relative() && std::abs (pos.y - ySpec (param (b, "thresh"))) < 5.0f
                && pos.x > juce::jmin (x0, xOf (fc) - 12.0f) && pos.x < juce::jmax (x1, xOf (fc) + 12.0f))
                return { thresh, b };
        }
    return {};
}

int GraphView::bandNear (juce::Point<float> pos) const
{
    if (auto h = hitTest2 (pos); h.band >= 0) return h.band;
    // 範囲の中にマウスがあるバンド (中心が近いもの)
    int best = -1;
    float bestD = 1.0e9f;
    for (int b = 0; b < numBands; ++b)
    {
        if (! isOn (b)) continue;
        const float fc = param (b, "freq"), tr = juce::jmax (0.5f, param (b, "track"));
        const float x0 = xOf (fc * std::pow (2.0f, -tr / 12.0f)), x1 = xOf (fc * std::pow (2.0f, tr / 12.0f));
        const float d = std::abs (pos.x - xOf (fc));
        if (pos.x >= x0 && pos.x <= x1 && d < bestD) { bestD = d; best = b; }
    }
    return best >= 0 ? best : proc.selectedBand.load();
}

void GraphView::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitTest2 (e.position);
    setMouseCursor (h.part == node ? juce::MouseCursor::PointingHandCursor
                    : (h.part == edgeLo || h.part == edgeHi) ? juce::MouseCursor::LeftRightResizeCursor
                    : h.part == thresh ? juce::MouseCursor::UpDownResizeCursor
                    : juce::MouseCursor::NormalCursor);
}

void GraphView::mouseDown (const juce::MouseEvent& e)
{
    drag = hitTest2 (e.position);
    if (drag.band < 0) return;
    select (drag.band);
    const int b = drag.band;
    if (drag.part == node)
    {
        dragOffset = juce::Point<float> (xOf (param (b, "freq")), yGain (param (b, "gain"))) - e.position;
        rap (b, "freq")->beginChangeGesture();
        rap (b, "gain")->beginChangeGesture();
    }
    else if (drag.part == thresh)
    {
        dragOffset = { 0.0f, ySpec (param (b, "thresh")) - e.position.y };
        rap (b, "thresh")->beginChangeGesture();
    }
    else
        rap (b, "track")->beginChangeGesture();
}

void GraphView::mouseDrag (const juce::MouseEvent& e)
{
    const int b = drag.band;
    if (b < 0) return;
    const auto pos = e.position + dragOffset;
    if (drag.part == node)
    {
        set (b, "freq", juce::jlimit (40.0f, 16000.0f, fOf (pos.x)));
        set (b, "gain", juce::jlimit (-30.0f, 30.0f, gainOf (pos.y)));
    }
    else if (drag.part == thresh)
        set (b, "thresh", juce::jlimit (-60.0f, 0.0f, specOf (pos.y)));
    else
    {
        const float st = 12.0f * std::abs (std::log2 (fOf (e.position.x) / param (b, "freq")));
        set (b, "track", juce::jlimit (0.0f, 12.0f, st < 0.15f ? 0.0f : st));
    }
}

void GraphView::mouseUp (const juce::MouseEvent&)
{
    const int b = drag.band;
    if (b >= 0)
    {
        if (drag.part == node) { rap (b, "freq")->endChangeGesture(); rap (b, "gain")->endChangeGesture(); }
        else if (drag.part == thresh) rap (b, "thresh")->endChangeGesture();
        else rap (b, "track")->endChangeGesture();
    }
    drag = {};
}

void GraphView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitTest2 (e.position);
    if (h.part == node)
    {
        setOnce (h.band, "on", isOn (h.band) ? 0.0f : 1.0f);
        select (h.band);
        return;
    }
    if (h.part != none || e.position.y > (float) plotH()) return;
    // 空いた所: オフのバンドを 1 つそこへ
    for (int b = 0; b < numBands; ++b)
        if (! isOn (b))
        {
            setOnce (b, "freq", juce::jlimit (40.0f, 16000.0f, fOf (e.position.x)));
            setOnce (b, "gain", juce::jlimit (-30.0f, 30.0f, gainOf (e.position.y)));
            setOnce (b, "on", 1.0f);
            select (b);
            return;
        }
}

void GraphView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int b = bandNear (e.position);
    if (b < 0 || wheel.deltaY == 0.0f) return;
    const float dy = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    setOnce (b, "q", juce::jlimit (1.0f, 40.0f, param (b, "q") * std::exp (dy * 1.5f)));
    select (b);
}

void GraphView::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    const int b = bandNear (e.position);
    if (b < 0) return;
    float st = juce::jmax (0.3f, param (b, "track")) * scale;
    if (st < 0.3f) st = 0.0f;
    setOnce (b, "track", juce::jlimit (0.0f, 12.0f, st));
    select (b);
}

void GraphView::paint (juce::Graphics& g)
{
    lab::drawLcd (g, getLocalBounds());
    const float w = (float) getWidth(), ph = (float) plotH();
    const bool rel = relative();
    const int sel = proc.selectedBand.load();

    // 目盛り: 縦 = 周波数、横 = ゲイン (左の数字)
    g.setFont (juce::FontOptions (9.0f));
    g.setColour (lab::col::lcdGrid);
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        g.drawVerticalLine (juce::roundToInt (xOf (f)), 0.0f, ph);
    for (float db : { -30.0f, -20.0f, -10.0f, 0.0f, 10.0f, 20.0f, 30.0f })
    {
        g.setColour (db == 0.0f ? lab::col::track : lab::col::lcdGrid);
        g.drawHorizontalLine (juce::roundToInt (yGain (db)), 0.0f, w);
        g.setColour (lab::col::dim.withAlpha (0.8f));
        g.drawText (db > 0.0f ? "+" + juce::String ((int) db) : juce::String ((int) db), 4, juce::roundToInt (yGain (db)) - 11, 30, 10, juce::Justification::left);
    }
    g.setColour (lab::col::dim.withAlpha (0.8f));
    g.drawText ("dB", 4, juce::roundToInt (yGain (0.0f)) + 2, 30, 10, juce::Justification::left);

    // スペクトル (目盛りは別: 上端 0 dBFS 〜 下端 -90 dBFS)
    if ((int) shown.size() == getWidth())
    {
        juce::Path p;
        p.startNewSubPath (0.0f, ph);
        for (int x = 0; x < getWidth(); ++x)
            p.lineTo ((float) x, juce::jmin (ph, ySpec (shown[(size_t) x])));
        p.lineTo (w, ph);
        p.closeSubPath();
        g.setColour (juce::Colour (0xff8a8a8a).withAlpha (0.30f));
        g.fillPath (p);
    }

    float fr[numBands], cur[numBands], q[numBands];
    bool act[numBands];
    for (int b = 0; b < numBands; ++b)
    {
        act[b] = isOn (b);
        fr[b] = proc.engine.trackedFreq (b);
        cur[b] = proc.engine.gainDb (b);
        q[b] = param (b, "q");
    }

    // バンドの範囲・しきい値・いちばん動いたときの形
    for (int b = 0; b < numBands; ++b)
    {
        if (! act[b]) continue;
        const float fc = param (b, "freq"), tr = param (b, "track"), gn = param (b, "gain");
        const auto c = bandColour (b);
        const bool isSel = b == sel;
        const float x0 = xOf (fc * std::pow (2.0f, -tr / 12.0f)), x1 = xOf (fc * std::pow (2.0f, tr / 12.0f));
        if (tr > 0.0f)
        {
            g.setColour (c.withAlpha (isSel ? 0.12f : 0.06f));
            g.fillRect (juce::Rectangle<float> (x0, 0.0f, x1 - x0, ph));
            g.setColour (c.withAlpha (isSel ? 0.7f : 0.3f));
            g.drawVerticalLine (juce::roundToInt (x0), 0.0f, ph);
            g.drawVerticalLine (juce::roundToInt (x1), 0.0f, ph);
            if (isSel)   // 端のつまみ
                for (float x : { x0, x1 })
                    g.fillRoundedRectangle (x - 2.0f, ph * 0.5f - 9.0f, 4.0f, 18.0f, 2.0f);
        }
        if (! rel)
        {
            const float y = ySpec (param (b, "thresh"));
            const float xa = juce::jmin (x0, xOf (fc) - 12.0f), xb = juce::jmax (x1, xOf (fc) + 12.0f);
            g.setColour (c.withAlpha (isSel ? 0.95f : 0.5f));
            const float dash[] = { 4.0f, 3.0f };
            g.drawDashedLine ({ xa, y, xb, y }, dash, 2, 1.0f);
            if (isSel)
            {
                g.setFont (juce::FontOptions (9.0f));
                g.drawText (juce::String::formatted ("THR %.1f", (double) param (b, "thresh")), juce::roundToInt (xb) + 4, juce::roundToInt (y) - 5, 60, 10, juce::Justification::left);
            }
        }
        // Gain いっぱいまで動いたときのベル (今追っている位置に)
        juce::Path p;
        const float y0 = yGain (0.0f);
        p.startNewSubPath (0.0f, y0);
        for (int x = 0; x < getWidth(); x += 2)
            p.lineTo ((float) x, yGain (bellDb (fOf ((float) x), fr[b], q[b], gn)));
        p.lineTo (w, y0);
        p.closeSubPath();
        g.setColour (c.withAlpha (isSel ? 0.22f : 0.12f));
        g.fillPath (p);
        g.setColour (c.withAlpha (0.9f));
        g.drawVerticalLine (juce::roundToInt (xOf (fr[b])), juce::jmin (y0, yGain (cur[b])), juce::jmax (y0, yGain (cur[b])));
    }

    // 今かかっているカーブ
    {
        juce::Path p;
        for (int x = 0; x < getWidth(); ++x)
        {
            const float f = fOf ((float) x);
            float db = 0.0f;
            for (int b = 0; b < numBands; ++b)
                if (act[b]) db += bellDb (f, fr[b], q[b], cur[b]);
            const float y = yGain (juce::jlimit (-gainSpan, gainSpan, db));
            if (x == 0) p.startNewSubPath (0.0f, y); else p.lineTo ((float) x, y);
        }
        g.setColour (lab::col::accent);
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

    // 点
    for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < numBands; ++b)
        {
            if ((pass == 1) != (b == sel)) continue;
            const juce::Point<float> c (xOf (param (b, "freq")), yGain (param (b, "gain")));
            const auto col = bandColour (b);
            const float r = b == sel ? 9.0f : 8.0f;
            if (act[b])
            {
                g.setColour (col);
                g.fillEllipse (c.x - r, c.y - r, 2.0f * r, 2.0f * r);
                if (b == sel)
                {
                    g.setColour (juce::Colours::white);
                    g.drawEllipse (c.x - r - 2.0f, c.y - r - 2.0f, 2.0f * r + 4.0f, 2.0f * r + 4.0f, 1.2f);
                }
                g.setColour (lab::col::onText);
            }
            else
            {
                g.setColour (lab::col::lcd);
                g.fillEllipse (c.x - r, c.y - r, 2.0f * r, 2.0f * r);
                g.setColour (b == sel ? col.withAlpha (0.8f) : lab::col::track);
                g.drawEllipse (c.x - r, c.y - r, 2.0f * r, 2.0f * r, 1.2f);
                g.setColour (b == sel ? col.withAlpha (0.8f) : lab::col::dim);
            }
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (c.x - r, c.y - r, 2.0f * r, 2.0f * r), juce::Justification::centred);
        }

    // 下端の周波数
    g.setColour (lab::col::lcdGrid);
    g.drawHorizontalLine (plotH(), 0.0f, w);
    g.setFont (juce::FontOptions (9.0f));
    g.setColour (lab::col::dim.withAlpha (0.8f));
    for (auto [f, t] : { std::pair<float, const char*> { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 500.0f, "500" }, { 1000.0f, "1k" },
                         { 2000.0f, "2k" }, { 5000.0f, "5k" }, { 10000.0f, "10k" } })
        g.drawText (t, juce::roundToInt (xOf (f)) - 20, plotH() + 3, 40, 10, juce::Justification::centred);
}

//==================================================================== Editor
TrackEQAudioProcessorEditor::TrackEQAudioProcessorEditor (TrackEQAudioProcessor& p)
    : AudioProcessorEditor (&p), graph (p), proc (p)
{
    setLookAndFeel (&look);
    addAndMakeVisible (graph);
    graph.onSelect = [this] (int b) { bind (b); };

    for (int b = 0; b < numBands; ++b)
    {
        auto& t = tabs[(size_t) b];
        t.setButtonText (juce::String (b + 1));
        t.onClick = [this, b] { proc.selectedBand = b; bind (b); };
        addAndMakeVisible (t);
    }
    auto id = [] (const char* n) { return TrackEQAudioProcessor::bandId (0, n); };
    on.setUp (*this, proc.apvts, id ("on"), "On");
    listen.setUp (*this, proc.apvts, id ("listen"), "Listen");
    freq.setUp (*this, proc.apvts, id ("freq"));
    gain.setUp (*this, proc.apvts, id ("gain"), 300);
    q.setUp (*this, proc.apvts, id ("q"), 300);
    track.setUp (*this, proc.apvts, id ("track"), 240);
    thresh.setUp (*this, proc.apvts, id ("thresh"), "Thresh");
    ratio.setUp (*this, proc.apvts, id ("ratio"), "Ratio");
    attack.setUp (*this, proc.apvts, id ("attack"), "Attack");
    release.setUp (*this, proc.apvts, id ("release"), "Release");
    relative.setUp (*this, proc.apvts, "relative", "Relative");
    bypass.setUp (*this, proc.apvts, "bypass", "Bypass");

    bind (juce::jlimit (0, numBands - 1, proc.selectedBand.load()));
    setSize (800, 540);
    startTimerHz (30);
}

TrackEQAudioProcessorEditor::~TrackEQAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

// 下の欄を、選んだバンドのパラメータにつなぎ直す
void TrackEQAudioProcessorEditor::bind (int b)
{
    if (b == current) return;
    current = b;
    auto id = [b] (const char* n) { return TrackEQAudioProcessor::bandId (b, n); };
    auto slider = [&] (juce::Slider& s, std::unique_ptr<lab::SliderAttach>& a, const char* n)
    {
        a.reset();
        a = std::make_unique<lab::SliderAttach> (proc.apvts, id (n), s);
        lab::resetOnDoubleClick (s, proc.apvts, id (n));
        s.repaint();
    };
    auto button = [&] (lab::Toggle& t, const char* n)
    {
        t.attach.reset();
        t.attach = std::make_unique<lab::ButtonAttach> (proc.apvts, id (n), t.button);
    };
    slider (freq.slider, freq.attach, "freq");
    slider (gain.slider, gain.attach, "gain");
    slider (q.slider, q.attach, "q");
    slider (track.slider, track.attach, "track");
    slider (thresh.slider, thresh.attach, "thresh");
    slider (ratio.slider, ratio.attach, "ratio");
    slider (attack.slider, attack.attach, "attack");
    slider (release.slider, release.attach, "release");
    button (on, "on");
    button (listen, "listen");
    for (int i = 0; i < numBands; ++i)
        tabs[(size_t) i].setToggleState (i == b, juce::dontSendNotification);
    shownGain = 0.0f;
    repaint();
}

void TrackEQAudioProcessorEditor::timerCallback()
{
    graph.update();
    const float gdb = proc.engine.gainDb (current);
    shownGain = std::abs (gdb) >= std::abs (shownGain) ? gdb : shownGain * 0.85f;   // 戻りはゆっくり見せる
    shownHz = proc.engine.trackedFreq (current);
    repaint (meter);
    repaint (tabArea.expanded (0, 4));
}

void TrackEQAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (lab::col::bg);
    lab::drawTitle (g, "TrackEQ");
    lab::drawPanel (g, panel, {});
    g.setColour (bandColour (current));
    g.fillRect (panel.getX() + 1, panel.getY() + 1, 3, panel.getHeight() - 2);

    // タブの下に色の線 (オンのバンドは明るく)
    for (int b = 0; b < numBands; ++b)
    {
        const auto r = tabs[(size_t) b].getBounds();
        const bool isOn = proc.apvts.getRawParameterValue (TrackEQAudioProcessor::bandId (b, "on"))->load() > 0.5f;
        g.setColour (bandColour (b).withAlpha (isOn ? 1.0f : 0.25f));
        g.fillRect (r.getX() + 1, r.getBottom() + 2, r.getWidth() - 2, 2);
    }

    g.setFont (juce::FontOptions (10.5f));
    g.setColour (lab::col::dim);
    const char* names[4] = { "Freq", "Gain", "Q", "Track" };
    for (int i = 0; i < 4; ++i)
        g.drawText (names[i], numLabel[i], juce::Justification::centredLeft);

    const bool live = on.button.getToggleState() || listen.button.getToggleState();
    auto m = meter;
    g.setColour (lab::col::dim);
    g.drawText ("NOW", m.removeFromTop (14), juce::Justification::centredLeft);
    g.setColour (lab::col::text);
    g.drawText (live ? juce::String::formatted ("%+.1f dB", (double) shownGain) : juce::String ("-"), meter.withHeight (14), juce::Justification::centredRight);
    // 中央が 0。左へ = 下げている、右へ = 持ち上げている
    auto bar = m.removeFromTop (8).toFloat();
    g.setColour (lab::col::lcdGrid);
    g.fillRect (bar);
    const float half = bar.getWidth() * 0.5f, len = half * juce::jlimit (0.0f, 1.0f, std::abs (shownGain) / 30.0f);
    g.setColour (lab::col::accent);
    g.fillRect (shownGain < 0.0f ? bar.withX (bar.getCentreX() - len).withWidth (len) : bar.withX (bar.getCentreX()).withWidth (len));
    g.setColour (lab::col::dim);
    g.fillRect (bar.getCentreX() - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
    m.removeFromTop (8);
    g.drawText ("AT", m.withHeight (14), juce::Justification::centredLeft);
    g.setColour (live ? bandColour (current) : lab::col::dim);
    g.drawText (live ? hz (shownHz) : juce::String ("-"), m.withHeight (14), juce::Justification::centredRight);
}

void TrackEQAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16, 0);
    auto head = area.removeFromTop (34);
    bypass.button.setBounds (head.removeFromRight (64).withSizeKeepingCentre (64, 18));
    head.removeFromRight (8);
    relative.button.setBounds (head.removeFromRight (72).withSizeKeepingCentre (72, 18));
    area.removeFromBottom (12);

    panel = area.removeFromBottom (100);
    area.removeFromBottom (8);
    graph.setBounds (area);

    auto in = panel.reduced (14, 10);
    // 左: バンドのタブと On / Listen
    auto left = in.removeFromLeft (150);
    tabArea = left.removeFromTop (22);
    for (int b = 0; b < numBands; ++b)
        tabs[(size_t) b].setBounds (tabArea.getX() + b * 30, tabArea.getY(), 28, 22);
    left.removeFromTop (16);
    auto btn = left.removeFromTop (22);
    on.button.setBounds (btn.removeFromLeft (58));
    btn.removeFromLeft (6);
    listen.button.setBounds (btn.removeFromLeft (84));
    in.removeFromLeft (18);

    // 数字 (2 × 2)
    auto nums = in.removeFromLeft (190);
    lab::NumberDrag* nd[4] = { &freq, &gain, &q, &track };
    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<int> (nums.getX() + (i % 2) * 98, nums.getY() + (i / 2) * 42, 92, 38);
        numLabel[i] = cell.removeFromTop (14);
        nd[i]->slider.setBounds (cell.withHeight (22));
    }
    in.removeFromLeft (10);

    meter = in.removeFromRight (112).withSizeKeepingCentre (112, 50);
    in.removeFromRight (14);
    const int kw = in.getWidth() / 4;
    thresh.setBounds (in.removeFromLeft (kw));
    ratio.setBounds (in.removeFromLeft (kw));
    attack.setBounds (in.removeFromLeft (kw));
    release.setBounds (in.removeFromLeft (kw));
}
