// SPDX-License-Identifier: GPL-3.0-only
#include "PresentModeEtw.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"

namespace fluvi::gpu {
namespace {

// Microsoft-Windows-Win32k
constexpr GUID kWin32k = {0x8c416c79, 0xd49b, 0x4f01, {0xa4, 0x67, 0xe5, 0x6d, 0x3a, 0xa8, 0x23, 0x4c}};
constexpr USHORT kTokenCompositionSurfaceObject = 201;
constexpr USHORT kTokenStateChanged = 301;
constexpr ULONGLONG kKeywordUpdates = 0x1000;   // both events carry it
constexpr wchar_t kSessionName[] = L"FluviPresentMode";

template <typename T>
bool ReadRaw(const EVENT_RECORD* r, size_t offset, T& out) {
    if (offset + sizeof(T) > r->UserDataLength) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(r->UserData) + offset, sizeof(T));
    return true;
}

// TDH fallback for event versions whose layout we have not verified.
bool ReadTdh(EVENT_RECORD* r, const wchar_t* name, uint64_t& out) {
    PROPERTY_DATA_DESCRIPTOR d{};
    d.PropertyName = reinterpret_cast<ULONGLONG>(name);
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (TdhGetPropertySize(r, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0 || size > 8) return false;
    uint64_t v = 0;
    if (TdhGetProperty(r, 0, nullptr, 1, &d, size, reinterpret_cast<PBYTE>(&v)) != ERROR_SUCCESS) return false;
    out = v;
    return true;
}

class EtwImpl final : public PresentModeEtw {
public:
    ~EtwImpl() override { Stop(); }

    bool Start() {
        pid_ = GetCurrentProcessId();
        props_.resize(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName) * 2);
        auto* p = Props();
        // Stop a session left behind by a crashed run.
        ControlTraceW(0, kSessionName, p, EVENT_TRACE_CONTROL_STOP);
        ResetProps();
        ULONG err = StartTraceW(&session_, kSessionName, p);
        if (err != ERROR_SUCCESS) {
            if (err == ERROR_ACCESS_DENIED)
                FLUVI_INFO("gpu", "present mode: ETW needs admin, reporting Unknown");
            else
                FLUVI_WARN("gpu", "present mode: StartTrace failed ({})", err);
            session_ = 0;
            return false;
        }

        // Only the two events we parse; the Updates keyword alone is chatty.
        std::vector<uint8_t> filterBuf(offsetof(EVENT_FILTER_EVENT_ID, Events) + 2 * sizeof(USHORT));
        auto* ids = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filterBuf.data());
        ids->FilterIn = TRUE;
        ids->Count = 2;
        ids->Events[0] = kTokenCompositionSurfaceObject;
        reinterpret_cast<USHORT*>(&ids->Events[0])[1] = kTokenStateChanged;
        EVENT_FILTER_DESCRIPTOR filter{};
        filter.Ptr = reinterpret_cast<ULONGLONG>(filterBuf.data());
        filter.Size = ULONG(filterBuf.size());
        filter.Type = EVENT_FILTER_TYPE_EVENT_ID;
        ENABLE_TRACE_PARAMETERS params{};
        params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
        params.EnableFilterDesc = &filter;
        params.FilterDescCount = 1;
        err = EnableTraceEx2(session_, &kWin32k, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                             kKeywordUpdates, 0, 0, &params);
        if (err != ERROR_SUCCESS)
            err = EnableTraceEx2(session_, &kWin32k, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                                 kKeywordUpdates, 0, 0, nullptr);
        if (err != ERROR_SUCCESS) {
            FLUVI_WARN("gpu", "present mode: EnableTraceEx2 failed ({})", err);
            Stop();
            return false;
        }

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName = const_cast<LPWSTR>(kSessionName);
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        lf.EventRecordCallback = &EtwImpl::OnEvent;
        lf.Context = this;
        consumer_ = OpenTraceW(&lf);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
            FLUVI_WARN("gpu", "present mode: OpenTrace failed ({})", GetLastError());
            consumer_ = 0;
            Stop();
            return false;
        }
        thread_ = std::thread([this] {
            TRACEHANDLE h = consumer_;
            ProcessTrace(&h, 1, nullptr, nullptr);
        });
        FLUVI_INFO("gpu", "present mode: ETW reader on (Win32k token events)");
        return true;
    }

    PresentMode Mode() const override {
        std::lock_guard lock(mutex_);
        const int64_t cutoff = time::NowQpc() - time::QpcFrequency();
        size_t iflip = 0, total = 0;
        for (auto& [qpc, flip] : verdicts_)
            if (qpc >= cutoff) { ++total; iflip += flip; }
        if (!total) return last_;
        return iflip * 2 > total ? PresentMode::IndependentFlip : PresentMode::Composed;
    }

    uint64_t PresentsSeen() const override {
        std::lock_guard lock(mutex_);
        return seen_;
    }

private:
    EVENT_TRACE_PROPERTIES* Props() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_.data()); }
    void ResetProps() {
        std::fill(props_.begin(), props_.end(), uint8_t(0));
        auto* p = Props();
        p->Wnode.BufferSize = ULONG(props_.size());
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;   // QPC timestamps
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p->FlushTimer = 1;
        p->BufferSize = 64;
        p->MinimumBuffers = 4;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    }

    void Stop() {
        if (session_) {
            ResetProps();
            ControlTraceW(session_, nullptr, Props(), EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
        }
        if (consumer_) {
            CloseTrace(consumer_);
            consumer_ = 0;
        }
        if (thread_.joinable()) thread_.join();
    }

    static void WINAPI OnEvent(EVENT_RECORD* r) { static_cast<EtwImpl*>(r->UserContext)->Handle(r); }

    void Handle(EVENT_RECORD* r) {
        if (r->EventHeader.ProviderId != kWin32k) return;
        const USHORT id = r->EventHeader.EventDescriptor.Id;
        const bool v1 = r->EventHeader.EventDescriptor.Version == 1;
        const size_t P = (r->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
        if (id == kTokenCompositionSurfaceObject) {
            if (r->EventHeader.ProcessId != pid_) return;
            uint64_t luid = 0;
            if (!(v1 ? ReadRaw(r, 2 * P + 12, luid) : ReadTdh(r, L"CompositionSurfaceLuid", luid))) return;
            std::lock_guard lock(mutex_);
            if (surfaces_.size() > 64) surfaces_.clear();
            surfaces_.insert(luid);
        } else if (id == kTokenStateChanged) {
            uint64_t luid = 0, presentCount = 0, iflip = 0;
            bool ok;
            if (v1) {
                uint32_t pc = 0, f = 0;
                ok = ReadRaw(r, P + 4, pc) && ReadRaw(r, P + 20, f) && ReadRaw(r, P + 28, luid);
                presentCount = pc;
                iflip = f;
            } else {
                ok = ReadTdh(r, L"CompositionSurfaceLuid", luid) && ReadTdh(r, L"PresentCount", presentCount) &&
                     ReadTdh(r, L"IndependentFlip", iflip);
            }
            if (!ok) return;
            std::lock_guard lock(mutex_);
            if (!surfaces_.count(luid)) return;
            auto& s = tokens_[luid];
            s.pending[presentCount] |= iflip != 0;
            s.newest = std::max(s.newest, presentCount);
            // A present is final once a few newer ones are moving through the token manager.
            const int64_t now = r->EventHeader.TimeStamp.QuadPart;
            for (auto it = s.pending.begin(); it != s.pending.end();) {
                if (it->first + 3 <= s.newest) {
                    verdicts_.emplace_back(now, it->second);
                    last_ = it->second ? PresentMode::IndependentFlip : PresentMode::Composed;
                    ++seen_;
                    it = s.pending.erase(it);
                } else {
                    ++it;
                }
            }
            const int64_t cutoff = now - 2 * time::QpcFrequency();
            while (!verdicts_.empty() && verdicts_.front().first < cutoff) verdicts_.pop_front();
        }
    }

    struct Surface {
        std::unordered_map<uint64_t, bool> pending;   // present count -> any IndependentFlip
        uint64_t newest = 0;
    };

    DWORD pid_ = 0;
    std::vector<uint8_t> props_;
    TRACEHANDLE session_ = 0, consumer_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::unordered_set<uint64_t> surfaces_;
    std::unordered_map<uint64_t, Surface> tokens_;
    std::deque<std::pair<int64_t, bool>> verdicts_;
    PresentMode last_ = PresentMode::Unknown;
    uint64_t seen_ = 0;
};

} // namespace

std::shared_ptr<PresentModeEtw> PresentModeEtw::Acquire() {
    static std::mutex mutex;
    static std::weak_ptr<PresentModeEtw> shared;
    static bool deniedLogged = false;
    std::lock_guard lock(mutex);
    if (auto existing = shared.lock()) return existing;
    if (deniedLogged) return nullptr;   // don't retry StartTrace on every swapchain once it failed
    auto etw = std::make_shared<EtwImpl>();
    if (!etw->Start()) {
        deniedLogged = true;
        return nullptr;
    }
    shared = etw;
    return etw;
}

} // namespace fluvi::gpu
