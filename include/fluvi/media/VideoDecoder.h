// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media: H.264 Annex-B access units in, NV12 textures on the gpu-core device out. Owner: agent F
// (src/media, include/fluvi/media). Recovery logic after Veyra's XboxSessionSource (GPL-3.0-only, c6633121).
//
// Flow (one decode thread, short drop-oldest queues on both sides, nothing accumulates):
//   Push(AU) -> [input LatestQueue, 8] -> decode thread:
//     parse (IDR/SPS, frame_num, first_mb) -> FFmpeg d3d12va on the shared ID3D12Device (FFmpeg's own
//     video-decode queue) -> media compute queue: GPU-waits FFmpeg's per-frame fence, copies the picture
//     into a pool texture (gpu-core Texture, NV12, SRVs for both planes) and compares its luma with the
//     previous picture -> decode thread reads the compare result -> [output LatestQueue, 2] -> TryPop().
//   Software decode (FFmpeg h264, uploaded on the same queue) is the fallback VideoDecodeRecovery asks for.
//
// Repeated frames (a 30 fps game in a 60 Hz stream): marked here, before any pipeline work. The authority is
// a GPU luma compare (16x16 tiles; a tile changed when >= changedPixels of its pixels moved by >= pixelDelta
// code values), not the bitstream: the console-class encoder (AMD VCN/AMF, measured on an RX 6600) codes a
// repeat as a 0.15-4 KB P-frame with refinement bits, and a small real change can be only 1.5x that, so
// size can't tell them apart; the refinement moves pixels by <= ~26, a real change by much more. IDR/I
// pictures and the first picture after a discontinuity are always new. Cost: one dispatch over the luma
// (~0.05 ms at 1440p) on the media queue.
//
// Synchronisation: a frame is published when its GPU work is complete (the decode thread waits on its own
// queue; the render thread never waits on the CPU). Still, call WaitOnQueue(queue) before recording work
// that reads the texture: it is a GPU-side wait, free when already complete, and keeps the contract if a
// later version publishes earlier. The texture is in D3D12_RESOURCE_STATE_COMMON: read it through implicit
// promotion (an SRV read on the direct or compute queue), don't Transition it, don't write it.
// Lifetime: the texture belongs to a small pool. Drop the DecodedFramePtr only once the GPU no longer reads
// it, or call ReleaseAfter(fence, value) first: the decoder then GPU-waits that fence before reusing it.
// Frames (and the pool) may outlive the VideoDecoder, never the gpu::Device.
//
// Times are fluvi::time::NowQpc() values. For WebRtcSession::videoPresented: rtpTimestamp, arrivalQpc,
// submittedQpc (sent to the decoder), decodedQpc (picture ready), plus the caller's presentedQpc.
// Session wiring helpers: fluvi/media/SessionVideo.h.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fluvi/core/Status.h"
#include "fluvi/gpu/Device.h"
#include "fluvi/gpu/Texture.h"

namespace fluvi {
struct OverlayStats;
}

namespace fluvi::media {

namespace detail { struct Slot; }

struct DecodedFrame {
    // NV12, state COMMON. texture.width/height is the surface (decoders align, e.g. 1088 for 1080): use
    // width/height below as Nv12Params::visibleWidth/visibleHeight.
    gpu::GpuTexture texture;
    uint32_t width = 0, height = 0;
    bool fullRange = false;        // Nv12Params::fullRange
    bool bt601 = false;            // Nv12Params::matrix: false = YuvMatrix::Bt709 (Xbox), true = Bt601

    // GPU sync: the picture is complete when readyFence reaches readyValue.
    ID3D12Fence* readyFence = nullptr;
    uint64_t readyValue = 0;
    void WaitOnQueue(ID3D12CommandQueue* queue) const { if (queue && readyFence) queue->Wait(readyFence, readyValue); }
    // The last GPU work that reads the texture; the pool waits for it (on the GPU) before reusing the texture.
    void ReleaseAfter(ID3D12Fence* fence, uint64_t value) const;

    // Timing (QPC).
    uint32_t rtpTimestamp = 0;     // 90 kHz, from the console
    int64_t arrivalQpc = 0;        // access unit complete (WebRtcSession)
    int64_t submittedQpc = 0;      // handed to FFmpeg
    int64_t decodedQpc = 0;        // copy + compare done on the GPU, published
    double decodeMs = 0;           // submitted -> decoded

    // Content.
    bool isNew = true;             // the picture differs from the previous decoded picture
    bool keyframe = false;         // IDR
    bool discontinuity = false;    // first picture after a start, a loss/recovery or a size change
    uint64_t index = 0;            // decoded pictures since Create
    uint64_t contentIndex = 0;     // new pictures since Create; a repeat carries the index of the one it repeats
    uint32_t repeatRun = 0;        // repeats since the last new picture (0 on a new one)
    // Content cadence, measured on new pictures with the console's RTP clock (exact capture grid):
    uint32_t contentDuration90k = 0;   // RTP time since the previous new picture (0 = unknown / discontinuity)
    double contentFps = 0;             // unique pictures per second over the last ~0.5 s (0 = unknown)
    double streamFps = 0;              // decoded pictures per second, same window
    // Diagnostics of the repeat decision.
    uint32_t auBytes = 0;          // slice bytes of the access unit
    uint32_t changedTiles = 0;     // 16x16 luma tiles that changed vs the previous picture
    uint32_t maxLumaDelta = 0;     // 0..255

    std::shared_ptr<detail::Slot> slot;   // pool bookkeeping
};
using DecodedFramePtr = std::shared_ptr<const DecodedFrame>;

struct RepeatDetectConfig {
    uint32_t pixelDelta = 32;      // |dY| (0..255) that counts a luma sample as changed (AMF refinement stays <= ~26)
    uint32_t changedPixels = 4;    // samples per 16x16 tile for the tile to count as changed
    uint32_t changedTiles = 1;     // tiles for the picture to count as new
};

struct VideoDecoderDesc {
    gpu::Device* device = nullptr;       // required; must outlive the decoder and every frame
    bool hardware = true;                // d3d12va; false = software from the start (tests, diagnostics)
    bool softwareFallback = true;        // allow VideoDecodeRecovery's UseSoftware step
    bool threaded = true;                // false: no thread, the caller drives Decode() (tests, probe tool)
    // Asked from the decode thread when a keyframe is needed (start, loss, decode error); rate-limited to one
    // per 300 ms while waiting. Typically WebRtcSession::requestKeyframe.
    std::function<void()> requestKeyframe;
    RepeatDetectConfig repeat;
};

struct VideoDecoderStats {
    uint64_t unitsIn = 0, framesDecoded = 0, framesNew = 0, framesRepeated = 0;
    uint64_t inputDropped = 0;           // AUs dropped by the input queue (decoder too slow)
    uint64_t outputDropped = 0;          // decoded frames replaced before the consumer popped them
    uint64_t unitsSkipped = 0;           // AUs discarded while waiting for a keyframe
    uint64_t lossEvents = 0;             // queue gaps, frame_num gaps, headless AUs
    uint64_t decodeErrors = 0, keyframeRequests = 0, poolExhausted = 0;
    double decodeMs = 0;                 // moving average, submitted -> decoded
    double contentFps = 0, streamFps = 0;
    uint32_t width = 0, height = 0;
    bool hardware = false;               // the active path is d3d12va
    bool waitingForKeyframe = true;
    bool failed = false;                 // VideoDecodeRecovery said Stop; the stream needs a reconnect
    std::string backend;                 // "d3d12va" / "software" / ""
};

class VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> Create(const VideoDecoderDesc& desc, Status& status);
    virtual ~VideoDecoder() = default;

    // Any thread, never blocks (drops the oldest queued AU when full; the decoder then resyncs on a keyframe).
    virtual void Push(std::vector<uint8_t>&& unit, uint32_t rtpTimestamp, int64_t arrivalQpc) = 0;
    // The oldest decoded frame not yet taken (the queue holds at most 2), or null.
    virtual DecodedFramePtr TryPop() = 0;
    virtual DecodedFramePtr WaitPop(uint32_t timeoutMs) = 0;

    // threaded == false only: decodes one AU on the calling thread; the frame it produced, or null (waiting
    // for a keyframe, error). Nothing goes to the output queue.
    virtual DecodedFramePtr Decode(std::vector<uint8_t>&& unit, uint32_t rtpTimestamp, int64_t arrivalQpc) = 0;

    // Forget the history and wait for a keyframe (new stream on the same decoder).
    virtual void Reset() = 0;
    virtual VideoDecoderStats Stats() const = 0;
    // Fills the fields media owns: streamWidth/Height, gameFps, streamFps, decodeMs, framesDropped (= media's
    // queue drops; a presenter adds its own after), warning (software decode / failure; left alone otherwise).
    virtual void FillOverlayStats(OverlayStats& stats) const = 0;
};

} // namespace fluvi::media
