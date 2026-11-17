#include "applemusic/AppleMusicBridgeProtocol.h"
#include "applemusic/AppleMusicPositionTracker.h"

#include <windows.h>
#include <aclapi.h>
#include <roapi.h>
#include <sddl.h>
#include <winstring.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <new>
#include <string>
#include <string_view>

namespace {

namespace bridge = smtc::applemusic::bridge;
HMODULE gSelfModule = nullptr;

constexpr GUID kIActivationFactoryGuid{0x00000035, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIPlayerStaticsGuid{0x34D100F9, 0x0F0A, 0x5980, {0x8F, 0x04, 0x7A, 0xF5, 0x7F, 0xDB, 0x29, 0xB0}};
constexpr GUID kIPlayerStateGuid{0xBEFB99EB, 0x336F, 0x5199, {0x9F, 0xB2, 0x63, 0x63, 0x5B, 0xC4, 0x73, 0x19}};

// WinRT 在接口成员变化时会重新生成接口 IID。Apple Music 1.1792.39682.0
// 更新了 IPlayer 和 INowPlayingItem，但保留了需要读取的前缀成员布局。
// 同时保留旧 IID，兼容仍使用旧版客户端的系统。
constexpr GUID kIPlayerGuids[]{
    {0xF807B158, 0xB2E7, 0x5CF7, {0xA5, 0x12, 0x8B, 0x0B, 0xE7, 0x67, 0xDF, 0x1B}},
    {0x958DA6F6, 0x2E93, 0x5BA7, {0xB6, 0x04, 0x28, 0xDD, 0x57, 0xFE, 0xB8, 0xE0}},
};

constexpr GUID kINowPlayingItemGuids[]{
    {0x2DC5F65C, 0x28F6, 0x58AA, {0x87, 0xCF, 0xE4, 0xA7, 0xDF, 0x9B, 0x9A, 0x4B}},
    {0xE376ACB7, 0x0569, 0x56C5, {0x84, 0x43, 0xA2, 0x88, 0xAC, 0xD3, 0x62, 0xEA}},
};

struct InspectableVTable {
    HRESULT (__stdcall* QueryInterface)(void*, REFIID, void**) noexcept;
    ULONG (__stdcall* AddRef)(void*) noexcept;
    ULONG (__stdcall* Release)(void*) noexcept;
    HRESULT (__stdcall* GetIids)(void*, ULONG*, IID**) noexcept;
    HRESULT (__stdcall* GetRuntimeClassName)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* GetTrustLevel)(void*, TrustLevel*) noexcept;
};

struct IPlayerStaticsVTable : InspectableVTable {
    HRESULT (__stdcall* get_Current)(void*, void**) noexcept;
};

struct IPlayerVTable : InspectableVTable {
    HRESULT (__stdcall* get_NowPlayingItem)(void*, void**) noexcept;
    HRESULT (__stdcall* get_PlayerState)(void*, void**) noexcept;
};

struct INowPlayingItemVTable : InspectableVTable {
    HRESULT (__stdcall* get_Subtitle)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_Artist)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_Album)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_TrackNumber)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_TrackCount)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_Artwork)(void*, void**) noexcept;
    HRESULT (__stdcall* get_PlaybackMediaType)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_ExplicitState)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_VideoRatingString)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_RottonTomatoRating)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_RottonTomatoScore)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_CommonSenseRating)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_ISOReleaseDate)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_Name)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_TVShowName)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_Genre)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_Description)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_Accessibility)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_IsPlaceholder)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_IsRadio)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_IsRadioLiveStream)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_Reactions)(void*, void**) noexcept;
    HRESULT (__stdcall* get_ShowCanonicalId)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_ScrubberMode)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_CurrentPosition)(void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* get_Duration)(void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* get_LoadTimeIsValid)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_LoadTime)(void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* get_MinLoadTime)(void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* get_CanBePositioned)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_LiveMode)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_CurrentDate)(void*, double*) noexcept;
    HRESULT (__stdcall* get_AudioFormat)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_AudioFormatSampleRate)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_AudioFormatBitDepth)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_HLSMetadata)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_HasVideo)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_VideoPresentationSize)(void*, void**) noexcept;
    HRESULT (__stdcall* get_VideoColorCapability)(void*, void**) noexcept;
    HRESULT (__stdcall* get_PlayQueueItemID)(void*, double*) noexcept;
    HRESULT (__stdcall* get_LyricsInfo)(void*, void**) noexcept;
    HRESULT (__stdcall* get_AudibleAlternateTracks)(void*, void**) noexcept;
    HRESULT (__stdcall* get_SelectedAudibleTrack)(void*, void**) noexcept;
    HRESULT (__stdcall* get_LegibleAlternateTracks)(void*, void**) noexcept;
    HRESULT (__stdcall* get_SelectedLegibleTrack)(void*, void**) noexcept;
    HRESULT (__stdcall* get_VisualAlternateTracks)(void*, void**) noexcept;
};

struct IPlayerStateVTable : InspectableVTable {
    HRESULT (__stdcall* get_CanPlay)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanPause)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanStop)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanRewind)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanFF)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanGoBack)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanSkipPrevious)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanSkipNext)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_PreferMusicControls)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_CanShuffle)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_ShuffleMode)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_CanRepeat)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_RepeatMode)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_TargetPlayRate)(void*, double*) noexcept;
    HRESULT (__stdcall* get_CanSetVolume)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_Volume)(void*, double*) noexcept;
    HRESULT (__stdcall* get_CanMute)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_IsMuted)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_IsPaused)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_IsPlaying)(void*, bool*) noexcept;
};

using DllGetActivationFactoryFn = HRESULT (__stdcall*)(HSTRING, void**) noexcept;

template <typename VTable>
VTable* vtable(void* object) {
    return *reinterpret_cast<VTable**>(object);
}

void releaseInspectable(void* object) {
    if (object) {
        vtable<InspectableVTable>(object)->Release(object);
    }
}

HRESULT queryInterface(void* object, REFIID iid, void** result) {
    if (!object || !result) return E_POINTER;
    *result = nullptr;
    return vtable<InspectableVTable>(object)->QueryInterface(object, iid, result);
}

template <std::size_t Size>
HRESULT queryFirstInterface(void* object, const GUID (&iids)[Size], void** result) {
    if (!object || !result) return E_POINTER;
    *result = nullptr;

    HRESULT lastResult = E_NOINTERFACE;
    for (const auto& iid : iids) {
        lastResult = queryInterface(object, iid, result);
        if (SUCCEEDED(lastResult) && *result) return lastResult;
    }
    *result = nullptr;
    return lastResult;
}

bool hasGuid(const GUID& guid) {
    constexpr GUID empty{};
    return !IsEqualGUID(guid, empty);
}

HRESULT queryFirstInterface(void* object, const GUID& preferred, const GUID* fallbacks, std::size_t fallbackCount, void** result) {
    if (!object || !result) return E_POINTER;
    *result = nullptr;

    HRESULT lastResult = E_NOINTERFACE;
    if (hasGuid(preferred)) {
        lastResult = queryInterface(object, preferred, result);
        if (SUCCEEDED(lastResult) && *result) return lastResult;
    }
    for (std::size_t i = 0; i < fallbackCount; ++i) {
        lastResult = queryInterface(object, fallbacks[i], result);
        if (SUCCEEDED(lastResult) && *result) return lastResult;
    }
    *result = nullptr;
    return lastResult;
}

HRESULT queryPlayerInterface(void* object, const bridge::SnapshotRequest& request, void** result) {
    return queryFirstInterface(object, request.playerIid, kIPlayerGuids, std::size(kIPlayerGuids), result);
}

HRESULT queryNowPlayingInterface(void* object, const bridge::SnapshotRequest& request, void** result) {
    return queryFirstInterface(object, request.nowPlayingItemIid, kINowPlayingItemGuids, std::size(kINowPlayingItemGuids), result);
}

std::wstring hstringToWide(HSTRING value) {
    if (!value) return {};
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(value, &length);
    std::wstring text(raw, raw + length);
    WindowsDeleteString(value);
    return text;
}

std::wstring getHString(void* object, HRESULT (__stdcall* getter)(void*, HSTRING*) noexcept) {
    HSTRING value = nullptr;
    if (!object || !getter || FAILED(getter(object, &value))) return {};
    return hstringToWide(value);
}

std::int64_t getInt64(
    void* object,
    HRESULT (__stdcall* getter)(void*, std::int64_t*) noexcept,
    bool* succeeded = nullptr) {
    std::int64_t value = 0;
    const bool ok = object && getter && SUCCEEDED(getter(object, &value));
    if (succeeded) *succeeded = ok;
    return ok ? value : 0;
}

bool getBool(void* object, HRESULT (__stdcall* getter)(void*, bool*) noexcept) {
    bool value = false;
    if (!object || !getter || FAILED(getter(object, &value))) return false;
    return value;
}

template <std::size_t Size>
void copyWide(wchar_t (&target)[Size], std::wstring_view source) {
    const std::size_t count = std::min<std::size_t>(source.size(), Size - 1);
    std::copy_n(source.data(), count, target);
    target[count] = L'\0';
}

void setError(bridge::SnapshotResponse& response, bridge::Status status, HRESULT hr, std::wstring_view message) {
    response.status = static_cast<std::uint32_t>(status);
    response.hresult = static_cast<std::int32_t>(hr);
    copyWide(response.error, message);
}

std::int64_t queryQpc() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

void captureQpc(bridge::SnapshotResponse& response) {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    response.queryQpc = counter.QuadPart;
    response.qpcFrequency = frequency.QuadPart;
}

std::uintptr_t objectIdentity(void* object) noexcept {
    void* identity = nullptr;
    if (!object || FAILED(queryInterface(object, smtc::applemusic::property_events::kUnknownGuid, &identity)) || !identity) {
        return 0;
    }
    const auto value = reinterpret_cast<std::uintptr_t>(identity);
    releaseInspectable(identity);
    return value;
}

// One subscription per player/item object. Position changes are stamped on the
// publishing thread; snapshot requests and the render timer remain low-cost.
class PositionEventObserver {
public:
    smtc::applemusic::TimedPositionSample snapshot(
        void* player, void* itemObject, const bridge::SnapshotRequest& request,
        std::int64_t rawPosition, std::int64_t rawQpc, bool playing,
        std::int64_t identityObservedQpc) noexcept {
        const auto playerIdentity = objectIdentity(player);
        const auto itemIdentity = objectIdentity(itemObject);
        if (!stopping_.load(std::memory_order_seq_cst)) {
            ensureBindings(player, playerIdentity, itemObject, itemIdentity, request, identityObservedQpc);
        }
        AcquireSRWLockExclusive(&sampleLock_);
        const auto sample = tracker_.query(itemIdentity, rawPosition, rawQpc, playing, identityObservedQpc);
        ReleaseSRWLockExclusive(&sampleLock_);
        return sample;
    }

    bool close(DWORD timeoutMs) noexcept {
        stopping_.store(true, std::memory_order_seq_cst);
        retire(playerBinding_);
        retire(itemBinding_);
        const auto started = GetTickCount64();
        Binding** cursor = &retired_;
        while (*cursor) {
            Binding* binding = *cursor;
            const auto elapsed = GetTickCount64() - started;
            const DWORD remaining = elapsed < timeoutMs ? static_cast<DWORD>(timeoutMs - elapsed) : 0;
            if (binding->subscription.close(remaining)) {
                *cursor = binding->next;
                delete binding;
            } else {
                cursor = &binding->next;
            }
        }
        return retired_ == nullptr;
    }

private:
    enum class Kind { Player, Item };
    struct Binding {
        PositionEventObserver* owner = nullptr;
        Kind kind = Kind::Item;
        void* player = nullptr;
        std::uintptr_t playerIdentity = 0;
        std::uintptr_t objectIdentity = 0;
        bridge::SnapshotRequest request;
        smtc::applemusic::property_events::PropertyChangedSubscription subscription;
        Binding* next = nullptr;

        ~Binding() {
            // Destruction only follows a successful subscription.close().
            releaseInspectable(player);
        }
    };

    static bool isRelevantProperty(void* args, Kind kind) noexcept {
        using namespace smtc::applemusic::property_events;
        if (!args) return false;
        void* propertyArgs = nullptr;
        HRESULT hr = table<UnknownVTable>(args)->QueryInterface(args, kArgsGuid, &propertyArgs);
        if (FAILED(hr) || !propertyArgs) {
            propertyArgs = nullptr;
            hr = table<UnknownVTable>(args)->QueryInterface(args, kLegacyArgsGuid, &propertyArgs);
        }
        if (FAILED(hr) || !propertyArgs) return false;
        HSTRING name = nullptr;
        hr = table<ArgsVTable>(propertyArgs)->get_PropertyName(propertyArgs, &name);
        release(propertyArgs);
        if (FAILED(hr)) return false;
        UINT32 length = 0;
        const wchar_t* text = WindowsGetStringRawBuffer(name, &length);
        const std::wstring_view property(text ? text : L"", length);
        const bool relevant = kind == Kind::Player ?
            property.empty() || property == L"NowPlayingItem" : property == L"CurrentPosition";
        WindowsDeleteString(name);
        return relevant;
    }

    static bool readPlaying(void* player, const bridge::SnapshotRequest& request) noexcept {
        void* stateObject = nullptr;
        void* state = nullptr;
        bool playing = false;
        if (player && SUCCEEDED(vtable<IPlayerVTable>(player)->get_PlayerState(player, &stateObject)) && stateObject) {
            const GUID& stateGuid = hasGuid(request.playerStateIid) ? request.playerStateIid : kIPlayerStateGuid;
            if (SUCCEEDED(queryInterface(stateObject, stateGuid, &state)) && state) {
                playing = getBool(state, vtable<IPlayerStateVTable>(state)->get_IsPlaying);
            }
        }
        releaseInspectable(state);
        releaseInspectable(stateObject);
        return playing;
    }

    static void capture(void* context, void* sender, void* args, std::int64_t qpc) noexcept {
        auto* binding = static_cast<Binding*>(context);
        auto* owner = binding->owner;
        if (owner->stopping_.load(std::memory_order_seq_cst) ||
            binding->playerIdentity != owner->playerIdentity_.load(std::memory_order_seq_cst) ||
            !isRelevantProperty(args, binding->kind)) return;

        void* itemObject = nullptr;
        void* item = nullptr;
        void* player = nullptr;
        if (binding->kind == Kind::Player) {
            if (SUCCEEDED(queryPlayerInterface(sender, binding->request, &player)) && player) {
                vtable<IPlayerVTable>(player)->get_NowPlayingItem(player, &itemObject);
            }
        } else if (sender) {
            itemObject = sender;
            vtable<InspectableVTable>(itemObject)->AddRef(itemObject);
        }
        if (itemObject) queryNowPlayingInterface(itemObject, binding->request, &item);
        bool positionRead = false;
        const auto position = item ? getInt64(item, vtable<INowPlayingItemVTable>(item)->get_CurrentPosition, &positionRead) : 0;
        const auto identity = objectIdentity(itemObject);
        const bool playing = readPlaying(binding->player, binding->request);
        if (positionRead && identity &&
            !owner->stopping_.load(std::memory_order_seq_cst) &&
            binding->playerIdentity == owner->playerIdentity_.load(std::memory_order_seq_cst)) {
            AcquireSRWLockExclusive(&owner->sampleLock_);
            if (binding->kind == Kind::Player) {
                // Object replacement does not establish when its cached
                // CurrentPosition was measured. Only the item's own position
                // notification is an accurate position timestamp.
                owner->tracker_.query(identity, position, qpc, playing);
            } else {
                owner->tracker_.event(identity, position, qpc, playing);
            }
            ReleaseSRWLockExclusive(&owner->sampleLock_);
        }
        releaseInspectable(item);
        releaseInspectable(itemObject);
        releaseInspectable(player);
    }

    Binding* makeBinding(void* observable, std::uintptr_t identity, Kind kind,
                         void* player, std::uintptr_t playerIdentity,
                         const bridge::SnapshotRequest& request) noexcept {
        auto* binding = new (std::nothrow) Binding;
        if (!binding) return nullptr;
        binding->owner = this;
        binding->kind = kind;
        binding->player = player;
        binding->playerIdentity = playerIdentity;
        binding->objectIdentity = identity;
        binding->request = request;
        vtable<InspectableVTable>(player)->AddRef(player);
        if (!binding->subscription.attach(observable, binding, &capture)) {
            delete binding;
            return nullptr;
        }
        return binding;
    }

    void retire(Binding*& binding) noexcept {
        if (!binding) return;
        binding->subscription.disable();
        binding->next = retired_;
        retired_ = binding;
        binding = nullptr;
    }

    void reapRetired() noexcept {
        Binding** cursor = &retired_;
        while (*cursor) {
            Binding* binding = *cursor;
            if (binding->subscription.close(0)) {
                *cursor = binding->next;
                delete binding;
            } else {
                cursor = &binding->next;
            }
        }
    }

    void ensureBindings(void* player, std::uintptr_t playerIdentity,
                        void* itemObject, std::uintptr_t itemIdentity,
                        const bridge::SnapshotRequest& request, std::int64_t qpc) noexcept {
        if (!playerIdentity || !itemIdentity) return;
        AcquireSRWLockExclusive(&sampleLock_);
        const bool currentIdentity = tracker_.track(itemIdentity, qpc);
        ReleaseSRWLockExclusive(&sampleLock_);
        if (!currentIdentity) return;
        const auto oldPlayerIdentity = playerIdentity_.load(std::memory_order_seq_cst);
        if (oldPlayerIdentity != playerIdentity) {
            retire(playerBinding_);
            retire(itemBinding_);
            playerIdentity_.store(playerIdentity, std::memory_order_seq_cst);
            failedPlayerIdentity_ = 0;
            failedItemIdentity_ = 0;
        }
        if (itemBinding_ && itemBinding_->objectIdentity != itemIdentity) retire(itemBinding_);
        if (!playerBinding_ && failedPlayerIdentity_ != playerIdentity) {
            playerBinding_ = makeBinding(player, playerIdentity, Kind::Player, player, playerIdentity, request);
            if (!playerBinding_) failedPlayerIdentity_ = playerIdentity;
        }
        if (!itemBinding_ && failedItemIdentity_ != itemIdentity) {
            itemBinding_ = makeBinding(itemObject, itemIdentity, Kind::Item, player, playerIdentity, request);
            if (!itemBinding_) failedItemIdentity_ = itemIdentity;
        }
        reapRetired();
    }

    SRWLOCK sampleLock_ = SRWLOCK_INIT;
    smtc::applemusic::AppleMusicPositionTracker tracker_;
    std::atomic<std::uintptr_t> playerIdentity_{0};
    std::atomic<bool> stopping_{false};
    std::uintptr_t failedPlayerIdentity_ = 0;
    std::uintptr_t failedItemIdentity_ = 0;
    Binding* playerBinding_ = nullptr;
    Binding* itemBinding_ = nullptr;
    Binding* retired_ = nullptr;
};

PositionEventObserver gPositionEvents;

bool validRequest(const bridge::SnapshotRequest& request) {
    return request.magic == bridge::kMagic &&
           request.version == bridge::kVersion &&
           (request.command == bridge::kCommandSnapshot || request.command == bridge::kCommandShutdown);
}

bridge::SnapshotResponse readSnapshot(const bridge::SnapshotRequest& request) {
    bridge::SnapshotResponse response;
    response.magic = bridge::kMagic;
    response.version = bridge::kVersion;
    captureQpc(response);

    HSTRING className = nullptr;
    HRESULT hr = WindowsCreateString(L"AMP.Services.Player", 19, &className);
    if (FAILED(hr)) {
        setError(response, bridge::Status::ActivationFailed, hr, L"创建 AMP.Services.Player HSTRING 失败");
        return response;
    }

    void* factoryRaw = nullptr;
    void* playerStaticsRaw = nullptr;

    HMODULE ampServices = GetModuleHandleW(L"AMP.Services.dll");
    if (!ampServices) {
        WindowsDeleteString(className);
        setError(response, bridge::Status::AmpServicesMissing, HRESULT_FROM_WIN32(GetLastError()), L"AppleMusic.exe 尚未加载 AMP.Services.dll");
        return response;
    }

    auto getActivationFactory = reinterpret_cast<DllGetActivationFactoryFn>(GetProcAddress(ampServices, "DllGetActivationFactory"));
    if (!getActivationFactory) {
        WindowsDeleteString(className);
        setError(response, bridge::Status::ActivationFailed, HRESULT_FROM_WIN32(GetLastError()), L"AMP.Services.dll 缺少 DllGetActivationFactory");
        return response;
    }

    hr = getActivationFactory(className, &factoryRaw);
    WindowsDeleteString(className);
    if (FAILED(hr) || !factoryRaw) {
        setError(response, bridge::Status::ActivationFailed, hr, L"获取 AMP.Services.Player activation factory 失败");
        return response;
    }

    const GUID& playerStaticsGuid = hasGuid(request.playerStaticsIid) ? request.playerStaticsIid : kIPlayerStaticsGuid;
    hr = queryInterface(factoryRaw, playerStaticsGuid, &playerStaticsRaw);
    if (FAILED(hr) || !playerStaticsRaw) {
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::InterfaceFailed, hr, L"获取 Apple Music IPlayerStatics 失败");
        return response;
    }

    void* playerRaw = nullptr;
    hr = vtable<IPlayerStaticsVTable>(playerStaticsRaw)->get_Current(playerStaticsRaw, &playerRaw);
    if (FAILED(hr) || !playerRaw) {
        releaseInspectable(playerStaticsRaw);
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::PlayerMissing, hr, L"Apple Music 内部 Player.Current 为空");
        return response;
    }

    void* playerInterfaceRaw = nullptr;
    hr = queryPlayerInterface(playerRaw, request, &playerInterfaceRaw);
    if (FAILED(hr) || !playerInterfaceRaw) {
        releaseInspectable(playerRaw);
        releaseInspectable(playerStaticsRaw);
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::InterfaceFailed, hr, L"获取 Apple Music IPlayer 失败");
        return response;
    }

    void* nowPlayingObjectRaw = nullptr;
    hr = vtable<IPlayerVTable>(playerInterfaceRaw)->get_NowPlayingItem(playerInterfaceRaw, &nowPlayingObjectRaw);
    const auto itemObservedQpc = queryQpc();
    if (FAILED(hr) || !nowPlayingObjectRaw) {
        releaseInspectable(playerInterfaceRaw);
        releaseInspectable(playerRaw);
        releaseInspectable(playerStaticsRaw);
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::NowPlayingMissing, hr, L"Apple Music 当前没有 NowPlayingItem");
        return response;
    }

    void* nowPlayingRaw = nullptr;
    hr = queryNowPlayingInterface(nowPlayingObjectRaw, request, &nowPlayingRaw);
    if (FAILED(hr) || !nowPlayingRaw) {
        releaseInspectable(nowPlayingObjectRaw);
        releaseInspectable(playerInterfaceRaw);
        releaseInspectable(playerRaw);
        releaseInspectable(playerStaticsRaw);
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::InterfaceFailed, hr, L"获取 Apple Music INowPlayingItem 失败");
        return response;
    }

    auto* now = vtable<INowPlayingItemVTable>(nowPlayingRaw);
    copyWide(response.title, getHString(nowPlayingRaw, now->get_Name));
    copyWide(response.subtitle, getHString(nowPlayingRaw, now->get_Subtitle));
    copyWide(response.artist, getHString(nowPlayingRaw, now->get_Artist));
    copyWide(response.album, getHString(nowPlayingRaw, now->get_Album));
    double playQueueId = 0;
    if (nowPlayingRaw && SUCCEEDED(now->get_PlayQueueItemID(nowPlayingRaw, &playQueueId)) && playQueueId > 0) {
        copyWide(response.playQueueItemId, std::to_wstring(static_cast<std::int64_t>(playQueueId)));
    }
    // The concrete NowPlayingItem exposes IContentDescriptor; INowPlayingItem itself does not.
    struct IContentDescriptorVTable : InspectableVTable {
        // Derived from AMP.Services.CommonModels.IContentDescriptor metadata.
        HRESULT (__stdcall* get_HasCloudCatalogIdentifiers)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_HasCloudIdentifiers)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_HasCloudLibraryIdentifiers)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_HasLocalLibraryIdentifiers)(void*, bool*) noexcept;
        HRESULT (__stdcall* HasMatchingIdentifiers)(void*, void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsAlbum)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsArtist)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsComposer)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsGenre)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsPlaylist)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsRadioStation)(void*, bool*) noexcept;
        HRESULT (__stdcall* MakeIdentifier)(void*, HSTRING*) noexcept;
        HRESULT (__stdcall* get_IsContainer)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsVideo)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_IsConcert)(void*, bool*) noexcept;
        HRESULT (__stdcall* get_AdamId)(void*, HSTRING*) noexcept;
        HRESULT (__stdcall* get_CanonicalId)(void*, HSTRING*) noexcept;
    };
    void* descriptorRaw = nullptr;
    // First obtain IContentDescriptorDriven from the concrete NowPlayingItem;
    // its GetContentDescriptor returns the IContentDescriptor object.
    const auto runNowPlayingDescriptor = [&] {
        if (FAILED(queryInterface(nowPlayingObjectRaw, request.contentDescriptorDrivenIid, &descriptorRaw)) || !descriptorRaw) {
            return;
        }
        struct ContentDescriptorDrivenVTable : InspectableVTable {
            HRESULT (__stdcall* get_ContentDescriptor)(void*, void**) noexcept;
            HRESULT (__stdcall* get_ContainerContentDescriptor)(void*, void**) noexcept;
        };
        void* contentDescriptorObject = nullptr;
        if (FAILED(vtable<ContentDescriptorDrivenVTable>(descriptorRaw)->get_ContentDescriptor(descriptorRaw, &contentDescriptorObject)) || !contentDescriptorObject) {
            releaseInspectable(descriptorRaw);
            return;
        }
        releaseInspectable(descriptorRaw);
        descriptorRaw = nullptr;
        if (FAILED(queryInterface(contentDescriptorObject, request.contentDescriptorIid, &descriptorRaw)) || !descriptorRaw) {
            releaseInspectable(contentDescriptorObject);
            return;
        }
        releaseInspectable(contentDescriptorObject);
        auto* descriptor = vtable<IContentDescriptorVTable>(descriptorRaw);
        copyWide(response.adamId, getHString(descriptorRaw, descriptor->get_AdamId));
        copyWide(response.canonicalId, getHString(descriptorRaw, descriptor->get_CanonicalId));
        releaseInspectable(descriptorRaw);
    };
    // 诊断只读当前对象的 IID 和运行时类型，便于确认是否存在可读取 ISRC 的扩展接口。
    runNowPlayingDescriptor();
    bool positionRead = false;
    response.position100ns = getInt64(nowPlayingRaw, now->get_CurrentPosition, &positionRead);
    if (positionRead) {
        response.queryQpc = queryQpc();
    }
    const auto positionQueryQpc = response.queryQpc;
    response.duration100ns = getInt64(nowPlayingRaw, now->get_Duration);

    void* playerStateObjectRaw = nullptr;
    void* playerStateRaw = nullptr;
    hr = vtable<IPlayerVTable>(playerInterfaceRaw)->get_PlayerState(playerInterfaceRaw, &playerStateObjectRaw);
    if (SUCCEEDED(hr) && playerStateObjectRaw) {
        const GUID& playerStateGuid = hasGuid(request.playerStateIid) ? request.playerStateIid : kIPlayerStateGuid;
        if (SUCCEEDED(queryInterface(playerStateObjectRaw, playerStateGuid, &playerStateRaw)) && playerStateRaw) {
            auto* state = vtable<IPlayerStateVTable>(playerStateRaw);
            response.playing = getBool(playerStateRaw, state->get_IsPlaying) ? 1 : 0;
            response.paused = getBool(playerStateRaw, state->get_IsPaused) ? 1 : 0;
        }
    }

    if (positionRead) {
        const auto sample = gPositionEvents.snapshot(playerInterfaceRaw, nowPlayingObjectRaw, request,
            response.position100ns, positionQueryQpc, response.playing != 0, itemObservedQpc);
        response.position100ns = sample.position100ns;
        response.queryQpc = sample.queryQpc;
        response.hresult = sample.fromEvent ? S_OK : S_FALSE;
        copyWide(response.contentProperties, bridge::kEventPositionCapability);
        response.status = static_cast<std::uint32_t>(bridge::Status::Ok);
    } else {
        setError(response, bridge::Status::InternalError, E_FAIL, L"读取 Apple Music 播放位置失败");
    }

    releaseInspectable(playerStateRaw);
    releaseInspectable(playerStateObjectRaw);
    releaseInspectable(nowPlayingRaw);
    releaseInspectable(nowPlayingObjectRaw);
    releaseInspectable(playerInterfaceRaw);
    releaseInspectable(playerRaw);
    releaseInspectable(playerStaticsRaw);
    releaseInspectable(factoryRaw);
    return response;
}

std::wstring pipeName() {
    return L"\\\\.\\pipe\\LOCAL\\" + std::wstring(bridge::kPipeNamePrefix) + std::to_wstring(GetCurrentProcessId());
}

PSECURITY_DESCRIPTOR createPipeSecurityDescriptor() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;WD)",
        SDDL_REVISION_1,
        &descriptor,
        nullptr);
    return descriptor;
}

bool servePipeClient(HANDLE pipe) {
    bridge::SnapshotRequest request;
    DWORD read = 0;
    bridge::SnapshotResponse response;
    response.magic = bridge::kMagic;
    response.version = bridge::kVersion;
    captureQpc(response);
    bool shutdownRequested = false;

    if (!ReadFile(pipe, &request, sizeof(request), &read, nullptr) || read != sizeof(request) || !validRequest(request)) {
        setError(response, bridge::Status::InvalidRequest, E_INVALIDARG, L"Apple Music bridge 收到无效请求");
    } else if (request.command == bridge::kCommandShutdown) {
        shutdownRequested = gPositionEvents.close(3000);
        if (shutdownRequested) {
            response.status = static_cast<std::uint32_t>(bridge::Status::Ok);
        } else {
            setError(response, bridge::Status::InternalError, HRESULT_FROM_WIN32(ERROR_BUSY),
                L"Apple Music 位置回调尚未释放，bridge 保持加载");
        }
    } else {
        response = readSnapshot(request);
    }

    DWORD written = 0;
    WriteFile(pipe, &response, sizeof(response), &written, nullptr);
    FlushFileBuffers(pipe);
    return shutdownRequested;
}

DWORD WINAPI pipeThread(void*) {
    const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    const bool roInitialized = SUCCEEDED(roHr);

    const auto name = pipeName();
    PSECURITY_DESCRIPTOR descriptor = createPipeSecurityDescriptor();
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;

    bool shutdownRequested = false;
    while (!shutdownRequested) {
        HANDLE pipe = CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            static_cast<DWORD>(sizeof(bridge::SnapshotResponse)),
            static_cast<DWORD>(sizeof(bridge::SnapshotRequest)),
            2000,
            descriptor ? &security : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            shutdownRequested = servePipeClient(pipe);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    if (descriptor) LocalFree(descriptor);
    if (roInitialized) RoUninitialize();
    if (gSelfModule) {
        FreeLibraryAndExitThread(gSelfModule, 0);
    }
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gSelfModule = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, pipeThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
