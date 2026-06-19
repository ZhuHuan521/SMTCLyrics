#pragma once

#include "applemusic/AppleMusicBridgeProtocol.h"
#include "playback/PlaybackState.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace smtc::applemusic {

struct AppleMusicInternalSnapshot {
    std::wstring name;
    std::wstring subtitle;
    std::wstring artist;
    std::wstring album;
    std::int64_t currentPosition100ns = 0;
    std::int64_t duration100ns = 0;
    std::int64_t queryQpc = 0;
    std::int64_t qpcFrequency = 0;
    bool playing = false;
};

struct AppleMusicBridgeStatus {
    DWORD appleMusicPid = 0;
    bool appleMusicRunning = false;
    bool bridgeModuleLoaded = false;
    bool bridgeResponding = false;
    bool snapshotValid = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring message;
};

playback::MediaState mapAppleMusicSnapshotToMediaState(const AppleMusicInternalSnapshot& snapshot);

class AppleMusicInternalProvider {
public:
    AppleMusicInternalProvider();
    ~AppleMusicInternalProvider();

    playback::MediaState readState();
    AppleMusicBridgeStatus detectBridge();
    bool loadBridge(std::wstring& message);
    bool unloadBridge(std::wstring& message);
    const std::wstring& lastError() const { return lastError_; }
    void shutdown();

private:
    bool requestSnapshot(DWORD appleMusicPid, AppleMusicInternalSnapshot& snapshot, bool allowReload);
    bool sendBridgeCommand(DWORD appleMusicPid, std::uint32_t command, bridge::SnapshotResponse& response);
    bool waitForBridgePipe(DWORD appleMusicPid);
    bool injectBridge(DWORD appleMusicPid, bool force);
    bool unloadBridgeModules(DWORD appleMusicPid, std::wstring* detail);
    bool isBridgeModuleLoaded(DWORD appleMusicPid) const;
    void clearRuntime();

    DWORD bridgePid_ = 0;
    std::filesystem::path bridgeDllPath_;
    std::wstring lastError_;
    std::chrono::steady_clock::time_point lastInjectAttempt_{};
};

}
