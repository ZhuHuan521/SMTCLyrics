#include "applemusic/AppleMusicInternalProvider.h"

#include "applemusic/AppleMusicBridgeProtocol.h"
#include "resource.h"
#include "util/Path.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::applemusic {
namespace {

using namespace std::chrono_literals;

constexpr auto kInjectRetryInterval = 5s;
constexpr DWORD kPipeWaitMs = 500;
constexpr int kPipeWaitAttempts = 8;
constexpr wchar_t kBridgeTempFilePrefix[] = L"SMTCLyricsAppleMusicBridge-";

struct LoadedBridgeModule {
    HMODULE handle = nullptr;
    std::wstring name;
    std::filesystem::path path;
};

void closeHandle(HANDLE handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

struct HandleDeleter {
    void operator()(void* handle) const {
        closeHandle(static_cast<HANDLE>(handle));
    }
};

using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

std::wstring hresultText(HRESULT hr) {
    std::wostringstream out;
    out << L"0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

std::wstring win32ErrorText(DWORD error) {
    return hresultText(HRESULT_FROM_WIN32(error));
}

std::wstring hex64(std::uint64_t value) {
    std::wostringstream out;
    out << std::hex << value;
    return out.str();
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::wstring> pipeNamesForPid(DWORD pid) {
    const auto suffix = std::wstring(bridge::kPipeNamePrefix) + std::to_wstring(pid);
    return {
        L"\\\\.\\pipe\\LOCAL\\" + suffix,
        L"\\\\.\\pipe\\" + suffix,
    };
}

DWORD findAppleMusicProcessId() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"AppleMusic.exe") == 0) {
            result = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return result;
}

std::wstring toLower(std::wstring_view text) {
    std::wstring result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

bool isBridgeModuleName(std::wstring_view name) {
    const auto lower = toLower(name);
    return lower.rfind(L"smtclyricsapplemusicbridge", 0) == 0 &&
           lower.size() >= 4 &&
           lower.substr(lower.size() - 4) == L".dll";
}

std::vector<LoadedBridgeModule> loadedBridgeModules(DWORD pid, DWORD* error = nullptr) {
    if (error) *error = ERROR_SUCCESS;
    std::vector<LoadedBridgeModule> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return modules;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry)) {
        if (isBridgeModuleName(entry.szModule)) {
            modules.push_back(LoadedBridgeModule{entry.hModule, entry.szModule, entry.szExePath});
        }
    }
    CloseHandle(snapshot);
    return modules;
}

std::int64_t normalizeMediaTimeMs(std::int64_t value) {
    if (value <= 0) return 0;
    return value / 10'000;
}

std::int64_t snapshotPositionMs(const AppleMusicInternalSnapshot& snapshot) {
    std::int64_t positionMs = normalizeMediaTimeMs(snapshot.currentPosition100ns);
    if (snapshot.playing && snapshot.queryQpc > 0 && snapshot.qpcFrequency > 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const auto elapsedMs = ((now.QuadPart - snapshot.queryQpc) * 1000) / snapshot.qpcFrequency;
        if (elapsedMs > 0 && elapsedMs < 10'000) {
            positionMs += elapsedMs;
        }
    }
    return positionMs;
}

std::wstring trim(std::wstring_view text) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && isSpace(*begin)) ++begin;
    while (begin != end && isSpace(*(end - 1))) --end;
    return {begin, end};
}

void applySubtitleFallback(AppleMusicInternalSnapshot& snapshot) {
    if (snapshot.subtitle.empty()) return;
    if (!snapshot.artist.empty() && !snapshot.album.empty()) return;

    const auto dash = snapshot.subtitle.find(L'\u2014');
    if (dash == std::wstring::npos) return;

    const auto left = trim(std::wstring_view(snapshot.subtitle).substr(0, dash));
    const auto right = trim(std::wstring_view(snapshot.subtitle).substr(dash + 1));
    if (left.empty() || right.empty()) return;

    if (snapshot.artist.empty()) snapshot.artist = left;
    if (snapshot.album.empty()) snapshot.album = right;
}

struct EmbeddedBridgeBytes {
    const std::uint8_t* data = nullptr;
    DWORD size = 0;
};

bool embeddedBridgeBytes(EmbeddedBridgeBytes& bytes, std::wstring* error) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = module ? FindResourceW(module, MAKEINTRESOURCEW(IDR_APPLE_MUSIC_BRIDGE), RT_RCDATA) : nullptr;
    if (!resource) {
        if (error) *error = L"主程序未内置 Apple Music bridge 资源";
        return false;
    }

    const DWORD size = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0) {
        if (error) *error = L"读取内置 Apple Music bridge 资源失败";
        return false;
    }

    bytes.data = static_cast<const std::uint8_t*>(data);
    bytes.size = size;
    return true;
}

bool fileMatchesBytes(const std::filesystem::path& path, const std::uint8_t* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    UniqueHandle fileHandle(file);

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart != static_cast<LONGLONG>(size)) return false;

    std::uint8_t buffer[64 * 1024];
    DWORD remaining = size;
    const std::uint8_t* cursor = data;
    while (remaining > 0) {
        const DWORD chunk = std::min<DWORD>(remaining, static_cast<DWORD>(sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(file, buffer, chunk, &read, nullptr) || read != chunk) return false;
        if (!std::equal(buffer, buffer + chunk, cursor)) return false;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

bool writeBytesToFile(const std::filesystem::path& path, const std::uint8_t* data, DWORD size, std::wstring* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        const auto message = ec.message();
        if (error) *error = L"创建 bridge 临时目录失败: " + std::wstring(message.begin(), message.end());
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"创建 bridge 临时文件失败: " + win32ErrorText(GetLastError());
        return false;
    }
    UniqueHandle fileHandle(file);

    DWORD remaining = size;
    const std::uint8_t* cursor = data;
    while (remaining > 0) {
        const DWORD chunk = std::min<DWORD>(remaining, 64 * 1024);
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
            if (error) *error = L"写入 bridge 临时文件失败: " + win32ErrorText(GetLastError());
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

void cleanupOldExtractedBridgeFiles(const std::filesystem::path& directory, const std::filesystem::path& keepPath) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto path = entry.path();
        if (path == keepPath) continue;
        const auto name = path.filename().wstring();
        if (name.rfind(kBridgeTempFilePrefix, 0) == 0 && path.extension() == L".dll") {
            std::filesystem::remove(path, ec);
            ec.clear();
        }
    }
}

std::filesystem::path embeddedBridgeDllPath(std::wstring* error) {
    EmbeddedBridgeBytes bytes;
    if (!embeddedBridgeBytes(bytes, error)) return {};

    const auto hash = hex64(fnv1a64(bytes.data, bytes.size));
    const auto directory = util::tempDirectory() / L"SMTCLyrics";
    const auto path = directory / (std::wstring(kBridgeTempFilePrefix) + hash + L".dll");
    cleanupOldExtractedBridgeFiles(directory, path);

    if (fileMatchesBytes(path, bytes.data, bytes.size)) {
        return path;
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    if (!writeBytesToFile(path, bytes.data, bytes.size, error)) return {};
    return path;
}

std::filesystem::path bridgeDllPath(std::wstring* detail = nullptr) {
    std::wstring embeddedError;
    const auto embeddedPath = embeddedBridgeDllPath(&embeddedError);
    if (!embeddedPath.empty()) return embeddedPath;

    const auto sidecar = util::executableDirectory() / L"SMTCLyricsAppleMusicBridge.dll";
    if (std::filesystem::is_regular_file(sidecar)) return sidecar;

    if (detail) {
        *detail = embeddedError.empty() ? L"内置 bridge 提取失败" : embeddedError;
        *detail += L"；同目录 bridge DLL 也不存在: " + sidecar.wstring();
    }
    return {};
}

std::wstring bridgeMissingMessage(std::wstring_view detail) {
    std::wstring message = L"Apple Music 内部 bridge DLL 不可用";
    if (!detail.empty()) message += L": " + std::wstring(detail);
    return message;
}

bool readExact(HANDLE file, void* buffer, DWORD bytes) {
    auto* cursor = static_cast<std::uint8_t*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD transferred = 0;
        if (!ReadFile(file, cursor, remaining, &transferred, nullptr)) return false;
        if (transferred == 0) return false;
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

std::wstring responseErrorMessage(const bridge::SnapshotResponse& response, std::wstring_view fallback) {
    std::wstring message = response.error[0] ? response.error : std::wstring(fallback);
    if (response.hresult) {
        message += L": " + hresultText(static_cast<HRESULT>(response.hresult));
    }
    return message;
}

AppleMusicInternalSnapshot snapshotFromResponse(const bridge::SnapshotResponse& response) {
    AppleMusicInternalSnapshot snapshot;
    snapshot.name = response.title;
    snapshot.subtitle = response.subtitle;
    snapshot.artist = response.artist;
    snapshot.album = response.album;
    snapshot.currentPosition100ns = response.position100ns;
    snapshot.duration100ns = response.duration100ns;
    snapshot.queryQpc = response.queryQpc;
    snapshot.qpcFrequency = response.qpcFrequency;
    snapshot.playing = response.playing != 0;
    applySubtitleFallback(snapshot);
    return snapshot;
}

std::wstring readableSnapshotMessage(const playback::MediaState& state) {
    if (!state.valid) return L"DLL 已加载，但当前内部状态没有歌曲标题";
    std::wstring message = L"DLL 已加载并可读取";
    message += state.playing ? L"（播放中）: " : L"（已暂停）: ";
    message += state.title;
    if (!state.artist.empty()) message += L" - " + state.artist;
    return message;
}

bool remoteFreeLibrary(DWORD pid, HMODULE module, DWORD* error = nullptr) {
    if (error) *error = ERROR_SUCCESS;
    HANDLE processRaw = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!processRaw) {
        if (error) *error = GetLastError();
        return false;
    }
    UniqueHandle process(processRaw);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto freeLibrary = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "FreeLibrary")) : nullptr;
    if (!freeLibrary) {
        if (error) *error = GetLastError();
        return false;
    }

    HANDLE threadRaw = CreateRemoteThread(process.get(), nullptr, 0, freeLibrary, module, 0, nullptr);
    if (!threadRaw) {
        if (error) *error = GetLastError();
        return false;
    }
    UniqueHandle thread(threadRaw);

    const DWORD wait = WaitForSingleObject(thread.get(), 5000);
    if (wait != WAIT_OBJECT_0) {
        if (error) *error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
        return false;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeThread(thread.get(), &exitCode)) {
        if (error) *error = GetLastError();
        return false;
    }
    return exitCode != 0;
}

}

playback::MediaState mapAppleMusicSnapshotToMediaState(const AppleMusicInternalSnapshot& input) {
    auto snapshot = input;
    applySubtitleFallback(snapshot);

    playback::MediaState state;
    state.title = snapshot.name.empty() ? snapshot.subtitle : snapshot.name;
    state.artist = snapshot.artist;
    state.album = snapshot.album;
    state.durationMs = normalizeMediaTimeMs(snapshot.duration100ns);
    state.positionMs = snapshotPositionMs(snapshot);
    state.playing = snapshot.playing;
    if (state.durationMs > 0) {
        state.positionMs = std::clamp(state.positionMs, std::int64_t{0}, state.durationMs);
    }
    state.valid = !state.title.empty();
    return state;
}

AppleMusicInternalProvider::AppleMusicInternalProvider() = default;

AppleMusicInternalProvider::~AppleMusicInternalProvider() {
    shutdown();
}

playback::MediaState AppleMusicInternalProvider::readState() {
    playback::MediaState state;
    lastError_.clear();

    const DWORD appleMusicPid = findAppleMusicProcessId();
    if (!appleMusicPid) {
        bridgePid_ = 0;
        lastError_ = L"Apple Music 未运行";
        return state;
    }

    AppleMusicInternalSnapshot snapshot;
    if (!requestSnapshot(appleMusicPid, snapshot, true)) {
        return state;
    }

    state = mapAppleMusicSnapshotToMediaState(snapshot);
    if (!state.valid) {
        lastError_ = L"Apple Music 内部状态没有歌曲标题";
    }
    return state;
}

AppleMusicBridgeStatus AppleMusicInternalProvider::detectBridge() {
    AppleMusicBridgeStatus status;
    lastError_.clear();

    const DWORD appleMusicPid = findAppleMusicProcessId();
    status.appleMusicPid = appleMusicPid;
    status.appleMusicRunning = appleMusicPid != 0;
    if (!appleMusicPid) {
        status.message = L"Apple Music 未运行";
        lastError_ = status.message;
        return status;
    }

    status.bridgeModuleLoaded = isBridgeModuleLoaded(appleMusicPid);

    bridge::SnapshotResponse response;
    if (!sendBridgeCommand(appleMusicPid, bridge::kCommandSnapshot, response)) {
        status.message = status.bridgeModuleLoaded ? L"DLL 已加载但未响应: " : L"DLL 未加载或未响应: ";
        status.message += lastError_;
        return status;
    }

    status.bridgeResponding = true;
    if (response.status != static_cast<std::uint32_t>(bridge::Status::Ok)) {
        lastError_ = responseErrorMessage(response, L"Apple Music 内部 bridge 读取失败");
        status.message = L"DLL 已响应，但读取失败: " + lastError_;
        return status;
    }

    const auto snapshot = snapshotFromResponse(response);
    const auto state = mapAppleMusicSnapshotToMediaState(snapshot);
    status.snapshotValid = state.valid;
    status.playing = state.playing;
    status.title = state.title;
    status.artist = state.artist;
    status.album = state.album;
    status.message = readableSnapshotMessage(state);
    if (!state.valid) lastError_ = status.message;
    return status;
}

bool AppleMusicInternalProvider::loadBridge(std::wstring& message) {
    message.clear();
    lastError_.clear();

    const DWORD appleMusicPid = findAppleMusicProcessId();
    if (!appleMusicPid) {
        clearRuntime();
        message = L"Apple Music 未运行";
        lastError_ = message;
        return false;
    }

    bridge::SnapshotResponse response;
    if (sendBridgeCommand(appleMusicPid, bridge::kCommandSnapshot, response)) {
        if (response.status == static_cast<std::uint32_t>(bridge::Status::Ok)) {
            message = readableSnapshotMessage(mapAppleMusicSnapshotToMediaState(snapshotFromResponse(response)));
            return true;
        }
        lastError_ = responseErrorMessage(response, L"Apple Music 内部 bridge 读取失败");
        message = L"DLL 已加载，但当前读取失败: " + lastError_;
        return true;
    }

    if (isBridgeModuleLoaded(appleMusicPid)) {
        std::wstring detail;
        if (!unloadBridgeModules(appleMusicPid, &detail)) {
            message = L"DLL 已加载但无法卸载旧实例: " + detail;
            lastError_ = message;
            return false;
        }
    }

    if (!injectBridge(appleMusicPid, true)) {
        message = lastError_;
        return false;
    }
    if (!waitForBridgePipe(appleMusicPid)) {
        message = lastError_;
        return false;
    }
    if (!sendBridgeCommand(appleMusicPid, bridge::kCommandSnapshot, response)) {
        message = lastError_;
        return false;
    }

    if (response.status != static_cast<std::uint32_t>(bridge::Status::Ok)) {
        lastError_ = responseErrorMessage(response, L"Apple Music 内部 bridge 读取失败");
        message = L"DLL 已加载，但当前读取失败: " + lastError_;
        return true;
    }

    message = readableSnapshotMessage(mapAppleMusicSnapshotToMediaState(snapshotFromResponse(response)));
    return true;
}

bool AppleMusicInternalProvider::unloadBridge(std::wstring& message) {
    message.clear();
    lastError_.clear();

    const DWORD appleMusicPid = findAppleMusicProcessId();
    if (!appleMusicPid) {
        clearRuntime();
        message = L"Apple Music 未运行，已清空本地 bridge 状态";
        return true;
    }

    bridge::SnapshotResponse response;
    const bool wasLoaded = isBridgeModuleLoaded(appleMusicPid);
    const bool shutdownSent = sendBridgeCommand(appleMusicPid, bridge::kCommandShutdown, response);
    if (shutdownSent) {
        Sleep(200);
    }

    std::wstring detail;
    const bool ok = unloadBridgeModules(appleMusicPid, &detail);
    clearRuntime();
    if (ok && (shutdownSent || wasLoaded) && detail == L"未检测到已加载的 DLL") {
        message = L"DLL 已卸载";
    } else {
        message = detail.empty() ? (ok ? L"DLL 已卸载" : L"DLL 卸载失败") : detail;
    }
    if (!ok) lastError_ = message;
    return ok;
}

void AppleMusicInternalProvider::shutdown() {
    std::wstring ignored;
    unloadBridge(ignored);
}

bool AppleMusicInternalProvider::requestSnapshot(DWORD appleMusicPid, AppleMusicInternalSnapshot& snapshot, bool allowReload) {
    bridge::SnapshotResponse response;
    if (!sendBridgeCommand(appleMusicPid, bridge::kCommandSnapshot, response)) {
        if (!allowReload) return false;

        if (isBridgeModuleLoaded(appleMusicPid)) {
            std::wstring detail;
            if (!unloadBridgeModules(appleMusicPid, &detail)) {
                lastError_ = L"Apple Music 内部 bridge 已加载但无法重新加载: " + detail;
                return false;
            }
        }

        if (!injectBridge(appleMusicPid, false)) {
            return false;
        }
        if (!waitForBridgePipe(appleMusicPid)) {
            return false;
        }
        if (!sendBridgeCommand(appleMusicPid, bridge::kCommandSnapshot, response)) {
            return false;
        }
    }

    if (response.status != static_cast<std::uint32_t>(bridge::Status::Ok)) {
        lastError_ = responseErrorMessage(response, L"Apple Music 内部 bridge 读取失败");
        return false;
    }

    snapshot = snapshotFromResponse(response);
    return true;
}

bool AppleMusicInternalProvider::sendBridgeCommand(DWORD appleMusicPid, std::uint32_t command, bridge::SnapshotResponse& response) {
    const auto pipeNames = pipeNamesForPid(appleMusicPid);
    bridgePid_ = appleMusicPid;

    DWORD lastOpenError = ERROR_FILE_NOT_FOUND;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        for (const auto& pipeName : pipeNames) {
            pipe = CreateFileW(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;

            lastOpenError = GetLastError();
            if (lastOpenError == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(pipeName.c_str(), 200);
            }
        }
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        lastError_ = L"Apple Music 内部 bridge 未运行: " + win32ErrorText(lastOpenError);
        return false;
    }
    UniqueHandle pipeHandle(pipe);

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    bridge::SnapshotRequest request;
    request.command = command;
    DWORD written = 0;
    if (!WriteFile(pipe, &request, sizeof(request), &written, nullptr) || written != sizeof(request)) {
        lastError_ = L"发送 Apple Music 内部 bridge 请求失败: " + win32ErrorText(GetLastError());
        return false;
    }

    if (!readExact(pipe, &response, sizeof(response))) {
        lastError_ = L"读取 Apple Music 内部 bridge 响应失败: " + win32ErrorText(GetLastError());
        return false;
    }

    if (response.magic != bridge::kMagic || response.version != bridge::kVersion) {
        lastError_ = L"Apple Music 内部 bridge 响应版本不匹配";
        return false;
    }
    return true;
}

bool AppleMusicInternalProvider::waitForBridgePipe(DWORD appleMusicPid) {
    const auto pipeNames = pipeNamesForPid(appleMusicPid);
    DWORD lastWaitError = ERROR_FILE_NOT_FOUND;
    for (int attempt = 0; attempt < kPipeWaitAttempts; ++attempt) {
        for (const auto& pipeName : pipeNames) {
            if (WaitNamedPipeW(pipeName.c_str(), kPipeWaitMs)) return true;
            lastWaitError = GetLastError();
        }
        Sleep(100);
    }
    lastError_ = L"Apple Music 内部 bridge 加载后未响应: " + win32ErrorText(lastWaitError);
    return false;
}

bool AppleMusicInternalProvider::injectBridge(DWORD appleMusicPid, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force &&
        lastInjectAttempt_ != std::chrono::steady_clock::time_point{} &&
        now - lastInjectAttempt_ < kInjectRetryInterval) {
        if (lastError_.empty()) lastError_ = L"Apple Music 内部 bridge 正在等待重试";
        return false;
    }
    lastInjectAttempt_ = now;

    std::wstring bridgePathError;
    bridgeDllPath_ = bridgeDllPath(&bridgePathError);
    if (bridgeDllPath_.empty() || !std::filesystem::is_regular_file(bridgeDllPath_)) {
        lastError_ = bridgeMissingMessage(bridgePathError);
        return false;
    }

    HANDLE processRaw = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        appleMusicPid);
    if (!processRaw) {
        lastError_ = L"打开 AppleMusic.exe 进程失败: " + win32ErrorText(GetLastError());
        return false;
    }
    UniqueHandle process(processRaw);

    const auto bridgePathText = bridgeDllPath_.wstring();
    const SIZE_T bytes = (bridgePathText.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        lastError_ = L"为 Apple Music bridge 分配远程参数失败: " + win32ErrorText(GetLastError());
        return false;
    }

    const auto freeRemotePath = [&] {
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
    };

    if (!WriteProcessMemory(process.get(), remotePath, bridgePathText.c_str(), bytes, nullptr)) {
        lastError_ = L"写入 Apple Music bridge 路径失败: " + win32ErrorText(GetLastError());
        freeRemotePath();
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW")) : nullptr;
    if (!loadLibrary) {
        lastError_ = L"定位 LoadLibraryW 失败: " + win32ErrorText(GetLastError());
        freeRemotePath();
        return false;
    }

    HANDLE threadRaw = CreateRemoteThread(process.get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!threadRaw) {
        lastError_ = L"注入 Apple Music bridge 失败: " + win32ErrorText(GetLastError());
        freeRemotePath();
        return false;
    }
    UniqueHandle thread(threadRaw);

    const DWORD wait = WaitForSingleObject(thread.get(), 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread.get(), &exitCode);
    freeRemotePath();

    if (wait != WAIT_OBJECT_0) {
        lastError_ = L"等待 Apple Music bridge 加载超时";
        return false;
    }
    if (exitCode == 0) {
        lastError_ = L"Apple Music bridge 加载失败";
        return false;
    }
    bridgePid_ = appleMusicPid;
    return true;
}

bool AppleMusicInternalProvider::unloadBridgeModules(DWORD appleMusicPid, std::wstring* detail) {
    bool sawModule = false;
    DWORD enumError = ERROR_SUCCESS;

    for (int attempt = 0; attempt < 8; ++attempt) {
        auto modules = loadedBridgeModules(appleMusicPid, &enumError);
        if (modules.empty()) {
            if (enumError != ERROR_SUCCESS) {
                if (detail) *detail = L"检测 DLL 模块失败: " + win32ErrorText(enumError);
                return false;
            }
            if (detail) *detail = sawModule ? L"DLL 已卸载" : L"未检测到已加载的 DLL";
            return true;
        }

        sawModule = true;
        for (const auto& module : modules) {
            DWORD freeError = ERROR_SUCCESS;
            remoteFreeLibrary(appleMusicPid, module.handle, &freeError);
        }
        Sleep(120);
    }

    if (detail) *detail = L"DLL 仍在 AppleMusic.exe 中，可能仍有引用未释放";
    return false;
}

bool AppleMusicInternalProvider::isBridgeModuleLoaded(DWORD appleMusicPid) const {
    DWORD error = ERROR_SUCCESS;
    return !loadedBridgeModules(appleMusicPid, &error).empty();
}

void AppleMusicInternalProvider::clearRuntime() {
    bridgePid_ = 0;
    bridgeDllPath_.clear();
    lastInjectAttempt_ = {};
}

}
