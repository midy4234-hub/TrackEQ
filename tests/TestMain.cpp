//  TrackEQTest — DAW を開かずに挙動を確かめるオフラインドライバ
//
//   TrackEQTest                            … 下の固有テストを走らせ、UI を renders/ui.png に撮る
//   TrackEQTest render in.wav out.wav k=v  … 試聴用 A/B (LabTest.h の cliMain)
//   TrackEQTest snapshot out.png [in.wav]  … UI の画像
//   TrackEQTest params / robust

#include "LabTest.h"
#include "LabRobust.h"
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <complex>

using Proc = TrackEQAudioProcessor;

namespace
{
    constexpr double sr = 48000.0;
    const double twoPi = juce::MathConstants<double>::twoPi;

    void band (Proc& p, int b, std::initializer_list<std::pair<const char*, float>> kv)
    {
        for (auto& [k, v] : kv)
            if (! lab::setParam (p, Proc::bandId (b, k), v)) std::printf ("!! no param %s\n", k);
    }

    // 入力を latency ぶん遅らせたもの
    juce::AudioBuffer<float> delayed (const juce::AudioBuffer<float>& in, int L)
    {
        juce::AudioBuffer<float> d (in.getNumChannels(), in.getNumSamples());
        d.clear();
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
            d.copyFrom (ch, L, in, ch, 0, in.getNumSamples() - L);
        return d;
    }

    // 音程が動く倍音の音。f0: 200 → 300 Hz (対数で直線) + ビブラート 6 Hz ±30 セント。倍音 loud だけ大きい
    struct Glide
    {
        std::vector<double> phase, f0;
        juce::AudioBuffer<float> audio;
    };
    Glide makeGlide (double seconds, int loud, double loudAmp, double otherAmp, bool vibrato = true)
    {
        const int n = (int) (sr * seconds);
        Glide g;
        g.phase.resize ((size_t) n); g.f0.resize ((size_t) n);
        g.audio.setSize (2, n);
        double ph = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double cents = 702.0 * t / seconds + (vibrato ? 30.0 * std::sin (twoPi * 6.0 * t) : 0.0);
            const double f = 200.0 * std::exp2 (cents / 1200.0);
            g.f0[(size_t) i] = f; g.phase[(size_t) i] = ph;
            double s = 0.0;
            for (int h = 1; h <= 12; ++h)
                s += (h == loud ? loudAmp : otherAmp) * std::sin (h * ph + 0.7 * h);
            g.audio.setSample (0, i, (float) s);
            g.audio.setSample (1, i, (float) s);
            ph += twoPi * f / sr;
        }
        return g;
    }

    // 倍音 h の振幅 (dB) の平均: 位相を知っているので複素の掛け算で取り出し、20 ms の箱で均す
    double harmDb (const juce::AudioBuffer<float>& x, const std::vector<double>& phase, int shift, int h, int from, int to)
    {
        const int win = (int) (0.020 * sr);
        double acc = 0.0; int cnt = 0;
        for (int s = from; s + win <= to; s += win)
        {
            std::complex<double> z (0.0, 0.0);
            for (int i = s; i < s + win; ++i)
                z += (double) x.getSample (0, i) * std::polar (1.0, -h * phase[(size_t) (i - shift)]);
            const double a = 2.0 * std::abs (z) / win;
            acc += a * a; ++cnt;
        }
        return 10.0 * std::log10 (acc / std::max (1, cnt) + 1.0e-30);
    }

    void testDefaults()
    {
        Proc p;
        lab::prepare (p, sr);
        const int L = p.getLatencySamples();
        auto in = lab::noise ((int) sr, 0.25f);
        auto out = lab::run (p, in);
        std::printf ("[1] defaults (all bands off): latency %d, residual vs delayed input %.1f dB\n",
                     L, lab::residualDb (out, delayed (in, L), L, in.getNumSamples() - L));
    }

    void testStatic()
    {
        // 追従なしの固定バンド。1 kHz のサイン波 -6 dBFS、しきい値 -24、Ratio 4 → 18 dB 超過 × 0.75 = 13.5 dB 下がるはず
        Proc p;
        lab::prepare (p, sr);
        band (p, 2, { { "on", 1 }, { "freq", 1000 }, { "track", 0 }, { "thresh", -24 }, { "ratio", 4 }, { "gain", -30 } });
        lab::prepare (p, sr);
        auto in = lab::sine (sr, 1000.0, 2.0, 0.5f);
        auto out = lab::run (p, in);
        const int a = (int) sr, len = (int) (0.9 * sr);
        std::printf ("[2] static band, 1 kHz -6 dBFS, thr -24, 4:1: gain %+.2f dB (expect -13.50)\n",
                     lab::db (lab::rms (out, a, len) / lab::rms (in, a, len)));
        // 持ち上げる向き
        lab::setParam (p, Proc::bandId (2, "gain"), 30.0f);
        lab::prepare (p, sr);
        auto up = lab::run (p, in);
        std::printf ("    same with gain +30: gain %+.2f dB (expect +13.50)\n", lab::db (lab::rms (up, a, len) / lab::rms (in, a, len)));
        lab::setParam (p, Proc::bandId (2, "gain"), -30.0f);
        // しきい値より下なら何もしない
        auto quiet = lab::sine (sr, 1000.0, 2.0, 0.02f);
        lab::prepare (p, sr);
        auto o2 = lab::run (p, quiet);
        const int L = p.getLatencySamples();
        std::printf ("    below threshold (-34 dBFS): residual vs delayed input %.1f dB\n",
                     lab::residualDb (o2, delayed (quiet, L), a, len));
    }

    void testTracking (bool vibrato)
    {
        // 倍音 5 だけ 20 dB 大きい。バンドは 1225 Hz ± 4 半音 (972〜1543 Hz)。倍音 5 は 1000 → 1500 Hz を動く
        auto g = makeGlide (3.0, 5, 0.2, 0.02, vibrato);
        const int n = g.audio.getNumSamples();
        std::printf ("[3] gliding tone 200 -> 300 Hz%s, harmonic 5 at -14 dBFS, others -34\n", vibrato ? " + vibrato 6 Hz +-30 ct" : "");

        for (int mode = 0; mode < 2; ++mode)
        {
            Proc p;
            lab::prepare (p, sr, 256);
            band (p, 2, { { "on", 1 }, { "freq", 1225 }, { "track", mode == 0 ? 4.0f : 0.0f }, { "q", 8 },
                          { "thresh", -30 }, { "ratio", 20 }, { "gain", -30 }, { "attack", 5 }, { "release", 100 } });
            lab::prepare (p, sr, 256);
            const int L = p.getLatencySamples();

            // ブロック 256 で流しながら、追っている周波数を記録
            juce::AudioBuffer<float> out (2, n), buf (2, 256);
            double errSum = 0.0, errMax = 0.0; int errN = 0;
            for (int s = 0; s + 256 <= n; s += 256)
            {
                for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, g.audio, ch, s, 256);
                juce::MidiBuffer midi;
                p.processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, s, buf, ch, 0, 256);
                const int at = s + 255 - L;   // 出ている音の時刻
                if (mode == 0 && at > (int) (0.3 * sr))
                {
                    const double e = std::abs (1200.0 * std::log2 (p.engine.trackedFreq (2) / (5.0 * g.f0[(size_t) at])));
                    errSum += e; errMax = std::max (errMax, e); ++errN;
                }
            }
            const int from = (int) (0.4 * sr), to = n - 512;
            auto din = delayed (g.audio, L);
            std::printf ("    %s:", mode == 0 ? "track +-4 st" : "fixed (track off)");
            for (int h : { 3, 4, 5, 6, 7 })
                std::printf ("  h%d %+.2f", h, harmDb (out, g.phase, L, h, from, to) - harmDb (din, g.phase, L, h, from, to));
            std::printf (" dB\n");
            if (mode == 0)
            {
                std::printf ("    tracked frequency vs 5 x f0: mean %.1f cents, max %.1f cents\n", errSum / std::max (1, errN), errMax);
                lab::writeWav (juce::File::getCurrentWorkingDirectory().getChildFile (vibrato ? "renders/test_glide_vib_out.wav" : "renders/test_glide_out.wav"), out, sr);
            }
        }
    }

    void testSelectivity()
    {
        // 範囲の中に 2 本: 1000 Hz (-12 dBFS) と 1300 Hz (-30 dBFS)。しきい値 -24 → 1000 Hz だけ下がる
        const int n = (int) (2.0 * sr);
        juce::AudioBuffer<float> in (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = (float) (0.25 * std::sin (twoPi * 1000.0 * i / sr) + 0.0316 * std::sin (twoPi * 1300.0 * i / sr));
            in.setSample (0, i, s); in.setSample (1, i, s);
        }
        Proc p;
        lab::prepare (p, sr);
        band (p, 2, { { "on", 1 }, { "freq", 1150 }, { "track", 4 }, { "q", 12 }, { "thresh", -24 }, { "ratio", 20 }, { "gain", -30 } });
        lab::prepare (p, sr);
        auto out = lab::run (p, in);
        auto tone = [&] (const juce::AudioBuffer<float>& x, double f)
        {
            std::complex<double> z (0.0, 0.0);
            const int a = (int) sr, len = (int) (0.9 * sr);
            for (int i = a; i < a + len; ++i) z += (double) x.getSample (0, i) * std::polar (1.0, -twoPi * f * i / sr);
            return lab::db (2.0 * std::abs (z) / len);
        };
        std::printf ("[4] two partials in range: 1000 Hz %+.1f -> %+.1f dBFS, 1300 Hz %+.1f -> %+.1f dBFS\n",
                     tone (in, 1000), tone (out, 1000), tone (in, 1300), tone (out, 1300));
    }

    void testBlockSize()
    {
        auto g = makeGlide (2.0, 5, 0.2, 0.02);
        juce::AudioBuffer<float> outs[2];
        int i = 0;
        for (int block : { 64, 1024 })
        {
            Proc p;
            lab::prepare (p, sr, block);
            band (p, 2, { { "on", 1 }, { "freq", 1225 }, { "track", 4 }, { "thresh", -30 }, { "ratio", 20 }, { "gain", -30 } });
            lab::prepare (p, sr, block);
            outs[i++] = lab::run (p, g.audio, block);
        }
        std::printf ("[5] block 64 vs 1024: residual %.1f dB\n", lab::residualDb (outs[0], outs[1], 0, outs[0].getNumSamples()));
    }

    void setAllOn (juce::AudioProcessor& p)
    {
        const float fr[5] = { 220, 440, 1000, 2500, 6000 };
        for (int b = 0; b < 5; ++b)
            for (auto& [k, v] : std::initializer_list<std::pair<const char*, float>> { { "on", 1 }, { "freq", fr[b] }, { "track", 4 }, { "thresh", -40 }, { "ratio", 8 }, { "gain", -24 } })
                lab::setParam (p, Proc::bandId (b, k), v);
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    auto robust = []
    {
        lab::RobustConfig c;
        c.setup = setAllOn;
        return c;
    };
    if (int r = lab::cliMain<Proc> (argc, argv, sr, robust); r >= 0)
        return r;

    testDefaults();
    testStatic();
    testTracking (false);
    testTracking (true);
    testSelectivity();
    testBlockSize();

    {
        Proc p;
        lab::prepare (p, sr);
        std::printf ("[6] CPU all off %.2f %%", lab::cpuPercent (p, sr));
        setAllOn (p);
        lab::prepare (p, sr);
        std::printf (", 5 bands on %.2f %% of realtime\n", lab::cpuPercent (p, sr));
    }

    Proc p;
    lab::prepare (p, sr, 256);
    band (p, 1, { { "on", 1 }, { "freq", 620 }, { "track", 3 }, { "thresh", -36 }, { "gain", 9 }, { "ratio", 20 } });
    band (p, 2, { { "on", 1 }, { "freq", 1225 }, { "track", 4 }, { "thresh", -30 }, { "ratio", 20 }, { "gain", -24 } });
    p.selectedBand = 2;
    lab::prepare (p, sr, 256);
    auto g = makeGlide (1.0, 5, 0.2, 0.02);
    lab::run (p, g.audio, 256);

    // 画面の操作: 点をドラッグ → Freq / Gain、空いた所をダブルクリック → 空きバンドが入る
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorAndMakeActive());
        auto& gv = static_cast<TrackEQAudioProcessorEditor*> (ed.get())->graph;
        auto val = [&] (int b, const char* n) { return p.apvts.getRawParameterValue (Proc::bandId (b, n))->load(); };
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&] (juce::Point<float> pos, juce::Point<float> down, bool dragged, int clicks = 1)
        {
            return juce::MouseEvent (src, pos, juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     &gv, &gv, juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), clicks, dragged);
        };
        const juce::Point<float> from (gv.xOf (val (2, "freq")), gv.yGain (val (2, "gain"))), to (gv.xOf (2000.0f), gv.yGain (-6.0f));
        gv.mouseDown (ev (from, from, false));
        gv.mouseDrag (ev (to, from, true));
        gv.mouseUp (ev (to, from, true));
        std::printf ("[7] drag node 3 to (2000 Hz, -6 dB): freq %.0f Hz, gain %+.1f dB\n", val (2, "freq"), val (2, "gain"));
        // 範囲の右端を 2000 Hz の 5 半音上へ
        const juce::Point<float> e0 (gv.xOf (2000.0f * std::pow (2.0f, val (2, "track") / 12.0f)), 40.0f), e1 (gv.xOf (2000.0f * std::pow (2.0f, 5.0f / 12.0f)), 40.0f);
        gv.mouseDown (ev (e0, e0, false));
        gv.mouseDrag (ev (e1, e0, true));
        gv.mouseUp (ev (e1, e0, true));
        std::printf ("    drag range edge to +5 st: track %.2f st\n", val (2, "track"));
        juce::MouseWheelDetails wd; wd.deltaX = 0.0f; wd.deltaY = 0.2f; wd.isReversed = false; wd.isSmooth = true; wd.isInertial = false;
        gv.mouseWheelMove (ev (to, to, false), wd);
        std::printf ("    wheel up on node: Q %.2f (was 8.00)\n", val (2, "q"));
        gv.mouseMagnify (ev (to, to, false), 0.5f);
        std::printf ("    pinch x0.5: track %.2f st\n", val (2, "track"));
        const juce::Point<float> empty (gv.xOf (6000.0f), gv.yGain (12.0f));
        gv.mouseDoubleClick (ev (empty, empty, false, 2));
        std::printf ("    double-click empty at (6000 Hz, +12 dB): band 1 on=%d freq %.0f gain %+.1f\n", (int) val (0, "on"), val (0, "freq"), val (0, "gain"));
        // 撮る前に元へ
        band (p, 0, { { "on", 0 } });
        band (p, 2, { { "freq", 1225 }, { "gain", -24 }, { "track", 4 }, { "q", 8 } });
        p.selectedBand = 2;
        p.editorBeingDeleted (ed.get());   // これを呼ばないと、次に作るエディタが消したものを指す
    }
    lab::run (p, g.audio, 256);
    auto png = juce::File::getCurrentWorkingDirectory().getChildFile ("renders/ui.png");
    lab::snapshot (p, png);
    std::printf ("[8] UI -> %s\n", png.getFullPathName().toRawUTF8());
    return 0;
}
