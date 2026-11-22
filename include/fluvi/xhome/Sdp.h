// SPDX-License-Identifier: GPL-3.0-only
// SDP helpers for the offer, pure string functions (tested offline).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

#include "fluvi/xhome/StreamConfig.h"

namespace fluvi::xhome {

// Greenlight's xHome bitrate slider (packages/desktop/renderer/pages/settings/streaming.tsx):
// <input type="range" min="0" max="40960" step="1024">, in kbps, so 40 Mbps is the highest value the
// reference client offers (its 0 means "no b=AS line"; here 0 means this value instead). The console
// treats b=AS as a ceiling and picks its own encoder rate below it.
inline constexpr uint32_t kGreenlightMaxBitrateKbps = 40960;

// The value written as b=AS for a config: maxBitrateKbps, or kGreenlightMaxBitrateKbps when it is 0.
uint32_t effectiveBitrateKbps(const StreamConfig& config);

// Greenlight/xbox-xcloud-player sdp.ts _setBitrate: b=AS:<kbps> in the first m=<media> section, after its
// i=/c= lines; an existing b= line there is replaced. Line endings of the input are preserved (CRLF in,
// CRLF out). Returns the input unchanged when the section is missing.
std::string setMediaBitrate(const std::string& sdp, const std::string& media, uint32_t kbps);

// b=AS value in the first m=<media> section, 0 when none.
uint32_t mediaBitrate(const std::string& sdp, const std::string& media);

// The m=, a=rtpmap: and a=fmtp: lines, for logs.
std::string describeCodecs(const std::string& sdp);

// Removes a=candidate / a=end-of-candidates lines (candidates go through the ICE exchange); output is CRLF.
std::string withoutCandidates(const std::string& sdp);

} // namespace fluvi::xhome
