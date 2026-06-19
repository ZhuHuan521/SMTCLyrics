#include "cache/Cache.h"

#include "json.hpp"
#include "util/Base64.h"
#include "util/Encoding.h"

#include <fstream>

namespace smtc::cache {
namespace {

// cache.json 的最小结构：按歌曲聚合歌词源和单曲微调，避免同一个 key 存两遍。
constexpr char kDefaultJson[] = R"({"songs":{}})";

nlohmann::json defaultJson() {
    return nlohmann::json{{"songs", nlohmann::json::object()}};
}

// 读取缓存时保持容错：文件为空、损坏或字段类型错误都回到默认结构。
nlohmann::json parseOrDefault(std::string_view text) {
    try {
        auto json = nlohmann::json::parse(text.empty() ? kDefaultJson : std::string(text));
        if (!json.is_object()) {
            return defaultJson();
        }

        const auto songsIt = json.find("songs");
        if (songsIt == json.end() || !songsIt->is_object()) {
            return defaultJson();
        }
        return nlohmann::json{{"songs", *songsIt}};
    } catch (...) {
        return defaultJson();
    }
}

// 歌曲关键字可能包含空格、中文和特殊字符，Base64 后更适合作为 JSON key。
std::string keyFor(std::string_view keywordUtf8) {
    return util::base64Encode(keywordUtf8);
}

std::optional<int> intField(const nlohmann::json& song, std::string_view fieldName) {
    if (!song.is_object()) return std::nullopt;
    const auto it = song.find(std::string(fieldName));
    if (it == song.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<int>();
}

nlohmann::json& songEntry(nlohmann::json& json, const std::string& key) {
    auto& songs = json["songs"];
    auto& song = songs[key];
    if (!song.is_object()) {
        song = nlohmann::json::object();
    }
    return song;
}

void eraseSongIfEmpty(nlohmann::json& json, const std::string& key) {
    auto& songs = json["songs"];
    const auto it = songs.find(key);
    if (it != songs.end() && it->is_object() && it->empty()) {
        songs.erase(it);
    }
}

}

LyricCache::LyricCache(std::filesystem::path path) : path_(std::move(path)) {}

void LyricCache::load() {
    // 缓存文件缺失或为空时先使用默认 JSON，真正创建文件由 ensureExists 负责。
    const auto bytes = util::readFileBytes(path_);
    if (bytes.empty()) {
        jsonText_ = kDefaultJson;
        return;
    }
    jsonText_.assign(bytes.begin(), bytes.end());
}

void LyricCache::save() const {
    // 保存前重新解析一遍，顺手修复内存中可能残留的非法结构。
    auto json = parseOrDefault(jsonText_);
    const auto text = json.dump();
    util::writeFileBytes(path_, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::optional<int> LyricCache::sourceFor(std::string_view keywordUtf8) const {
    const auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    const auto& songs = json["songs"];
    const auto songIt = songs.find(key);
    if (songIt == songs.end()) return std::nullopt;
    return intField(*songIt, "source");
}

void LyricCache::setSource(std::string_view keywordUtf8, int sourceIndex) {
    auto json = parseOrDefault(jsonText_);
    songEntry(json, keyFor(keywordUtf8))["source"] = sourceIndex;
    jsonText_ = json.dump();
}

void LyricCache::removeSource(std::string_view keywordUtf8) {
    // 只删除指定歌曲的来源缓存，不影响单曲偏移。
    auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    auto& songs = json["songs"];
    const auto songIt = songs.find(key);
    if (songIt != songs.end() && songIt->is_object()) {
        songIt->erase("source");
        eraseSongIfEmpty(json, key);
    }
    jsonText_ = json.dump();
}

void LyricCache::clear() {
    // 清空的是内存内容；调用方随后 save 才会写回磁盘。
    jsonText_ = kDefaultJson;
}

void LyricCache::ensureExists() const {
    // 首次启动时创建默认缓存文件，后续 load/save 都可假定路径存在。
    if (std::filesystem::exists(path_)) return;
    const std::string text(kDefaultJson);
    util::writeFileBytes(path_, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::optional<int> LyricCache::offsetFor(std::string_view keywordUtf8) const {
    const auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    const auto& songs = json["songs"];
    const auto songIt = songs.find(key);
    if (songIt == songs.end()) return std::nullopt;
    return intField(*songIt, "offset");
}

void LyricCache::setOffset(std::string_view keywordUtf8, int offsetMs) {
    // 单曲偏移用于修正个别歌词文件或在线歌词的时间轴误差。
    auto json = parseOrDefault(jsonText_);
    songEntry(json, keyFor(keywordUtf8))["offset"] = offsetMs;
    jsonText_ = json.dump();
}

}
