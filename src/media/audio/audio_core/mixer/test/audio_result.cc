// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/media/audio/audio_core/mixer/test/audio_result.h"

#include <cstdio>
#include <string>

#include "src/media/audio/audio_core/mixer/test/mixer_tests_shared.h"

namespace media::audio::test {

// See audio_result.h for in-depth descriptions of these class members/consts.
//
// In summary, however:
// * For all TOLERANCE measurements, smaller is better (tighter tolerance). Measured results must be
//   WITHIN the tolerance.
// * For ALL other measurements (frequency response, SINAD, level, noise floor), larger results are
//   better (e.g. frequency response closer to 0, higher noise floor or SINAD).

//
//
// Input
//
double AudioResult::LevelToleranceSource8 = 0.0;
double AudioResult::LevelToleranceSource16 = 0.0;
double AudioResult::LevelToleranceSource24 = 0.0;
double AudioResult::LevelToleranceSourceFloat = 0.0;

constexpr double AudioResult::kPrevLevelToleranceSource8;
constexpr double AudioResult::kPrevLevelToleranceSource16;
constexpr double AudioResult::kPrevLevelToleranceSource24;
constexpr double AudioResult::kPrevLevelToleranceSourceFloat;

double AudioResult::LevelSource8 = -INFINITY;
double AudioResult::LevelSource16 = -INFINITY;
double AudioResult::LevelSource24 = -INFINITY;
double AudioResult::LevelSourceFloat = -INFINITY;

double AudioResult::FloorSource8 = -INFINITY;
double AudioResult::FloorSource16 = -INFINITY;
double AudioResult::FloorSource24 = -INFINITY;
double AudioResult::FloorSourceFloat = -INFINITY;

constexpr double AudioResult::kPrevFloorSource8;
constexpr double AudioResult::kPrevFloorSource16;
constexpr double AudioResult::kPrevFloorSource24;
constexpr double AudioResult::kPrevFloorSourceFloat;

//
//
// Rechannel
//
double AudioResult::LevelToleranceStereoMono = 0.0;
constexpr double AudioResult::kPrevLevelToleranceStereoMono;

double AudioResult::LevelStereoMono = -INFINITY;
constexpr double AudioResult::kPrevLevelStereoMono;

double AudioResult::FloorStereoMono = -INFINITY;
constexpr double AudioResult::kPrevFloorStereoMono;

//
//
// Interpolate
//
double AudioResult::LevelToleranceInterpolation = 0.0;
constexpr double AudioResult::kPrevLevelToleranceInterpolation;

std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespPointUnity = {NAN};

std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincUnity = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincDown0 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincDown1 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincDown2 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincMicro = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincUp1 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincUp2 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::FreqRespSincUp3 = {NAN};

// We test our interpolation fidelity across these six rate-conversion ratios:
// - 1:1 (referred to in these variables and constants as Unity)
// - 191999:48000, significant but not perfectly integral down-sampling (referred to as Down0)
// - 2:1, which equates to 96k -> 48k (Down1)
// - 294:160, which equates to 88.2k -> 48k (Down2)
// - 48001:48000, representing small adjustment for multi-device sync (Micro)
// - 147:160, which equates to 44.1k -> 48k (Up1)
// - 1:2, which equates to 24k -> 48k, or 48k -> 96k (Up2)
// - 12001:48000, significant but not perfectly integral up-sampling (Up3)
//
// For Frequency Response, values closer to 0 (flatter response) are desired. Below you see that for
// 1:1 and 2:1, response is near-ideal. For other rates, response drops off at higher frequencies.
//
// clang-format off
const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespPointUnity = {
       0.000,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincUnity = {
       0.000,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincDown0 = {
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
       0.000,    -0.002,    -0.002,    -0.002,     0.000,     0.000,     0.000,    -0.550,    -3.474      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincDown1 = {
       0.000,     0.000,     0.000,     0.000,     0.000,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
       0.000,    -0.002,    -0.002,    -0.002,     0.000,     0.000,     0.000,    -0.550,    -3.474      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincDown2 = {
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,
       0.000,    -0.002,    -0.002,    -0.002,     0.000,     0.000,     0.000,    -0.551,    -3.474      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincMicro = {
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,
      -0.001,    -0.001,    -0.001,    -0.001,    -0.001,     0.000,     0.000,    -0.001,     0.000,    -0.001,
       0.000,    -0.002,    -0.002,    -0.002,     0.000,     0.000,     0.000,    -0.550,    -3.474      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincUp1 = {
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,    -0.001,
      -0.001,    -0.001,    -0.001,    -0.001,     0.000,     0.000,    -0.001,    -0.001,     0.000,    -0.001,
       0.000,     0.000,     0.000,    -0.024,    -0.297,    -0.881,    -1.902,    -6.011, -INFINITY      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincUp2 = {
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,    -0.001,    -0.001,    -0.001,
      -0.001,    -0.001,     0.000,     0.000,    -0.001,     0.000,    -0.001,     0.000,    -0.001,    -0.002,
       0.000,    -5.999, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>
    AudioResult::kPrevFreqRespSincUp3 = {
       0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,     0.000,
       0.000,     0.000,     0.000,     0.000,    -0.001,    -0.001,    -0.001,    -0.001,    -0.001,     0.000,
       0.000,    -0.001,     0.000,    -0.001,     0.000,    -0.001,    -0.002,     0.000, -INFINITY, -INFINITY,
   -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY      };
// clang-format on

std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadPointUnity = {NAN};

std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincUnity = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincDown0 = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincDown1 = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincDown2 = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincMicro = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincUp1 = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincUp2 = {NAN};
std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::SinadSincUp3 = {NAN};

// We test our interpolation fidelity across these six rate-conversion ratios:
// - 1:1 (referred to in these variables and constants as Unity)
// - 191999:48000, significant but not perfectly integral down-sampling (referred to as Down0)
// - 2:1, which equates to 96k -> 48k (Down1)
// - 294:160, which equates to 88.2k -> 48k (Down2)
// - 48001:48000, representing small adjustment for multi-device sync (Micro)
// - 147:160, which equates to 44.1k -> 48k (Up1)
// - 1:2, which equates to 24k -> 48k, or 48k -> 96k (Up2)
// - 12001:48000, significant but not perfectly integral up-sampling (Up3)
//
// For SINAD, higher values (lower noise/artifacts vs. signal) are desired. For 1:1 and 2:1, SINAD
// is near-ideal. For other rates, performance drops off (lower values) at higher frequencies.
//
// clang-format off
const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadPointUnity = {
      160.000,   153.714,   153.745,   153.745,   153.714,   153.745,   153.745,   153.745,   153.745,   153.745,
      153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,
      153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,
      153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincUnity = {
      160.000,   153.714,   153.745,   153.745,   153.714,   153.745,   153.745,   153.745,   153.745,   153.745,
      153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,
      153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,
      153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745,   153.745, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincDown0 = {
      129.402,   133.966,   133.944,   133.892,   133.856,   133.825,   133.818,   133.600,   133.476,   133.162,
      132.762,   132.216,   131.286,   130.383,   129.155,   127.705,   126.118,   124.358,   122.546,   120.582,
      118.714,   116.822,   114.701,   112.782,   110.851,   108.850,   106.782,   104.845,   102.837,   100.767,
       98.830,    97.247,    94.749,    93.029,    92.809,    92.595,    92.382,    91.962,    91.445,     6.020,
        8.953,    12.817,    25.034,    49.327,    69.102,    88.851,    97.578,    94.967,    99.620,    95.011,
      113.842,   122.436      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincDown1 = {
      160.000,   138.942,   138.949,   138.935,   138.826,   138.936,   138.912,   138.866,   138.944,   138.889,
      138.905,   138.934,   138.885,   138.948,   138.862,   138.902,   138.966,   138.924,   138.879,   139.009,
      138.986,   138.942,   138.944,   138.937,   138.968,   139.014,   138.849,   139.028,   138.963,   138.882,
      138.961,   139.032,   138.955,   138.971,   138.788,   138.792,   138.910,   138.852,   136.570,     6.020,
        8.953,    12.817,    25.034,    49.324,    69.070,    88.061,    94.238,    91.785,    93.294,    88.936,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincDown2 = {
      128.762,   132.047,   131.923,   131.860,   131.675,   131.552,   131.207,   130.703,   130.031,   129.082,
      127.980,   126.790,   125.017,   123.574,   121.842,   120.024,   118.255,   116.453,   114.736,   113.041,
      111.549,   110.053,   108.181,   106.198,   104.185,   102.298,   100.195,    98.302,    96.279,    94.212,
       92.284,    90.702,    88.194,    86.473,    86.257,    86.050,    85.831,    85.417,    84.879,     6.021,
        8.954,    12.818,    25.036,    49.329,    69.104,    88.021,    91.848,    91.920, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincMicro = {
       88.988,    92.000,    92.003,    92.004,    92.008,    92.012,    92.021,    92.034,    92.055,    92.092,
       92.146,    92.223,    92.385,    92.584,    92.937,    93.532,    94.503,    96.257,    99.576,   106.674,
      101.358,    94.706,    91.390,    92.145,    98.565,    90.301,    92.595,    87.213,    87.999,    85.787,
       80.329,    76.134,    74.969,    75.913,    49.400,    44.095,    50.769,    23.713,     6.174,     3.012,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincUp1 = {
       88.989,    92.001,    92.003,    92.005,    92.009,    92.014,    92.024,    92.040,    92.065,    92.108,
       92.173,    92.265,    92.457,    92.694,    93.118,    93.835,    95.023,    97.223,   101.587,   106.990,
       98.288,    93.152,    91.160,    93.974,    96.174,    89.221,    93.525,    87.749,    84.539,    82.209,
       83.988,    78.889,    63.366,    51.199,    29.186,    19.439,    12.226,     0.020, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincUp2 = {
       88.931,    88.945,    88.951,    88.957,    88.970,    88.987,    89.023,    89.076,    89.164,    89.314,
       89.539,    89.867,    90.574,    91.490,    93.276,    96.953,   108.101,   100.196,    92.123,    88.866,
       89.904,   107.422,    88.653,    94.128,    86.840,    90.064,    88.079,    80.904,    84.836,    75.734,
       49.454,     0.044, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };

const std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrevSinadSincUp3 = {
       88.989,    92.038,    92.064,    92.089,    92.143,    92.210,    92.353,    92.570,    92.931,    93.569,
       94.575,    96.154,   100.188,   106.452,   101.313,    94.449,    91.441,    92.165,    98.614,    89.985,
       92.530,    87.251,    86.964,    85.827,    80.321,    81.931,    75.049,    49.392, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY      };
// clang-format on

constexpr double AudioResult::kPhaseTolerance;

std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhasePointUnity = {NAN};

std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincUnity = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincDown0 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincDown1 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincDown2 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincMicro = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincUp1 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincUp2 = {NAN};
std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::PhaseSincUp3 = {NAN};

// clang-format off
const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhasePointUnity = {
      0.00000,   0.00000,   0.00000,   0.00000,   0.00000,  -0.00000,   0.00000,   0.00000,   0.00000,   0.00000,
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,   0.00000,   0.00000,   0.00000,  -0.00000,
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincUnity = {
      0.00000,   0.00000,   0.00000,   0.00000,   0.00000,  -0.00000,   0.00000,   0.00000,   0.00000,   0.00000,
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,   0.00000,   0.00000,   0.00000,  -0.00000,
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincDown0 = {
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00001,  -0.00001,  -0.00001,  -0.00001,  -0.00001,
     -0.00002,  -0.00002,  -0.00003,  -0.00003,  -0.00003,  -0.00004,  -0.00004,  -0.00004,  -0.00004    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincDown1 = {
      0.00000,   0.00000,  -0.00000,  -0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
      0.00000,  -0.00000,   0.00000,   0.00000,  -0.00000,   0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,   0.00000    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincDown2 = {
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00001,  -0.00001,  -0.00001,  -0.00001,  -0.00001,  -0.00002,  -0.00002,  -0.00003,
     -0.00003,  -0.00004,  -0.00005,  -0.00006,  -0.00007,  -0.00007,  -0.00007,  -0.00007,  -0.00008    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincMicro = {
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00001,
     -0.00001,  -0.00001,  -0.00001,  -0.00001,  -0.00002,  -0.00002,  -0.00003,  -0.00003,  -0.00004,  -0.00006,
     -0.00007,  -0.00008,  -0.00011,  -0.00014,  -0.00014,  -0.00014,  -0.00015,  -0.00015,  -0.00016    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincUp1 = {
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00001,
     -0.00001,  -0.00001,  -0.00001,  -0.00002,  -0.00002,  -0.00002,  -0.00003,  -0.00004,  -0.00005,  -0.00006,
     -0.00008,  -0.00009,  -0.00012,  -0.00015,  -0.00015,  -0.00016,  -0.00016,  -0.00017, -INFINITY    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincUp2 = {
      0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
      0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,  -0.00000,   0.00000,   0.00000,   0.00000,   0.00000,
      0.00000,   0.00000,   0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,   0.00000,   0.00000,
     -0.00000,  -0.00000, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY    };

const std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrevPhaseSincUp3 = {
      0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,  -0.00000,
     -0.00000,  -0.00000,  -0.00000,  -0.00001,  -0.00001,  -0.00001,  -0.00001,  -0.00001,  -0.00002,  -0.00002,
     -0.00003,  -0.00003,  -0.00004,  -0.00006,  -0.00007,  -0.00009,  -0.00011,  -0.00014, -INFINITY, -INFINITY,
    -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY, -INFINITY    };

// clang-format on
//
//
// Scale
//
Gain::AScale AudioResult::ScaleEpsilon = 0;
constexpr float AudioResult::kMaxGainDbNonUnity;

Gain::AScale AudioResult::MinScaleNonZero = 0;
constexpr float AudioResult::kMinGainDbNonMute;

double AudioResult::DynRangeTolerance = 0.0;
constexpr double AudioResult::kPrevDynRangeTolerance;

double AudioResult::LevelEpsilonDown = -INFINITY;
constexpr double AudioResult::kPrevLevelEpsilonDown;

double AudioResult::SinadEpsilonDown = -INFINITY;
constexpr double AudioResult::kPrevSinadEpsilonDown;

double AudioResult::Level30Down = -INFINITY;
double AudioResult::Level60Down = -INFINITY;
double AudioResult::Level90Down = -INFINITY;

double AudioResult::Sinad30Down = -INFINITY;
constexpr double AudioResult::kPrevSinad30Down;

double AudioResult::Sinad60Down = -INFINITY;
constexpr double AudioResult::kPrevSinad60Down;

double AudioResult::Sinad90Down = -INFINITY;
constexpr double AudioResult::kPrevSinad90Down;

//
//
// Sum
//
double AudioResult::LevelToleranceMix8 = 0.0;
double AudioResult::LevelToleranceMix16 = 0.0;
double AudioResult::LevelToleranceMix24 = 0.0;
double AudioResult::LevelToleranceMixFloat = 0.0;

constexpr double AudioResult::kPrevLevelToleranceMix8;
constexpr double AudioResult::kPrevLevelToleranceMix16;
constexpr double AudioResult::kPrevLevelToleranceMix24;
constexpr double AudioResult::kPrevLevelToleranceMixFloat;

double AudioResult::LevelMix8 = -INFINITY;
double AudioResult::LevelMix16 = -INFINITY;
double AudioResult::LevelMix24 = -INFINITY;
double AudioResult::LevelMixFloat = -INFINITY;

constexpr double AudioResult::kPrevLevelMix8;
constexpr double AudioResult::kPrevLevelMix16;
constexpr double AudioResult::kPrevLevelMix24;
constexpr double AudioResult::kPrevLevelMixFloat;

double AudioResult::FloorMix8 = -INFINITY;
double AudioResult::FloorMix16 = -INFINITY;
double AudioResult::FloorMix24 = -INFINITY;
double AudioResult::FloorMixFloat = -INFINITY;

constexpr double AudioResult::kPrevFloorMix8;
constexpr double AudioResult::kPrevFloorMix16;
constexpr double AudioResult::kPrevFloorMix24;
constexpr double AudioResult::kPrevFloorMixFloat;

//
//
// Output
//
double AudioResult::LevelToleranceOutput8 = -INFINITY;
double AudioResult::LevelToleranceOutput16 = -INFINITY;
double AudioResult::LevelToleranceOutput24 = -INFINITY;
double AudioResult::LevelToleranceOutputFloat = -INFINITY;

constexpr double AudioResult::kPrevLevelToleranceOutput8;
constexpr double AudioResult::kPrevLevelToleranceOutput16;
constexpr double AudioResult::kPrevLevelToleranceOutput24;
constexpr double AudioResult::kPrevLevelToleranceOutputFloat;

double AudioResult::LevelOutput8 = -INFINITY;
double AudioResult::LevelOutput16 = -INFINITY;
double AudioResult::LevelOutput24 = -INFINITY;
double AudioResult::LevelOutputFloat = -INFINITY;

constexpr double AudioResult::kPrevLevelOutput8;
constexpr double AudioResult::kPrevLevelOutput16;
constexpr double AudioResult::kPrevLevelOutput24;
constexpr double AudioResult::kPrevLevelOutputFloat;

double AudioResult::FloorOutput8 = -INFINITY;
double AudioResult::FloorOutput16 = -INFINITY;
double AudioResult::FloorOutput24 = -INFINITY;
double AudioResult::FloorOutputFloat = -INFINITY;

constexpr double AudioResult::kPrevFloorOutput8;
constexpr double AudioResult::kPrevFloorOutput16;
constexpr double AudioResult::kPrevFloorOutput24;
constexpr double AudioResult::kPrevFloorOutputFloat;

//
// The subsequent methods are used when updating the kPrev threshold arrays. They display the
// current run's results in an easily-imported format.
//
void AudioResult::DumpThresholdValues() {
  DumpFreqRespValues();
  DumpSinadValues();
  DumpPhaseValues();

  DumpLevelValues();
  DumpLevelToleranceValues();
  DumpNoiseFloorValues();
  DumpDynamicRangeValues();

  printf("\n\n");
}

void AudioResult::DumpFreqRespValues() {
  printf("\n\n Frequency Response");
  printf("\n   (all results given in dB)");

  DumpFreqRespValueSet(AudioResult::FreqRespPointUnity.data(), "FreqRespPointUnity");

  DumpFreqRespValueSet(AudioResult::FreqRespSincUnity.data(), "FreqRespSincUnity");
  DumpFreqRespValueSet(AudioResult::FreqRespSincDown0.data(), "FreqRespSincDown0");
  DumpFreqRespValueSet(AudioResult::FreqRespSincDown1.data(), "FreqRespSincDown1");
  DumpFreqRespValueSet(AudioResult::FreqRespSincDown2.data(), "FreqRespSincDown2");
  DumpFreqRespValueSet(AudioResult::FreqRespSincMicro.data(), "FreqRespSincMicro");
  DumpFreqRespValueSet(AudioResult::FreqRespSincUp1.data(), "FreqRespSincUp1");
  DumpFreqRespValueSet(AudioResult::FreqRespSincUp2.data(), "FreqRespSincUp2");
  DumpFreqRespValueSet(AudioResult::FreqRespSincUp3.data(), "FreqRespSincUp3");
}

void AudioResult::DumpSinadValues() {
  printf("\n\n Signal-to-Noise+Distortion");
  printf("\n   (all results given in dB)");

  DumpSinadValueSet(AudioResult::SinadPointUnity.data(), "SinadPointUnity");

  DumpSinadValueSet(AudioResult::SinadSincUnity.data(), "SinadSincUnity");
  DumpSinadValueSet(AudioResult::SinadSincDown0.data(), "SinadSincDown0");
  DumpSinadValueSet(AudioResult::SinadSincDown1.data(), "SinadSincDown1");
  DumpSinadValueSet(AudioResult::SinadSincDown2.data(), "SinadSincDown2");
  DumpSinadValueSet(AudioResult::SinadSincMicro.data(), "SinadSincMicro");
  DumpSinadValueSet(AudioResult::SinadSincUp1.data(), "SinadSincUp1");
  DumpSinadValueSet(AudioResult::SinadSincUp2.data(), "SinadSincUp2");
  DumpSinadValueSet(AudioResult::SinadSincUp3.data(), "SinadSincUp3");
}

void AudioResult::DumpPhaseValues() {
  printf("\n\n Phase Response");
  printf("\n   (all results given in radians)");

  DumpPhaseValueSet(AudioResult::PhasePointUnity.data(), "PhasePointUnity");

  DumpPhaseValueSet(AudioResult::PhaseSincUnity.data(), "PhaseSincUnity");
  DumpPhaseValueSet(AudioResult::PhaseSincDown0.data(), "PhaseSincDown0");
  DumpPhaseValueSet(AudioResult::PhaseSincDown1.data(), "PhaseSincDown1");
  DumpPhaseValueSet(AudioResult::PhaseSincDown2.data(), "PhaseSincDown2");
  DumpPhaseValueSet(AudioResult::PhaseSincMicro.data(), "PhaseSincMicro");
  DumpPhaseValueSet(AudioResult::PhaseSincUp1.data(), "PhaseSincUp1");
  DumpPhaseValueSet(AudioResult::PhaseSincUp2.data(), "PhaseSincUp2");
  DumpPhaseValueSet(AudioResult::PhaseSincUp3.data(), "PhaseSincUp3");
}

// Display a single frequency response results array, for import and processing.
void AudioResult::DumpFreqRespValueSet(double* freq_resp_vals, const std::string& arr_name) {
  printf("\nconst std::array<double, FrequencySet::kFirstOutBandRefFreqIdx>");
  printf("\n    AudioResult::kPrev%s = {", arr_name.c_str());
  for (auto freq_idx = 0u; freq_idx < FrequencySet::kFirstOutBandRefFreqIdx; ++freq_idx) {
    if (freq_idx % 10 == 0) {
      printf("\n  ");
    }
    if (freq_idx >= FrequencySet::kFirstInBandRefFreqIdx) {
      // For positive values, display 0 as threshold (we also bump up LevelTolerance, which limits
      // future values from being FURTHER above 0 dB)
      if (!isinf(freq_resp_vals[freq_idx])) {
        printf(" %9.3lf", std::min(0.0, std::floor(freq_resp_vals[freq_idx] / kFreqRespTolerance) *
                                            kFreqRespTolerance));
      } else {
        printf(" %sINFINITY", (freq_resp_vals[freq_idx] < 0) ? "-" : " ");
      }
      if (freq_idx < FrequencySet::kFirstOutBandRefFreqIdx - 1) {
        printf(",");
      }
    } else {
      printf("           ");
    }
  }
  printf("      };\n");
}

// Display a single sinad results array, for import and processing.
void AudioResult::DumpSinadValueSet(double* sinad_vals, const std::string& arr_name) {
  printf("\nconst std::array<double, FrequencySet::kNumReferenceFreqs> AudioResult::kPrev%s = {",
         arr_name.c_str());
  for (auto freq_idx = 0u; freq_idx < FrequencySet::kNumReferenceFreqs; ++freq_idx) {
    if (freq_idx % 10 == 0) {
      printf("\n   ");
    }
    if (!isinf(sinad_vals[freq_idx])) {
      printf(" %9.3lf", std::floor(sinad_vals[freq_idx] / kSinadTolerance) * kSinadTolerance);
    } else if (sinad_vals[freq_idx] > 0.0) {
      printf(" %9.3lf", 160.0);
    } else {
      printf(" -INFINITY");
    }

    if (freq_idx < FrequencySet::kNumReferenceFreqs - 1) {
      printf(",");
    }
  }
  printf("      };\n");
}

// Display a single phase results array, for import and processing.
void AudioResult::DumpPhaseValueSet(double* phase_vals, const std::string& arr_name) {
  printf(
      "\nconst std::array<double, FrequencySet::kFirstOutBandRefFreqIdx> AudioResult::kPrev%s = {",
      arr_name.c_str());
  for (auto freq_idx = 0u; freq_idx < FrequencySet::kFirstOutBandRefFreqIdx; ++freq_idx) {
    if (freq_idx % 10 == 0) {
      printf("\n   ");
    }
    if (freq_idx >= FrequencySet::kFirstInBandRefFreqIdx) {
      if (!isinf(phase_vals[freq_idx])) {
        printf(" %9.5lf", phase_vals[freq_idx]);
      } else {
        printf(" %sINFINITY", (phase_vals[freq_idx] < 0) ? "-" : " ");
      }
      if (freq_idx < FrequencySet::kFirstOutBandRefFreqIdx - 1) {
        printf(",");
      }
    } else {
      printf("           ");
    }
  }
  printf("    };\n");
}

void AudioResult::DumpLevelValues() {
  printf("\n\n Level (in dB)");
  printf("\n       8-bit:   Source %13.6le  Mix %13.6le  Output %13.6le", AudioResult::LevelSource8,
         AudioResult::LevelMix8, AudioResult::LevelOutput8);

  printf("\n       16-bit:  Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelSource16, AudioResult::LevelMix16, AudioResult::LevelOutput16);

  printf("\n       24-bit:  Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelSource24, AudioResult::LevelMix24, AudioResult::LevelOutput24);

  printf("\n       Float:   Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelSourceFloat, AudioResult::LevelMixFloat, AudioResult::LevelOutputFloat);
  printf("\n       Stereo-to-Mono: %13.6le", AudioResult::LevelStereoMono);

  printf("\n");
}

void AudioResult::DumpLevelToleranceValues() {
  printf("\n\n Level Tolerance (in dB)");
  printf("\n       8-bit:   Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelToleranceSource8, AudioResult::LevelToleranceMix8,
         AudioResult::LevelToleranceOutput8);

  printf("\n       16-bit:  Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelToleranceSource16, AudioResult::LevelToleranceMix16,
         AudioResult::LevelToleranceOutput16);

  printf("\n       24-bit:  Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelToleranceSource24, AudioResult::LevelToleranceMix24,
         AudioResult::LevelToleranceOutput24);

  printf("\n       Float:   Source %13.6le  Mix %13.6le  Output %13.6le",
         AudioResult::LevelToleranceSourceFloat, AudioResult::LevelToleranceMixFloat,
         AudioResult::LevelToleranceOutputFloat);

  printf("\n       Stereo-to-Mono: %13.6le             ", AudioResult::LevelToleranceStereoMono);
  printf("Interpolation: %13.6le", LevelToleranceInterpolation);

  printf("\n");
}

void AudioResult::DumpNoiseFloorValues() {
  printf("\n\n Noise Floor (in dB)");
  printf("\n       8-bit:   Source %9.5lf  Mix %9.5lf  Output %9.5lf", AudioResult::FloorSource8,
         AudioResult::FloorMix8, AudioResult::FloorOutput8);
  printf("\n       16-bit:  Source %9.5lf  Mix %9.5lf  Output %9.5lf", AudioResult::FloorSource16,
         AudioResult::FloorMix16, AudioResult::FloorOutput16);
  printf("\n       24-bit:  Source %9.5lf  Mix %9.5lf  Output %9.5lf", AudioResult::FloorSource24,
         AudioResult::FloorMix24, AudioResult::FloorOutput24);
  printf("\n       Float:   Source %9.5lf  Mix %9.5lf  Output %9.5lf",
         AudioResult::FloorSourceFloat, AudioResult::FloorMixFloat, AudioResult::FloorOutputFloat);
  printf("\n       Stereo-to-Mono: %9.5lf", AudioResult::FloorStereoMono);

  printf("\n");
}

void AudioResult::DumpDynamicRangeValues() {
  printf("\n\n Dynamic Range");
  printf("\n       Epsilon:  %7.4e  (%10.4le dB)", AudioResult::ScaleEpsilon,
         Gain::ScaleToDb(1.0 - AudioResult::ScaleEpsilon));
  printf("  Level: %11.4le dB  Sinad: %8.4lf dB", AudioResult::LevelEpsilonDown,
         AudioResult::SinadEpsilonDown);

  printf("\n       -30 dB down:                          ");
  printf("  Level: %11.4lf dB  Sinad: %8.4lf dB", AudioResult::Level30Down,
         AudioResult::Sinad30Down);

  printf("\n       -60 dB down:                          ");
  printf("  Level: %11.4lf dB  Sinad: %8.4lf dB", AudioResult::Level60Down,
         AudioResult::Sinad60Down);

  printf("\n       -90 dB down:                          ");
  printf("  Level: %11.4lf dB  Sinad: %8.4lf dB", AudioResult::Level90Down,
         AudioResult::Sinad90Down);

  printf("\n       Gain Accuracy: +/- %9.4le dB", AudioResult::DynRangeTolerance);

  printf("\n       MinScale: %8.6f  (%8.5f dB)", AudioResult::MinScaleNonZero,
         Gain::ScaleToDb(AudioResult::MinScaleNonZero));

  printf("\n");
}

}  // namespace media::audio::test
