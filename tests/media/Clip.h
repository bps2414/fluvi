// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media tests: the committed clips (tests/media/data, made with the dev ffmpeg.exe, libx264, no B-frames,
// IDR every 60 pictures):
//   dup720p.h264   1280x720, 180 AUs: [0,60) 30 fps content doubled to 60 (even = new, odd = repeat),
//                  [60,120) real 60 fps (all new), [120,180) doubled again. IDRs at 0, 60, 120.
//   dup1440p.h264  2560x1440, 60 AUs, 30 fps doubled to 60. IDR at 0.
// Regenerate (low priority, ~1 s of CPU):
//   ffmpeg -filter_complex "testsrc2=s=1280x720:r=30:d=1,fps=60[a];testsrc2=s=1280x720:r=60:d=1[b];
//     testsrc2=s=1280x720:r=30:d=1,fps=60[c];[a][b][c]concat=n=3[v]" -map [v] -c:v libx264 -threads 1
//     -preset veryfast -bf 0 -g 60 -keyint_min 60 -sc_threshold 0 -pix_fmt yuv420p -crf 34 -f h264 dup720p.h264
//   ffmpeg -f lavfi -i testsrc2=s=2560x1440:r=30:d=1,fps=60 (same options) -crf 38 -f h264 dup1440p.h264
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fluvi::test {

// Access units of a clip in tests/media/data (ctest runs from the repo root). Empty if missing.
std::vector<std::vector<uint8_t>> LoadClip(const std::string& name);
// Ground truth for dup720p (and dup1440p, which is the first segment only).
inline bool ClipIsNew(size_t index) { return index == 0 || (index >= 60 && index < 120) || index % 2 == 0; }

} // namespace fluvi::test
