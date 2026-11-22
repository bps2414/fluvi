// SPDX-License-Identifier: GPL-3.0-only
// fluvi_media_probe: decodes an H.264 Annex-B file through VideoDecoder (no window) and prints, per frame,
// isNew and the numbers behind it plus the decode time, then a summary. For tuning RepeatDetectConfig on a
// capture of the real stream.
//   fluvi_media_probe <file.h264> [--software] [--delta N] [--pixels N] [--quiet]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fluvi/core/Time.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/media/VideoDecoder.h"
#include "H264Parse.h"

using namespace fluvi;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fluvi_media_probe <file.h264> [--software] [--delta N] [--pixels N] [--quiet]\n");
        return 2;
    }
    media::VideoDecoderDesc desc;
    desc.threaded = false;
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--software")) desc.hardware = false;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--delta") && i + 1 < argc) desc.repeat.pixelDelta = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--pixels") && i + 1 < argc) desc.repeat.changedPixels = uint32_t(std::atoi(argv[++i]));
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto units = media::h264::SplitAccessUnits(bytes.data(), bytes.size());

    Status st;
    auto device = gpu::Device::Create(gpu::DeviceDesc{}, st);
    if (!device) { std::fprintf(stderr, "device: %s\n", st.message.c_str()); return 1; }
    desc.device = device.get();
    int keyframes = 0;
    desc.requestKeyframe = [&] { ++keyframes; };
    auto decoder = media::VideoDecoder::Create(desc, st);
    if (!decoder) { std::fprintf(stderr, "decoder: %s\n", st.message.c_str()); return 1; }

    std::vector<double> ms;
    if (!quiet) std::printf("frame  new  tiles  maxdY  bytes  decode_ms  key\n");
    for (size_t i = 0; i < units.size(); ++i) {
        auto f = decoder->Decode(std::move(units[i]), uint32_t(i * 1500), time::NowQpc());
        if (!f) { if (!quiet) std::printf("%5zu  (no picture)\n", i); continue; }
        ms.push_back(f->decodeMs);
        if (!quiet)
            std::printf("%5zu  %3s  %5u  %5u  %6u  %9.2f  %s\n", i, f->isNew ? "yes" : "-", f->changedTiles, f->maxLumaDelta,
                        f->auBytes, f->decodeMs, f->keyframe ? "IDR" : "");
    }
    const auto s = decoder->Stats();
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("\n%s %ux%u: %zu AUs, %llu frames, %llu new, %llu repeated, content %.1f fps; keyframe requests %d\n",
                s.backend.c_str(), s.width, s.height, units.size(), (unsigned long long)s.framesDecoded,
                (unsigned long long)s.framesNew, (unsigned long long)s.framesRepeated, s.contentFps, keyframes);
    if (!ms.empty())
        std::printf("decode ms: mean %.2f, p50 %.2f, p95 %.2f, max %.2f\n", sum / double(ms.size()), ms[ms.size() / 2],
                    ms[ms.size() * 95 / 100], ms.back());
    return 0;
}
