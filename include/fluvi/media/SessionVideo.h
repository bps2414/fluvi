// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media: wiring an xhome WebRtcSession to a VideoDecoder (header-only; the app links fluvi_xhome and
// fluvi_media). Typical use:
//
//   media::VideoDecoderDesc d;  d.device = &gpuDevice;
//   d.requestKeyframe = media::KeyframeRequester(session);
//   auto decoder = media::VideoDecoder::Create(d, status);
//   xhome::WebRtcCallbacks cb;  media::AttachVideo(cb, *decoder);  /* + audio, rumble, ... */
//   session.setCallbacks(std::move(cb));  session.start(...);
//   // render thread, per refresh:
//   if (auto f = decoder->TryPop()) { f->WaitOnQueue(directQueue); /* NV12 -> RGB ... */ current = f; }
//   // after Present of a frame that showed `current`:
//   media::ReportPresented(session, *current, presentedQpc);
#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "fluvi/media/VideoDecoder.h"
#include "fluvi/xhome/WebRtcSession.h"

namespace fluvi::media {

// Access units from the session go straight into the decoder's input queue (never blocks the network thread).
inline void AttachVideo(xhome::WebRtcCallbacks& callbacks, VideoDecoder& decoder) {
    callbacks.video = [&decoder](std::vector<uint8_t>&& unit, uint32_t rtpTimestamp, int64_t arrivalQpc) {
        decoder.Push(std::move(unit), rtpTimestamp, arrivalQpc);
    };
}

// VideoDecoderDesc::requestKeyframe for a session (the decoder already rate-limits).
inline std::function<void()> KeyframeRequester(xhome::WebRtcSession& session) {
    return [&session] { session.requestKeyframe(); };
}

// The console's video feedback: one report per picture shown (the session drops repeats of the same RTP ts).
inline void ReportPresented(xhome::WebRtcSession& session, const DecodedFrame& frame, int64_t presentedQpc) {
    session.videoPresented(frame.rtpTimestamp, frame.arrivalQpc, frame.submittedQpc, frame.decodedQpc, presentedQpc);
}

} // namespace fluvi::media
