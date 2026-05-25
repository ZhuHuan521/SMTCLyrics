#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace smtc::cache {

// 轻量歌词缓存：用 JSON 保存“歌曲关键字 -> 上次成功歌词源/单曲偏移”。
class LyricCache {
public:
    // path 通常是可执行文件旁的 cache.json。
    explicit LyricCache(std::filesystem::path path);

    // 从磁盘加载或保存当前 JSON 文本。
    void load();
    void save() const;
    // 读取/写入某首歌上次成功命中的在线歌词源。
    std::optional<int> sourceFor(std::string_view keywordUtf8) const;
    void setSource(std::string_view keywordUtf8, int sourceIndex);
    void removeSource(std::string_view keywordUtf8);

    // 读取/写入某首歌独立的毫秒级歌词偏移。
    std::optional<int> offsetFor(std::string_view keywordUtf8) const;
    void setOffset(std::string_view keywordUtf8, int offsetMs);
    // 清空内存中的缓存内容；ensureExists 只负责创建默认文件。
    void clear();
    void ensureExists() const;

private:
    // 关键字会先 Base64，避免 JSON key 中出现特殊字符或编码差异。
    std::filesystem::path path_;
    std::string jsonText_ = R"({"source":{},"offset":{}})";
};

}
