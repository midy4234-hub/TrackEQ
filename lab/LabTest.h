#pragma once

// PluginLab 共通のオフライン検証ハーネス (JUCE プラグイン用、ヘッダだけ)
//
// 使い方 (tests/TestMain.cpp):
//   #include "LabTest.h"
//   int main (int argc, char* argv[])
//   {
//       juce::ScopedJuceInitialiser_GUI init;
//       if (int r = lab::cliMain<MyProcessor> (argc, argv); r >= 0) return r;   // render / snapshot / params
//       ... プラグイン固有のテスト ...
//   }
//
// cliMain が受け付けるコマンド:
//   render in.wav out.wav [id=value ...] [--plain]   試聴用 A/B (原音2周 → 0.4秒無音 → 処理後2周、1周目は捨てる)
//                                                     --plain なら入力をそのまま1回処理して書く
//   snapshot out.png [in.wav] [id=value ...]          UI の画像 (in.wav を先に流してから撮る)
//   params                                            パラメータ ID・範囲・初期値の一覧
//   robust                                            標準の耐久テスト (LabRobust.h、12 項目)
//
// パラメータは ID で指定する (APVTS の ParameterID)。値は実際の単位 (dB, Hz など)。bool は 0/1、choice は番号。

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace lab
{
    // 出力を行ごとに流す (パイプ越しでも途中経過が見える)。MSVC の CRT は _IOLBF を _IOFBF 扱いにし、
    // サイズ 0 を不正な引数としてプロセスを落とす (0xC0000409、main より前) ので、Windows ではバッファ無しにする
   #if defined (_MSC_VER)
    struct FlushStdout { FlushStdout() { std::setvbuf (stdout, nullptr, _IONBF, 0); } };
   #else
    struct FlushStdout { FlushStdout() { std::setvbuf (stdout, nullptr, _IOLBF, 0); } };
   #endif
    inline FlushStdout flushStdoutOnce;

    inline double db (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }

    inline double peak (const juce::AudioBuffer<float>& b, int start = 0, int len = -1)
    {
        if (len < 0) len = b.getNumSamples() - start;
        double m = 0.0;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = start; i < start + len; ++i)
                m = std::max (m, (double) std::abs (b.getSample (ch, i)));
        return m;
    }

    inline double rms (const juce::AudioBuffer<float>& b, int start = 0, int len = -1)
    {
        if (len < 0) len = b.getNumSamples() - start;
        double s = 0.0;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = start; i < start + len; ++i)
                s += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return std::sqrt (s / std::max (1, len * b.getNumChannels()));
    }

    // 2 つの信号の差 (dB、ref の RMS 比)。delay は out 側の遅れ (レイテンシ)
    inline double residualDb (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& ref,
                               int start, int len, int delay = 0)
    {
        double e = 0.0, r = 0.0;
        for (int ch = 0; ch < std::min (out.getNumChannels(), ref.getNumChannels()); ++ch)
            for (int i = start; i < start + len; ++i)
            {
                const double d = out.getSample (ch, i + delay) - ref.getSample (ch, i);
                e += d * d;
                r += (double) ref.getSample (ch, i) * ref.getSample (ch, i);
            }
        return 10.0 * std::log10 (std::max (e, 1.0e-30) / std::max (r, 1.0e-30));
    }

    //------------------------------------------------------------------ 音声ファイル
    inline juce::AudioBuffer<float> readAudio (const juce::File& f, double& sampleRate, int channels = 2)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) return {};
        sampleRate = r->sampleRate;
        juce::AudioBuffer<float> b (channels, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);   // モノラルは両チャンネルに入る
        return b;
    }

    inline bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sampleRate, int bits = 24)
    {
        f.deleteFile();
        f.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                                                          .withNumChannels (b.getNumChannels())
                                                                          .withBitsPerSample (bits));
        return w != nullptr && w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    inline juce::AudioBuffer<float> tile (const juce::AudioBuffer<float>& src, int times)
    {
        const int L = src.getNumSamples();
        juce::AudioBuffer<float> t (src.getNumChannels(), L * times);
        for (int k = 0; k < times; ++k)
            for (int ch = 0; ch < src.getNumChannels(); ++ch)
                t.copyFrom (ch, k * L, src, ch, 0, L);
        return t;
    }

    //------------------------------------------------------------------ 試験信号
    inline juce::AudioBuffer<float> sine (double sr, double freq, double seconds, float amp = 0.25f, int channels = 2)
    {
        const int n = (int) (sr * seconds);
        juce::AudioBuffer<float> b (channels, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr);
            for (int ch = 0; ch < channels; ++ch) b.setSample (ch, i, v);
        }
        return b;
    }

    inline juce::AudioBuffer<float> noise (int n, float amp = 0.25f, int seed = 1, int channels = 2)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, n);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample (ch, i, amp * (r.nextFloat() * 2.0f - 1.0f));
        return b;
    }

    inline juce::AudioBuffer<float> impulse (int n, float amp = 1.0f, int at = 0, int channels = 2)
    {
        juce::AudioBuffer<float> b (channels, n);
        b.clear();
        for (int ch = 0; ch < channels; ++ch) b.setSample (ch, at, amp);
        return b;
    }

    //------------------------------------------------------------------ パラメータ
    inline juce::RangedAudioParameter* findParam (juce::AudioProcessor& p, const juce::String& id)
    {
        for (auto* param : p.getParameters())
            if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (withId->getParameterID() == id)
                    return withId;
        return nullptr;
    }

    inline bool setParam (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        if (auto* param = findParam (p, id))
        {
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            return true;
        }
        std::printf ("unknown parameter id: %s\n", id.toRawUTF8());
        return false;
    }

    // "id=value" の並びを適用。知らない ID があれば false
    inline bool applyArgs (juce::AudioProcessor& p, const juce::StringArray& args)
    {
        bool ok = true;
        for (auto& a : args)
            if (a.contains ("=") && ! a.startsWith ("--"))
                ok = setParam (p, a.upToFirstOccurrenceOf ("=", false, false), a.fromFirstOccurrenceOf ("=", false, false).getFloatValue()) && ok;
        return ok;
    }

    inline void printParams (juce::AudioProcessor& p)
    {
        for (auto* param : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (param))
            {
                const auto range = r->getNormalisableRange();
                std::printf ("  %-12s %-24s %g .. %g  default %g  (%s)\n", r->getParameterID().toRawUTF8(), r->getName (40).toRawUTF8(),
                             (double) range.start, (double) range.end, (double) r->convertFrom0to1 (r->getDefaultValue()),
                             r->getText (r->getDefaultValue(), 32).toRawUTF8());
            }
    }

    //------------------------------------------------------------------ 処理
    inline void prepare (juce::AudioProcessor& p, double sr, int block = 512, int channels = 2)
    {
        p.setPlayConfigDetails (channels, channels, sr, block);
        p.prepareToPlay (sr, block);
    }

    struct MidiAt { int sample; juce::MidiMessage msg; };

    // input をブロックごとに流して出力を返す。events は MIDI を使うプラグイン用
    inline juce::AudioBuffer<float> run (juce::AudioProcessor& p, const juce::AudioBuffer<float>& input,
                                         int block = 512, const std::vector<MidiAt>& events = {})
    {
        const int n = input.getNumSamples();
        const int nch = std::max (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> out (nch, n);
        juce::AudioBuffer<float> buf (nch, block);
        for (int start = 0; start < n; start += block)
        {
            const int len = std::min (block, n - start);
            buf.setSize (nch, len, false, false, true);
            buf.clear();
            for (int ch = 0; ch < std::min (nch, input.getNumChannels()); ++ch)
                buf.copyFrom (ch, 0, input, ch, start, len);
            juce::MidiBuffer midi;
            for (auto& e : events)
                if (e.sample >= start && e.sample < start + len)
                    midi.addEvent (e.msg, e.sample - start);
            p.processBlock (buf, midi);
            for (int ch = 0; ch < nch; ++ch)
                out.copyFrom (ch, start, buf, ch, 0, len);
        }
        return out;
    }

    // 1 回だけ流して処理時間を測る。戻り値はリアルタイム比 (%)
    inline double cpuPercent (juce::AudioProcessor& p, double sr, double seconds = 10.0)
    {
        auto in = noise ((int) (sr * seconds), 0.25f);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        run (p, in);
        return (juce::Time::getMillisecondCounterHiRes() - t0) / (seconds * 10.0);
    }

    //------------------------------------------------------------------ 試聴用 A/B
    struct ABResult { double inPeak, outPeak, inRms, outRms, trim; };

    // 原音 2 周 → 0.4 秒無音 → 処理後 2 周。処理は 3 周ぶん流して 1 周目を捨てる (状態が落ち着いてから)
    // 全体のピークが -1 dBFS を超えたら、A と B を同じだけ下げる (音量比較はそのまま)
    inline ABResult writeAB (juce::AudioProcessor& p, const juce::AudioBuffer<float>& src, double sr, const juce::File& outFile)
    {
        const int L = src.getNumSamples();
        auto in3 = tile (src, 3);
        auto out = run (p, in3);
        const int gap = (int) (0.4 * sr);
        juce::AudioBuffer<float> ab (2, L * 4 + gap);
        ab.clear();
        for (int ch = 0; ch < 2; ++ch)
        {
            ab.copyFrom (ch, 0, in3, std::min (ch, in3.getNumChannels() - 1), 0, L * 2);
            ab.copyFrom (ch, L * 2 + gap, out, std::min (ch, out.getNumChannels() - 1), L, L * 2);
        }
        ABResult r { peak (in3, L, 2 * L), peak (out, L, 2 * L), rms (in3, L, 2 * L), rms (out, L, 2 * L), 1.0 };
        const double abPk = peak (ab);
        if (abPk > 0.891) r.trim = 0.891 / abPk;
        ab.applyGain ((float) r.trim);
        writeWav (outFile, ab, sr);
        return r;
    }

    inline void printAB (const ABResult& r)
    {
        std::printf ("peak %+6.1f -> %+6.1f dBFS, rms %+6.1f -> %+6.1f dBFS, trim %.1f dB\n",
                     db (r.inPeak), db (r.outPeak), db (r.inRms), db (r.outRms), db (r.trim));
    }

    //------------------------------------------------------------------ UI スナップショット
    // エディタを作り、メッセージループを少し回して (タイマーで表示が更新される) から PNG に撮る
    inline bool snapshot (juce::AudioProcessor& p, const juce::File& png, float scale = 2.0f,
                          std::function<void (juce::AudioProcessorEditor&)> beforeShot = {})
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditorAndMakeActive());
        if (ed == nullptr) return false;
        ed->setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        if (beforeShot) beforeShot (*ed);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, scale);
        png.deleteFile();
        png.getParentDirectory().createDirectory();
        juce::FileOutputStream os (png);
        return juce::PNGImageFormat().writeImageToStream (img, os);
    }

    // 子コンポーネントを再帰的に探す (擬似マウス操作のテスト用)
    template <typename T>
    inline void findChildren (juce::Component& root, std::vector<T*>& found)
    {
        for (auto* c : root.getChildren())
        {
            if (auto* t = dynamic_cast<T*> (c)) found.push_back (t);
            findChildren<T> (*c, found);
        }
    }

    // スライダーを縦にドラッグする (dy < 0 で上)。戻り値はドラッグ後の値
    inline double dragSlider (juce::Slider& s, float dy)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto c = s.getLocalBounds().getCentre().toFloat();
        auto ev = [&] (juce::Point<float> pos, bool dragged)
        {
            return juce::MouseEvent (src, pos, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &s, &s, juce::Time::getCurrentTime(),
                                     c, juce::Time::getCurrentTime(), 1, dragged);
        };
        s.mouseDown (ev (c, false));
        s.mouseDrag (ev (c.translated (0.0f, dy), true));
        s.mouseUp (ev (c.translated (0.0f, dy), true));
        return s.getValue();
    }

}

#include "LabRobust.h"

namespace lab
{
    //------------------------------------------------------------------ コマンドライン
    // render / snapshot / params / robust を処理したら終了コード (0 以上) を、どれでもなければ -1 を返す
    // robustCfg: 耐久テストで使う設定 (効果が出るパラメータ、MIDI など)
    template <typename Proc>
    int cliMain (int argc, char* argv[], double defaultSr = 48000.0, std::function<RobustConfig()> robustCfg = {})
    {
        if (argc < 2) return -1;
        juce::StringArray args;
        for (int i = 1; i < argc; ++i) args.add (juce::String::fromUTF8 (argv[i]));
        const auto cmd = args[0];

        if (cmd == "robust")
        {
            auto cfg = robustCfg ? robustCfg() : RobustConfig {};
            cfg.sampleRate = defaultSr;
            return robustMain<Proc> (cfg);
        }

        if (cmd == "params")
        {
            Proc p;
            prepare (p, defaultSr);
            printParams (p);
            return 0;
        }

        if (cmd == "render" && args.size() >= 3)
        {
            double sr = defaultSr;
            auto src = readAudio (juce::File (args[1]), sr);
            if (src.getNumSamples() == 0) { std::printf ("cannot read %s\n", args[1].toRawUTF8()); return 1; }
            Proc p;
            prepare (p, sr);
            if (! applyArgs (p, args)) return 1;
            prepare (p, sr);   // スムージングを飛ばして設定値から始める
            if (args.contains ("--plain"))
            {
                auto out = run (p, src);
                writeWav (juce::File (args[2]), out, sr);
                std::printf ("peak %+6.1f -> %+6.1f dBFS, rms %+6.1f -> %+6.1f dBFS\n",
                             db (peak (src)), db (peak (out)), db (rms (src)), db (rms (out)));
            }
            else
            {
                printAB (writeAB (p, src, sr, juce::File (args[2])));
            }
            return 0;
        }

        if (cmd == "snapshot" && args.size() >= 2)
        {
            double sr = defaultSr;
            Proc p;
            prepare (p, sr);
            if (! applyArgs (p, args)) return 1;
            prepare (p, sr);
            if (args.size() >= 3 && ! args[2].contains ("="))
            {
                auto src = readAudio (juce::File (args[2]), sr);
                if (src.getNumSamples() > 0)
                {
                    prepare (p, sr);
                    run (p, tile (src, 2));
                }
            }
            const bool ok = snapshot (p, juce::File (args[1]));
            std::printf ("%s %s\n", ok ? "wrote" : "FAILED", args[1].toRawUTF8());
            return ok ? 0 : 1;
        }

        return -1;
    }
}
