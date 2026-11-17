#include "applemusic/AppleMusicBridgeProtocol.h"
#include "playback/PlaybackClock.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace bridge = smtc::applemusic::bridge;

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool transfer(HANDLE pipe, bool write, void* buffer, DWORD size) {
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation{};
    operation.hEvent = ready.value;
    DWORD count = 0;
    BOOL ok = write ? WriteFile(pipe, buffer, size, &count, &operation) : ReadFile(pipe, buffer, size, &count, &operation);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ready.value, 5000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            WaitForSingleObject(ready.value, INFINITE);
            return false;
        }
        ok = GetOverlappedResult(pipe, &operation, &count, FALSE);
    }
    return ok && count == size;
}

bool request(const std::wstring& prefix, std::uint32_t command, bridge::SnapshotResponse& response) {
    const auto pipeName = L"\\\\.\\pipe\\LOCAL\\" + prefix + std::to_wstring(GetCurrentProcessId());
    const auto started = GetTickCount64();
    HANDLE raw = INVALID_HANDLE_VALUE;
    while (raw == INVALID_HANDLE_VALUE && GetTickCount64() - started < 3000) {
        raw = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (raw == INVALID_HANDLE_VALUE) Sleep(5);
    }
    if (raw == INVALID_HANDLE_VALUE) return false;
    Handle pipe(raw);
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr);
    bridge::SnapshotRequest input;
    input.command = command;
    return transfer(pipe.value, true, &input, sizeof(input)) &&
           transfer(pipe.value, false, &response, sizeof(response)) &&
           response.magic == bridge::kMagic && response.version == bridge::kVersion;
}

bridge::SnapshotResponse snapshot(const std::wstring& prefix) {
    bridge::SnapshotResponse response;
    require(request(prefix, bridge::kCommandSnapshot, response), "snapshot IPC failed");
    require(response.status == static_cast<std::uint32_t>(bridge::Status::Ok), "fake AMP snapshot returned an error");
    require(std::wcscmp(response.contentProperties, bridge::kEventPositionCapability) == 0,
            "the production bridge must advertise its event timestamp capability");
    return response;
}

std::wstring filename(const std::wstring& path) {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

template <class Function> Function exported(HMODULE module, const char* name) {
    auto result = reinterpret_cast<Function>(GetProcAddress(module, name));
    require(result != nullptr, "fake AMP export is unavailable");
    return result;
}

std::int64_t counterMilliseconds(std::int64_t counter, std::int64_t frequency) {
    return (counter / frequency) * 1000 + ((counter % frequency) * 1000) / frequency;
}

std::int64_t clockNow(std::int64_t frequency) {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counterMilliseconds(counter.QuadPart, frequency);
}

void testSmoothClockWithRealEventTimes(const std::wstring& prefix,
    void (__stdcall* setPosition)(std::int64_t, bool), const bridge::SnapshotResponse& measured) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto eventTime = counterMilliseconds(measured.queryQpc, frequency.QuadPart);
    const auto eventPosition = measured.position100ns / 10000;
    smtc::playback::PlaybackClock clock;
    clock.synchronize(eventPosition - 1200, eventTime - 1000, true);
    const auto before = clock.positionAt(eventTime);
    clock.update(eventPosition, eventTime, true);
    require(clock.positionAt(eventTime) == before, "the first 200ms correction must not jump the displayed position");
    auto previousTime = clockNow(frequency.QuadPart);
    auto previousPosition = clock.positionAt(previousTime);
    std::int64_t nextSample = eventTime + 1000;
    while (clockNow(frequency.QuadPart) - eventTime < 2100) {
        Sleep(16);
        auto now = clockNow(frequency.QuadPart);
        if (now >= nextSample) {
            setPosition((eventPosition + now - eventTime) * 10000, true);
            const auto fresh = snapshot(prefix);
            require(fresh.hresult == S_OK, "clock correction must use real paired event timestamps");
            const auto sampleTime = counterMilliseconds(fresh.queryQpc, frequency.QuadPart);
            const auto oldPosition = clock.positionAt(sampleTime);
            clock.update(fresh.position100ns / 10000, sampleTime, true);
            require(clock.positionAt(sampleTime) == oldPosition,
                    "low-frequency event samples must not create a visible sweep jump");
            nextSample += 1000;
            now = clockNow(frequency.QuadPart);
        }
        const auto position = clock.positionAt(now);
        const auto advance = position - previousPosition;
        const auto elapsed = now - previousTime;
        require(advance >= 0 && advance * 5 >= elapsed * 4 - 5 && advance * 5 <= elapsed * 6 + 5,
                "event-driven correction must remain monotonic and bounded to 0.8-1.2x speed");
        previousTime = now;
        previousPosition = position;
    }
    const auto now = clockNow(frequency.QuadPart);
    const auto residual = eventPosition + now - eventTime - clock.positionAt(now);
    require(std::llabs(residual) <= 20, "a persistent 200ms lag must converge within one render frame in about two seconds");
    std::cout << "smooth_phase_residual_ms=" << residual << '\n';
}

}

// This host loads the real bridge into its own test process. It never opens,
// writes to, injects into, or controls an Apple Music/user process.
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) return 2;
    const std::wstring prefix = argc == 4 ? argv[3] : bridge::kPipeNamePrefix;
    const std::wstring bridgeName = filename(argv[2]);
    HMODULE fake = LoadLibraryW(argv[1]);
    if (!fake) {
        std::cerr << "Fake AMP library load failed: " << GetLastError() << '\n';
        return 2;
    }
    HMODULE observer = nullptr;
    using SetPosition = void (__stdcall*)(std::int64_t, bool);
    using SetPlaying = void (__stdcall*)(bool);
    using Notify = void (__stdcall*)();
    using SetRetain = void (__stdcall*)(bool);
    Notify releaseRetained = nullptr;
    SetRetain setRetain = nullptr;
    int result = 0;
    try {
        const auto setPosition = exported<SetPosition>(fake, "SetPlaybackPosition");
        const auto setCached = exported<SetPosition>(fake, "SetCachedPlaybackPosition");
        const auto setPlaying = exported<SetPlaying>(fake, "SetPlaying");
        const auto firePosition = exported<Notify>(fake, "FirePositionChanged");
        const auto changeItem = exported<SetPosition>(fake, "ChangeNowPlayingItem");
        setRetain = exported<SetRetain>(fake, "SetRetainCallbacksOnRemove");
        releaseRetained = exported<Notify>(fake, "ReleaseRetainedCallbacks");
        setCached(420000000, true);
        observer = LoadLibraryW(argv[2]);
        require(observer != nullptr, "bridge load into its own mock host failed");

        auto first = snapshot(prefix);
        require(first.hresult == S_FALSE && first.position100ns == 420000000,
                "a first cached query must remain approximate");
        setPosition(430000000, true);
        auto measured = snapshot(prefix);
        require(measured.hresult == S_OK && measured.position100ns == 430000000,
                "a real CurrentPosition notification must produce an accurate paired sample");
        const auto eventQpc = measured.queryQpc;
        Sleep(30);
        measured = snapshot(prefix);
        require(measured.hresult == S_OK && measured.queryQpc == eventQpc,
                "repeated IPC snapshots must retain actual property update time");
        firePosition();
        measured = snapshot(prefix);
        require(measured.queryQpc == eventQpc, "repeated CurrentPosition notifications must not restamp unchanged cached values");

        setCached(440000000, true);
        measured = snapshot(prefix);
        require(measured.hresult == S_FALSE && measured.position100ns == 440000000,
                "query-only changes must remain approximate until a real position event");
        firePosition();
        measured = snapshot(prefix);
        require(measured.hresult == S_OK && measured.position100ns == 440000000,
                "a real position event after the query must recover an accurate timestamp");
        testSmoothClockWithRealEventTimes(prefix, setPosition, measured);

        changeItem(50000000, true);
        measured = snapshot(prefix);
        require(measured.hresult == S_FALSE && measured.position100ns == 50000000,
                "Player.NowPlayingItem replacement must not pretend to be an exact position update");
        setPosition(60000000, true);
        measured = snapshot(prefix);
        require(measured.hresult == S_OK && measured.position100ns == 60000000,
                "the replacement item's position event must be tracked after subscription changes");

        setPosition(65000000, false);
        const auto paused = snapshot(prefix);
        require(paused.playing == 0 && paused.position100ns == 65000000,
                "pause must use the current raw position");
        Sleep(50);
        setPlaying(true);
        measured = snapshot(prefix);
        require(measured.playing != 0 && measured.position100ns == 65000000 &&
                measured.queryQpc > paused.queryQpc,
                "resume at an unchanged position must not carry the paused timestamp forward");
        setPosition(66000000, true);
        measured = snapshot(prefix);
        require(measured.hresult == S_OK, "normal playback must regain precise samples after resume");

        setRetain(true);
        bridge::SnapshotResponse shutdown;
        require(request(prefix, bridge::kCommandShutdown, shutdown), "first shutdown IPC failed");
        require(shutdown.status == static_cast<std::uint32_t>(bridge::Status::InternalError) && FAILED(shutdown.hresult),
                "externally retained callbacks must reject shutdown acknowledgement");
        require(GetModuleHandleW(bridgeName.c_str()) != nullptr,
                "a bridge with retained callbacks must remain loaded");
        releaseRetained();
        setRetain(false);
        require(request(prefix, bridge::kCommandShutdown, shutdown), "second shutdown IPC failed");
        require(shutdown.status == static_cast<std::uint32_t>(bridge::Status::Ok) && SUCCEEDED(shutdown.hresult),
                "shutdown may succeed after all external callback references drain");
        const auto started = GetTickCount64();
        while (GetModuleHandleW(bridgeName.c_str()) && GetTickCount64() - started < 3000) Sleep(10);
        require(GetModuleHandleW(bridgeName.c_str()) == nullptr, "the bridge worker must safely unload its own DLL");
        observer = nullptr;
        std::cout << "All Apple Music bridge IPC integration tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Bridge IPC integration test failed: " << error.what() << '\n';
        result = 1;
    }
    if (observer && GetModuleHandleW(bridgeName.c_str())) {
        if (releaseRetained) releaseRetained();
        if (setRetain) setRetain(false);
        bridge::SnapshotResponse response;
        if (request(prefix, bridge::kCommandShutdown, response) &&
            response.status == static_cast<std::uint32_t>(bridge::Status::Ok)) {
            const auto started = GetTickCount64();
            while (GetModuleHandleW(bridgeName.c_str()) && GetTickCount64() - started < 3000) Sleep(10);
        }
        // A failure leaves the DLL loaded until this disposable host exits.
        // It is never forcibly unloaded while callbacks might still reference it.
    }
    if (!GetModuleHandleW(bridgeName.c_str())) FreeLibrary(fake);
    return result;
}
