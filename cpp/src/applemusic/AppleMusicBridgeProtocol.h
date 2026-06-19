#pragma once

#include <cstddef>
#include <cstdint>

namespace smtc::applemusic::bridge {

constexpr std::uint32_t kMagic = 0x534D5443; // SMTC
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kCommandSnapshot = 1;
constexpr std::uint32_t kCommandShutdown = 2;
constexpr wchar_t kPipeNamePrefix[] = L"SMTCLyricsAppleMusicBridge3-";
constexpr std::size_t kTextChars = 256;
constexpr std::size_t kErrorChars = 512;

enum class Status : std::uint32_t {
    Ok = 0,
    AmpServicesMissing = 1,
    ActivationFailed = 2,
    PlayerMissing = 3,
    NowPlayingMissing = 4,
    InterfaceFailed = 5,
    InvalidRequest = 6,
    InternalError = 7,
};

struct SnapshotRequest {
    std::uint32_t magic = kMagic;
    std::uint32_t version = kVersion;
    std::uint32_t command = kCommandSnapshot;
    std::uint32_t reserved = 0;
};

struct SnapshotResponse {
    std::uint32_t magic = kMagic;
    std::uint32_t version = kVersion;
    std::uint32_t status = static_cast<std::uint32_t>(Status::InternalError);
    std::int32_t hresult = 0;
    std::int64_t position100ns = 0;
    std::int64_t duration100ns = 0;
    std::int64_t queryQpc = 0;
    std::int64_t qpcFrequency = 0;
    std::uint32_t playing = 0;
    std::uint32_t paused = 0;
    wchar_t title[kTextChars]{};
    wchar_t subtitle[kTextChars]{};
    wchar_t artist[kTextChars]{};
    wchar_t album[kTextChars]{};
    wchar_t error[kErrorChars]{};
};

static_assert(sizeof(SnapshotRequest) == 16);

}
