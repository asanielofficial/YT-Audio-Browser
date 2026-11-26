// BPM and key estimation: filename hints first, signal analysis as a fallback.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace analysis
{
inline const juce::StringArray& keyNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray n;
        for (auto* mode : { "maj", "min" })
            for (auto* note : { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" })
                n.add (juce::String (note) + " " + mode);
        return n;
    }();
    return names;
}

inline juce::StringArray tokens (const juce::String& fileName)
{
    return juce::StringArray::fromTokens (fileName, " _-.()[]", "");
}

// "loop_128bpm", "128 BPM loop", or a lone number from 60 to 200.
inline double bpmFromName (const juce::String& name)
{
    auto t = tokens (name);
    for (int i = 0; i < t.size(); ++i)
    {
        auto s = t[i].toLowerCase();
        if (s.endsWith ("bpm"))
        {
            auto num = s.dropLastCharacters (3);
            if (num.isEmpty() && i > 0) num = t[i - 1];
            if (num.isNotEmpty() && num.containsOnly ("0123456789")) return num.getDoubleValue();
        }
    }
    for (auto& s : t)
        if (s.containsOnly ("0123456789") && s.length() >= 2)
        {
            auto v = s.getIntValue();
            if (v >= 60 && v <= 200 && s[0] != '0') return v;
        }
    return 0;
}

// Accepts Am, A#min, Bbmaj, F#minor. A bare letter is too ambiguous to count.
inline int keyFromName (const juce::String& name)
{
    auto t = tokens (name);
    for (int i = t.size(); --i >= 0;)
    {
        auto s = t[i];
        auto first = juce::CharacterFunctions::toUpperCase (s[0]);
        if (first < 'A' || first > 'G') continue;
        const int bases[] = { 9, 11, 0, 2, 4, 5, 7 };
        int note = bases[first - 'A'], pos = 1;
        if (s[pos] == '#') { ++note; ++pos; }
        else if (s[pos] == 'b') { --note; ++pos; }
        auto mode = s.substring (pos);
        auto lower = mode.toLowerCase();
        const bool minor = mode == "m" || lower == "min" || lower == "minor";
        const bool major = lower == "maj" || lower == "major";
        if (minor || major) return (note + 12) % 12 + (minor ? 12 : 0);
    }
    return -1;
}

// Onset-strength autocorrelation. Returns 0 if there is no clear pulse.
inline double detectBpm (const float* x, int n, double sr)
{
    if (n < sr * 2.0) return 0;
    const int hop = 256, win = 2048; // a long window hides beating between notes
    const double fps = sr / hop;
    std::vector<double> onset;
    double prev = -1;
    int hits = 0;
    for (int i = 0; i + win <= n; i += hop)
    {
        double e = 0;
        for (int j = 0; j < win; ++j) e += (double) x[i + j] * x[i + j];
        e = std::log1p (1000.0 * std::sqrt (e / win));
        const double rise = prev < 0 ? 0.0 : std::max (0.0, e - prev);
        if (rise > 0.2) ++hits;
        onset.push_back (rise);
        prev = e;
    }
    if (hits < 4) return 0; // sustained material has no beat to measure
    double mean = 0;
    for (auto v : onset) mean += v;
    mean /= (double) onset.size();
    double energy = 0;
    for (auto& v : onset) { v -= mean; energy += v * v; }
    energy /= (double) onset.size();
    if (energy < 1.0e-6) return 0;

    const int count = (int) onset.size();
    auto corr = [&] (int lag)
    {
        if (lag >= count) return 0.0;
        double s = 0;
        for (int i = lag; i < count; ++i) s += onset[(size_t) i] * onset[(size_t) (i - lag)];
        return s / (count - lag);
    };

    double best = 0, bestBpm = 0, bestRaw = 0;
    for (double bpm = 70.0; bpm <= 180.0; bpm += 0.5)
    {
        const double lag = fps * 60.0 / bpm;
        double raw = 0;
        for (int mult = 1; mult <= 4; ++mult)
        {
            const double l = lag * mult;
            const int a = (int) l;
            raw += corr (a) + (l - a) * (corr (a + 1) - corr (a));
        }
        raw /= 4.0;
        const double octave = std::log2 (bpm / 120.0);
        const double score = raw * std::exp (-0.5 * octave * octave); // favour common tempos
        if (score > best) { best = score; bestBpm = bpm; bestRaw = raw; }
    }
    return bestRaw / energy > 0.1 ? std::round (bestBpm) : 0;
}

// Share of 55-2000 Hz energy held in narrow spectral peaks: near 1 for pitched
// material, near 0 for drums and noise.
inline double tonalness (const float* x, int n, double sr)
{
    const int order = 14, size = 1 << order;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<float> buf ((size_t) size * 2), nearby;
    const int lo = (int) (55.0 * size / sr), hi = std::min (size / 2 - 40, (int) (2000.0 * size / sr));
    double inPeaks = 0, total = 0;
    for (int start = 0; start == 0 || start < n - size / 4; start += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        std::copy (x + start, x + std::min (n, start + size), buf.begin());
        window.multiplyWithWindowingTable (buf.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k < size / 2; ++k) buf[(size_t) k] *= buf[(size_t) k]; // power
        for (int k = lo; k < hi; ++k)
        {
            total += buf[(size_t) k];
            const float p = buf[(size_t) k];
            if (k < 2 || p <= buf[(size_t) k - 1] || p < buf[(size_t) k + 1]) continue;
            nearby.assign (buf.begin() + std::max (0, k - 30), buf.begin() + k + 30);
            std::nth_element (nearby.begin(), nearby.begin() + (long) nearby.size() / 2, nearby.end());
            if (p > 8.0f * nearby[nearby.size() / 2])
                for (int j = k - 2; j <= k + 2; ++j) inPeaks += buf[(size_t) j];
        }
    }
    return total > 0 ? std::min (1.0, inPeaks / total) : 0.0;
}

// Chroma vector matched against Krumhansl key profiles. Returns -1 if unclear.
inline int detectKey (const float* x, int n, double sr)
{
    const int order = 14, size = 1 << order;
    if (n < size / 2) return -1;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<float> buf ((size_t) size * 2);
    double chroma[12] = {};
    for (int start = 0; start < n; start += size / 2)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        std::copy (x + start, x + std::min (n, start + size), buf.begin());
        window.multiplyWithWindowingTable (buf.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 2; k < size / 2; ++k)
        {
            const double f = k * sr / size;
            if (f < 55.0 || f > 2000.0) continue;
            if (buf[(size_t) k] <= buf[(size_t) k - 1] || buf[(size_t) k] < buf[(size_t) k + 1]) continue; // peaks only
            const int midi = juce::roundToInt (69.0 + 12.0 * std::log2 (f / 440.0));
            chroma[midi % 12] += buf[(size_t) k];
        }
    }
    double total = 0;
    int used = 0;
    for (auto c : chroma) total += c;
    for (auto c : chroma) if (c > total * 0.05) ++used;
    if (total <= 0 || used < 3) return -1; // a single note does not define a key

    static const double major[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minor[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    double best = 0.5;
    int key = -1;
    for (int k = 0; k < 24; ++k)
    {
        auto* profile = k < 12 ? major : minor;
        double mp = 0, mc = total / 12.0, num = 0, dp = 0, dc = 0;
        for (int i = 0; i < 12; ++i) mp += profile[i] / 12.0;
        for (int i = 0; i < 12; ++i)
        {
            const double p = profile[i] - mp, c = chroma[(i + k) % 12] - mc;
            num += p * c; dp += p * p; dc += c * c;
        }
        const double r = num / std::sqrt (dp * dc + 1.0e-12);
        if (r > best) { best = r; key = k; }
    }
    return key;
}
// "pad_432Hz" or "pad 432 Hz". Only plausible A4 values, so "100hz" sub tones don't count.
inline double tuningFromName (const juce::String& name)
{
    auto t = tokens (name);
    for (int i = 0; i < t.size(); ++i)
    {
        auto s = t[i].toLowerCase();
        if (! s.endsWith ("hz")) continue;
        auto num = s.dropLastCharacters (2);
        if (num.isEmpty() && i > 0) num = t[i - 1];
        const double v = num.getDoubleValue();
        if (num.containsOnly ("0123456789") && v >= 400 && v <= 480) return v;
    }
    return 0;
}

// Estimates the A4 reference: how far spectral peaks sit from the A = 440 semitone
// grid, averaged on a circle. Range is 440 +/- half a semitone. Returns 0 if unclear.
inline double detectTuning (const float* x, int n, double sr)
{
    if (n < sr * 0.1) return 0;
    const int order = 15, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2);
    double re = 0, im = 0, total = 0;
    for (int start = 0; start < n; start += size / 2)
    {
        const int len = std::min (size, n - start);
        if (start > 0 && len < size / 4) break;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < len; ++i) // window the audio itself, then zero-pad
            buf[(size_t) i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / len));
        fft.performFrequencyOnlyForwardTransform (buf.data());

        const int lo = std::max (2, (int) (50.0 * size / sr)), hi = std::min (size / 2 - 1, (int) (5000.0 * size / sr));
        float top = 0;
        for (int k = lo; k < hi; ++k) top = std::max (top, buf[(size_t) k]);
        if (top <= 1.0f) continue; // near silence

        for (int k = lo; k < hi; ++k)
        {
            const float m = buf[(size_t) k];
            if (m < top * 0.1f || m <= buf[(size_t) k - 1] || m < buf[(size_t) k + 1]) continue;
            // Parabolic fit on log magnitude for sub-bin accuracy.
            const double a = std::log (buf[(size_t) k - 1] + 1.0e-9), b = std::log (m + 1.0e-9),
                         c = std::log (buf[(size_t) k + 1] + 1.0e-9);
            const double offset = 0.5 * (a - c) / (a - 2.0 * b + c - 1.0e-12);
            const double semitones = 12.0 * std::log2 ((k + offset) * sr / size / 440.0);
            const double angle = juce::MathConstants<double>::twoPi * semitones;
            re += m * std::cos (angle);
            im += m * std::sin (angle);
            total += m;
        }
    }
    if (total <= 0 || std::sqrt (re * re + im * im) / total < 0.8) return 0; // peaks disagree
    const double semitones = std::atan2 (im, re) / juce::MathConstants<double>::twoPi;
    return 440.0 * std::pow (2.0, semitones / 12.0);
}
} // namespace analysis
