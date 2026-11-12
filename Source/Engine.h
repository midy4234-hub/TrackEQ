#pragma once

// TrackEQ の音の処理
//
//   各バンド = 1 本のベル (カットだけ) のダイナミック EQ。ベルの中心周波数が、指定した範囲
//   (Freq ± Track 半音) の中でいちばん強いスペクトルの山を追いかける。Track = 0 なら普通の固定ダイナミック EQ。
//
//   - 山の位置: 入力の STFT (Hann、hop = N/8) の範囲内の極大のうち最大のもの。放物線補間で周波数を出す。
//     今追っている山が残っているなら、別の山が 3 dB 以上上回るまで乗り換えない
//   - 音の経路: 入力を N/2 サンプル遅らせ (STFT の窓の中心と時刻を合わせる)、TPT の SVF ベルを直列に通す。
//     レイテンシは N/2 (48 kHz で 1024 サンプル)
//   - 検出: 遅らせた原音を、追っている周波数のバンドパス (ベルと同じ Q) に通した音量。
//     しきい値を超えた分を Ratio で、最大 |Gain| dB まで動かす (Gain が負なら下げる、正なら持ち上げる)。Relative のときは全体の音量との差で見る

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace trackeq
{
    constexpr int numBands = 5;
    constexpr int maxChannels = 2;

    struct BandParams
    {
        bool  on = false, listen = false;
        float freq = 1000.0f, track = 3.0f, q = 8.0f;
        float thresh = -24.0f, ratio = 4.0f, gain = -12.0f;   // gain: しきい値を超えたときに動かす最大量 (負 = 下げる、正 = 持ち上げる)
        float attackMs = 5.0f, releaseMs = 100.0f;
    };

    // TPT の状態変数フィルター (Simper)。係数を毎サンプル変えても安定
    struct Svf
    {
        double ic1 = 0.0, ic2 = 0.0;
        void reset() { ic1 = ic2 = 0.0; }
        // 戻り値はバンドパスの v1 (ピークの利得 1/k)
        inline double tick (double v0, double a1, double a2, double a3)
        {
            const double v3 = v0 - ic2;
            const double v1 = a1 * ic1 + a2 * v3;
            const double v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            return v1;
        }
    };

    class Engine
    {
    public:
        void prepare (double sampleRate, int channels)
        {
            sr = sampleRate;
            numCh = juce::jlimit (1, maxChannels, channels);
            order = sr <= 50000.0 ? 11 : (sr <= 100000.0 ? 12 : 13);
            N = 1 << order;
            hop = N / 8;
            binHz = sr / N;
            fft = std::make_unique<juce::dsp::FFT> (order);
            window.resize ((size_t) N);
            for (int i = 0; i < N; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) N);
            for (auto& r : ring) r.assign ((size_t) N, 0.0f);
            fftBuf.assign ((size_t) (2 * N), 0.0f);
            power.assign ((size_t) (N / 2 + 1), 0.0f);
            if (specSize != N / 2 + 1)
            {
                specSize = N / 2 + 1;
                spectrum = std::make_unique<std::atomic<float>[]> ((size_t) specSize);
            }
            for (int i = 0; i < specSize; ++i) spectrum[(size_t) i].store (0.0f, std::memory_order_relaxed);
            ringPos = 0;
            hopCount = 0;

            delayLen = N / 2;
            for (auto& d : delay) d.assign ((size_t) delayLen, 0.0f);
            delayPos = 0;

            const auto coef = [this] (double ms) { return 1.0 - std::exp (-1.0 / (sr * ms * 0.001)); };
            cFreq = coef (8.0);
            cQ = coef (20.0);
            cFade = 1.0 / (sr * 0.010);
            cWide = coef (50.0);
            widePow = 0.0;
            listenMix = 0.0;
            for (auto& b : bands) b.reset();
            first = true;
        }

        int latency() const { return delayLen; }
        int fftSize() const { return N; }
        double binWidth() const { return binHz; }

        // UI 用 (どのスレッドから読んでもよい)
        float trackedFreq (int b) const { return bands[(size_t) b].uiFreq.load (std::memory_order_relaxed); }
        float gainDb (int b) const { return bands[(size_t) b].uiGr.load (std::memory_order_relaxed); }
        int   spectrumSize() const { return specSize; }
        float spectrumAmp (int k) const { return spectrum[(size_t) k].load (std::memory_order_relaxed); }

        // io[ch][0..n) を書き換える。bypassTarget: 1 で素通し (遅延だけ)
        void process (float* const* io, int n, const std::array<BandParams, numBands>& prm, bool relative, bool bypass)
        {
            if (first)
            {
                for (int b = 0; b < numBands; ++b)
                {
                    auto& s = bands[(size_t) b];
                    s.logF = s.targetLogF = std::log2 ((double) clampFreq (prm[(size_t) b].freq));
                    s.q = prm[(size_t) b].q;
                    s.onMix = prm[(size_t) b].on ? 1.0 : 0.0;
                    s.listenMix = prm[(size_t) b].listen ? 1.0 : 0.0;
                    s.sgn = prm[(size_t) b].gain < 0.0f ? -1.0 : 1.0;
                }
                bypassMix = bypass ? 1.0 : 0.0;
                first = false;
            }

            // ブロックごとに変わらないもの
            struct Blk { double atk, rel, slope, thr, range; bool live; };
            std::array<Blk, numBands> blk;
            for (int b = 0; b < numBands; ++b)
            {
                const auto& p = prm[(size_t) b];
                auto& s = bands[(size_t) b];
                blk[(size_t) b].atk = 1.0 - std::exp (-1.0 / (sr * juce::jmax (0.05f, p.attackMs) * 0.001));
                blk[(size_t) b].rel = 1.0 - std::exp (-1.0 / (sr * juce::jmax (1.0f, p.releaseMs) * 0.001));
                blk[(size_t) b].slope = 1.0 - 1.0 / juce::jmax (1.0, (double) p.ratio);
                blk[(size_t) b].thr = p.thresh;
                blk[(size_t) b].range = std::abs (p.gain);
                const bool live = p.on || p.listen || s.onMix > 0.0 || s.listenMix > 0.0;
                if (live && ! s.wasLive) s.resetFilters();
                s.wasLive = live;
                blk[(size_t) b].live = live;
                if (! live) { s.gr = 0.0; s.uiGr.store (0.0f, std::memory_order_relaxed); }
                // 手で動かした中心 (追従なし、または山が見つかる前) はここで反映
                if (p.track <= 0.0f || ! s.hasPeak)
                    s.targetLogF = std::log2 ((double) clampFreq (p.freq));
            }

            const double bypassTarget = bypass ? 1.0 : 0.0;

            for (int i = 0; i < n; ++i)
            {
                // --- 解析 (遅らせる前の入力)
                for (int ch = 0; ch < numCh; ++ch)
                    ring[(size_t) ch][(size_t) ringPos] = io[ch][i];
                ringPos = (ringPos + 1) & (N - 1);
                if (++hopCount >= hop)
                {
                    hopCount = 0;
                    analyse (prm);
                }

                // --- 遅延
                double x[maxChannels] = { 0.0, 0.0 };
                for (int ch = 0; ch < numCh; ++ch)
                {
                    auto& d = delay[(size_t) ch][(size_t) delayPos];
                    x[ch] = d;
                    d = io[ch][i];
                }
                if (++delayPos >= delayLen) delayPos = 0;

                double wp = 0.0;
                for (int ch = 0; ch < numCh; ++ch) wp = juce::jmax (wp, x[ch] * x[ch]);
                widePow += cWide * (wp - widePow);
                const double wideDb = 10.0 * std::log10 (2.0 * widePow + 1.0e-12);

                double y[maxChannels] = { x[0], x[1] };
                double lis[maxChannels] = { 0.0, 0.0 };
                double anyListen = 0.0;

                for (int b = 0; b < numBands; ++b)
                {
                    if (! blk[(size_t) b].live) continue;
                    const auto& p = prm[(size_t) b];
                    auto& s = bands[(size_t) b];

                    s.logF += cFreq * (s.targetLogF - s.logF);
                    s.q += cQ * ((double) p.q - s.q);
                    s.onMix = step (s.onMix, p.on ? 1.0 : 0.0, cFade);
                    s.listenMix = step (s.listenMix, p.listen ? 1.0 : 0.0, cFade);

                    const double f = std::exp2 (s.logF);
                    const double g = std::tan (juce::MathConstants<double>::pi * f / sr);

                    // 検出: バンドパス (ピークの利得 1)
                    const double kd = 1.0 / s.q;
                    const double d1 = 1.0 / (1.0 + g * (g + kd)), d2 = g * d1, d3 = g * d2;
                    double pw = 0.0;
                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        const double bp = kd * s.det[ch].tick (x[ch], d1, d2, d3);
                        pw = juce::jmax (pw, bp * bp);
                        lis[ch] += bp * s.listenMix;
                    }
                    anyListen = juce::jmax (anyListen, s.listenMix);
                    // 音量の平均: 3 周期 (1〜20 ms)
                    const double tau = juce::jlimit (0.001, 0.020, 3.0 / f);
                    s.pow += juce::jmin (1.0, 1.0 / (sr * tau)) * (pw - s.pow);
                    const double levDb = 10.0 * std::log10 (2.0 * s.pow + 1.0e-12);   // サイン波ならピーク値の dB

                    double over;
                    if (relative)
                        over = wideDb > -90.0 ? (levDb - wideDb) - blk[(size_t) b].thr : -100.0;
                    else
                        over = levDb - blk[(size_t) b].thr;

                    // 6 dB のソフトニー
                    constexpr double knee = 6.0;
                    double red = 0.0;
                    if (over >= knee * 0.5)        red = over * blk[(size_t) b].slope;
                    else if (over > -knee * 0.5)   red = blk[(size_t) b].slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
                    // s.gr は動かす量の大きさ (0 以上)。向きは sgn (-1 = 下げる、+1 = 持ち上げる) を 10 ms でなめらかに切り替える
                    const double target = juce::jmin (blk[(size_t) b].range, red);
                    s.gr += (target > s.gr ? blk[(size_t) b].atk : blk[(size_t) b].rel) * (target - s.gr);
                    if (s.gr < 1.0e-4 && target == 0.0) s.gr = 0.0;
                    s.sgn = step (s.sgn, p.gain < 0.0f ? -1.0 : 1.0, 2.0 * cFade);

                    // ベル
                    const double gdb = s.gr * s.sgn * s.onMix;
                    if (gdb != s.lastDb) { s.A = std::pow (10.0, gdb / 40.0); s.lastDb = gdb; }
                    const double A = s.A;
                    const double k = 1.0 / (s.q * A);
                    const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
                    const double m1 = k * (A * A - 1.0);
                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        const double v1 = s.bell[ch].tick (y[ch], a1, a2, a3);
                        if (m1 != 0.0) y[ch] += m1 * v1;
                    }

                    if (std::abs (gdb) > std::abs (s.grMin)) s.grMin = gdb;
                }

                bypassMix = step (bypassMix, bypassTarget, cFade);
                listenMix = step (listenMix, anyListen > 0.0 ? 1.0 : 0.0, cFade);
                for (int ch = 0; ch < numCh; ++ch)
                {
                    double o = y[ch];
                    if (listenMix > 0.0) o += (lis[ch] - o) * listenMix;
                    if (bypassMix > 0.0) o += (x[ch] - o) * bypassMix;
                    io[ch][i] = (float) o;
                }

                if (++uiCount >= 256)
                {
                    uiCount = 0;
                    for (auto& s : bands)
                    {
                        s.uiFreq.store ((float) std::exp2 (s.logF), std::memory_order_relaxed);
                        s.uiGr.store ((float) s.grMin, std::memory_order_relaxed);
                        s.grMin = 0.0;
                    }
                }
            }
        }

    private:
        struct Band
        {
            Svf det[maxChannels], bell[maxChannels];
            double logF = 10.0, targetLogF = 10.0, q = 8.0;
            double onMix = 0.0, listenMix = 0.0, sgn = -1.0;
            double pow = 0.0, gr = 0.0, A = 1.0, lastDb = 0.0, grMin = 0.0;
            bool hasPeak = false, wasLive = false;
            std::atomic<float> uiFreq { 1000.0f }, uiGr { 0.0f };

            void resetFilters()
            {
                for (auto& f : det) f.reset();
                for (auto& f : bell) f.reset();
                pow = 0.0; gr = 0.0; A = 1.0; lastDb = 0.0;
            }
            void reset() { resetFilters(); hasPeak = false; wasLive = false; grMin = 0.0; onMix = listenMix = 0.0; }
        };

        static double step (double cur, double target, double inc)
        {
            if (cur < target) return juce::jmin (target, cur + inc);
            if (cur > target) return juce::jmax (target, cur - inc);
            return cur;
        }

        float clampFreq (float f) const { return juce::jlimit (20.0f, (float) (sr * 0.45), f); }

        // 1 フレームぶんのスペクトルを作り、各バンドの追う先を決める
        void analyse (const std::array<BandParams, numBands>& prm)
        {
            std::fill (power.begin(), power.end(), 0.0f);
            for (int ch = 0; ch < numCh; ++ch)
            {
                const auto& r = ring[(size_t) ch];
                for (int i = 0; i < N; ++i)
                    fftBuf[(size_t) i] = r[(size_t) ((ringPos + i) & (N - 1))] * window[(size_t) i];
                std::fill (fftBuf.begin() + N, fftBuf.end(), 0.0f);
                fft->performFrequencyOnlyForwardTransform (fftBuf.data(), true);
                for (int k = 0; k <= N / 2; ++k)
                    power[(size_t) k] += fftBuf[(size_t) k] * fftBuf[(size_t) k];
            }
            // 振幅 (サイン波のピーク値) に直す係数: Hann の和 = N/2
            const float norm = 2.0f / (float) (N / 2);
            for (int k = 0; k <= N / 2; ++k)
                spectrum[(size_t) k].store (std::sqrt (power[(size_t) k]) * norm, std::memory_order_relaxed);

            const float floorPow = std::pow (10.0f, -80.0f / 10.0f) / (norm * norm);   // -80 dBFS より下は追わない

            for (int b = 0; b < numBands; ++b)
            {
                const auto& p = prm[(size_t) b];
                auto& s = bands[(size_t) b];
                if (p.track <= 0.0f || ! (p.on || p.listen)) { s.hasPeak = false; continue; }

                const double fc = clampFreq (p.freq);
                const double lo = fc * std::exp2 (-p.track / 12.0), hi = juce::jmin (sr * 0.45, fc * std::exp2 (p.track / 12.0));
                const int kLo = juce::jmax (2, (int) std::ceil (lo / binHz));
                const int kHi = juce::jmin (N / 2 - 2, (int) std::floor (hi / binHz));

                const double prev = std::exp2 (s.targetLogF);
                const double nearBins = juce::jmax (2.0, prev * (std::exp2 (60.0 / 1200.0) - 1.0) / binHz);
                int best = -1, cont = -1;
                for (int k = kLo; k <= kHi; ++k)
                {
                    const float m = power[(size_t) k];
                    if (m < floorPow || m < power[(size_t) (k - 1)] || m < power[(size_t) (k + 1)]) continue;
                    if (best < 0 || m > power[(size_t) best]) best = k;
                    if (s.hasPeak && std::abs (k * binHz - prev) <= nearBins * binHz)
                        if (cont < 0 || m > power[(size_t) cont]) cont = k;
                }
                if (best < 0)   // 山が無いときは今の位置のまま (範囲の外に出ていたら端へ)
                {
                    s.targetLogF = juce::jlimit (std::log2 (lo), std::log2 (hi), s.targetLogF);
                    continue;
                }
                // 乗り換えは 3 dB (パワーで 2 倍) 上回ってから
                int pick = best;
                if (cont >= 0 && power[(size_t) best] < 2.0f * power[(size_t) cont]) pick = cont;

                const double a = 10.0 * std::log10 ((double) power[(size_t) (pick - 1)] + 1.0e-30);
                const double c0 = 10.0 * std::log10 ((double) power[(size_t) pick] + 1.0e-30);
                const double c = 10.0 * std::log10 ((double) power[(size_t) (pick + 1)] + 1.0e-30);
                const double den = a - 2.0 * c0 + c;
                const double delta = std::abs (den) > 1.0e-9 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den) : 0.0;
                const double f = juce::jlimit (lo, hi, (pick + delta) * binHz);
                s.targetLogF = std::log2 (f);
                s.hasPeak = true;
            }
        }

        double sr = 48000.0, binHz = 23.4;
        int numCh = 2, order = 11, N = 2048, hop = 256;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, fftBuf, power;
        std::array<std::vector<float>, maxChannels> ring, delay;
        std::unique_ptr<std::atomic<float>[]> spectrum;
        int specSize = 0, ringPos = 0, hopCount = 0, delayLen = 1024, delayPos = 0, uiCount = 0;

        std::array<Band, numBands> bands;
        double cFreq = 0.0, cQ = 0.0, cFade = 0.0, cWide = 0.0;
        double widePow = 0.0, listenMix = 0.0, bypassMix = 0.0;
        bool first = true;
    };
}
