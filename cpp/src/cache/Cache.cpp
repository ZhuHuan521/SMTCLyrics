#include "cache/Cache.h"

#include "json.hpp"
#include "util/Base64.h"
#include "util/Encoding.h"

#include <fstream>

namespace smtc::cache {
namespace {

constexpr char kDefaultJson[] = R"({"source":{},"offset":{}})";

nlohmann::json parseOrDefault(std::string_view text) {
    try {
        auto json = nlohmann::json::parse(text.empty() ? kDefaultJson : std::string(text));
        if (!json.contains("source") || !json["source"].is_object()) {
            json["source"] = nlohmann::json::object();
        }
        if (!json.contains("offset") || !json["offset"].is_object()) {
            json["offset"] = nlohmann::json::object();
        }
        return json;
    } catch (...) {
        return nlohmann::json{{"source", nlohmann::json::object()}, {"offset", nlohmann::json::object()}};
    }
}

std::string keyFor(std::string_view keywordUtf8) {
    return util::base64Encode(keywordUtf8);
}

}

LyricCache::LyricCache(std::filesystem::path path) : path_(std::move(path)) {}

void LyricCache::load() {
    const auto bytes = util::readFileBytes(path_);
    if (bytes.empty()) {
        jsonText_ = kDefaultJson;
        return;
    }
    jsonText_.assign(bytes.begin(), bytes.end());
}

void LyricCache::save() const {
    auto json = parseOrDefault(jsonText_);
    const auto text = json.dump();
    util::writeFileBytes(path_, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::optional<int> LyricCache::sourceFor(std::string_view keywordUtf8) const {
    const auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    if (!json["source"].contains(key)) {
        return std::nullopt;
    }
    const auto& value = json["source"][key];
    try {
        if (value.is_number_integer()) return value.get<int>();
        if (value.is_string()) return std::stoi(value.get<std::string>());
    } catch (...) {
    }
    return std::nullopt;
}

void LyricCache::setSource(std::string_view keywordUtf8, int sourceIndex) {
    auto json = parseOrDefault(jsonText_);
    json["source"][keyFor(keywordUtf8)] = std::to_string(sourceIndex);
    jsonText_ = json.dump();
}

void LyricCache::removeSource(std::string_view keywordUtf8) {
    auto json = parseOrDefault(jsonText_);
    json["source"].erase(keyFor(keywordUtf8));
    jsonText_ = json.dump();
}

void LyricCache::clear() {
    jsonText_ = kDefaultJson;
}

void LyricCache::ensureExists() const {
    if (std::filesystem::exists(path_)) return;
    const std::string text(kDefaultJson);
    util::writeFileBytes(path_, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::optional<int> LyricCache::offsetFor(std::string_view keywordUtf8) const {
    const auto json = parseOrDefault(jsonText_);
    const auto key = keyFor(keywordUtf8);
    if (!json["offset"].contains(key)) {
        return std::nullopt;
    }
    const auto& value = json["offset"][key];
    try {
        if (value.is_number_integer()) return value.get<int>();
        if (value.is_string()) return std::stoi(value.get<std::string>());
    } catch (...) {
    }
    return std::nullopt;
}

void LyricCache::setOffset(std::string_view keywordUtf8, int offsetMs) {
    auto json = parseOrDefault(jsonText_);
    json["offset"][keyFor(keywordUtf8)] = std::to_string(offsetMs);
    jsonText_ = json.dump();
}

}
