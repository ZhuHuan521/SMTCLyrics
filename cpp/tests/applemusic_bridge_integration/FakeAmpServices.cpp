#include "applemusic/AppleMusicPositionTracker.h"

#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

// This test DLL is loaded normally by the integration host. It implements only
// the read-only AMP ABI used by the bridge and never accesses another process.
namespace {
namespace events = smtc::applemusic::property_events;

constexpr GUID kInspectable{0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
constexpr GUID kActivationFactory{0x00000035, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kPlayerStatics{0x34D100F9, 0x0F0A, 0x5980, {0x8F, 0x04, 0x7A, 0xF5, 0x7F, 0xDB, 0x29, 0xB0}};
constexpr GUID kPlayerState{0xBEFB99EB, 0x336F, 0x5199, {0x9F, 0xB2, 0x63, 0x63, 0x5B, 0xC4, 0x73, 0x19}};
constexpr GUID kCurrentPlayer{0xF807B158, 0xB2E7, 0x5CF7, {0xA5, 0x12, 0x8B, 0x0B, 0xE7, 0x67, 0xDF, 0x1B}};
constexpr GUID kLegacyPlayer{0x958DA6F6, 0x2E93, 0x5BA7, {0xB6, 0x04, 0x28, 0xDD, 0x57, 0xFE, 0xB8, 0xE0}};
constexpr GUID kCurrentItem{0x2DC5F65C, 0x28F6, 0x58AA, {0x87, 0xCF, 0xE4, 0xA7, 0xDF, 0x9B, 0x9A, 0x4B}};
constexpr GUID kLegacyItem{0xE376ACB7, 0x0569, 0x56C5, {0x84, 0x43, 0xA2, 0x88, 0xAC, 0xD3, 0x62, 0xEA}};

using StringGetter = HRESULT (__stdcall*)(void*, HSTRING*) noexcept;
using IntGetter = HRESULT (__stdcall*)(void*, std::int32_t*) noexcept;
using LongGetter = HRESULT (__stdcall*)(void*, std::int64_t*) noexcept;
using BoolGetter = HRESULT (__stdcall*)(void*, bool*) noexcept;
using DoubleGetter = HRESULT (__stdcall*)(void*, double*) noexcept;
using ObjectGetter = HRESULT (__stdcall*)(void*, void**) noexcept;

struct InspectableVTable {
    HRESULT (__stdcall* QueryInterface)(void*, REFIID, void**) noexcept;
    ULONG (__stdcall* AddRef)(void*) noexcept;
    ULONG (__stdcall* Release)(void*) noexcept;
    HRESULT (__stdcall* GetIids)(void*, ULONG*, IID**) noexcept;
    HRESULT (__stdcall* GetRuntimeClassName)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* GetTrustLevel)(void*, TrustLevel*) noexcept;
};
struct FactoryVTable : InspectableVTable { ObjectGetter ActivateInstance; };
struct StaticsVTable : InspectableVTable { ObjectGetter Current; };
struct PlayerVTable : InspectableVTable { ObjectGetter NowPlayingItem; ObjectGetter PlayerState; };
struct ItemVTable : InspectableVTable {
    StringGetter Subtitle;
    StringGetter Artist;
    StringGetter Album;
    IntGetter TrackNumber;
    IntGetter TrackCount;
    ObjectGetter Artwork;
    IntGetter PlaybackMediaType;
    IntGetter ExplicitState;
    StringGetter VideoRatingString;
    StringGetter RottonTomatoRating;
    StringGetter RottonTomatoScore;
    StringGetter CommonSenseRating;
    StringGetter ISOReleaseDate;
    StringGetter Name;
    StringGetter TVShowName;
    StringGetter Genre;
    StringGetter Description;
    StringGetter Accessibility;
    BoolGetter IsPlaceholder;
    BoolGetter IsRadio;
    BoolGetter IsRadioLiveStream;
    ObjectGetter Reactions;
    StringGetter ShowCanonicalId;
    IntGetter ScrubberMode;
    LongGetter CurrentPosition;
    LongGetter Duration;
    BoolGetter LoadTimeIsValid;
    LongGetter LoadTime;
    LongGetter MinLoadTime;
    BoolGetter CanBePositioned;
    IntGetter LiveMode;
    DoubleGetter CurrentDate;
    StringGetter AudioFormat;
    StringGetter AudioFormatSampleRate;
    StringGetter AudioFormatBitDepth;
    StringGetter HLSMetadata;
    BoolGetter HasVideo;
    ObjectGetter VideoPresentationSize;
    ObjectGetter VideoColorCapability;
    DoubleGetter PlayQueueItemID;
    ObjectGetter LyricsInfo;
    ObjectGetter AudibleAlternateTracks;
    ObjectGetter SelectedAudibleTrack;
    ObjectGetter LegibleAlternateTracks;
    ObjectGetter SelectedLegibleTrack;
    ObjectGetter VisualAlternateTracks;
};
struct StateVTable : InspectableVTable {
    BoolGetter CanPlay;
    BoolGetter CanPause;
    BoolGetter CanStop;
    BoolGetter CanRewind;
    BoolGetter CanFF;
    BoolGetter CanGoBack;
    BoolGetter CanSkipPrevious;
    BoolGetter CanSkipNext;
    BoolGetter PreferMusicControls;
    BoolGetter CanShuffle;
    IntGetter ShuffleMode;
    BoolGetter CanRepeat;
    IntGetter RepeatMode;
    DoubleGetter TargetPlayRate;
    BoolGetter CanSetVolume;
    DoubleGetter Volume;
    BoolGetter CanMute;
    BoolGetter IsMuted;
    BoolGetter IsPaused;
    BoolGetter IsPlaying;
};
struct NotifyVTable : InspectableVTable {
    HRESULT (__stdcall* Add)(void*, void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* Remove)(void*, std::int64_t) noexcept;
};
struct ArgsVTable : InspectableVTable { StringGetter PropertyName; };
struct DelegateVTable {
    HRESULT (__stdcall* QueryInterface)(void*, REFIID, void**) noexcept;
    ULONG (__stdcall* AddRef)(void*) noexcept;
    ULONG (__stdcall* Release)(void*) noexcept;
    HRESULT (__stdcall* Invoke)(void*, void*, void*) noexcept;
};

template <class VTable> VTable* table(void* object) noexcept {
    return *reinterpret_cast<VTable**>(object);
}
void addUnknown(void* object) noexcept {
    if (object) table<InspectableVTable>(object)->AddRef(object);
}
void releaseUnknown(void* object) noexcept {
    if (object) table<InspectableVTable>(object)->Release(object);
}

struct FakeObject;
struct Interface { const void* vtable = nullptr; FakeObject* owner = nullptr; };
enum class Kind { Factory, Player, Item, State, Args };
struct Handler { std::int64_t token = 0; void* delegate = nullptr; };

std::atomic<bool> gPlaying{true};
std::atomic<bool> gRetainRemoved{false};
std::mutex gRetainedLock;
std::vector<void*> gRetained;

struct FakeObject {
    explicit FakeObject(Kind value) noexcept : kind(value) {}
    ~FakeObject() {
        // Normal teardown occurs after the host has closed and unloaded bridge.
        for (const auto& handler : handlers) releaseUnknown(handler.delegate);
    }
    std::atomic<ULONG> references{1};
    Kind kind;
    Interface primary;
    Interface secondary;
    std::atomic<std::int64_t> position100ns{420'000'000};
    std::wstring property;
    std::mutex handlerLock;
    std::vector<Handler> handlers;
    std::int64_t nextToken = 1;
};

FakeObject* owner(void* object) noexcept { return static_cast<Interface*>(object)->owner; }
HRESULT __stdcall queryInterface(void* object, REFIID iid, void** result) noexcept;
ULONG __stdcall addRef(void* object) noexcept;
ULONG __stdcall release(void* object) noexcept;
HRESULT __stdcall getIids(void*, ULONG*, IID**) noexcept;
HRESULT __stdcall runtimeClass(void*, HSTRING*) noexcept;
HRESULT __stdcall trustLevel(void*, TrustLevel*) noexcept;
HRESULT __stdcall currentPlayer(void*, void**) noexcept;
HRESULT __stdcall currentItem(void*, void**) noexcept;
HRESULT __stdcall currentState(void*, void**) noexcept;
HRESULT __stdcall stringEmpty(void*, HSTRING*) noexcept;
HRESULT __stdcall artist(void*, HSTRING*) noexcept;
HRESULT __stdcall album(void*, HSTRING*) noexcept;
HRESULT __stdcall subtitle(void*, HSTRING*) noexcept;
HRESULT __stdcall songName(void*, HSTRING*) noexcept;
HRESULT __stdcall propertyName(void*, HSTRING*) noexcept;
HRESULT __stdcall intZero(void*, std::int32_t*) noexcept;
HRESULT __stdcall longZero(void*, std::int64_t*) noexcept;
HRESULT __stdcall objectNull(void*, void**) noexcept;
HRESULT __stdcall boolTrue(void*, bool*) noexcept;
HRESULT __stdcall boolFalse(void*, bool*) noexcept;
HRESULT __stdcall doubleZero(void*, double*) noexcept;
HRESULT __stdcall doubleOne(void*, double*) noexcept;
HRESULT __stdcall position(void*, std::int64_t*) noexcept;
HRESULT __stdcall duration(void*, std::int64_t*) noexcept;
HRESULT __stdcall isPlaying(void*, bool*) noexcept;
HRESULT __stdcall isPaused(void*, bool*) noexcept;
HRESULT __stdcall addHandler(void*, void*, std::int64_t*) noexcept;
HRESULT __stdcall removeHandler(void*, std::int64_t) noexcept;

const InspectableVTable kInspectableTable{queryInterface, addRef, release, getIids, runtimeClass, trustLevel};
const FactoryVTable kFactoryTable{kInspectableTable, currentPlayer};
const StaticsVTable kStaticsTable{kInspectableTable, currentPlayer};
const PlayerVTable kPlayerTable{kInspectableTable, currentItem, currentState};
const ItemVTable kItemTable{
    kInspectableTable,
    subtitle, artist, album, intZero, intZero, objectNull, intZero, intZero,
    stringEmpty, stringEmpty, stringEmpty, stringEmpty, stringEmpty,
    songName, stringEmpty, stringEmpty, stringEmpty, stringEmpty,
    boolFalse, boolFalse, boolFalse, objectNull, stringEmpty, intZero,
    position, duration, boolFalse, longZero, longZero, boolTrue,
    intZero, doubleZero, stringEmpty, stringEmpty, stringEmpty, stringEmpty,
    boolFalse, objectNull, objectNull, doubleOne,
    objectNull, objectNull, objectNull, objectNull, objectNull, objectNull};
const StateVTable kStateTable{
    kInspectableTable,
    boolTrue, boolTrue, boolTrue, boolFalse, boolFalse, boolFalse,
    boolFalse, boolFalse, boolFalse, boolFalse, intZero, boolFalse, intZero,
    doubleOne, boolFalse, doubleOne, boolFalse, boolFalse, isPaused, isPlaying};
const NotifyVTable kNotifyTable{kInspectableTable, addHandler, removeHandler};
const ArgsVTable kArgsTable{kInspectableTable, propertyName};

FakeObject* makeObject(Kind kind) {
    auto* object = new FakeObject(kind);
    object->primary.owner = object;
    object->secondary.owner = object;
    switch (kind) {
    case Kind::Factory: object->primary.vtable = &kFactoryTable; object->secondary.vtable = &kStaticsTable; break;
    case Kind::Player: object->primary.vtable = &kPlayerTable; object->secondary.vtable = &kNotifyTable; break;
    case Kind::Item: object->primary.vtable = &kItemTable; object->secondary.vtable = &kNotifyTable; break;
    case Kind::State: object->primary.vtable = &kStateTable; break;
    case Kind::Args: object->primary.vtable = &kArgsTable; break;
    }
    return object;
}

struct World {
    FakeObject* factory = makeObject(Kind::Factory);
    FakeObject* player = makeObject(Kind::Player);
    FakeObject* state = makeObject(Kind::State);
    FakeObject* item = makeObject(Kind::Item);
    std::mutex itemLock;
    ~World() {
        release(&item->primary);
        release(&state->primary);
        release(&player->primary);
        release(&factory->primary);
    }
};
World& world() { static World instance; return instance; }

ULONG __stdcall addRef(void* object) noexcept {
    return owner(object)->references.fetch_add(1, std::memory_order_relaxed) + 1;
}
ULONG __stdcall release(void* object) noexcept {
    auto* value = owner(object);
    const ULONG remaining = value->references.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!remaining) delete value;
    return remaining;
}
HRESULT __stdcall queryInterface(void* object, REFIID iid, void** result) noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    auto* value = owner(object);
    if (IsEqualGUID(iid, events::kUnknownGuid) || IsEqualGUID(iid, kInspectable) || IsEqualGUID(iid, events::kAgileGuid)) {
        *result = &value->primary;
    } else {
        switch (value->kind) {
        case Kind::Factory:
            if (IsEqualGUID(iid, kActivationFactory)) *result = &value->primary;
            else if (IsEqualGUID(iid, kPlayerStatics)) *result = &value->secondary;
            break;
        case Kind::Player:
            if (IsEqualGUID(iid, kCurrentPlayer) || IsEqualGUID(iid, kLegacyPlayer)) *result = &value->primary;
            else if (IsEqualGUID(iid, events::kNotifyGuid) || IsEqualGUID(iid, events::kLegacyNotifyGuid)) *result = &value->secondary;
            break;
        case Kind::Item:
            if (IsEqualGUID(iid, kCurrentItem) || IsEqualGUID(iid, kLegacyItem)) *result = &value->primary;
            else if (IsEqualGUID(iid, events::kNotifyGuid) || IsEqualGUID(iid, events::kLegacyNotifyGuid)) *result = &value->secondary;
            break;
        case Kind::State:
            if (IsEqualGUID(iid, kPlayerState)) *result = &value->primary;
            break;
        case Kind::Args:
            if (IsEqualGUID(iid, events::kArgsGuid) || IsEqualGUID(iid, events::kLegacyArgsGuid)) *result = &value->primary;
            break;
        }
    }
    if (!*result) return E_NOINTERFACE;
    addRef(*result);
    return S_OK;
}
HRESULT __stdcall getIids(void*, ULONG* count, IID** ids) noexcept {
    if (!count || !ids) return E_POINTER;
    *count = 0;
    *ids = nullptr;
    return S_OK;
}
HRESULT makeString(const wchar_t* text, HSTRING* result) noexcept {
    if (!result) return E_POINTER;
    return WindowsCreateString(text, static_cast<UINT32>(std::wcslen(text)), result);
}
HRESULT __stdcall runtimeClass(void* object, HSTRING* result) noexcept {
    switch (owner(object)->kind) {
    case Kind::Factory: return makeString(L"AMP.Services.PlayerFactory", result);
    case Kind::Player: return makeString(L"AMP.Services.Player", result);
    case Kind::Item: return makeString(L"AMP.Services.CommonModels.NowPlayingItem", result);
    case Kind::State: return makeString(L"AMP.Services.CommonModels.PlayerState", result);
    case Kind::Args: return makeString(L"Microsoft.UI.Xaml.Data.PropertyChangedEventArgs", result);
    }
    return E_UNEXPECTED;
}
HRESULT __stdcall trustLevel(void*, TrustLevel* result) noexcept {
    if (!result) return E_POINTER;
    *result = BaseTrust;
    return S_OK;
}
HRESULT __stdcall currentPlayer(void*, void** result) noexcept {
    if (!result) return E_POINTER;
    *result = &world().player->primary;
    addRef(*result);
    return S_OK;
}
HRESULT __stdcall currentItem(void*, void** result) noexcept {
    if (!result) return E_POINTER;
    std::lock_guard lock(world().itemLock);
    *result = &world().item->primary;
    addRef(*result);
    return S_OK;
}
HRESULT __stdcall currentState(void*, void** result) noexcept {
    if (!result) return E_POINTER;
    *result = &world().state->primary;
    addRef(*result);
    return S_OK;
}
HRESULT __stdcall stringEmpty(void*, HSTRING* out) noexcept { return makeString(L"", out); }
HRESULT __stdcall artist(void*, HSTRING* out) noexcept { return makeString(L"Integration artist", out); }
HRESULT __stdcall album(void*, HSTRING* out) noexcept { return makeString(L"Integration album", out); }
HRESULT __stdcall subtitle(void*, HSTRING* out) noexcept { return makeString(L"Integration artist \u2014 Integration album", out); }
HRESULT __stdcall songName(void*, HSTRING* out) noexcept { return makeString(L"Bridge integration song", out); }
HRESULT __stdcall propertyName(void* object, HSTRING* out) noexcept { return makeString(owner(object)->property.c_str(), out); }

template <class T> HRESULT assign(T* result, T value) noexcept {
    if (!result) return E_POINTER;
    *result = value;
    return S_OK;
}
HRESULT __stdcall intZero(void*, std::int32_t* out) noexcept { return assign(out, std::int32_t{0}); }
HRESULT __stdcall longZero(void*, std::int64_t* out) noexcept { return assign(out, std::int64_t{0}); }
HRESULT __stdcall objectNull(void*, void** out) noexcept { return assign(out, static_cast<void*>(nullptr)); }
HRESULT __stdcall boolTrue(void*, bool* out) noexcept { return assign(out, true); }
HRESULT __stdcall boolFalse(void*, bool* out) noexcept { return assign(out, false); }
HRESULT __stdcall doubleZero(void*, double* out) noexcept { return assign(out, 0.0); }
HRESULT __stdcall doubleOne(void*, double* out) noexcept { return assign(out, 1.0); }
HRESULT __stdcall position(void* object, std::int64_t* out) noexcept { return assign(out, owner(object)->position100ns.load()); }
HRESULT __stdcall duration(void*, std::int64_t* out) noexcept { return assign(out, std::int64_t{3'000'000'000}); }
HRESULT __stdcall isPlaying(void*, bool* out) noexcept { return assign(out, gPlaying.load()); }
HRESULT __stdcall isPaused(void*, bool* out) noexcept { return assign(out, !gPlaying.load()); }

HRESULT __stdcall addHandler(void* object, void* handler, std::int64_t* token) noexcept {
    if (!handler || !token) return E_POINTER;
    void* typed = nullptr;
    HRESULT hr = table<DelegateVTable>(handler)->QueryInterface(handler, events::kHandlerGuid, &typed);
    if (FAILED(hr)) hr = table<DelegateVTable>(handler)->QueryInterface(handler, events::kLegacyHandlerGuid, &typed);
    if (FAILED(hr) || !typed) return E_NOINTERFACE;
    try {
        auto* value = owner(object);
        std::lock_guard lock(value->handlerLock);
        *token = value->nextToken++;
        value->handlers.push_back({*token, typed});
        return S_OK;
    } catch (...) {
        releaseUnknown(typed);
        return E_OUTOFMEMORY;
    }
}
HRESULT __stdcall removeHandler(void* object, std::int64_t token) noexcept {
    void* removed = nullptr;
    {
        auto* value = owner(object);
        std::lock_guard lock(value->handlerLock);
        const auto entry = std::find_if(value->handlers.begin(), value->handlers.end(),
            [token](const Handler& handler) { return handler.token == token; });
        if (entry == value->handlers.end()) return S_OK;
        removed = entry->delegate;
        value->handlers.erase(entry);
    }
    if (gRetainRemoved.load()) {
        try {
            std::lock_guard lock(gRetainedLock);
            gRetained.push_back(removed);
            return S_OK;
        } catch (...) {
            releaseUnknown(removed);
            return E_OUTOFMEMORY;
        }
    }
    releaseUnknown(removed);
    return S_OK;
}

void publish(FakeObject* source, const wchar_t* property) {
    std::vector<void*> callbacks;
    {
        std::lock_guard lock(source->handlerLock);
        callbacks.reserve(source->handlers.size());
        for (const auto& handler : source->handlers) {
            addUnknown(handler.delegate);
            callbacks.push_back(handler.delegate);
        }
    }
    auto* args = makeObject(Kind::Args);
    args->property = property;
    // Invoke occupies slot 3 immediately after IUnknown's three methods.
    // Publishing without either source lock permits a callback to read state.
    for (void* callback : callbacks) {
        table<DelegateVTable>(callback)->Invoke(callback, &source->primary, &args->primary);
        releaseUnknown(callback);
    }
    release(&args->primary);
}

FakeObject* acquireCurrentItem() {
    std::lock_guard lock(world().itemLock);
    auto* item = world().item;
    addRef(&item->primary);
    return item;
}
void setCachedPosition(std::int64_t value, bool playing) {
    auto* item = acquireCurrentItem();
    item->position100ns.store(value);
    gPlaying.store(playing);
    release(&item->primary);
}
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllGetActivationFactory(HSTRING className, void** result) noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    UINT32 length = 0;
    const wchar_t* text = WindowsGetStringRawBuffer(className, &length);
    constexpr wchar_t expected[] = L"AMP.Services.Player";
    if (!text || length != 19 || std::wmemcmp(text, expected, 19) != 0) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        *result = &world().factory->primary;
        addRef(*result);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

extern "C" __declspec(dllexport) void __stdcall SetPlaybackPosition(std::int64_t value, bool playing) {
    auto* item = acquireCurrentItem();
    item->position100ns.store(value);
    gPlaying.store(playing);
    publish(item, L"CurrentPosition");
    release(&item->primary);
}
extern "C" __declspec(dllexport) void __stdcall SetCachedPlaybackPosition(std::int64_t value, bool playing) {
    setCachedPosition(value, playing);
}
extern "C" __declspec(dllexport) void __stdcall SetPlaying(bool playing) { gPlaying.store(playing); }
extern "C" __declspec(dllexport) void __stdcall FirePositionChanged() {
    auto* item = acquireCurrentItem();
    publish(item, L"CurrentPosition");
    release(&item->primary);
}
extern "C" __declspec(dllexport) void __stdcall ChangeNowPlayingItem(std::int64_t value, bool playing) {
    auto* item = makeObject(Kind::Item);
    item->position100ns.store(value);
    FakeObject* previous = nullptr;
    {
        std::lock_guard lock(world().itemLock);
        previous = std::exchange(world().item, item);
        gPlaying.store(playing);
    }
    release(&previous->primary);
    publish(world().player, L"NowPlayingItem");
}
extern "C" __declspec(dllexport) void __stdcall SetRetainCallbacksOnRemove(bool retain) { gRetainRemoved.store(retain); }
extern "C" __declspec(dllexport) void __stdcall ReleaseRetainedCallbacks() {
    std::vector<void*> callbacks;
    {
        std::lock_guard lock(gRetainedLock);
        callbacks.swap(gRetained);
    }
    for (void* callback : callbacks) releaseUnknown(callback);
}
