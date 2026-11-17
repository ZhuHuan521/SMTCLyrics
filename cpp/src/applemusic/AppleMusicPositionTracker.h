#pragma once

#include <windows.h>
#include <winstring.h>

#include <atomic>
#include <cstdint>
#include <new>

namespace smtc::applemusic {

struct TimedPositionSample {
    std::int64_t position100ns = 0;
    std::int64_t queryQpc = 0;
    bool fromEvent = false;
    bool playing = false;
};

// Calls are serialized by the bridge. A query must never give an unchanged
// cached position a new timestamp; only a property event supplies an exact one.
class AppleMusicPositionTracker {
public:
    void reset(std::uintptr_t identity = 0, std::int64_t qpc = 0) noexcept {
        identity_ = identity;
        trackObservedQpc_ = qpc;
        sample_ = {};
        haveSample_ = false;
        haveQuery_ = false;
        lastQueryPlaying_ = false;
        pauseObservedQpc_ = 0;
        haveNotification_ = false;
        notificationPosition_ = 0;
        notificationQpc_ = 0;
        notificationPlaying_ = false;
    }

    bool track(std::uintptr_t identity, std::int64_t qpc) noexcept {
        if (identity != identity_) {
            if (qpc < trackObservedQpc_) return false;
            reset(identity, qpc);
        } else if (qpc > trackObservedQpc_) {
            trackObservedQpc_ = qpc;
        }
        return true;
    }

    bool event(std::uintptr_t identity, std::int64_t position100ns,
               std::int64_t qpc, bool playing, bool mayChangeTrack = false) noexcept {
        if (!identity || qpc <= 0) return false;
        if (identity != identity_) {
            if (!mayChangeTrack || qpc < trackObservedQpc_) return false;
            reset(identity, qpc);
        }
        if (haveNotification_ && qpc < notificationQpc_) return false;
        if (haveSample_ && qpc < sample_.queryQpc &&
            (sample_.fromEvent || sample_.position100ns != position100ns || sample_.playing != playing)) return false;
        const bool changed = haveNotification_ ?
            notificationPosition_ != position100ns || notificationPlaying_ != playing :
            !haveSample_ || sample_.position100ns != position100ns || sample_.playing != playing;
        notificationPosition_ = position100ns;
        notificationQpc_ = qpc;
        notificationPlaying_ = playing;
        haveNotification_ = true;
        if (!changed) {
            // Compare successive notifications, not queries: a query may have
            // already read the newly published value before its callback runs.
            return sample_.fromEvent;
        }
        sample_ = {position100ns, qpc, true, playing};
        haveSample_ = true;
        return true;
    }

    TimedPositionSample query(std::uintptr_t identity, std::int64_t position100ns,
                              std::int64_t qpc, bool playing,
                              std::int64_t identityObservedQpc = 0) noexcept {
        if (!track(identity, identityObservedQpc > 0 ? identityObservedQpc : qpc)) {
            return {position100ns, qpc, false, playing};
        }
        if (!playing) {
            sample_ = {position100ns, qpc, false, false};
            haveSample_ = true;
            pauseObservedQpc_ = qpc;
        } else {
            const bool resumed = haveQuery_ && !lastQueryPlaying_;
            const bool eventIsUsable = haveSample_ && sample_.fromEvent && sample_.playing &&
                (!resumed || sample_.queryQpc > pauseObservedQpc_) &&
                (sample_.position100ns == position100ns || sample_.queryQpc > qpc);
            if (!eventIsUsable) {
                if (!haveSample_ || sample_.fromEvent || !sample_.playing ||
                    sample_.position100ns != position100ns || resumed) {
                    sample_ = {position100ns, qpc, false, true};
                    haveSample_ = true;
                }
            }
        }
        haveQuery_ = true;
        lastQueryPlaying_ = playing;
        return sample_;
    }

    std::uintptr_t identity() const noexcept { return identity_; }

private:
    std::uintptr_t identity_ = 0;
    std::int64_t trackObservedQpc_ = 0;
    std::int64_t pauseObservedQpc_ = 0;
    TimedPositionSample sample_;
    bool haveSample_ = false;
    bool haveQuery_ = false;
    bool lastQueryPlaying_ = false;
    bool haveNotification_ = false;
    std::int64_t notificationPosition_ = 0;
    std::int64_t notificationQpc_ = 0;
    bool notificationPlaying_ = false;
};

namespace property_events {

inline constexpr GUID kUnknownGuid{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
inline constexpr GUID kAgileGuid{0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
inline constexpr GUID kNotifyGuid{0x90B17601, 0xB065, 0x586E, {0x83, 0xD9, 0x9A, 0xDC, 0x3A, 0x69, 0x52, 0x84}};
inline constexpr GUID kHandlerGuid{0xE3DE52F6, 0x1E32, 0x5DA6, {0xBB, 0x2D, 0xB5, 0xB6, 0x09, 0x6C, 0x96, 0x2D}};
inline constexpr GUID kArgsGuid{0x63D0C952, 0x396B, 0x54F4, {0xAF, 0x8C, 0xBA, 0x87, 0x24, 0xA4, 0x27, 0xBF}};
inline constexpr GUID kLegacyNotifyGuid{0xCF75D69C, 0xF2F4, 0x486B, {0xB3, 0x02, 0xBB, 0x4C, 0x09, 0xBA, 0xEB, 0xFA}};
inline constexpr GUID kLegacyHandlerGuid{0x50F19C16, 0x0A22, 0x4D8E, {0xA0, 0x89, 0x1E, 0xA9, 0x95, 0x16, 0x57, 0xD2}};
inline constexpr GUID kLegacyArgsGuid{0x4F33A9A0, 0x5CF8, 0x47A4, {0xB1, 0x6F, 0xD7, 0xFA, 0xAF, 0x17, 0x45, 0x7E}};

struct UnknownVTable {
    HRESULT (__stdcall* QueryInterface)(void*, REFIID, void**) noexcept;
    ULONG (__stdcall* AddRef)(void*) noexcept;
    ULONG (__stdcall* Release)(void*) noexcept;
};

struct InspectableVTable : UnknownVTable {
    HRESULT (__stdcall* GetIids)(void*, ULONG*, IID**) noexcept;
    HRESULT (__stdcall* GetRuntimeClassName)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* GetTrustLevel)(void*, std::int32_t*) noexcept;
};

struct NotifyVTable : InspectableVTable {
    HRESULT (__stdcall* add_PropertyChanged)(void*, void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* remove_PropertyChanged)(void*, std::int64_t) noexcept;
};

struct ArgsVTable : InspectableVTable {
    HRESULT (__stdcall* get_PropertyName)(void*, HSTRING*) noexcept;
};

template <class Table> inline Table* table(void* object) noexcept {
    return *reinterpret_cast<Table**>(object);
}

inline void release(void* object) noexcept {
    if (object) table<UnknownVTable>(object)->Release(object);
}

inline std::int64_t qpcNow() noexcept {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    return qpc.QuadPart;
}

// WinRT delegates inherit IUnknown, not IInspectable. The callback runs on the
// publisher's thread and performs only a small read and a timestamp capture.
class PropertyChangedSubscription {
public:
    using Callback = void (*)(void*, void*, void*, std::int64_t) noexcept;

    PropertyChangedSubscription() = default;
    PropertyChangedSubscription(const PropertyChangedSubscription&) = delete;
    PropertyChangedSubscription& operator=(const PropertyChangedSubscription&) = delete;

    ~PropertyChangedSubscription() {
        disable();
        // Owners retain a failed close until the publisher has released its
        // delegate. Destruction is reached only after a successful close.
        close(0);
    }

    bool attach(void* object, void* context, Callback callback) noexcept {
        if (delegate_ || !object || !callback) return false;
        auto* unknown = table<UnknownVTable>(object);
        HRESULT hr = unknown->QueryInterface(object, kNotifyGuid, &notify_);
        if (FAILED(hr) || !notify_) {
            notify_ = nullptr;
            hr = unknown->QueryInterface(object, kLegacyNotifyGuid, &notify_);
        }
        if (FAILED(hr) || !notify_) return false;
        delegate_ = new (std::nothrow) Delegate;
        if (!delegate_) {
            release(notify_);
            notify_ = nullptr;
            return false;
        }
        delegate_->vtable = &kDelegateVTable;
        delegate_->context = context;
        delegate_->callback = callback;
        hr = table<NotifyVTable>(notify_)->add_PropertyChanged(notify_, delegate_, &token_);
        if (FAILED(hr)) {
            delegate_->enabled.store(false, std::memory_order_seq_cst);
            release(notify_);
            notify_ = nullptr;
            // A failed add must not retain the delegate under the WinRT contract.
            releaseDelegate(delegate_);
            delegate_ = nullptr;
            return false;
        }
        registered_ = true;
        return true;
    }

    void disable() noexcept {
        if (delegate_) delegate_->enabled.store(false, std::memory_order_seq_cst);
    }

    // No bridge lock may be held while this calls the publisher. A failed close
    // keeps the disabled delegate alive; callers must not unload the DLL.
    bool close(DWORD timeoutMs = 0) noexcept {
        disable();
        if (registered_) {
            const HRESULT hr = table<NotifyVTable>(notify_)->remove_PropertyChanged(notify_, token_);
            if (FAILED(hr)) return false;
            registered_ = false;
        }
        if (notify_) {
            release(notify_);
            notify_ = nullptr;
        }
        if (!delegate_) return true;
        const ULONGLONG started = GetTickCount64();
        while (delegate_->active.load(std::memory_order_seq_cst) != 0 ||
               delegate_->references.load(std::memory_order_seq_cst) != 1) {
            if (GetTickCount64() - started >= timeoutMs) return false;
            Sleep(1);
        }
        releaseDelegate(delegate_);
        delegate_ = nullptr;
        return true;
    }

private:
    struct Delegate;
    struct DelegateVTable : UnknownVTable {
        HRESULT (__stdcall* Invoke)(Delegate*, void*, void*) noexcept;
    };
    struct Delegate {
        DelegateVTable* vtable = nullptr;
        std::atomic<ULONG> references{1};
        std::atomic<ULONG> active{0};
        std::atomic<bool> enabled{true};
        void* context = nullptr;
        Callback callback = nullptr;
    };

    static HRESULT __stdcall queryDelegate(void* raw, REFIID iid, void** result) noexcept {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualGUID(iid, kUnknownGuid) || IsEqualGUID(iid, kHandlerGuid) ||
            IsEqualGUID(iid, kLegacyHandlerGuid) || IsEqualGUID(iid, kAgileGuid)) {
            *result = raw;
            addDelegate(raw);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    static ULONG __stdcall addDelegate(void* raw) noexcept {
        return static_cast<Delegate*>(raw)->references.fetch_add(1, std::memory_order_seq_cst) + 1;
    }
    static ULONG __stdcall releaseDelegate(void* raw) noexcept {
        auto* delegate = static_cast<Delegate*>(raw);
        const ULONG remaining = delegate->references.fetch_sub(1, std::memory_order_seq_cst) - 1;
        if (!remaining) delete delegate;
        return remaining;
    }
    static HRESULT __stdcall invokeDelegate(Delegate* delegate, void* sender, void* args) noexcept {
        delegate->active.fetch_add(1, std::memory_order_seq_cst);
        const auto qpc = qpcNow();
        if (delegate->enabled.load(std::memory_order_seq_cst)) {
            delegate->callback(delegate->context, sender, args, qpc);
        }
        delegate->active.fetch_sub(1, std::memory_order_seq_cst);
        return S_OK;
    }

    inline static DelegateVTable kDelegateVTable{
        {&queryDelegate, &addDelegate, &releaseDelegate}, &invokeDelegate};
    void* notify_ = nullptr;
    Delegate* delegate_ = nullptr;
    std::int64_t token_ = 0;
    bool registered_ = false;
};

}
}
