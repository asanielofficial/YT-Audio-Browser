// The detection the app uses: QM-DSP where it measured better, our own code elsewhere.
#pragma once

namespace detect
{
// Tempo in BPM, 0 if unclear. Loops (up to 30 s) use the onset autocorrelation in
// Analysis.h; longer material uses the Queen Mary beat tracker.
double bpm (const float* mono, int numSamples, double sampleRate);

// Queen Mary beat tracker on its own. Returns 0 if it finds too few beats.
double qmBpm (const float* mono, int numSamples, double sampleRate);

// 0-23 (see analysis::keyNames), -1 if unclear. a4 is the tuning reference in Hz.
int key (const float* mono, int numSamples, double sampleRate, double a4);
} // namespace detect
