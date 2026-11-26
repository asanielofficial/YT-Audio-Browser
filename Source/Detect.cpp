#include "Detect.h"
#include "Analysis.h"

#include <algorithm>
#include <array>
#include <vector>

#include "dsp/keydetection/GetKeyMode.h"
#include "dsp/onsets/DetectionFunction.h"
#include "dsp/tempotracking/TempoTrackV2.h"

namespace detect
{
double qmBpm (const float* x, int n, double sr)
{
    // About 11.6 ms per detection-function step, as the tracker expects.
    const int hop = juce::roundToInt (sr * 0.01161);
    int win = 2;
    while (win < hop * 2) win *= 2;

    DFConfig config { hop, win, DF_COMPLEXSD, 3.0, false, -1.0, -1.0 };
    DetectionFunction function (config);
    std::vector<double> frame ((size_t) win), df;
    for (int i = 0; i + win <= n; i += hop)
    {
        std::copy (x + i, x + i + win, frame.begin());
        df.push_back (function.processTimeDomain (frame.data()));
    }
    if (df.size() < 520) return 0; // the tracker needs about 6 s
    df.erase (df.begin(), df.begin() + 2);

    std::vector<double> period (df.size()), tempi, beats;
    TempoTrackV2 tracker ((float) sr, hop);
    tracker.calculateBeatPeriod (df, period, tempi);
    tracker.calculateBeats (df, period, beats);
    if (beats.size() < 4) return 0;

    // Average the gaps near the median: finer than the median alone.
    std::vector<double> gaps;
    for (size_t i = 1; i < beats.size(); ++i) gaps.push_back (beats[i] - beats[i - 1]);
    std::sort (gaps.begin(), gaps.end());
    const double median = gaps[gaps.size() / 2];
    double sum = 0;
    int used = 0;
    for (auto g : gaps)
        if (std::abs (g - median) <= median * 0.15) { sum += g; ++used; }
    return used > 0 && sum > 0 ? 60.0 * sr / (sum / used * hop) : 0;
}

double bpm (const float* x, int n, double sr)
{
    // Our estimator also rejects material with no beat, so it gates both paths.
    const double own = analysis::detectBpm (x, n, sr);
    if (own <= 0 || n <= sr * 30.0) return own;
    const double qm = qmBpm (x, n, sr);
    return qm > 0 ? qm : own;
}

int key (const float* x, int n, double sr, double a4)
{
    // Leave single notes, drums and noise blank: QM always names some key.
    const int head = std::min (n, (int) (sr * 30.0));
    if (analysis::tonalness (x, head, sr) < 0.35 || analysis::detectKey (x, head, sr) < 0) return -1;

    GetKeyMode detector (GetKeyMode::Config (sr, (float) (a4 > 0 ? a4 : 440.0)));
    const int block = detector.getBlockSize(), hop = detector.getHopSize();
    std::vector<double> buffer ((size_t) block);
    std::array<int, 25> votes {};
    for (int start = 0; start < n; start += hop)
    {
        for (int i = 0; i < block; ++i)
            buffer[(size_t) i] = start + i < n ? x[start + i] : 0.0;
        const int result = detector.process (buffer.data());
        if (result > 0 && result <= 24) ++votes[(size_t) result];
    }
    const auto best = (int) (std::max_element (votes.begin(), votes.end()) - votes.begin());
    // QM counts 1-12 as C..B major and 13-24 as C..B minor.
    return votes[(size_t) best] > 0 ? best - 1 : -1;
}
} // namespace detect
