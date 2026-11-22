// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media, GPU (label gpu): d3d12va decode of the committed clips into gpu-core NV12 textures, repeat
// marking against the clip's ground truth, keyframe recovery after dropped AUs, software path, decode ms.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Clip.h"
#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/media/VideoDecoder.h"

using namespace fluvi;
using namespace fluvi::media;

namespace {

gpu::Device* SharedDevice() {
    static std::unique_ptr<gpu::Device> device = [] {
        gpu::DeviceDesc d;
        d.debugLayer = true;
        Status st;
        auto dev = gpu::Device::Create(d, st);
        if (!dev) MESSAGE("no D3D12 device: " << st.message);
        return dev;
    }();
    return device.get();
}

struct Harness {
    std::unique_ptr<VideoDecoder> decoder;
    int keyframeRequests = 0;
    explicit Harness(bool hardware, bool threaded = false) {
        VideoDecoderDesc d;
        d.device = SharedDevice();
        d.hardware = hardware;
        d.threaded = threaded;
        d.requestKeyframe = [this] { ++keyframeRequests; };
        Status st;
        if (d.device) decoder = VideoDecoder::Create(d, st);
        if (!decoder) MESSAGE("decoder: " << st.message);
    }
    DecodedFramePtr Feed(const std::vector<uint8_t>& au, size_t i) {
        std::vector<uint8_t> copy = au;
        return decoder->Decode(std::move(copy), uint32_t(i * 1500), time::NowQpc());   // 60 Hz on the 90 kHz clock
    }
};

void CheckRepeats(bool hardware, size_t count) {
    const auto clip = test::LoadClip("dup720p.h264");
    REQUIRE(clip.size() == 180);
    Harness h(hardware);
    REQUIRE(h.decoder.get() != nullptr);
    std::vector<DecodedFramePtr> frames;
    for (size_t i = 0; i < count; ++i) {
        auto f = h.Feed(clip[i], i);
        CAPTURE(i);
        REQUIRE(f.get() != nullptr);
        CHECK(f->texture.format == DXGI_FORMAT_NV12);
        CHECK(bool(f->texture.resource));
        CHECK(f->texture.srv.ptr != 0);
        CHECK(f->texture.srvChroma.ptr != 0);
        CHECK(f->width == 1280);
        CHECK(f->height == 720);
        CHECK(f->texture.width >= 1280);
        CHECK(f->texture.height >= 720);
        CHECK(f->index == i);
        CHECK(f->keyframe == (i % 60 == 0));
        CHECK(f->isNew == test::ClipIsNew(i));
        if (!f->isNew) CHECK(f->contentIndex == frames.back()->contentIndex);
        CHECK(f->readyFence->GetCompletedValue() >= f->readyValue);
        frames.push_back(std::move(f));
        if (frames.size() > 4) frames.erase(frames.begin());   // a consumer holding a few frames
    }
    const auto s = h.decoder->Stats();
    CHECK(s.framesDecoded == count);
    CHECK(s.framesNew + s.framesRepeated == count);
    CHECK(s.hardware == hardware);
    if (count >= 60) {
        // Doubled 30 fps content: about 30 unique pictures per second, 60 decoded.
        CHECK(frames.back()->contentFps == doctest::Approx(30.0).epsilon(0.1));
        CHECK(frames.back()->streamFps == doctest::Approx(60.0).epsilon(0.05));
    }
}

} // namespace

TEST_CASE("media gpu: d3d12va decodes the clip into NV12 textures, repeats marked exactly") {
    if (!SharedDevice()) return;
    CheckRepeats(true, 180);
}

TEST_CASE("media gpu: software path gives the same pictures and marks") {
    if (!SharedDevice()) return;
    CheckRepeats(false, 60);
}

TEST_CASE("media gpu: content cadence follows the clip (30 -> 60 -> 30)") {
    if (!SharedDevice()) return;
    const auto clip = test::LoadClip("dup720p.h264");
    Harness h(true);
    REQUIRE(h.decoder.get() != nullptr);
    double fpsAt119 = 0, fpsAt59 = 0;
    uint32_t durationAt58 = 0;
    for (size_t i = 0; i < clip.size(); ++i) {
        auto f = h.Feed(clip[i], i);
        REQUIRE(f.get() != nullptr);
        if (i == 58) durationAt58 = f->contentDuration90k;
        if (i == 59) fpsAt59 = f->contentFps;
        if (i == 119) fpsAt119 = f->contentFps;
    }
    CHECK(durationAt58 == 3000);   // two 60 Hz slots between new pictures
    CHECK(fpsAt59 == doctest::Approx(30.0).epsilon(0.1));
    CHECK(fpsAt119 == doctest::Approx(60.0).epsilon(0.05));
}

TEST_CASE("media gpu: recovery after dropped access units resumes at the next keyframe") {
    if (!SharedDevice()) return;
    const auto clip = test::LoadClip("dup720p.h264");
    Harness h(true);
    REQUIRE(h.decoder.get() != nullptr);
    size_t produced = 0, firstAfter = 0;
    for (size_t i = 0; i < clip.size(); ++i) {
        if (i >= 20 && i < 25) continue;   // lost on the network
        auto f = h.Feed(clip[i], i);
        if (i > 25 && i < 60) CHECK(f.get() == nullptr);   // broken chain: wait for the IDR, never show garbage
        if (!f) continue;
        ++produced;
        if (i >= 25 && !firstAfter) {
            firstAfter = i;
            CHECK(f->keyframe);
            CHECK(f->discontinuity);
            CHECK(f->isNew);
        }
    }
    CHECK(firstAfter == 60);
    CHECK(produced == 20 + 120);
    CHECK(h.keyframeRequests >= 1);
    const auto s = h.decoder->Stats();
    CHECK(s.lossEvents == 1);
    CHECK(s.unitsSkipped == 35);
    CHECK_FALSE(s.waitingForKeyframe);
    CHECK_FALSE(s.failed);
}

TEST_CASE("media gpu: threaded decoder with short queues delivers every frame when paced") {
    if (!SharedDevice()) return;
    const auto clip = test::LoadClip("dup720p.h264");
    Harness h(true, true);
    REQUIRE(h.decoder.get() != nullptr);
    size_t got = 0;
    for (size_t i = 0; i < 90; ++i) {
        std::vector<uint8_t> copy = clip[i];
        h.decoder->Push(std::move(copy), uint32_t(i * 1500), time::NowQpc());
        if (auto f = h.decoder->WaitPop(3000)) {
            ++got;
            CHECK(f->index == i);
        }
    }
    CHECK(got == 90);
    const auto s = h.decoder->Stats();
    CHECK(s.inputDropped == 0);
    CHECK(s.outputDropped == 0);
    CHECK(s.unitsIn == 90);
}

TEST_CASE("media gpu: 1440p decode time per frame") {
    if (!SharedDevice()) return;
    const auto clip = test::LoadClip("dup1440p.h264");
    REQUIRE(clip.size() == 60);
    Harness h(true);
    REQUIRE(h.decoder.get() != nullptr);
    std::vector<double> ms;
    for (size_t i = 0; i < clip.size(); ++i) {
        auto f = h.Feed(clip[i], i);
        REQUIRE(f.get() != nullptr);
        CHECK(f->width == 2560);
        CHECK(f->height == 1440);
        CHECK(f->isNew == test::ClipIsNew(i));
        if (i >= 5) ms.push_back(f->decodeMs);   // skip the decoder's warm-up
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    const double mean = sum / double(ms.size()), p50 = ms[ms.size() / 2], p95 = ms[ms.size() * 95 / 100];
    std::printf("media: 1440p decode (submit -> NV12 ready + compare), %zu frames: mean %.2f ms, p50 %.2f ms, p95 %.2f ms\n",
                ms.size(), mean, p50, p95);
    MESSAGE("1440p decode mean " << mean << " ms, p95 " << p95 << " ms");
    CHECK(mean < 16.0);
}
