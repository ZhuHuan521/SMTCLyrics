#include "applemusic/AppleMusicBridgeProtocol.h"

#include <windows.h>
#include <aclapi.h>
#include <roapi.h>
#include <sddl.h>
#include <winstring.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace bridge = smtc::applemusic::bridge;
HMODULE gSelfModule = nullptr;

constexpr GUID kIActivationFactoryGuid{0x00000035, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIPlayerStaticsGuid{0x34D100F9, 0x0F0A, 0x5980, {0x8F, 0x04, 0x7A, 0xF5, 0x7F, 0xDB, 0x29, 0xB0}};
constexpr GUID kIPlayerGuid{0x958DA6F6, 0x2E93, 0x5BA7, {0xB6, 0x04, 0x28, 0xDD, 0x57, 0xFE, 0xB8, 0xE0}};
constexpr GUID kINowPlayingItemGuid{0xE376ACB7, 0x0569, 0x56C5, {0x84, 0x43, 0xA2, 0x88, 0xAC, 0xD3, 0x62, 0xEA}};
constexpr GUID kIPlayerStateGuid{0xBEFB99EB, 0x336F, 0x5199, {0x9F, 0xB2, 0x63, 0x63, 0x5B, 0xC4, 0x73, 0x19}};

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

std::int64_t getInt64(void* object, HRESULT (__stdcall* getter)(void*, std::int64_t*) noexcept) {
    std::int64_t value = 0;
    if (!object || !getter || FAILED(getter(object, &value))) return 0;
    return value;
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

void captureQpc(bridge::SnapshotResponse& response) {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    response.queryQpc = counter.QuadPart;
    response.qpcFrequency = frequency.QuadPart;
}

bool validRequest(const bridge::SnapshotRequest& request) {
    return request.magic == bridge::kMagic &&
           request.version == bridge::kVersion &&
           (request.command == bridge::kCommandSnapshot || request.command == bridge::kCommandShutdown);
}

bridge::SnapshotResponse readSnapshot() {
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

    hr = queryInterface(factoryRaw, kIPlayerStaticsGuid, &playerStaticsRaw);
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
    hr = queryInterface(playerRaw, kIPlayerGuid, &playerInterfaceRaw);
    if (FAILED(hr) || !playerInterfaceRaw) {
        releaseInspectable(playerRaw);
        releaseInspectable(playerStaticsRaw);
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::InterfaceFailed, hr, L"获取 Apple Music IPlayer 失败");
        return response;
    }

    void* nowPlayingObjectRaw = nullptr;
    hr = vtable<IPlayerVTable>(playerInterfaceRaw)->get_NowPlayingItem(playerInterfaceRaw, &nowPlayingObjectRaw);
    if (FAILED(hr) || !nowPlayingObjectRaw) {
        releaseInspectable(playerInterfaceRaw);
        releaseInspectable(playerRaw);
        releaseInspectable(playerStaticsRaw);
        releaseInspectable(factoryRaw);
        setError(response, bridge::Status::NowPlayingMissing, hr, L"Apple Music 当前没有 NowPlayingItem");
        return response;
    }

    void* nowPlayingRaw = nullptr;
    hr = queryInterface(nowPlayingObjectRaw, kINowPlayingItemGuid, &nowPlayingRaw);
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
    response.position100ns = getInt64(nowPlayingRaw, now->get_CurrentPosition);
    response.duration100ns = getInt64(nowPlayingRaw, now->get_Duration);

    void* playerStateObjectRaw = nullptr;
    void* playerStateRaw = nullptr;
    hr = vtable<IPlayerVTable>(playerInterfaceRaw)->get_PlayerState(playerInterfaceRaw, &playerStateObjectRaw);
    if (SUCCEEDED(hr) && playerStateObjectRaw) {
        if (SUCCEEDED(queryInterface(playerStateObjectRaw, kIPlayerStateGuid, &playerStateRaw)) && playerStateRaw) {
            auto* state = vtable<IPlayerStateVTable>(playerStateRaw);
            response.playing = getBool(playerStateRaw, state->get_IsPlaying) ? 1 : 0;
            response.paused = getBool(playerStateRaw, state->get_IsPaused) ? 1 : 0;
        }
    }

    response.status = static_cast<std::uint32_t>(bridge::Status::Ok);

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

    if (!ReadFile(pipe, &request, sizeof(request), &read, nullptr) || read != sizeof(request) || !validRequest(request)) {
        setError(response, bridge::Status::InvalidRequest, E_INVALIDARG, L"Apple Music bridge 收到无效请求");
    } else if (request.command == bridge::kCommandShutdown) {
        response.status = static_cast<std::uint32_t>(bridge::Status::Ok);
    } else {
        response = readSnapshot();
    }

    DWORD written = 0;
    WriteFile(pipe, &response, sizeof(response), &written, nullptr);
    FlushFileBuffers(pipe);
    return validRequest(request) && request.command == bridge::kCommandShutdown;
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
