#pragma once

// PluginLab 標準の耐久テスト (LabTest.h から include される。単独では使わない)
//
//   lab::RobustConfig cfg;
//   cfg.setup = [] (juce::AudioProcessor& p) { lab::setParam (p, "drive", 12.0f); };   // 効果が出る設定
//   return lab::robustMain<MyProcessor> (cfg);
//
// 実際の DAW で壊れやすいところを 12 項目で調べ、PASS / WARN / FAIL の表を出す。
// 合格ラインは暫定 (research/NEXT_robustness.md)。変えたら knowledge/verification.md に理由を書く。

#include <algorithm>
#include <atomic>
#include <random>
#include <thread>

namespace lab
{
    struct RobustConfig
    {
        double sampleRate = 48000.0;
        double seconds = 2.0;
        std::function<void (juce::AudioProcessor&)> setup;                  // 効果が出る設定 (無ければ既定値のまま)
        std::function<std::vector<MidiAt> (double sr, int numSamples)> midi; // MIDI で鳴るプラグイン用
        juce::StringArray skipAutomation;                                    // オートメーション試験から外すパラメータ ID
        juce::StringArray skipRandomize;                                     // 状態試験で乱数にしないパラメータ ID
        bool threads = true;
        // 状態 (並べ替えの種など) を prepare をまたいで保つ設計のプラグイン用。true なら「prepare し直しても、
        // 同じ状態を読み込んだ新品と同じ音」を確かめる (既定の「新品と同じ音に戻る」の代わり)。Scramble で追加
        bool stateSurvivesPrepare = false;
    };

    struct RobustResult
    {
        juce::String name;
        int status = 0;   // 0 PASS, 1 WARN, 2 FAIL, 3 N/A
        juce::String detail;
    };

    inline bool allFinite (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (ch, i)))
                    return false;
        return true;
    }

    // 擬似の再生位置: テンポと拍位置 (ppq) をホストの代わりに与える。runWith がブロックごとに位置を進める
    struct SimPlayHead  : public juce::AudioPlayHead
    {
        double bpm = 120.0, sampleRate = 48000.0, ppqAtZero = 0.0;
        bool playing = true;
        int64_t samplePos = 0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm (bpm);
            p.setIsPlaying (playing);
            p.setTimeInSamples (samplePos);
            p.setTimeInSeconds (samplePos / sampleRate);
            p.setPpqPosition (ppqAtZero + samplePos / sampleRate * bpm / 60.0);
            p.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
            return p;
        }
    };

    struct RunOptions
    {
        SimPlayHead* playhead = nullptr;                       // テンポ同期のプラグイン用
        int block = 512;
        bool randomBlocks = false;
        unsigned seed = 1;
        std::function<void (juce::AudioProcessor&, int start, int total)> beforeBlock;
        std::vector<MidiAt> midi;
        std::vector<double>* secondsPerSample = nullptr;   // ブロックごとの処理時間 / サンプル数
    };

    // ブロックの長さ・途中のパラメータ変更・時間計測を指定できる run
    inline juce::AudioBuffer<float> runWith (juce::AudioProcessor& p, const juce::AudioBuffer<float>& input, const RunOptions& o)
    {
        const int n = input.getNumSamples();
        const int nch = std::max (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> out (nch, n), buf (nch, 4096);
        std::mt19937 rng (o.seed);
        std::uniform_int_distribution<int> dist (1, 2048);
        int start = 0;
        while (start < n)
        {
            const int len = std::min (o.randomBlocks ? dist (rng) : o.block, n - start);
            buf.setSize (nch, len, false, false, true);
            buf.clear();
            for (int ch = 0; ch < std::min (nch, input.getNumChannels()); ++ch)
                buf.copyFrom (ch, 0, input, ch, start, len);
            juce::MidiBuffer midi;
            for (auto& e : o.midi)
                if (e.sample >= start && e.sample < start + len)
                    midi.addEvent (e.msg, e.sample - start);
            if (o.beforeBlock)
                o.beforeBlock (p, start, n);
            if (o.playhead != nullptr)
            {
                o.playhead->samplePos = start;
                p.setPlayHead (o.playhead);
            }
            const auto t0 = juce::Time::getHighResolutionTicks();
            p.processBlock (buf, midi);
            if (o.secondsPerSample != nullptr)
                o.secondsPerSample->push_back (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0) / len);
            for (int ch = 0; ch < nch; ++ch)
                out.copyFrom (ch, start, buf, ch, 0, len);
            start += len;
        }
        return out;
    }

    namespace robust_detail
    {
        // 試験用の音: 110 Hz と 440 Hz のサイン波 + 250 ms ごとに減衰する「ノイズの粒」(左右で少し違う)。
        // ノイズは 200 Hz〜15 kHz の 48 本のサイン波の和で作る (帯域を固定しないと、サンプルレートを上げたとき
        // ナイキストまでのノイズの中身自体が変わってしまい、レート比較が無意味になる)
        inline juce::AudioBuffer<float> testSignal (double sr, double seconds, bool withNoise = true)
        {
            const int n = (int) (sr * seconds);
            juce::AudioBuffer<float> b (2, n);
            b.clear();
            const int period = (int) (sr * 0.25);
            const double twoPi = juce::MathConstants<double>::twoPi;
            std::vector<double> freq, phL, phR;
            juce::Random r (7);
            for (int k = 0; k < 48; ++k)
            {
                freq.push_back (200.0 * std::pow (75.0, r.nextDouble()));   // 200 Hz〜15 kHz を対数で
                phL.push_back (r.nextDouble() * twoPi);
                phR.push_back (r.nextDouble() * twoPi);
            }
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                const float tone = 0.2f * (float) std::sin (twoPi * 110.0 * t) + 0.1f * (float) std::sin (twoPi * 440.0 * t);
                float nl = 0.0f, nr = 0.0f;
                if (withNoise)
                {
                    const float env = 0.3f * (float) std::exp (-(i % period) / (0.05 * sr)) / 5.0f;
                    for (size_t k = 0; k < freq.size(); ++k)
                    {
                        nl += env * (float) std::sin (twoPi * freq[k] * t + phL[k]);
                        nr += env * (float) std::sin (twoPi * freq[k] * t + phR[k]);
                    }
                }
                // クリック試験用 (withNoise = false) にも「音の粒」を入れる: 330 Hz、3 ms で滑らかに立ち上がり 80 ms で減衰。
                // 持続音だけだとトランジェント系のつまみが何もせず、クリック試験が素通りで合格してしまう
                float burst = 0.0f;
                if (! withNoise)
                {
                    const int k = i % period;
                    const double rise = std::min (1.0, k / (0.003 * sr));
                    burst = 0.25f * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * rise))
                                  * (float) std::exp (-k / (0.08 * sr)) * (float) std::sin (twoPi * 330.0 * k / sr);
                }
                b.setSample (0, i, tone + nl + burst);
                b.setSample (1, i, tone * 0.9f + nr + burst);
            }
            return b;
        }

        inline juce::AudioBuffer<float> dualMono (const juce::AudioBuffer<float>& b)
        {
            juce::AudioBuffer<float> d (2, b.getNumSamples());
            d.copyFrom (0, 0, b, 0, 0, b.getNumSamples());
            d.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
            return d;
        }

        // 高域のスパイク検出: 2 階差分のエネルギーを 5 ms 窓ごとに dB で。最大値を返す
        inline double hfPeakDb (const juce::AudioBuffer<float>& b, double sr, int from = 0)
        {
            const int w = std::max (1, (int) (sr * 0.005));
            double best = -300.0;
            for (int s = std::max (2, from); s + w <= b.getNumSamples(); s += w)
            {
                double e = 0.0;
                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                    for (int i = s; i < s + w; ++i)
                    {
                        const double d = b.getSample (ch, i) - 2.0 * b.getSample (ch, i - 1) + b.getSample (ch, i - 2);
                        e += d * d;
                    }
                best = std::max (best, 10.0 * std::log10 (e / w + 1e-30));
            }
            return best;
        }

        inline double rmsDb (const juce::AudioBuffer<float>& b, int start = 0, int len = -1) { return db (rms (b, start, len)); }

        // 聞こえる帯域 (18 kHz 以下) だけの RMS。サンプルレートを上げると、共鳴や歪みがナイキストまでの
        // 超高域に成分を足すことがあり、そのまま比べると聞こえない差で不合格になる (ChordRes で発覚)
        inline double audibleRmsDb (const juce::AudioBuffer<float>& b, double sr)
        {
            auto c = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, 18000.0f, 0.5412f);   // 4 次バターワースの 1 段目
            auto d = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, 18000.0f, 1.3066f);   // 2 段目
            double sum = 0.0;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                juce::dsp::IIR::Filter<float> f1 (c), f2 (d);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float y = f2.processSample (f1.processSample (b.getSample (ch, i)));
                    sum += (double) y * y;
                }
            }
            return db (std::sqrt (sum / std::max (1, b.getNumSamples() * b.getNumChannels())));
        }

        template <typename Proc>
        std::unique_ptr<Proc> fresh (const RobustConfig& c, double sr, int channels = 2)
        {
            auto p = std::make_unique<Proc>();
            prepare (*p, sr, 4096, channels);
            if (c.setup) c.setup (*p);
            prepare (*p, sr, 4096, channels);   // スムージングを飛ばして設定値から
            return p;
        }

        inline std::vector<juce::RangedAudioParameter*> params (juce::AudioProcessor& p)
        {
            std::vector<juce::RangedAudioParameter*> v;
            for (auto* x : p.getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (x))
                    v.push_back (r);
            return v;
        }

        inline juce::String fmt (const char* f, double a, double b = 0.0, double c = 0.0) { return juce::String::formatted (f, a, b, c); }
    }

    template <typename Proc>
    std::vector<RobustResult> robustness (const RobustConfig& cfg)
    {
        using namespace robust_detail;
        std::vector<RobustResult> res;
        const double sr = cfg.sampleRate;
        const auto sig = testSignal (sr, cfg.seconds);
        const int n = sig.getNumSamples();
        auto midiAt = [&] (double rate, int len) { return cfg.midi ? cfg.midi (rate, len) : std::vector<MidiAt> {}; };
        bool anyNonFinite = false;
        auto check = [&] (const juce::AudioBuffer<float>& b) { if (! allFinite (b)) anyNonFinite = true; return b; };

        RunOptions base;
        base.midi = midiAt (sr, n);

        // 基準の出力 (ブロック 512)
        auto p0 = fresh<Proc> (cfg, sr);
        const int latency = p0->getLatencySamples();
        const auto ref = check (runWith (*p0, sig, base));
        const bool silent = rms (ref) < 1.0e-6;

        //------------------------------------------------------------ 2 サンプルレート
        {
            RobustResult r { "sample rates (44.1/48/88.2/96k)" };
            double worst = 0.0;
            juce::String detail;
            for (double rate : { 44100.0, 88200.0, 96000.0 })
            {
                auto p = fresh<Proc> (cfg, rate);
                RunOptions o; o.midi = midiAt (rate, (int) (rate * cfg.seconds));
                const auto out = check (runWith (*p, testSignal (rate, cfg.seconds), o));
                const double d = audibleRmsDb (out, rate) - audibleRmsDb (ref, sr);
                worst = std::max (worst, std::abs (d));
                detail << juce::String ((int) (rate / 100) / 10.0) << "k " << juce::String (d, 2) << " dB  ";
            }
            r.status = worst <= 0.5 ? 0 : worst <= 2.0 ? 1 : 2;
            r.detail = "audible-band RMS vs 48k: " + detail;
            res.push_back (r);
        }

        //------------------------------------------------------------ 3 ブロックサイズ
        {
            RobustResult r { "block sizes (1/7/64/511/4096/random)" };
            double worst = -300.0, worstSteady = -300.0;
            juce::String worstName = "all identical";
            const int settle = (int) (0.5 * sr);
            for (int bs : { 1, 7, 64, 511, 4096, -1 })
            {
                auto p = fresh<Proc> (cfg, sr);
                RunOptions o = base;
                o.block = bs > 0 ? bs : 512;
                o.randomBlocks = bs < 0;
                const auto out = check (runWith (*p, sig, o));
                const double d = silent ? -300.0 : residualDb (out, ref, 0, n);
                const double ds = silent ? -300.0 : residualDb (out, ref, settle, n - settle);
                if (d > worst) { worst = d; worstName = bs > 0 ? juce::String (bs) : juce::String ("random"); }
                worstSteady = std::max (worstSteady, ds);
            }
            // 判定は全体の値で。起動直後だけ違うのか、ずっと違うのかは detail で分かるようにする
            r.status = worst <= -120.0 ? 0 : worst <= -60.0 ? 1 : 2;
            r.detail = fmt ("worst residual vs block 512: %.1f dB (", worst) + worstName
                       + fmt ("), after the first 0.5 s: %.1f dB", worstSteady);
            res.push_back (r);
        }

        //------------------------------------------------------------ 4 オートメーション
        {
            RobustResult r { "automation clicks (sweep / step)" };
            const auto tone = testSignal (sr, cfg.seconds, false);
            double worst = -300.0;
            juce::String worstName;
            int tested = 0;
            std::vector<std::pair<double, juce::String>> ranking;
            auto* bypassParam = p0->getBypassParameter();
            for (auto* prm : params (*p0))
            {
                const auto id = prm->getParameterID();
                if (prm == bypassParam || cfg.skipAutomation.contains (id))
                    continue;
                ++tested;
                const int idx = prm->getParameterIndex();
                auto setNorm = [idx] (juce::AudioProcessor& p, float v) { p.getParameters()[idx]->setValueNotifyingHost (v); };

                double staticMax = -300.0;
                for (float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                {
                    auto p = fresh<Proc> (cfg, sr);
                    setNorm (*p, v);
                    prepare (*p, sr, 4096);
                    RunOptions o = base;
                    staticMax = std::max (staticMax, hfPeakDb (check (runWith (*p, tone, o)), sr, (int) (0.1 * sr)));
                }
                for (int mode = 0; mode < 2; ++mode)
                {
                    auto p = fresh<Proc> (cfg, sr);
                    setNorm (*p, 0.0f);
                    prepare (*p, sr, 4096);
                    RunOptions o = base;
                    if (mode == 0)   // 全域を直線でスイープ
                        o.beforeBlock = [&] (juce::AudioProcessor& q, int start, int total) { setNorm (q, (float) start / (float) total); };
                    else             // 250 ms ごとに最小と最大を行き来
                        o.beforeBlock = [&] (juce::AudioProcessor& q, int start, int) { setNorm (q, ((start / (int) (sr * 0.25)) % 2) ? 1.0f : 0.0f); };
                    const double spike = hfPeakDb (check (runWith (*p, tone, o)), sr, (int) (0.1 * sr)) - staticMax;
                    ranking.push_back ({ spike, id + (mode == 0 ? " sweep" : " step") });
                    if (spike > worst) { worst = spike; worstName = id + (mode == 0 ? " (sweep)" : " (step)"); }
                }
            }
            std::sort (ranking.begin(), ranking.end(), [] (auto& x, auto& y) { return x.first > y.first; });
            juce::String top;
            for (size_t i = 0; i < std::min<size_t> (3, ranking.size()); ++i)
                top << ranking[i].second << " " << juce::String (ranking[i].first, 1) << " dB; ";
            if (tested == 0 || silent)
                r.status = 3, r.detail = silent ? "output is silent" : "no parameters";
            else
            {
                r.status = worst < 10.0 ? 0 : worst < 20.0 ? 1 : 2;
                r.detail = juce::String (tested) + " params, HF spike vs static (worst first): " + top;
            }
            res.push_back (r);
        }

        //------------------------------------------------------------ 5 状態の保存と復元
        {
            RobustResult r { "state save / restore" };
            auto p1 = fresh<Proc> (cfg, sr);
            juce::Random rnd (99);
            auto* bypassParam = p1->getBypassParameter();
            for (auto* prm : params (*p1))
                if (prm != bypassParam && ! cfg.skipRandomize.contains (prm->getParameterID()))
                    prm->setValueNotifyingHost (rnd.nextFloat());
            prepare (*p1, sr, 4096);
            juce::MemoryBlock mb;
            p1->getStateInformation (mb);
            auto p2 = std::make_unique<Proc>();
            prepare (*p2, sr, 4096);
            p2->setStateInformation (mb.getData(), (int) mb.getSize());
            prepare (*p2, sr, 4096);
            // パラメータは表示の文字で比べる (bool / choice に 0.3 のような値を入れると、getValue は 0.3 のままでも
            // 保存されるのは丸めた値なので、正規化値の比較だと差が出てしまう)
            juce::StringArray differ;
            auto a = params (*p1), b = params (*p2);
            for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
                if (a[i]->getCurrentValueAsText() != b[i]->getCurrentValueAsText())
                    differ.add (a[i]->getParameterID());
            const auto o1 = check (runWith (*p1, sig, base)), o2 = check (runWith (*p2, sig, base));
            const double d = rms (o1) < 1.0e-9 ? -300.0 : residualDb (o2, o1, 0, n);
            r.status = (differ.isEmpty() && d <= -120.0) ? 0 : (d <= -60.0) ? 1 : 2;
            r.detail = juce::String (differ.size()) + " params differ as text" + (differ.isEmpty() ? juce::String() : " (" + differ.joinIntoString (",") + ")")
                       + fmt (", output residual %.1f dB", d) + " (" + juce::String ((int) mb.getSize()) + " bytes)";
            res.push_back (r);
        }

        //------------------------------------------------------------ 6 レイテンシ
        {
            RobustResult r { "reported latency" };
            auto p = fresh<Proc> (cfg, sr);
            auto imp = impulse ((int) sr, 1.0f, 4800);
            const auto out = check (runWith (*p, imp, base));
            int pk = 0;
            float best = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    if (std::abs (out.getSample (ch, i)) > best) { best = std::abs (out.getSample (ch, i)); pk = i; }
            if (best < 1.0e-6)
                r.status = 3, r.detail = "no output for an impulse";
            else
            {
                const int measured = pk - 4800;
                r.status = std::abs (measured - latency) <= 2 ? 0 : 1;
                r.detail = juce::String ("reported ") + juce::String (latency) + ", impulse peak at " + juce::String (measured)
                           + (r.status ? " (peak can move if the effect smears the impulse; check by ear/phase)" : "");
            }
            res.push_back (r);
        }

        //------------------------------------------------------------ 7 バイパス
        {
            RobustResult r { "bypass toggle (click / latency match)" };
            auto probe = std::make_unique<Proc>();
            auto* bp = probe->getBypassParameter();
            if (bp == nullptr)
            {
                // レイテンシが 0 ならホストのバイパス (入力をそのまま) で困らない。レイテンシがあるとずれる
                r.status = latency > 0 ? 1 : 0;
                r.detail = latency > 0 ? "no bypass parameter: host bypass will not delay the dry signal (" + juce::String (latency) + " samples)"
                                       : "no bypass parameter (latency 0, host bypass is fine)";
            }
            else
            {
                const int bidx = bp->getParameterIndex();
                const auto tone = testSignal (sr, cfg.seconds, false);
                auto staticRun = [&] (float v)
                {
                    auto p = fresh<Proc> (cfg, sr);
                    p->getParameters()[bidx]->setValueNotifyingHost (v);
                    prepare (*p, sr, 4096);
                    return check (runWith (*p, tone, base));
                };
                const double staticMax = std::max (hfPeakDb (staticRun (0.0f), sr, (int) (0.1 * sr)), hfPeakDb (staticRun (1.0f), sr, (int) (0.1 * sr)));
                auto p = fresh<Proc> (cfg, sr);
                RunOptions o = base;
                const int on = (int) (0.6 * sr), off = (int) (1.3 * sr);
                o.beforeBlock = [&] (juce::AudioProcessor& q, int start, int) { q.getParameters()[bidx]->setValueNotifyingHost (start >= on && start < off ? 1.0f : 0.0f); };
                const auto out = check (runWith (*p, tone, o));
                const double spike = hfPeakDb (out, sr, (int) (0.1 * sr)) - staticMax;
                // バイパス中の区間 (0.8〜1.2 s) は、入力を latency だけ遅らせたものと一致するはず
                const int s0 = (int) (0.8 * sr), len = (int) (0.4 * sr);
                double e = 0.0, ref2 = 0.0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = s0; i < s0 + len; ++i)
                    {
                        const double d = out.getSample (ch, i) - tone.getSample (ch, i - latency);
                        e += d * d;
                        ref2 += (double) tone.getSample (ch, i) * tone.getSample (ch, i);
                    }
                const double match = 10.0 * std::log10 (e / ref2 + 1e-30);
                r.status = (spike < 10.0 && match <= -60.0) ? 0 : (spike < 20.0 && match <= -30.0) ? 1 : 2;
                r.detail = fmt ("toggle HF spike %+.1f dB, bypassed output vs delayed input %.1f dB", spike, match);
            }
            res.push_back (r);
        }

        //------------------------------------------------------------ 8 無音とデノーマル
        {
            RobustResult r { "silence tail / denormals" };
            const int total = (int) (sr * 7.0);
            juce::AudioBuffer<float> in (2, total);
            in.clear();
            const auto s1 = testSignal (sr, 1.0);
            for (int ch = 0; ch < 2; ++ch) in.copyFrom (ch, 0, s1, ch, 0, s1.getNumSamples());
            auto p = fresh<Proc> (cfg, sr);
            std::vector<double> t;
            RunOptions o;
            o.midi = midiAt (sr, (int) sr);
            o.secondsPerSample = &t;
            const auto out = check (runWith (*p, in, o));
            const size_t perSec = t.size() / 7;
            auto median = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };
            const double busy = median ({ t.begin(), t.begin() + (long) perSec });
            const double quiet = median ({ t.end() - (long) (3 * perSec), t.end() });
            const double ratio = quiet / std::max (busy, 1.0e-12);
            const double tail = rmsDb (out, total - (int) sr, (int) sr);
            r.status = ratio > 3.0 ? 2 : (ratio > 1.5 || tail > -60.0) ? 1 : 0;
            r.detail = fmt ("CPU in silence x%.2f of busy, last second %.1f dBFS", ratio, tail);
            res.push_back (r);
        }

        //------------------------------------------------------------ 9 極端な入力
        {
            RobustResult r { "extreme inputs (DC/square/+12dBFS/spikes/20k)" };
            const int len = (int) sr;
            std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> cases;
            juce::AudioBuffer<float> dc (2, len), sq (2, len), loud = noise (len, 4.0f, 5), spikes (2, len), hi = sine (sr, 20000.0, 1.0, 0.99f);
            spikes.clear();
            for (int i = 0; i < len; ++i)
                for (int ch = 0; ch < 2; ++ch)
                {
                    dc.setSample (ch, i, 0.5f);
                    sq.setSample (ch, i, ((i / (int) (sr / 200.0)) % 2) ? 1.0f : -1.0f);
                    if (i % 1000 == 0) spikes.setSample (ch, i, 1.0f);
                }
            cases = { { "DC", dc }, { "square", sq }, { "+12dBFS", loud }, { "spikes", spikes }, { "20k", hi } };
            double worstPk = -300.0;
            juce::String worstName, bad;
            for (auto& [name, in] : cases)
            {
                auto p = fresh<Proc> (cfg, sr);
                RunOptions o; o.midi = midiAt (sr, len);
                const auto out = runWith (*p, in, o);
                if (! allFinite (out)) { bad << name << " "; continue; }
                const double pk = db (peak (out));
                if (pk > worstPk) { worstPk = pk; worstName = name; }
            }
            r.status = bad.isNotEmpty() ? 2 : worstPk > 60.0 ? 2 : worstPk > 24.0 ? 1 : 0;
            r.detail = bad.isNotEmpty() ? "NaN/Inf with: " + bad : fmt ("highest output peak %+.1f dBFS (", worstPk) + worstName + ")";
            res.push_back (r);
        }

        //------------------------------------------------------------ 10 prepareToPlay の繰り返し
        {
            RobustResult r { cfg.stateSurvivesPrepare ? "prepareToPlay again keeps state" : "prepareToPlay again resets state" };
            auto p = fresh<Proc> (cfg, sr);
            RunOptions o = base;
            runWith (*p, noise (n, 0.5f, 3), o);
            prepare (*p, sr, 4096);
            auto expect = ref;
            if (cfg.stateSurvivesPrepare)
            {
                // 同じ状態を読み込んだ新品を基準にする
                juce::MemoryBlock mb;
                p->getStateInformation (mb);
                auto q = std::make_unique<Proc>();
                q->setStateInformation (mb.getData(), (int) mb.getSize());
                prepare (*q, sr, 4096);
                expect = runWith (*q, sig, base);
            }
            const auto out = check (runWith (*p, sig, base));
            const double d = silent ? -300.0 : residualDb (out, expect, 0, n);
            r.status = d <= -120.0 ? 0 : d <= -40.0 ? 1 : 2;
            r.detail = fmt (cfg.stateSurvivesPrepare ? "residual vs a fresh instance loaded with the same state %.1f dB"
                                                     : "residual vs a fresh instance %.1f dB", d);
            res.push_back (r);
        }

        //------------------------------------------------------------ 11 モノラル
        {
            RobustResult r { "mono layout" };
            auto p = std::make_unique<Proc>();
            juce::AudioProcessor::BusesLayout mono;
            mono.inputBuses.add (juce::AudioChannelSet::mono());
            mono.outputBuses.add (juce::AudioChannelSet::mono());
            if (! p->checkBusesLayoutSupported (mono))
                r.status = 3, r.detail = "mono not supported";
            else
            {
                const auto dm = dualMono (sig);
                auto ps = fresh<Proc> (cfg, sr);
                const auto st = check (runWith (*ps, dm, base));
                p->setBusesLayout (mono);
                prepare (*p, sr, 4096, 1);
                if (cfg.setup) cfg.setup (*p);
                prepare (*p, sr, 4096, 1);
                juce::AudioBuffer<float> m (1, n);
                m.copyFrom (0, 0, sig, 0, 0, n);
                const auto mo = check (runWith (*p, m, base));
                juce::AudioBuffer<float> left (1, n), right (1, n);
                left.copyFrom (0, 0, st, 0, 0, n);
                right.copyFrom (0, 0, st, 1, 0, n);
                // 左右が同じ入力で左右が違う音になるなら、左右を広げるタイプ (Width など)。
                // その場合「モノラル = ステレオの左」は成り立たないので、音量だけを比べる
                const bool spreads = rms (left) > 1.0e-9 && residualDb (right, left, 0, n) > -60.0;
                if (spreads)
                {
                    const double lv = audibleRmsDb (mo, sr) - audibleRmsDb (st, sr);
                    r.status = std::abs (lv) <= 3.0 ? 0 : std::abs (lv) <= 6.0 ? 1 : 2;
                    r.detail = fmt ("stereo-spreading effect: mono level vs stereo %+.1f dB", lv);
                }
                else
                {
                    const double d = rms (left) < 1.0e-9 ? -300.0 : residualDb (mo, left, 0, n);
                    r.status = d <= -100.0 ? 0 : d <= -30.0 ? 1 : 2;
                    r.detail = fmt ("mono vs left of dual-mono stereo: %.1f dB", d);
                }
            }
            res.push_back (r);
        }

        //------------------------------------------------------------ 12 スレッド
        if (cfg.threads)
        {
            RobustResult r { "parameter changes from another thread" };
            auto p = fresh<Proc> (cfg, sr);
            auto ps = params (*p);
            auto* bp = p->getBypassParameter();
            std::atomic<bool> stop { false };
            std::thread hammer ([&]
            {
                juce::Random rnd (5);
                while (! stop.load())
                    for (auto* prm : ps)
                        if (prm != bp)
                            prm->setValueNotifyingHost (rnd.nextFloat());
            });
            const auto out = runWith (*p, testSignal (sr, 3.0), base);
            stop = true;
            hammer.join();
            const bool fin = allFinite (out);
            r.status = fin ? 0 : 2;
            r.detail = fin ? "no crash, output finite (run a TSan build for data races)" : "NaN/Inf";
            res.push_back (r);
        }

        //------------------------------------------------------------ 1 NaN / Inf (全体)
        res.insert (res.begin(), RobustResult { "NaN / Inf in any test", anyNonFinite ? 2 : 0,
                                                anyNonFinite ? "found non-finite output" : "none" });
        return res;
    }

    inline int printRobust (const std::vector<RobustResult>& res)
    {
        const char* tag[] = { "PASS", "WARN", "FAIL", "N/A " };
        int fails = 0, warns = 0;
        for (auto& r : res)
        {
            std::printf ("  [%s] %-46s %s\n", tag[r.status], r.name.toRawUTF8(), r.detail.toRawUTF8());
            fails += r.status == 2;
            warns += r.status == 1;
        }
        std::printf ("  => %d FAIL, %d WARN\n", fails, warns);
        return fails;
    }

    template <typename Proc>
    int robustMain (const RobustConfig& cfg = {})
    {
        std::printf ("robustness (%.0f Hz, %.1f s test signal)\n", cfg.sampleRate, cfg.seconds);
        return printRobust (robustness<Proc> (cfg)) > 0 ? 1 : 0;
    }
}
