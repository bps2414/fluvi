// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media: FFmpeg d3d12va decode into gpu-core NV12 textures, repeat marking, keyframe recovery.
// Contract and design notes: include/fluvi/media/VideoDecoder.h. Recovery flow after Veyra's
// XboxSessionSource::decodeLoop (GPL-3.0-only, c6633121): history reset + keyframe request on loss,
// VideoDecodeRecovery for repeated errors (reset -> reopen hardware -> software -> stop).
#include "fluvi/media/VideoDecoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <wrl/client.h>

#include "fluvi/common/Stats.h"
#include "fluvi/core/LatestQueue.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/xhome/VideoDecodeRecovery.h"
#include "H264Parse.h"
#include "PictureCompare.dxil.h"

#pragma warning(push, 0)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixdesc.h>
}
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

namespace fluvi::media {

namespace detail {
struct Slot {
    std::unique_ptr<gpu::Texture> texture;
    uint32_t width = 0, height = 0;
    std::mutex mutex;
    ComPtr<ID3D12Fence> readFence;   // consumer's last read (ReleaseAfter)
    ComPtr<ID3D12Fence> readyFence;  // keeps DecodedFrame::readyFence alive past the decoder
    uint64_t readValue = 0;
};
} // namespace detail

void DecodedFrame::ReleaseAfter(ID3D12Fence* fence, uint64_t value) const {
    if (!slot) return;
    std::lock_guard lock(slot->mutex);
    slot->readFence = fence;
    slot->readValue = value;
}

namespace {

constexpr size_t kMaxSlots = 8;
constexpr int64_t kKeyframeRetryMs = 300;
constexpr int64_t kCadenceWindow90k = 45000;   // 0.5 s

struct Unit {
    std::vector<uint8_t> data;
    uint32_t rtp = 0;
    int64_t arrival = 0;
    uint64_t seq = 0;
};

struct Meta {
    int64_t pts = 0;
    uint32_t rtp = 0;
    int64_t arrival = 0, submitted = 0;
    bool idr = false;
    uint32_t bytes = 0;
};

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

std::string AvError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

class VideoDecoderImpl final : public VideoDecoder {
public:
    explicit VideoDecoderImpl(const VideoDecoderDesc& desc) : desc_(desc), useSoftware_(!desc.hardware) {}

    ~VideoDecoderImpl() override {
        stop_ = true;
        input_.Close();
        if (thread_.joinable()) thread_.join();
        CloseCodec();
        if (hwDevice_) av_buffer_unref(&hwDevice_);
        WaitGpu(fenceValue_);
        if (event_) CloseHandle(event_);
        if (uploadPtr_) upload_->Unmap(0, nullptr);
    }

    Status Init() {
        ID3D12Device* d = desc_.device->D3D();
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) return Status::Fail("media: CreateCommandQueue falhou");
        queue_->SetName(L"fluvi.media.queue");
        if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator_))) ||
            FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_))))
            return Status::Fail("media: command list falhou");
        list_->Close();
        if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return Status::Fail("media: CreateFence falhou");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) return Status::Fail("media: CreateEvent falhou");

        if (FAILED(d->CreateRootSignature(0, g_PictureCompare, sizeof(g_PictureCompare), IID_PPV_ARGS(&rootSig_))))
            return Status::Fail("media: root signature do PictureCompare falhou");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rootSig_.Get();
        pd.CS = {g_PictureCompare, sizeof(g_PictureCompare)};
        if (FAILED(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_)))) return Status::Fail("media: PSO do PictureCompare falhou");

        if (!CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, 256, true, &result_) ||
            !CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, 256, false, &zero_) ||
            !CreateBuffer(D3D12_HEAP_TYPE_READBACK, 256, false, &readback_))
            return Status::Fail("media: buffers do PictureCompare falharam");
        void* p = nullptr;
        if (FAILED(zero_->Map(0, nullptr, &p))) return Status::Fail("media: Map falhou");
        std::memset(p, 0, 256);
        zero_->Unmap(0, nullptr);

        if (desc_.threaded) thread_ = std::thread([this] { Run(); });
        return Status::Ok();
    }

    // ---- VideoDecoder ----------------------------------------------------------------------------------------
    void Push(std::vector<uint8_t>&& unit, uint32_t rtp, int64_t arrival) override {
        bool dropped;
        {
            std::lock_guard lock(pushMutex_);
            dropped = input_.Push(Unit{std::move(unit), rtp, arrival, pushSeq_++});
        }
        if (dropped) { std::lock_guard lock(statsMutex_); ++stats_.inputDropped; }
    }

    DecodedFramePtr TryPop() override { auto f = output_.TryPop(); return f ? std::move(*f) : nullptr; }
    DecodedFramePtr WaitPop(uint32_t timeoutMs) override {
        auto f = output_.WaitPop(std::chrono::milliseconds(timeoutMs));
        return f ? std::move(*f) : nullptr;
    }

    DecodedFramePtr Decode(std::vector<uint8_t>&& unit, uint32_t rtp, int64_t arrival) override {
        if (desc_.threaded) return nullptr;
        return Process(Unit{std::move(unit), rtp, arrival, pushSeq_++});
    }

    void Reset() override {
        if (desc_.threaded) resetRequested_ = true;
        else DoReset();
    }

    VideoDecoderStats Stats() const override { std::lock_guard lock(statsMutex_); return stats_; }

    void FillOverlayStats(OverlayStats& o) const override {
        const auto s = Stats();
        o.streamWidth = s.width;
        o.streamHeight = s.height;
        o.gameFps = s.contentFps;
        o.streamFps = s.streamFps;
        o.decodeMs = s.decodeMs;
        o.framesDropped = s.inputDropped + s.outputDropped + s.poolExhausted;
        if (s.failed) o.warning = "decode de vídeo falhou; reconecte";
        else if (!s.hardware && !s.backend.empty()) o.warning = "decode por software (d3d12va indisponível)";
    }

private:
    // ---- thread ----------------------------------------------------------------------------------------------
    void Run() {
        FLUVI_INFO("media", "decode thread started");
        while (!stop_) {
            auto unit = input_.WaitPop(std::chrono::milliseconds(50));
            if (!unit) continue;
            if (auto frame = Process(std::move(*unit))) {
                if (output_.Push(std::move(frame))) { std::lock_guard lock(statsMutex_); ++stats_.outputDropped; }
            }
        }
        FLUVI_INFO("media", "decode thread stopped");
    }

    void DoReset() {
        if (ctx_) avcodec_flush_buffers(ctx_);
        parser_.Reset();
        frameNums_.Reset();
        metas_.clear();
        prevSlot_.reset();
        waitingKey_ = true;
        discontinuity_ = true;
        RequestKeyframe(true);
    }

    // ---- one access unit -------------------------------------------------------------------------------------
    DecodedFramePtr Process(Unit&& unit) {
        if (resetRequested_.exchange(false)) DoReset();
        if (failed_) return nullptr;
        { std::lock_guard lock(statsMutex_); ++stats_.unitsIn; }

        bool loss = haveSeq_ && unit.seq != nextSeq_;   // the input queue dropped AUs
        haveSeq_ = true;
        nextSeq_ = unit.seq + 1;

        const h264::AuInfo info = parser_.Parse(unit.data.data(), unit.data.size());
        const xhome::H264SpsInfo* sps = parser_.Sps();
        const uint32_t maxFrameNum = sps ? (1u << std::min(sps->log2MaxFrameNum, 16u)) : 0;
        const bool startPoint = info.hasIdr || info.hasSps;

        if (!waitingKey_) {
            if (info.hasSlice && info.headerOk && info.firstMb != 0) loss = true;   // head of the picture lost
            if (sps && frameNums_.Observe(info, maxFrameNum, sps->gapsInFrameNumAllowed)) loss = true;
            if (info.spsChanged) discontinuity_ = true;
            if (loss) {
                { std::lock_guard lock(statsMutex_); ++stats_.lossEvents; }
                FLUVI_WARN("media", "access unit lost (frame_num {}); history reset, waiting for a keyframe", info.frameNum);
                EnterRecovery();
            }
        }
        if (waitingKey_) {
            if (!startPoint) {
                { std::lock_guard lock(statsMutex_); ++stats_.unitsSkipped; }
                RequestKeyframe(false);
                return nullptr;
            }
            waitingKey_ = false;
            discontinuity_ = true;
            frameNums_.Reset();
            if (sps) frameNums_.Observe(info, maxFrameNum, sps->gapsInFrameNumAllowed);
            { std::lock_guard lock(statsMutex_); stats_.waitingForKeyframe = false; }
        }

        if (!ctx_ && !OpenCodec()) {
            Fail("no usable H.264 decoder");
            return nullptr;
        }

        Meta meta{ptsCounter_++, unit.rtp, unit.arrival, time::NowQpc(), info.hasIdr, info.sliceBytes};
        metas_.push_back(meta);
        if (metas_.size() > 16) metas_.pop_front();

        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, int(unit.data.size())) < 0) { av_packet_free(&pkt); return nullptr; }
        std::memcpy(pkt->data, unit.data.data(), unit.data.size());
        pkt->pts = meta.pts;
        int ret = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            DecodeFailed("send_packet: " + AvError(ret));
            return nullptr;
        }
        AVFrame* frame = av_frame_alloc();
        AVFrame* latest = nullptr;
        for (;;) {
            ret = avcodec_receive_frame(ctx_, frame);
            if (ret < 0) break;
            if (latest) av_frame_free(&latest);   // never more than one with LOW_DELAY and no B-frames
            latest = av_frame_clone(frame);
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            if (latest) av_frame_free(&latest);
            DecodeFailed("receive_frame: " + AvError(ret));
            return nullptr;
        }
        if (!latest) return nullptr;
        if ((latest->flags & AV_FRAME_FLAG_CORRUPT) || latest->decode_error_flags) {
            av_frame_free(&latest);
            DecodeFailed("corrupt picture");
            return nullptr;
        }
        recovery_.picture();
        for (const auto& m : metas_) if (m.pts == latest->pts) { meta = m; break; }
        auto out = Output(latest, meta);
        av_frame_free(&latest);
        return out;
    }

    // ---- recovery --------------------------------------------------------------------------------------------
    void EnterRecovery() {
        if (ctx_) avcodec_flush_buffers(ctx_);
        metas_.clear();
        waitingKey_ = true;
        discontinuity_ = true;
        { std::lock_guard lock(statsMutex_); stats_.waitingForKeyframe = true; }
        RequestKeyframe(true);
    }

    void DecodeFailed(const std::string& detail) {
        const bool hardware = !useSoftware_;
        const auto action = recovery_.failed(hardware);
        { std::lock_guard lock(statsMutex_); ++stats_.decodeErrors; }
        const char* name = action == xhome::VideoDecodeRecoveryAction::ResetHistory ? "reset-history"
                         : action == xhome::VideoDecodeRecoveryAction::ReopenHardware ? "reopen-hardware"
                         : action == xhome::VideoDecodeRecoveryAction::UseSoftware ? "use-software" : "stop";
        FLUVI_WARN("media", "decode error ({}), action {}", detail, name);
        switch (action) {
        case xhome::VideoDecodeRecoveryAction::ResetHistory: EnterRecovery(); break;
        case xhome::VideoDecodeRecoveryAction::ReopenHardware: CloseCodec(); EnterRecovery(); break;
        case xhome::VideoDecodeRecoveryAction::UseSoftware:
            if (!desc_.softwareFallback) { Fail("hardware decode keeps failing"); break; }
            useSoftware_ = true;
            CloseCodec();
            EnterRecovery();
            break;
        case xhome::VideoDecodeRecoveryAction::Stop: Fail("decode keeps failing after recovery"); break;
        }
    }

    void Fail(const char* why) {
        FLUVI_ERROR("media", "video decode stopped: {}", why);
        failed_ = true;
        std::lock_guard lock(statsMutex_);
        stats_.failed = true;
    }

    void RequestKeyframe(bool force) {
        const int64_t now = time::NowQpc();
        if (!force && lastKeyframeRequest_ && time::ElapsedMs(lastKeyframeRequest_, now) < double(kKeyframeRetryMs)) return;
        lastKeyframeRequest_ = now;
        { std::lock_guard lock(statsMutex_); ++stats_.keyframeRequests; }
        if (desc_.requestKeyframe) desc_.requestKeyframe();
    }

    // ---- FFmpeg ----------------------------------------------------------------------------------------------
    static AVPixelFormat GetFormat(AVCodecContext* c, const AVPixelFormat* formats) {
        auto* self = static_cast<VideoDecoderImpl*>(c->opaque);
        if (!self->useSoftware_)
            for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
                if (*p == AV_PIX_FMT_D3D12) return *p;
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                if (!self->useSoftware_) FLUVI_WARN("media", "d3d12va refused this stream; decoding in software");
                return *p;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    bool CreateHwDevice() {
        hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
        if (!hwDevice_) return false;
        auto* hw = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
        auto* d3d = static_cast<AVD3D12VADeviceContext*>(hw->hwctx);
        d3d->device = desc_.device->D3D();
        d3d->device->AddRef();   // released by FFmpeg with the context
        const int ret = av_hwdevice_ctx_init(hwDevice_);
        if (ret < 0) {
            FLUVI_WARN("media", "d3d12va device init failed: {}", AvError(ret));
            av_buffer_unref(&hwDevice_);
            return false;
        }
        return true;
    }

    bool OpenCodec() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return false;
        if (!useSoftware_ && !hwDevice_ && !CreateHwDevice()) {
            if (!desc_.softwareFallback) return false;
            useSoftware_ = true;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return false;
        ctx_->opaque = this;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->get_format = &VideoDecoderImpl::GetFormat;
        if (!useSoftware_) {
            ctx_->hw_device_ctx = av_buffer_ref(hwDevice_);
            ctx_->thread_count = 1;
        } else {
            ctx_->thread_type = FF_THREAD_SLICE;   // frame threads would add a frame of latency per thread
            ctx_->thread_count = 0;
        }
        const int ret = avcodec_open2(ctx_, codec, nullptr);
        if (ret < 0) {
            FLUVI_ERROR("media", "avcodec_open2 failed: {}", AvError(ret));
            avcodec_free_context(&ctx_);
            return false;
        }
        FLUVI_INFO("media", "H.264 decoder open ({})", useSoftware_ ? "software" : "d3d12va");
        return true;
    }

    void CloseCodec() { if (ctx_) avcodec_free_context(&ctx_); }

    // ---- GPU -------------------------------------------------------------------------------------------------
    bool CreateBuffer(D3D12_HEAP_TYPE heap, uint64_t size, bool uav, ComPtr<ID3D12Resource>* out) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                          : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                             : D3D12_RESOURCE_STATE_COMMON;
        return SUCCEEDED(desc_.device->D3D()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&(*out))));
    }

    void WaitGpu(uint64_t value) {
        if (!fence_ || fence_->GetCompletedValue() >= value) return;
        fence_->SetEventOnCompletion(value, event_);
        while (WaitForSingleObject(event_, 2000) == WAIT_TIMEOUT) {
            if (fence_->GetCompletedValue() >= value) return;
            if (FAILED(desc_.device->CheckRemoved())) return;
            FLUVI_WARN("media", "GPU wait > 2 s (value {})", value);
        }
    }

    std::shared_ptr<detail::Slot> AcquireSlot(uint32_t w, uint32_t h) {
        std::shared_ptr<detail::Slot> found;
        for (auto it = slots_.begin(); it != slots_.end();) {
            const bool free = it->use_count() == 1;
            if (free && ((*it)->width != w || (*it)->height != h)) { it = slots_.erase(it); continue; }
            if (free && !found) found = *it;
            ++it;
        }
        if (found) return found;
        if (slots_.size() >= kMaxSlots) return nullptr;
        auto slot = std::make_shared<detail::Slot>();
        gpu::TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = DXGI_FORMAT_NV12;
        td.shaderRead = true;
        td.initialState = D3D12_RESOURCE_STATE_COMMON;
        td.debugName = L"fluvi.media.nv12";
        Status st;
        slot->texture = desc_.device->CreateTexture(td, st);
        if (!slot->texture) {
            FLUVI_ERROR("media", "NV12 texture {}x{}: {}", w, h, st.message);
            return nullptr;
        }
        slot->width = w;
        slot->height = h;
        slots_.push_back(slot);
        return slot;
    }

    // Software picture -> upload buffer (NV12 footprint of the slot texture). False on an unknown format.
    bool StageSoftware(const AVFrame* f, ID3D12Resource* dst, D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]) {
        const D3D12_RESOURCE_DESC rd = dst->GetDesc();
        UINT rows[2] = {};
        UINT64 rowBytes[2] = {}, total = 0;
        desc_.device->D3D()->GetCopyableFootprints(&rd, 0, 2, 0, fp, rows, rowBytes, &total);
        if (uploadSize_ < total) {
            if (uploadPtr_) upload_->Unmap(0, nullptr);
            uploadPtr_ = nullptr;
            upload_.Reset();
            if (!CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, total, false, &upload_) ||
                FAILED(upload_->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr_)))) { uploadSize_ = 0; return false; }
            uploadSize_ = total;
        }
        const uint32_t w = uint32_t(f->width), h = uint32_t(f->height);
        uint8_t* luma = uploadPtr_ + fp[0].Offset;
        uint8_t* chroma = uploadPtr_ + fp[1].Offset;
        const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
        for (uint32_t y = 0; y < h; ++y) std::memcpy(luma + size_t(y) * fp[0].Footprint.RowPitch, f->data[0] + ptrdiff_t(y) * f->linesize[0], w);
        if (f->format == AV_PIX_FMT_NV12) {
            for (uint32_t y = 0; y < ch; ++y) std::memcpy(chroma + size_t(y) * fp[1].Footprint.RowPitch, f->data[1] + ptrdiff_t(y) * f->linesize[1], size_t(cw) * 2);
        } else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
            for (uint32_t y = 0; y < ch; ++y) {
                uint8_t* row = chroma + size_t(y) * fp[1].Footprint.RowPitch;
                const uint8_t* u = f->data[1] + ptrdiff_t(y) * f->linesize[1];
                const uint8_t* v = f->data[2] + ptrdiff_t(y) * f->linesize[2];
                for (uint32_t x = 0; x < cw; ++x) { row[2 * x] = u[x]; row[2 * x + 1] = v[x]; }
            }
        } else {
            return false;
        }
        return true;
    }

    DecodedFramePtr Output(const AVFrame* f, const Meta& meta) {
        const bool hw = f->format == AV_PIX_FMT_D3D12;
        ID3D12Resource* src = nullptr;
        UINT srcSlice = 0, srcArray = 1;
        ID3D12Fence* srcFence = nullptr;
        uint64_t srcFenceValue = 0;
        uint32_t texW = (uint32_t(f->width) + 1) & ~1u, texH = (uint32_t(f->height) + 1) & ~1u;
        if (hw) {
            const auto* d3d = reinterpret_cast<const AVD3D12VAFrame*>(f->data[0]);
            if (!d3d || !d3d->texture) return nullptr;
            src = d3d->texture;
            const D3D12_RESOURCE_DESC sd = src->GetDesc();
            if (sd.Format != DXGI_FORMAT_NV12) {
                FLUVI_ERROR("media", "d3d12va surface format {} is not NV12", int(sd.Format));
                return nullptr;
            }
            srcSlice = UINT(d3d->subresource_index);
            srcArray = sd.DepthOrArraySize;
            srcFence = d3d->sync_ctx.fence;
            srcFenceValue = d3d->sync_ctx.fence_value;
            texW = uint32_t(sd.Width);   // the decoder's aligned surface; the copy takes it whole
            texH = sd.Height;
        }
        const uint32_t visW = uint32_t(f->width), visH = uint32_t(f->height);
        if (visW != lastW_ || visH != lastH_) {
            discontinuity_ = true;
            lastW_ = visW;
            lastH_ = visH;
        }
        if (prevSlot_ && (prevSlot_->width != texW || prevSlot_->height != texH)) prevSlot_.reset();

        auto slot = AcquireSlot(texW, texH);
        if (!slot) {
            std::lock_guard lock(statsMutex_);
            ++stats_.poolExhausted;
            return nullptr;
        }
        const bool compare = prevSlot_ && !discontinuity_ && !meta.idr;

        allocator_->Reset();
        list_->Reset(allocator_.Get(), nullptr);
        ID3D12Resource* dst = slot->texture->View().resource;
        auto b = Transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->ResourceBarrier(1, &b);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = plane;
            D3D12_TEXTURE_COPY_LOCATION from{};
            if (hw) {
                from.pResource = src;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = srcSlice + plane * srcArray;
            }
            if (!hw && plane == 0 && !StageSoftware(f, dst, footprints_)) {
                list_->Close();
                FLUVI_ERROR("media", "software picture format {} not supported", int(f->format));
                return nullptr;
            }
            if (!hw) {
                from.pResource = upload_.Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                from.PlacedFootprint = footprints_[plane];
            }
            list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        if (compare) {
            D3D12_RESOURCE_BARRIER bs[2] = {
                Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                Transition(result_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
            list_->ResourceBarrier(2, bs);
            list_->CopyBufferRegion(result_.Get(), 0, zero_.Get(), 0, 16);
            b = Transition(result_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list_->ResourceBarrier(1, &b);
            ID3D12DescriptorHeap* heaps[] = {desc_.device->SrvUavHeap().Heap()};
            list_->SetDescriptorHeaps(1, heaps);
            list_->SetComputeRootSignature(rootSig_.Get());
            list_->SetPipelineState(pso_.Get());
            const uint32_t constants[4] = {visW, visH, desc_.repeat.pixelDelta, desc_.repeat.changedPixels};
            list_->SetComputeRoot32BitConstants(0, 4, constants, 0);
            list_->SetComputeRootDescriptorTable(1, slot->texture->View().srv);
            list_->SetComputeRootDescriptorTable(2, prevSlot_->texture->View().srv);
            list_->SetComputeRootUnorderedAccessView(3, result_->GetGPUVirtualAddress());
            list_->Dispatch((visW + 15) / 16, (visH + 15) / 16, 1);
            D3D12_RESOURCE_BARRIER after[2] = {
                Transition(result_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)};
            list_->ResourceBarrier(2, after);
            list_->CopyBufferRegion(readback_.Get(), 0, result_.Get(), 0, 16);
            b = Transition(result_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            list_->ResourceBarrier(1, &b);
        } else {
            b = Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            list_->ResourceBarrier(1, &b);
        }
        if (FAILED(list_->Close())) {
            FLUVI_ERROR("media", "command list Close failed");
            return nullptr;
        }
        // GPU-side waits: the decoder's picture, and the consumer's last read of this pool texture.
        if (srcFence) queue_->Wait(srcFence, srcFenceValue);
        {
            std::lock_guard lock(slot->mutex);
            if (slot->readFence) queue_->Wait(slot->readFence.Get(), slot->readValue);
            slot->readFence.Reset();
        }
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fenceValue_);
        WaitGpu(fenceValue_);   // this thread only; the AVFrame (FFmpeg's surface) is released after this

        auto frame = std::make_shared<DecodedFrame>();
        frame->texture = slot->texture->View();
        frame->width = visW;
        frame->height = visH;
        frame->fullRange = f->color_range == AVCOL_RANGE_JPEG;
        frame->bt601 = f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M;
        slot->readyFence = fence_;
        frame->readyFence = fence_.Get();
        frame->readyValue = fenceValue_;
        frame->rtpTimestamp = meta.rtp;
        frame->arrivalQpc = meta.arrival;
        frame->submittedQpc = meta.submitted;
        frame->decodedQpc = time::NowQpc();
        frame->decodeMs = time::ElapsedMs(meta.submitted, frame->decodedQpc);
        frame->keyframe = meta.idr;
        frame->discontinuity = discontinuity_;
        frame->auBytes = meta.bytes;
        frame->slot = slot;
        if (compare) {
            const D3D12_RANGE read{0, 16}, none{0, 0};
            const uint32_t* r = nullptr;
            if (SUCCEEDED(readback_->Map(0, &read, reinterpret_cast<void**>(const_cast<uint32_t**>(&r))))) {
                frame->changedTiles = r[0];
                frame->maxLumaDelta = r[1];
                readback_->Unmap(0, &none);
            }
            frame->isNew = frame->changedTiles >= std::max(desc_.repeat.changedTiles, 1u);
        }
        hardwareActive_ = hw;
        Cadence(*frame);
        discontinuity_ = false;
        prevSlot_ = slot;
        return frame;
    }

    // ---- content cadence -------------------------------------------------------------------------------------
    void Cadence(DecodedFrame& f) {
        if (haveRtp_ && f.rtpTimestamp < lastRtp_ && lastRtp_ - f.rtpTimestamp > 0x80000000u) rtpHigh_ += int64_t(1) << 32;
        lastRtp_ = f.rtpTimestamp;
        haveRtp_ = true;
        const int64_t rtp = rtpHigh_ + int64_t(f.rtpTimestamp);
        if (f.discontinuity) { window_.clear(); haveNew_ = false; }
        f.index = decoded_++;
        if (f.isNew) {
            ++contentIndex_;
            f.contentDuration90k = haveNew_ && rtp > lastNewRtp_ ? uint32_t(std::min<int64_t>(rtp - lastNewRtp_, UINT32_MAX)) : 0;
            lastNewRtp_ = rtp;
            haveNew_ = true;
            repeatRun_ = 0;
        } else {
            ++repeatRun_;
        }
        f.contentIndex = contentIndex_;
        f.repeatRun = repeatRun_;
        window_.push_back({rtp, f.isNew});
        while (window_.size() > 2 && rtp - window_.front().first > kCadenceWindow90k) window_.pop_front();
        double streamFps = 0, contentFps = 0;
        if (window_.size() >= 2 && rtp > window_.front().first)
            streamFps = double(window_.size() - 1) * 90000.0 / double(rtp - window_.front().first);
        int64_t firstNew = -1, lastNew = -1;
        size_t news = 0;
        for (const auto& [t, isNew] : window_)
            if (isNew) { if (firstNew < 0) firstNew = t; lastNew = t; ++news; }
        if (news >= 2 && lastNew > firstNew) contentFps = double(news - 1) * 90000.0 / double(lastNew - firstNew);
        f.streamFps = streamFps;
        f.contentFps = contentFps;

        std::lock_guard lock(statsMutex_);
        ++stats_.framesDecoded;
        if (f.isNew) ++stats_.framesNew; else ++stats_.framesRepeated;
        stats_.decodeMs = stats_.decodeMs > 0 ? stats_.decodeMs * 0.9 + f.decodeMs * 0.1 : f.decodeMs;
        stats_.streamFps = streamFps;
        stats_.contentFps = contentFps;
        stats_.width = f.width;
        stats_.height = f.height;
        stats_.hardware = hardwareActive_;
        stats_.backend = hardwareActive_ ? "d3d12va" : "software";
    }

    VideoDecoderDesc desc_;
    // Thread + queues.
    std::thread thread_;
    std::atomic<bool> stop_{false}, resetRequested_{false};
    LatestQueue<Unit, 8> input_;
    LatestQueue<DecodedFramePtr, 2> output_;
    std::mutex pushMutex_;
    uint64_t pushSeq_ = 0, nextSeq_ = 0;
    bool haveSeq_ = false;
    // Decode state (decode thread only).
    AVBufferRef* hwDevice_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    bool useSoftware_ = false, hardwareActive_ = false, failed_ = false;
    bool waitingKey_ = true, discontinuity_ = true;
    int64_t lastKeyframeRequest_ = 0;
    int64_t ptsCounter_ = 0;
    std::deque<Meta> metas_;
    h264::Parser parser_;
    h264::FrameNumTracker frameNums_;
    xhome::VideoDecodeRecovery recovery_;
    uint32_t lastW_ = 0, lastH_ = 0;
    // Cadence.
    bool haveRtp_ = false, haveNew_ = false;
    uint32_t lastRtp_ = 0, repeatRun_ = 0;
    int64_t rtpHigh_ = 0, lastNewRtp_ = 0;
    uint64_t decoded_ = 0, contentIndex_ = 0;
    std::deque<std::pair<int64_t, bool>> window_;
    // GPU.
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fenceValue_ = 0;
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12Resource> result_, zero_, readback_, upload_;
    uint8_t* uploadPtr_ = nullptr;
    uint64_t uploadSize_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints_[2]{};
    std::vector<std::shared_ptr<detail::Slot>> slots_;
    std::shared_ptr<detail::Slot> prevSlot_;
    // Stats.
    mutable std::mutex statsMutex_;
    VideoDecoderStats stats_;
};

} // namespace

std::unique_ptr<VideoDecoder> VideoDecoder::Create(const VideoDecoderDesc& desc, Status& status) {
    if (!desc.device) { status = Status::Fail("media: VideoDecoderDesc::device é obrigatório"); return nullptr; }
    auto decoder = std::make_unique<VideoDecoderImpl>(desc);
    status = decoder->Init();
    if (!status) return nullptr;
    return decoder;
}

} // namespace fluvi::media
