# Third-party code

| Component | Used for | Licence |
|---|---|---|
| [JUCE 8.0.4](https://github.com/juce-framework/JUCE) | App framework, audio | AGPL-3.0 or commercial JUCE licence |
| [QM-DSP](https://github.com/c4dm/qm-dsp) (commit e34a3cc) | Key detection, beat tracking | GPL-2.0-or-later |
| KISS FFT (bundled with QM-DSP) | FFT | BSD-3-Clause |

Because QM-DSP is GPL, a distributed build of Samplite Explorer must be offered under GPL-compatible
terms (with JUCE under the AGPL: AGPL-3.0), including the complete source code. A commercial JUCE
licence does not remove the QM-DSP obligation.
