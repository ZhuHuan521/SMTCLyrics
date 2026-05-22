#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace smtc::cache {

class LyricCache {
public:
    explicit LyricCache(std::filesystem::path path);

    void load();
    void save() const;
    std::optional<int> sourceFor(std::string_view keywordUtf8) const;
    void setSource(std::string_view keywordUtf8, int sourceIndex);
    void removeSource(std::string_view keywordUtf8);
    void clear();
    void ensureExists() const;

private:
    std::filesystem::path path_;
    std::string jsonText_ = R"({"source":{}})";
};

}
