// SPDX-License-Identifier: GPL-3.0-only
// Fractional stereo resampler (4-point Catmull-Rom). The ratio can change slowly from block to block without
// clicks because the read position is continuous. At ratio exactly 1.0 it is a bit-exact passthrough.
// Pure class, single thread.
#pragma once
#include <cstddef>
#include <vector>

namespace fluvi::audio {

class DriftResampler {
public:
    // ratio = input frames consumed per output frame (inRate / outRate * (1 + ppm * 1e-6)).
    void SetRatio(double ratio) { ratio_ = ratio; }
    double ratio() const { return ratio_; }
    void Feed(const float* stereo, size_t frames);
    // Outputs producible with the input fed so far (cubic needs 2 frames of lookahead).
    size_t OutputsAvailable() const;
    // Returns the number of frames written (<= frames).
    size_t Read(float* stereoOut, size_t frames);
    // Input frames not consumed yet (fractional part rounded down).
    size_t PendingInput() const;
    // Linear fade to silence over the last `frames` of pending input (before a discontinuity).
    void FadeTail(size_t frames);
    void Reset();

private:
    std::vector<float> buf_ = std::vector<float>(2, 0.0f);   // interleaved stereo; frame 0 is history
    double pos_ = 1.0;                                       // read position, in frames of buf_
    double ratio_ = 1.0;
};

} // namespace fluvi::audio
