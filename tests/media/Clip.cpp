// SPDX-License-Identifier: GPL-3.0-only
#include "Clip.h"

#include <fstream>
#include <iterator>

#include "H264Parse.h"

namespace fluvi::test {

std::vector<std::vector<uint8_t>> LoadClip(const std::string& name) {
    for (const char* dir : {"tests/media/data/", "../tests/media/data/", "../../tests/media/data/"}) {
        std::ifstream in(std::string(dir) + name, std::ios::binary);
        if (!in) continue;
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return media::h264::SplitAccessUnits(bytes.data(), bytes.size());
    }
    return {};
}

} // namespace fluvi::test
