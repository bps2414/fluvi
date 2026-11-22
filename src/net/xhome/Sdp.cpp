// SPDX-License-Identifier: GPL-3.0-only
// b=AS logic: unknownskl/xbox-xcloud-player src/lib/sdp.ts _setBitrate (also in unknownskl/greenlight
// packages/player/src/client/lib/sdp.ts), MIT.
#include "fluvi/xhome/Sdp.h"

#include <sstream>
#include <vector>

namespace fluvi::xhome {
namespace {

std::vector<std::string> splitLines(const std::string& sdp) {
    std::vector<std::string> lines;
    std::istringstream in(sdp);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string joinCrlf(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) { out += l; out += "\r\n"; }
    return out;
}

// Index of the first line after the i=/c= lines of the m=<media> section, or npos.
size_t afterMediaHeader(const std::vector<std::string>& lines, const std::string& media) {
    const std::string prefix = "m=" + media;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind(prefix, 0) != 0) continue;
        size_t at = i + 1;
        while (at < lines.size() && (lines[at].rfind("i=", 0) == 0 || lines[at].rfind("c=", 0) == 0)) ++at;
        return at;
    }
    return std::string::npos;
}

} // namespace

uint32_t effectiveBitrateKbps(const StreamConfig& config) {
    return config.maxBitrateKbps ? config.maxBitrateKbps : kGreenlightMaxBitrateKbps;
}

std::string setMediaBitrate(const std::string& sdp, const std::string& media, uint32_t kbps) {
    auto lines = splitLines(sdp);
    const size_t at = afterMediaHeader(lines, media);
    if (at == std::string::npos) return sdp;
    const std::string line = "b=AS:" + std::to_string(kbps);
    if (at < lines.size() && lines[at].rfind("b=", 0) == 0) lines[at] = line;
    else lines.insert(lines.begin() + std::ptrdiff_t(at), line);
    return joinCrlf(lines);
}

uint32_t mediaBitrate(const std::string& sdp, const std::string& media) {
    const auto lines = splitLines(sdp);
    const size_t at = afterMediaHeader(lines, media);
    if (at == std::string::npos || at >= lines.size() || lines[at].rfind("b=AS:", 0) != 0) return 0;
    return uint32_t(std::strtoul(lines[at].c_str() + 5, nullptr, 10));
}

std::string describeCodecs(const std::string& sdp) {
    std::string out;
    for (const auto& line : splitLines(sdp))
        if (line.rfind("a=rtpmap:", 0) == 0 || line.rfind("a=fmtp:", 0) == 0 || line.rfind("m=", 0) == 0 || line.rfind("b=", 0) == 0)
            out += line + "\n";
    return out;
}

std::string withoutCandidates(const std::string& sdp) {
    std::vector<std::string> kept;
    for (auto& line : splitLines(sdp))
        if (line.rfind("a=candidate:", 0) != 0 && line != "a=end-of-candidates") kept.push_back(std::move(line));
    return joinCrlf(kept);
}

} // namespace fluvi::xhome
