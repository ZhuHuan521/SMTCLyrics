#include "cache/Cache.h"

#include "json.hpp"
#include "util/Base64.h"
#include "util/Encoding.h"

#include <fstream>

namespace smtc::cache {
namespace {

// cache.json 的最小结构：source 记歌词源，offset 记单曲微调。
constexpr char kDefaultJson[] = R"({"source":{},"offset":{}})";

// 读取缓存时保持容错：文件为空、损坏或字段类型错误都回到默认结构。
nlohmann::json parseOrDefault(std::string_view text) {
    try {
        auto json = nlohmann::json::parse(text.empty() ? kDefaultJson : std::string(text));
        if (!json.is_object()) {
            json = nlohmann::json::object();
        }

        const auto sourceIt = json.find("source");
        if (sourceIt == json.end() || !sourceIt->is_object()) {
            json["source"] = nlohmann::json::object();
        }

        const auto offsetIt = json.find("offset");
        if (offsetIt == json.end() || !offsetIt->is_object()) {
            json["offset"] = nlohmann::json::object();
        }
        return json;
    } catch (...) {
        return nlohmann::json{{"source", nlohmann::json::object()}, {"offset", nlohmann::json::object()}};
    }
}

// 歌曲关键字可能包含空格、中文和特殊字符，Base64 后更适合作为 JSON key。
std::string keyFor(std::string_view keywordUtf8) {
    return util::base64Encode(keywordUtf8);
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
    // 历史版本可能把 source 存成数字或字符串，这里两种都兼容。
    const auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    const auto sourceIt = json.find("source");
    if (sourceIt == json.end() || !sourceIt->is_object()) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->find(key);
    if (valueIt == sourceIt->end()) {
        return std::nullopt;
    }
    const auto& value = *valueIt;
    try {
        if (value.is_number_integer()) return value.get<int>();
        if (value.is_string()) return std::stoi(value.get<std::string>());
    } catch (...) {
    }
    return std::nullopt;
}

void LyricCache::setSource(std::string_view keywordUtf8, int sourceIndex) {
    // 仍保存为字符串，兼容旧缓存文件格式。
    auto json = parseOrDefault(jsonText_);
    auto& source = json["source"];
    source[keyFor(keywordUtf8)] = std::to_string(sourceIndex);
    jsonText_ = json.dump();
}

void LyricCache::removeSource(std::string_view keywordUtf8) {
    // 只删除指定歌曲的来源缓存，不影响单曲偏移。
    auto json = parseOrDefault(jsonText_);
    auto sourceIt = json.find("source");
    if (sourceIt != json.end() && sourceIt->is_object()) {
        sourceIt->erase(keyFor(keywordUtf8));
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
    // 单曲偏移和歌词源一样兼容数字/字符串两种旧格式。
    const auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    const auto offsetIt = json.find("offset");
    if (offsetIt == json.end() || !offsetIt->is_object()) {
        return std::nullopt;
    }
    const auto valueIt = offsetIt->find(key);
    if (valueIt == offsetIt->end()) {
        return std::nullopt;
    }
    const auto& value = *valueIt;
    try {
        if (value.is_number_integer()) return value.get<int>();
        if (value.is_string()) return std::stoi(value.get<std::string>());
    } catch (...) {
    }
    return std::nullopt;
}

void LyricCache::setOffset(std::string_view keywordUtf8, int offsetMs) {
    // 单曲偏移用于修正个别歌词文件或在线歌词的时间轴误差。
    auto json = parseOrDefault(jsonText_);
    auto& offset = json["offset"];
    offset[keyFor(keywordUtf8)] = std::to_string(offsetMs);
    jsonText_ = json.dump();
}

}
