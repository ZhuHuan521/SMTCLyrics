#include "app/Application.h"
#include "applemusic/AppleMusicInternalProvider.h"
#include "applemusic/AppleMusicBridgeProtocol.h"
#include "util/Encoding.h"
#include "util/Path.h"

#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <roapi.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winstring.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "Shcore.lib")

namespace {

constexpr wchar_t kProbeAppleMusicSwitch[] = L"--smtclyrics-probe-applemusic-internal";
constexpr wchar_t kProbeAppleMusicCoreSwitch[] = L"--smtclyrics-probe-applemusic-core";
constexpr wchar_t kProbeAppleMusicInjectSwitch[] = L"--smtclyrics-probe-applemusic-inject";
constexpr wchar_t kAppleMusicPackageFamilyName[] = L"AppleInc.AppleMusicWin_nzyj5cx40ttqa";

constexpr GUID kIUnknownGuid{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIInspectableGuid{0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
constexpr GUID kIClassFactoryGuid{0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIPSFactoryBufferGuid{0xD5F569D0, 0x593B, 0x101A, {0xB5, 0x69, 0x08, 0x00, 0x2B, 0x2D, 0xBF, 0x7A}};
constexpr GUID kIActivationFactoryGuid{0x00000035, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIAsyncInfoGuid{0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kAmpMusicLibraryClassGuid{0x68E7097C, 0xF969, 0x4006, {0xAA, 0xC3, 0x95, 0x11, 0x5F, 0x0E, 0xD1, 0xC4}};
constexpr GUID kIAmpMusicLibraryGuid{0x68E7097C, 0xF969, 0x4006, {0xAA, 0xC3, 0x95, 0x11, 0x5F, 0x0E, 0xD1, 0xC4}};
constexpr GUID kIAmpLibraryGuid{0xF707A913, 0xE0CE, 0x4FD4, {0xBC, 0xE3, 0x42, 0x5D, 0xD1, 0x53, 0x28, 0x5B}};
constexpr GUID kPlaybackEventDelegateGuid{0xF0E209BE, 0x7FB1, 0x5E1E, {0x98, 0x79, 0xB6, 0x22, 0xAE, 0x7C, 0xCA, 0x41}};

struct InspectableVTable {
    HRESULT (__stdcall* QueryInterface)(void*, REFIID, void**) noexcept;
    ULONG (__stdcall* AddRef)(void*) noexcept;
    ULONG (__stdcall* Release)(void*) noexcept;
    HRESULT (__stdcall* GetIids)(void*, ULONG*, IID**) noexcept;
    HRESULT (__stdcall* GetRuntimeClassName)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* GetTrustLevel)(void*, TrustLevel*) noexcept;
};

struct ActivationFactoryVTable : InspectableVTable {
    HRESULT (__stdcall* ActivateInstance)(void*, void**) noexcept;
};

struct IAsyncInfoVTable : InspectableVTable {
    HRESULT (__stdcall* get_Id)(void*, UINT32*) noexcept;
    HRESULT (__stdcall* get_Status)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* get_ErrorCode)(void*, HRESULT*) noexcept;
    HRESULT (__stdcall* Cancel)(void*) noexcept;
    HRESULT (__stdcall* Close)(void*) noexcept;
};

struct IAsyncOperationHStringVTable : InspectableVTable {
    HRESULT (__stdcall* put_Completed)(void*, void*) noexcept;
    HRESULT (__stdcall* get_Completed)(void*, void**) noexcept;
    HRESULT (__stdcall* GetResults)(void*, HSTRING*) noexcept;
};

struct RegisterClientResult {
    UINT32 clientID;
    UINT64 persistentMachineID;
    INT32 status;
};

struct IAmpLibraryVTable : InspectableVTable {
    HRESULT (__stdcall* get_Version)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_ServerInfo)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* get_State)(void*, std::int32_t*) noexcept;
    HRESULT (__stdcall* Setup)(void*, UINT32, UINT32*, INT32*) noexcept;
    HRESULT (__stdcall* Cleanup)(void*, UINT32, INT32*) noexcept;
    HRESULT (__stdcall* MediaAppStarting)(void*, std::int32_t, HSTRING*) noexcept;
    HRESULT (__stdcall* MediaAppQuitting)(void*, INT32*) noexcept;
    HRESULT (__stdcall* MediaAppGetLibraryInfoAsync)(void*, void**) noexcept;
    HRESULT (__stdcall* MediaAppOpenLibraryAsync)(void*, void**) noexcept;
    HRESULT (__stdcall* MediaAppSetLibraryLocationAsync)(void*, HSTRING, void**) noexcept;
    HRESULT (__stdcall* MediaAppExecFetchRequestAsync)(void*, HSTRING, void**) noexcept;
    HRESULT (__stdcall* MediaAppReleaseFetchRequestWriteStream)(void*, UINT64, UINT64, INT32*) noexcept;
    HRESULT (__stdcall* MediaAppCloseFetchRequest)(void*, UINT64, UINT64, INT32*) noexcept;
    HRESULT (__stdcall* MediaAppExecAgentCommandAsync)(void*, HSTRING, void**) noexcept;
    HRESULT (__stdcall* get_ClientID)(void*, UINT32*) noexcept;
    HRESULT (__stdcall* get_MediaDomainsOpened)(void*, bool*) noexcept;
    HRESULT (__stdcall* get_ServerProtocolVersion)(void*, HSTRING*) noexcept;
    HRESULT (__stdcall* RegisterLibraryClient)(void*, HSTRING, UINT32, RegisterClientResult*) noexcept;
    HRESULT (__stdcall* GetDomainInfoAsync)(void*, std::int32_t, UINT32, HSTRING, void**) noexcept;
    HRESULT (__stdcall* OpenDomainsAsync)(void*, std::int32_t, UINT32, HSTRING, void**) noexcept;
    HRESULT (__stdcall* CloseDomainsForClientID)(void*, UINT32, INT32*) noexcept;
    HRESULT (__stdcall* SendDBChangesToLibraryAsync)(void*, UINT32, HSTRING, void**) noexcept;
    HRESULT (__stdcall* FetchLibraryFromRevsionAsync)(void*, UINT32, std::int32_t, bool, void**) noexcept;
    HRESULT (__stdcall* SaveAgentLogToFileAsync)(void*, HSTRING, HSTRING, void**) noexcept;
    HRESULT (__stdcall* add_PlaybackEventHandler)(void*, void*, std::int64_t*) noexcept;
    HRESULT (__stdcall* remove_PlaybackEventHandler)(void*, std::int64_t) noexcept;
};

struct PlaybackDelegate;

struct PlaybackDelegateVTable : InspectableVTable {
    HRESULT (__stdcall* Invoke)(PlaybackDelegate*, UINT32, HSTRING) noexcept;
};

struct PlaybackDelegate {
    PlaybackDelegateVTable* lpVtbl;
    std::atomic<ULONG> refCount;
    std::ofstream* out;
    std::mutex* mutex;
};

using DllGetClassObjectFn = HRESULT (__stdcall*)(REFCLSID, REFIID, void**) noexcept;

template <typename VTable>
VTable* vtable(void* object) {
    return *reinterpret_cast<VTable**>(object);
}

std::string hresultUtf8(HRESULT hr);

std::wstring hstringToWide(HSTRING value) {
    if (!value) return {};
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(value, &length);
    std::wstring text(raw, raw + length);
    WindowsDeleteString(value);
    return text;
}

std::wstring hstringViewToWide(HSTRING value) {
    if (!value) return {};
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(value, &length);
    return std::wstring(raw, raw + length);
}

HRESULT queryInterface(void* object, REFIID iid, void** result) {
    if (!object || !result) return E_POINTER;
    *result = nullptr;
    return vtable<InspectableVTable>(object)->QueryInterface(object, iid, result);
}

void releaseInspectable(void* object) {
    if (object) vtable<InspectableVTable>(object)->Release(object);
}

HRESULT __stdcall playbackDelegateQueryInterface(PlaybackDelegate* self, REFIID iid, void** result) noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (IsEqualGUID(iid, kIUnknownGuid) || IsEqualGUID(iid, kIInspectableGuid) || IsEqualGUID(iid, kPlaybackEventDelegateGuid)) {
        *result = self;
        self->refCount.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG __stdcall playbackDelegateAddRef(PlaybackDelegate* self) noexcept {
    return self->refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG __stdcall playbackDelegateRelease(PlaybackDelegate* self) noexcept {
    const ULONG count = self->refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    return count;
}

HRESULT __stdcall playbackDelegateGetIids(PlaybackDelegate*, ULONG* count, IID** iids) noexcept {
    if (!count || !iids) return E_POINTER;
    *count = 1;
    *iids = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
    if (!*iids) return E_OUTOFMEMORY;
    (*iids)[0] = kPlaybackEventDelegateGuid;
    return S_OK;
}

HRESULT __stdcall playbackDelegateGetRuntimeClassName(PlaybackDelegate*, HSTRING* name) noexcept {
    if (!name) return E_POINTER;
    return WindowsCreateString(L"AMP.Core.PlaybackEventDelegate", 30, name);
}

HRESULT __stdcall playbackDelegateGetTrustLevel(PlaybackDelegate*, TrustLevel* trustLevel) noexcept {
    if (!trustLevel) return E_POINTER;
    *trustLevel = BaseTrust;
    return S_OK;
}

HRESULT __stdcall playbackDelegateInvoke(PlaybackDelegate* self, UINT32 message, HSTRING paramsDict) noexcept {
    if (!self || !self->out || !self->mutex) return S_OK;
    const auto params = hstringViewToWide(paramsDict);
    std::lock_guard<std::mutex> lock(*self->mutex);
    *self->out << "PlaybackEvent.message=" << message << "\n"
               << "PlaybackEvent.paramsDict=" << smtc::util::wideToUtf8(params) << "\n";
    self->out->flush();
    return S_OK;
}

PlaybackDelegateVTable kPlaybackDelegateVTable{
    reinterpret_cast<decltype(InspectableVTable::QueryInterface)>(&playbackDelegateQueryInterface),
    reinterpret_cast<decltype(InspectableVTable::AddRef)>(&playbackDelegateAddRef),
    reinterpret_cast<decltype(InspectableVTable::Release)>(&playbackDelegateRelease),
    reinterpret_cast<decltype(InspectableVTable::GetIids)>(&playbackDelegateGetIids),
    reinterpret_cast<decltype(InspectableVTable::GetRuntimeClassName)>(&playbackDelegateGetRuntimeClassName),
    reinterpret_cast<decltype(InspectableVTable::GetTrustLevel)>(&playbackDelegateGetTrustLevel),
    &playbackDelegateInvoke
};

std::wstring guidToWide(const GUID& guid) {
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

bool packagePathFromFullName(const std::wstring& fullName, std::filesystem::path& path) {
    UINT32 length = 0;
    LONG rc = GetPackagePathByFullName(fullName.c_str(), &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0) return false;
    std::wstring buffer(length, L'\0');
    rc = GetPackagePathByFullName(fullName.c_str(), &length, buffer.data());
    if (rc != ERROR_SUCCESS) return false;
    if (length > 0 && buffer[length - 1] == L'\0') buffer.resize(length - 1);
    else buffer.resize(length);
    path = buffer;
    return std::filesystem::is_directory(path);
}

std::filesystem::path resolveAppleMusicPackagePath() {
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    LONG rc = FindPackagesByPackageFamily(kAppleMusicPackageFamilyName, PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &count, nullptr, &bufferLength, nullptr, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0 || bufferLength == 0) return {};

    std::vector<PWSTR> names(count);
    std::wstring buffer(bufferLength, L'\0');
    rc = FindPackagesByPackageFamily(kAppleMusicPackageFamilyName, PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &count, names.data(), &bufferLength, buffer.data(), nullptr);
    if (rc != ERROR_SUCCESS) return {};

    for (UINT32 i = 0; i < count; ++i) {
        std::filesystem::path path;
        if (names[i] && packagePathFromFullName(names[i], path)) return path;
    }
    return {};
}

DWORD findProcessIdByName(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, processName) == 0) {
            result = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return result;
}

std::string hresultUtf8(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

std::optional<std::filesystem::path> outputPathFromCommandLine(std::wstring_view targetSwitch) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return std::nullopt;

    std::optional<std::filesystem::path> outputPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == targetSwitch && i + 1 < argc) {
            outputPath = argv[i + 1];
            break;
        }
        const std::wstring prefix = std::wstring(targetSwitch) + L"=";
        if (arg.rfind(prefix, 0) == 0) {
            outputPath = std::wstring(arg.substr(prefix.size()));
            break;
        }
    }

    LocalFree(argv);
    return outputPath;
}

int runAppleMusicInternalProbe(const std::filesystem::path& outputPath) {
    smtc::applemusic::AppleMusicInternalProvider provider;
    const auto before = provider.detectBridge();
    const auto state = provider.readState();
    const auto after = provider.detectBridge();

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) return 3;

    const auto writeWide = [&out](std::string_view key, const std::wstring& value) {
        out << key << "=" << smtc::util::wideToUtf8(value) << "\n";
    };
    const auto writeInt = [&out](std::string_view key, long long value) {
        out << key << "=" << value << "\n";
    };
    const auto writeBool = [&out](std::string_view key, bool value) {
        out << key << "=" << (value ? "1" : "0") << "\n";
    };

    writeBool("bridgeLoadedBefore", before.bridgeModuleLoaded);
    writeBool("bridgeRespondingBefore", before.bridgeResponding);
    writeWide("bridgeStatusBefore", before.message);
    writeBool("bridgeLoadedAfter", after.bridgeModuleLoaded);
    writeBool("bridgeRespondingAfter", after.bridgeResponding);
    writeWide("bridgeStatusAfter", after.message);
    writeBool("valid", state.valid);
    writeBool("playing", state.playing);
    writeWide("title", state.title);
    writeWide("artist", state.artist);
    writeWide("album", state.album);
    writeInt("positionMs", state.positionMs);
    writeInt("durationMs", state.durationMs);
    writeWide("error", provider.lastError());
    return state.valid ? 0 : 2;
}

int runAppleMusicInjectProbe(const std::filesystem::path& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) return 3;

    const auto writeLine = [&out](std::string_view key, std::string_view value) {
        out << key << "=" << value << "\n";
    };
    const auto writeWide = [&out](std::string_view key, const std::wstring& value) {
        out << key << "=" << smtc::util::wideToUtf8(value) << "\n";
    };
    const auto writeInt = [&out](std::string_view key, long long value) {
        out << key << "=" << value << "\n";
    };
    const auto writeBool = [&out](std::string_view key, bool value) {
        out << key << "=" << (value ? "1" : "0") << "\n";
    };

    writeLine("probe", "AppleMusicInjectBridge");
    smtc::applemusic::AppleMusicInternalProvider provider;
    const auto before = provider.detectBridge();
    std::wstring loadMessage;
    const bool loaded = provider.loadBridge(loadMessage);
    const auto state = provider.readState();
    const auto after = provider.detectBridge();

    writeInt("AppleMusicPid", after.appleMusicPid ? after.appleMusicPid : before.appleMusicPid);
    writeBool("bridgeLoadedBefore", before.bridgeModuleLoaded);
    writeBool("bridgeRespondingBefore", before.bridgeResponding);
    writeWide("bridgeStatusBefore", before.message);
    writeBool("loadBridge", loaded);
    writeWide("loadMessage", loadMessage);
    writeBool("bridgeLoadedAfter", after.bridgeModuleLoaded);
    writeBool("bridgeRespondingAfter", after.bridgeResponding);
    writeWide("bridgeStatusAfter", after.message);
    writeBool("valid", state.valid);
    writeBool("playing", state.playing);
    writeWide("title", state.title);
    writeWide("artist", state.artist);
    writeWide("album", state.album);
    writeInt("positionMs", state.positionMs);
    writeInt("durationMs", state.durationMs);
    writeWide("error", provider.lastError());
    return loaded && state.valid ? 0 : 2;
}

int runAppleMusicCoreProbe(const std::filesystem::path& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) return 3;

    const auto writeLine = [&out](std::string_view key, std::string_view value) {
        out << key << "=" << value << "\n";
    };
    const auto writeWide = [&out](std::string_view key, const std::wstring& value) {
        out << key << "=" << smtc::util::wideToUtf8(value) << "\n";
    };
    const auto writeHr = [&out](std::string_view key, HRESULT hr) {
        out << key << "=" << hresultUtf8(hr) << "\n";
    };
    const auto writeInt = [&out](std::string_view key, long long value) {
        out << key << "=" << value << "\n";
    };
    const auto makeKey = [](std::string_view prefix, std::string_view suffix) {
        std::string key(prefix);
        key += suffix;
        return key;
    };
    const auto makeHString = [](std::wstring_view text, HSTRING* value) {
        return WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), value);
    };
    const auto writeAsyncHString = [&](std::string_view keyPrefix, void* operationRaw) {
        writeLine(makeKey(keyPrefix, ".present"), operationRaw ? "1" : "0");
        if (!operationRaw) return;

        void* asyncInfoRaw = nullptr;
        HRESULT asyncInfoHr = queryInterface(operationRaw, kIAsyncInfoGuid, &asyncInfoRaw);
        writeHr(makeKey(keyPrefix, ".QueryIAsyncInfo"), asyncInfoHr);
        if (SUCCEEDED(asyncInfoHr) && asyncInfoRaw) {
            auto* asyncInfo = vtable<IAsyncInfoVTable>(asyncInfoRaw);
            UINT32 id = 0;
            HRESULT idHr = asyncInfo->get_Id(asyncInfoRaw, &id);
            writeHr(makeKey(keyPrefix, ".Id.hr"), idHr);
            if (SUCCEEDED(idHr)) writeInt(makeKey(keyPrefix, ".Id"), id);

            std::int32_t status = 0;
            HRESULT statusHr = E_FAIL;
            for (int i = 0; i < 100; ++i) {
                statusHr = asyncInfo->get_Status(asyncInfoRaw, &status);
                if (FAILED(statusHr) || status != 0) break;
                Sleep(50);
            }
            writeHr(makeKey(keyPrefix, ".Status.hr"), statusHr);
            if (SUCCEEDED(statusHr)) writeInt(makeKey(keyPrefix, ".Status"), status);

            HRESULT errorCode = S_OK;
            HRESULT errorHr = asyncInfo->get_ErrorCode(asyncInfoRaw, &errorCode);
            writeHr(makeKey(keyPrefix, ".ErrorCode.hr"), errorHr);
            if (SUCCEEDED(errorHr)) writeHr(makeKey(keyPrefix, ".ErrorCode"), errorCode);
        }

        HSTRING asyncResult = nullptr;
        HRESULT resultHr = vtable<IAsyncOperationHStringVTable>(operationRaw)->GetResults(operationRaw, &asyncResult);
        writeHr(makeKey(keyPrefix, ".GetResults.hr"), resultHr);
        if (SUCCEEDED(resultHr)) writeWide(makeKey(keyPrefix, ".Result"), hstringToWide(asyncResult));

        if (asyncInfoRaw) {
            writeHr(makeKey(keyPrefix, ".Close.hr"), vtable<IAsyncInfoVTable>(asyncInfoRaw)->Close(asyncInfoRaw));
            releaseInspectable(asyncInfoRaw);
        }
        releaseInspectable(operationRaw);
    };

    writeLine("probe", "AMP.Core.AMPMusicLibrary");
    const DWORD currentPid = GetCurrentProcessId();
    const DWORD appleMusicPid = findProcessIdByName(L"AppleMusic.exe");
    const DWORD setupPid = appleMusicPid ? appleMusicPid : currentPid;
    writeInt("CurrentPid", currentPid);
    writeInt("AppleMusicPid", appleMusicPid);
    writeInt("SetupPid", setupPid);

    const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    const bool roInitialized = SUCCEEDED(roHr);
    writeHr("RoInitialize", roHr);

    DWORD proxyRegistrationCookie = 0;
    void* proxyClassFactoryRaw = nullptr;
    HMODULE proxyModule = nullptr;
    const auto appleMusicDir = resolveAppleMusicPackagePath();
    writeWide("AppleMusicDir", appleMusicDir.wstring());
    const auto proxyPath = appleMusicDir / L"AMPLibraryAgent.Proxies.dll";
    proxyModule = LoadLibraryExW(proxyPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (proxyModule) {
        writeHr("LoadLibraryEx.ProxyDll", S_OK);
    } else {
        writeHr("LoadLibraryEx.ProxyDll", HRESULT_FROM_WIN32(GetLastError()));
        proxyModule = LoadPackagedLibrary(L"AMPLibraryAgent.Proxies.dll", 0);
        if (proxyModule) writeHr("LoadPackagedLibrary.ProxyDll", S_OK);
        else writeHr("LoadPackagedLibrary.ProxyDll", HRESULT_FROM_WIN32(GetLastError()));
    }
    if (!proxyModule && !appleMusicDir.empty()) {
        const auto localProxyPath = smtc::util::executableDirectory() / L"applemusic-AMPLibraryAgent.Proxies.dll";
        if (CopyFileW(proxyPath.c_str(), localProxyPath.c_str(), FALSE)) {
            writeWide("ProxyDllCopiedTo", localProxyPath.wstring());
            proxyModule = LoadLibraryExW(localProxyPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (proxyModule) writeHr("LoadLibraryEx.LocalProxyDll", S_OK);
            else writeHr("LoadLibraryEx.LocalProxyDll", HRESULT_FROM_WIN32(GetLastError()));
        } else {
            writeHr("CopyFile.ProxyDll", HRESULT_FROM_WIN32(GetLastError()));
        }
    }
    if (proxyModule) {
        auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(proxyModule, "DllGetClassObject"));
        writeHr("GetProcAddress.DllGetClassObject", getClassObject ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        if (getClassObject) {
            HRESULT proxyHr = getClassObject(kIAmpLibraryGuid, kIPSFactoryBufferGuid, &proxyClassFactoryRaw);
            writeHr("DllGetClassObject.Proxy", proxyHr);
            if (SUCCEEDED(proxyHr) && proxyClassFactoryRaw) {
                proxyHr = CoRegisterClassObject(kIAmpLibraryGuid, static_cast<IUnknown*>(proxyClassFactoryRaw), CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &proxyRegistrationCookie);
                writeHr("CoRegisterClassObject.Proxy", proxyHr);
            }
        }
    }

    writeHr("CoRegisterPSClsid.IAMPMusicLibrary", CoRegisterPSClsid(kIAmpMusicLibraryGuid, kIAmpLibraryGuid));
    writeHr("CoRegisterPSClsid.IAMPLibrary", CoRegisterPSClsid(kIAmpLibraryGuid, kIAmpLibraryGuid));
    writeHr("CoRegisterPSClsid.PlaybackEventDelegate", CoRegisterPSClsid(kPlaybackEventDelegateGuid, kIAmpLibraryGuid));

    HSTRING className = nullptr;
    HRESULT hr = WindowsCreateString(L"AMP.Core.AMPMusicLibrary", 24, &className);
    writeHr("WindowsCreateString", hr);

    void* activatedRaw = nullptr;
    void* factoryRaw = nullptr;
    if (SUCCEEDED(hr)) {
        hr = RoGetActivationFactory(className, kIActivationFactoryGuid, &factoryRaw);
        writeHr("RoGetActivationFactory", hr);
        if (SUCCEEDED(hr) && factoryRaw) {
            hr = vtable<ActivationFactoryVTable>(factoryRaw)->ActivateInstance(factoryRaw, &activatedRaw);
            writeHr("ActivateInstance", hr);
        }
        WindowsDeleteString(className);
    }

    void* comInspectableRaw = nullptr;
    hr = CoCreateInstance(kAmpMusicLibraryClassGuid, nullptr, CLSCTX_LOCAL_SERVER, kIInspectableGuid, &comInspectableRaw);
    writeHr("CoCreateInstance.IInspectable", hr);

    if (comInspectableRaw) {
        HSTRING runtimeClassName = nullptr;
        hr = vtable<InspectableVTable>(comInspectableRaw)->GetRuntimeClassName(comInspectableRaw, &runtimeClassName);
        writeHr("IInspectable.GetRuntimeClassName", hr);
        if (SUCCEEDED(hr)) writeWide("IInspectable.RuntimeClassName", hstringToWide(runtimeClassName));

        ULONG iidCount = 0;
        IID* iids = nullptr;
        hr = vtable<InspectableVTable>(comInspectableRaw)->GetIids(comInspectableRaw, &iidCount, &iids);
        writeHr("IInspectable.GetIids", hr);
        if (SUCCEEDED(hr)) {
            writeInt("IInspectable.IidCount", iidCount);
            for (ULONG i = 0; i < iidCount && i < 16; ++i) {
                writeWide("IInspectable.Iid" + std::to_string(i), guidToWide(iids[i]));
            }
            CoTaskMemFree(iids);
        }

        void* ampMusicLibraryRaw = nullptr;
        hr = queryInterface(comInspectableRaw, kIAmpMusicLibraryGuid, &ampMusicLibraryRaw);
        writeHr("QueryInterface.IAMPMusicLibrary", hr);
        releaseInspectable(ampMusicLibraryRaw);
    }

    void* comMusicLibraryRaw = nullptr;
    hr = CoCreateInstance(kAmpMusicLibraryClassGuid, nullptr, CLSCTX_LOCAL_SERVER, kIAmpMusicLibraryGuid, &comMusicLibraryRaw);
    writeHr("CoCreateInstance.IAMPMusicLibrary", hr);

    void* comLibraryRaw = nullptr;
    hr = CoCreateInstance(kAmpMusicLibraryClassGuid, nullptr, CLSCTX_LOCAL_SERVER, kIAmpLibraryGuid, &comLibraryRaw);
    writeHr("CoCreateInstance.IAMPLibrary", hr);

    void* candidate = activatedRaw ? activatedRaw : (comLibraryRaw ? comLibraryRaw : comInspectableRaw);
    void* libraryRaw = nullptr;
    if (candidate == comLibraryRaw) {
        libraryRaw = comLibraryRaw;
        comLibraryRaw = nullptr;
        writeHr("QueryInterface.IAMPLibrary", S_OK);
    } else if (candidate) {
        hr = queryInterface(candidate, kIAmpLibraryGuid, &libraryRaw);
        writeHr("QueryInterface.IAMPLibrary", hr);
    } else {
        writeHr("QueryInterface.IAMPLibrary", E_NOINTERFACE);
    }

    if (libraryRaw) {
        auto* library = vtable<IAmpLibraryVTable>(libraryRaw);

        HSTRING value = nullptr;
        hr = library->get_Version(libraryRaw, &value);
        writeHr("Version.hr", hr);
        if (SUCCEEDED(hr)) writeWide("Version", hstringToWide(value));

        value = nullptr;
        hr = library->get_ServerInfo(libraryRaw, &value);
        writeHr("ServerInfo.hr", hr);
        if (SUCCEEDED(hr)) writeWide("ServerInfo", hstringToWide(value));

        std::int32_t state = 0;
        hr = library->get_State(libraryRaw, &state);
        writeHr("State.hr", hr);
        if (SUCCEEDED(hr)) writeInt("State", state);

        UINT32 agentPid = 0;
        INT32 setupResult = 0;
        hr = library->Setup(libraryRaw, setupPid, &agentPid, &setupResult);
        writeHr("Setup.hr", hr);
        if (SUCCEEDED(hr)) {
            writeInt("Setup.agentPid", agentPid);
            writeInt("Setup.result", setupResult);
        }

        value = nullptr;
        hr = library->MediaAppStarting(libraryRaw, 1, &value);
        writeHr("MediaAppStarting.hr", hr);
        if (SUCCEEDED(hr)) writeWide("MediaAppStarting.result", hstringToWide(value));

        UINT32 clientId = 0;
        hr = library->get_ClientID(libraryRaw, &clientId);
        writeHr("ClientID.hr", hr);
        if (SUCCEEDED(hr)) writeInt("ClientID", clientId);

        bool mediaDomainsOpened = false;
        hr = library->get_MediaDomainsOpened(libraryRaw, &mediaDomainsOpened);
        writeHr("MediaDomainsOpened.hr", hr);
        if (SUCCEEDED(hr)) writeInt("MediaDomainsOpened", mediaDomainsOpened ? 1 : 0);

        value = nullptr;
        hr = library->get_ServerProtocolVersion(libraryRaw, &value);
        writeHr("ServerProtocolVersion.hr", hr);
        if (SUCCEEDED(hr)) writeWide("ServerProtocolVersion", hstringToWide(value));

        const auto registerClient = [&](std::string_view keyPrefix, std::wstring_view name, UINT32 requestedClientId) {
            HSTRING nameRaw = nullptr;
            HRESULT nameHr = makeHString(name, &nameRaw);
            writeHr(makeKey(keyPrefix, ".WindowsCreateString"), nameHr);
            if (FAILED(nameHr)) return RegisterClientResult{};

            RegisterClientResult result{};
            HRESULT registerHr = library->RegisterLibraryClient(libraryRaw, nameRaw, requestedClientId, &result);
            writeHr(makeKey(keyPrefix, ".hr"), registerHr);
            if (SUCCEEDED(registerHr)) {
                writeInt(makeKey(keyPrefix, ".clientID"), result.clientID);
                writeInt(makeKey(keyPrefix, ".persistentMachineID"), static_cast<long long>(result.persistentMachineID));
                writeInt(makeKey(keyPrefix, ".status"), result.status);
            }
            WindowsDeleteString(nameRaw);
            return result;
        };

        const auto emptyPlistDict = std::wstring_view(
            L"62706C6973743030D0080000000000000101000000000000000100000000000000000000000000000009");
        const auto execAgentCommandPlayer = std::wstring_view(
            L"62706C6973743030D101025F10116B457865634167656E74436F6D6D616E6456506C61796572080B1F0000000000000101000000000000000300000000000000000000000000000026");
        const auto commandPlayer = std::wstring_view(
            L"62706C6973743030D1010257636F6D6D616E6456506C61796572080B13000000000000010100000000000000030000000000000000000000000000001A");

        const auto runAsyncWithParams = [&](std::string_view keyPrefix, std::wstring_view params,
                                            auto&& invoker) {
            HSTRING paramsRaw = nullptr;
            HRESULT paramsHr = makeHString(params, &paramsRaw);
            writeHr(makeKey(keyPrefix, ".WindowsCreateString"), paramsHr);
            if (FAILED(paramsHr)) return;

            void* operationRaw = nullptr;
            HRESULT callHr = invoker(paramsRaw, &operationRaw);
            writeHr(makeKey(keyPrefix, ".hr"), callHr);
            if (SUCCEEDED(callHr)) writeAsyncHString(keyPrefix, operationRaw);
            else releaseInspectable(operationRaw);
            WindowsDeleteString(paramsRaw);
        };

        const auto runGetDomainInfo = [&](std::string_view keyPrefix, UINT32 targetClientId) {
            runAsyncWithParams(keyPrefix, emptyPlistDict, [&](HSTRING paramsRaw, void** operationRaw) {
                return library->GetDomainInfoAsync(libraryRaw, 1, targetClientId, paramsRaw, operationRaw);
            });
        };
        const auto runOpenDomains = [&](std::string_view keyPrefix, UINT32 targetClientId) {
            runAsyncWithParams(keyPrefix, emptyPlistDict, [&](HSTRING paramsRaw, void** operationRaw) {
                return library->OpenDomainsAsync(libraryRaw, 1, targetClientId, paramsRaw, operationRaw);
            });
        };
        const auto runAgentCommand = [&](std::string_view keyPrefix, std::wstring_view params) {
            runAsyncWithParams(keyPrefix, params, [&](HSTRING paramsRaw, void** operationRaw) {
                return library->MediaAppExecAgentCommandAsync(libraryRaw, paramsRaw, operationRaw);
            });
        };

        const auto smtcLyricsClient = registerClient("RegisterLibraryClient.SMTCLyrics.0", L"SMTCLyrics", 0);
        const auto musicClient = registerClient("RegisterLibraryClient.AppleMusic.0", L"Apple Music", 0);
        const auto musicShortClient = registerClient("RegisterLibraryClient.Music.0", L"Music", 0);

        UINT32 clientIdAfterRegister = 0;
        hr = library->get_ClientID(libraryRaw, &clientIdAfterRegister);
        writeHr("ClientIDAfterRegister.hr", hr);
        if (SUCCEEDED(hr)) writeInt("ClientIDAfterRegister", clientIdAfterRegister);

        if (clientId != 0) {
            runGetDomainInfo("GetDomainInfo.ClientID", clientId);
            runOpenDomains("OpenDomains.ClientID", clientId);
        }
        if (smtcLyricsClient.clientID != 0) {
            runGetDomainInfo("GetDomainInfo.SMTCLyricsClient", smtcLyricsClient.clientID);
            runOpenDomains("OpenDomains.SMTCLyricsClient", smtcLyricsClient.clientID);
        }
        if (musicClient.clientID != 0) {
            runGetDomainInfo("GetDomainInfo.AppleMusicClient", musicClient.clientID);
            runOpenDomains("OpenDomains.AppleMusicClient", musicClient.clientID);
        }
        if (musicShortClient.clientID != 0) {
            runGetDomainInfo("GetDomainInfo.MusicClient", musicShortClient.clientID);
            runOpenDomains("OpenDomains.MusicClient", musicShortClient.clientID);
        }

        runAgentCommand("MediaAppExecAgentCommand.EmptyString", L"");
        runAgentCommand("MediaAppExecAgentCommand.EmptyPlistDict", emptyPlistDict);
        runAgentCommand("MediaAppExecAgentCommand.KExecAgentCommandPlayer", execAgentCommandPlayer);
        runAgentCommand("MediaAppExecAgentCommand.CommandPlayer", commandPlayer);

        std::mutex callbackMutex;
        PlaybackDelegate playbackDelegate{&kPlaybackDelegateVTable, 1, &out, &callbackMutex};
        std::int64_t playbackToken = 0;
        hr = library->add_PlaybackEventHandler(libraryRaw, &playbackDelegate, &playbackToken);
        writeHr("add_PlaybackEventHandler.hr", hr);
        if (SUCCEEDED(hr)) {
            writeInt("PlaybackEvent.token", playbackToken);
            out.flush();
            Sleep(5000);
            writeHr("remove_PlaybackEventHandler.hr", library->remove_PlaybackEventHandler(libraryRaw, playbackToken));
        }

        if (setupPid == currentPid) {
            INT32 cleanupResult = 0;
            hr = library->Cleanup(libraryRaw, currentPid, &cleanupResult);
            writeHr("Cleanup.hr", hr);
            if (SUCCEEDED(hr)) writeInt("Cleanup.result", cleanupResult);
        } else {
            writeLine("Cleanup.skipped", "setupPidIsAppleMusic");
        }
    }

    releaseInspectable(libraryRaw);
    releaseInspectable(activatedRaw);
    releaseInspectable(comInspectableRaw);
    releaseInspectable(comMusicLibraryRaw);
    releaseInspectable(comLibraryRaw);
    releaseInspectable(factoryRaw);
    if (proxyRegistrationCookie) CoRevokeClassObject(proxyRegistrationCookie);
    releaseInspectable(proxyClassFactoryRaw);
    if (proxyModule) FreeLibrary(proxyModule);
    if (roInitialized) RoUninitialize();

    return libraryRaw ? 0 : 2;
}

}

// Windows GUI 程序入口：先启用高 DPI 感知，再把生命周期交给应用层。
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    if (const auto probeOutputPath = outputPathFromCommandLine(kProbeAppleMusicSwitch)) {
        return runAppleMusicInternalProbe(*probeOutputPath);
    }
    if (const auto probeOutputPath = outputPathFromCommandLine(kProbeAppleMusicInjectSwitch)) {
        return runAppleMusicInjectProbe(*probeOutputPath);
    }
    if (const auto probeOutputPath = outputPathFromCommandLine(kProbeAppleMusicCoreSwitch)) {
        return runAppleMusicCoreProbe(*probeOutputPath);
    }

    // 开启逐显示器 DPI 感知，避免高分屏上歌词窗口发虚。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    smtc::app::Application app;
    return app.run();
}
