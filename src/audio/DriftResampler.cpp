// SPDX-License-Identifier: GPL-3.0-only
#include "DriftResampler.h"
#include <algorithm>
#include <cmath>

namespace fluvi::audio {

void DriftResampler::Reset() {
    buf_.assign(2, 0.0f);
    pos_ = 1.0;
}

void DriftResampler::Feed(const float* stereo, size_t frames) {
    buf_.insert(buf_.end(), stereo, stereo + frames * 2);
}

size_t DriftResampler::OutputsAvailable() const {
    const double n = static_cast<double>(buf_.size() / 2);
    const double room = n - 2.0 - pos_;          // outputs need floor(pos) + 2 < n
    if (room <= 0 || ratio_ <= 0) return 0;
    return static_cast<size_t>(std::ceil(room / ratio_));
}

size_t DriftResampler::PendingInput() const {
    const double n = static_cast<double>(buf_.size() / 2);
    return n > pos_ ? static_cast<size_t>(n - pos_) : 0;
}

static inline float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}

size_t DriftResampler::Read(float* out, size_t frames) {
    const size_t n = buf_.size() / 2;
    size_t produced = 0;
    for (; produced < frames; ++produced) {
        const size_t i = static_cast<size_t>(pos_);
        if (i + 2 >= n) break;
        const float t = static_cast<float>(pos_ - static_cast<double>(i));
        const float* p = buf_.data();
        for (size_t c = 0; c < 2; ++c)
            out[produced * 2 + c] = CatmullRom(p[(i - 1) * 2 + c], p[i * 2 + c], p[(i + 1) * 2 + c], p[(i + 2) * 2 + c], t);
        pos_ += ratio_;
    }
    // drop what can no longer be read, keeping one frame of history
    const size_t i = static_cast<size_t>(pos_);
    if (i > 1) {
        const size_t drop = std::min(i - 1, n);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop * 2));
        pos_ -= static_cast<double>(drop);
    }
    return produced;
}

void DriftResampler::FadeTail(size_t frames) {
    const size_t n = buf_.size() / 2;
    frames = std::min(frames, n - 1);            // never touch the history frame
    for (size_t d = 0; d < frames; ++d) {
        const float g = static_cast<float>(d) / static_cast<float>(frames);   // last frame -> 0
        const size_t f = n - 1 - d;
        buf_[f * 2] *= g;
        buf_[f * 2 + 1] *= g;
    }
}

} // namespace fluvi::audio
