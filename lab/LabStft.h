#pragma once

// PluginLab 共通のストリーミング STFT エンジン (knowledge/stft.md の結論どおり: Hann + Hann、重なり 75 %)
//
//   lab::Stft stft;
//   stft.prepare (2048, 512, numChannels, numOutputs);
//   stft.process (buffer, [&] (int ch, const std::complex<float>* in, int bins, lab::Stft::Outputs& out)
//   {
//       auto* y = out.spectrum (0);      // 出力 0 を使う (呼んだ出力だけ逆変換する)
//       for (int k = 0; k < bins; ++k) y[k] = in[k];
//   });
//   // 出力 0 は buffer に、1 以上は getOutput (o) に入る
//
// - レイテンシは fftSize サンプル (getLatencySamples で申告する)
// - フレームは hop ごと。ブロックの区切りとは無関係に内部のカウンターで数える (ブロックサイズで結果が変わらない)
// - 出力を複数持てる (帯域ごとに分けて時間領域で開閉する、など)。そのフレームで spectrum() を呼ばなかった出力は
//   逆変換を省く (= そのフレームは 0 を足す)
// - frameStart(): いま処理しているフレームが入力のどこか (入力の通し番号で、窓の先頭)。グリッドとの対応づけに使う

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <functional>
#include <vector>

namespace lab
{
    class Stft
    {
    public:
        class Outputs
        {
        public:
            std::complex<float>* spectrum (int o)
            {
                used[(size_t) o] = true;
                auto& s = spec[(size_t) o];
                std::fill (s.begin(), s.end(), std::complex<float> {});
                return s.data();
            }
            bool isUsed (int o) const { return used[(size_t) o]; }

        private:
            friend class Stft;
            std::vector<std::vector<std::complex<float>>> spec;
            std::vector<bool> used;
        };

        using FrameFn = std::function<void (int ch, const std::complex<float>* in, int bins, Outputs& out)>;

        void prepare (int fftSize, int hopSize, int numChannels, int numOutputs = 1)
        {
            n = fftSize;
            hop = hopSize;
            channels = numChannels;
            outputs = numOutputs;
            bins = n / 2 + 1;
            order = 0;
            while ((1 << order) < n) ++order;
            jassert ((1 << order) == n && n % hop == 0);
            fft = std::make_unique<juce::dsp::FFT> (order);

            window.assign ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
                window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / n));   // 周期的 Hann
            // Hann × Hann を hop ずつ重ねた和で割る (75 % なら一定 = 1.5)
            double acc = 0.0;
            for (int i = 0; i < hop; ++i)
            {
                double s = 0.0;
                for (int k = i; k < n; k += hop)
                    s += (double) window[(size_t) k] * window[(size_t) k];
                acc += s;
            }
            olaNorm = (float) (hop / acc);

            inFifo.assign ((size_t) channels, std::vector<float> ((size_t) n, 0.0f));
            olaAcc.assign ((size_t) channels * (size_t) outputs, std::vector<float> ((size_t) n, 0.0f));
            fftBuf.assign ((size_t) (2 * n), 0.0f);
            outs.spec.assign ((size_t) outputs, std::vector<std::complex<float>> ((size_t) bins));
            outs.used.assign ((size_t) outputs, false);
            inSpec.assign ((size_t) bins, {});
            extra.clear();
            reset();
        }

        void reset()
        {
            for (auto& f : inFifo) std::fill (f.begin(), f.end(), 0.0f);
            for (auto& a : olaAcc) std::fill (a.begin(), a.end(), 0.0f);
            fill = 0;
            inputCount = 0;
            frameStartSample = 0;
        }

        int getLatencySamples() const { return n; }
        int getFftSize() const { return n; }
        int getHop() const { return hop; }
        int getNumBins() const { return bins; }
        int64_t frameStart() const { return frameStartSample; }

        // buffer を処理する。出力 0 は buffer に上書き、出力 1 以上は getOutput (o) で読む (同じ長さ)
        void process (juce::AudioBuffer<float>& buffer, const FrameFn& fn)
        {
            const int len = buffer.getNumSamples();
            const int nch = std::min (channels, buffer.getNumChannels());
            if ((int) extra.size() != outputs - 1)
                extra.assign ((size_t) std::max (0, outputs - 1), juce::AudioBuffer<float>());
            for (auto& e : extra)
                e.setSize (nch, len, false, false, true);

            int pos = 0;
            while (pos < len)
            {
                const int take = std::min (hop - fill, len - pos);
                for (int ch = 0; ch < nch; ++ch)
                {
                    auto& fifo = inFifo[(size_t) ch];
                    // 入力を右端に詰める (直近 n サンプル)
                    std::copy (fifo.begin() + take, fifo.end(), fifo.begin());
                    const float* src = buffer.getReadPointer (ch, pos);
                    std::copy (src, src + take, fifo.end() - take);
                    for (int o = 0; o < outputs; ++o)
                    {
                        const auto& acc = olaAcc[(size_t) (ch * outputs + o)];
                        float* dst = o == 0 ? buffer.getWritePointer (ch, pos) : extra[(size_t) (o - 1)].getWritePointer (ch, pos);
                        std::copy (acc.begin() + fill, acc.begin() + fill + take, dst);
                    }
                }
                fill += take;
                pos += take;
                inputCount += take;
                if (fill == hop)
                {
                    frameStartSample = inputCount - n;
                    for (int ch = 0; ch < nch; ++ch)
                        frame (ch, fn);
                    fill = 0;
                }
            }
        }

        const juce::AudioBuffer<float>& getOutput (int o) const { return extra[(size_t) (o - 1)]; }

    private:
        void frame (int ch, const FrameFn& fn)
        {
            auto& fifo = inFifo[(size_t) ch];
            std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
            for (int i = 0; i < n; ++i)
                fftBuf[(size_t) i] = fifo[(size_t) i] * window[(size_t) i];
            fft->performRealOnlyForwardTransform (fftBuf.data(), true);
            for (int k = 0; k < bins; ++k)
                inSpec[(size_t) k] = { fftBuf[(size_t) (2 * k)], fftBuf[(size_t) (2 * k + 1)] };

            std::fill (outs.used.begin(), outs.used.end(), false);
            fn (ch, inSpec.data(), bins, outs);

            for (int o = 0; o < outputs; ++o)
            {
                auto& acc = olaAcc[(size_t) (ch * outputs + o)];
                // hop 出したぶんを捨てて左へ
                std::copy (acc.begin() + hop, acc.end(), acc.begin());
                std::fill (acc.end() - hop, acc.end(), 0.0f);
                if (! outs.used[(size_t) o])
                    continue;
                const auto& s = outs.spec[(size_t) o];
                for (int k = 0; k < bins; ++k)
                {
                    fftBuf[(size_t) (2 * k)] = s[(size_t) k].real();
                    fftBuf[(size_t) (2 * k + 1)] = s[(size_t) k].imag();
                }
                for (int k = 2 * bins; k < 2 * n; ++k)
                    fftBuf[(size_t) k] = 0.0f;
                fft->performRealOnlyInverseTransform (fftBuf.data());
                for (int i = 0; i < n; ++i)
                    acc[(size_t) i] += fftBuf[(size_t) i] * window[(size_t) i] * olaNorm;
            }
        }

        int n = 2048, hop = 512, channels = 2, outputs = 1, bins = 1025, order = 11, fill = 0;
        int64_t inputCount = 0, frameStartSample = 0;
        float olaNorm = 1.0f;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftBuf;
        std::vector<std::vector<float>> inFifo, olaAcc;
        std::vector<std::complex<float>> inSpec;
        Outputs outs;
        std::vector<juce::AudioBuffer<float>> extra;
    };
}
