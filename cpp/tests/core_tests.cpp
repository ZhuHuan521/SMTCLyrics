#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/LrcParser.h"
#include "lyrics/LyricRepository.h"
#include "util/Encoding.h"
#include "util/Path.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testLrcParser() {
    const auto runtimeDir = smtc::util::executableDirectory();
    const auto lrcPath = runtimeDir / L"lyrics" / L"Lo Ta-You 皇后大道東 (feat. 蔣志光).lrc";
    const auto bytes = smtc::util::readFileBytes(lrcPath);
    require(!bytes.empty(), "sample lrc should exist");

    smtc::lyrics::LrcParser parser;
    require(parser.parseBytes(bytes), "sample lrc should parse");
    auto frame = parser.frameAt(21'280, 1, 0);
    require(frame.text.find(L"皇后大道西") != std::wstring::npos, "single-line lyric should match timestamp");
    frame = parser.frameAt(21'280, 2, 0);
    require(frame.text.find(L"\n") != std::wstring::npos, "two-line-down mode should include next line");
    frame = parser.frameAt(24'760, 3, 0);
    require(frame.text.find(L"\n") != std::wstring::npos, "two-line-up mode should include previous line");
}

void testConfig() {
    smtc::config::ConfigStore store(smtc::util::executableDirectory() / L"config.ini");
    const auto config = store.load();
    require(config.font.size > 0, "font size should load");
    require(config.displayMode >= 1 && config.displayMode <= 3, "display mode should be valid");
    require(config.sourcePriority.size() == 4, "source priority should contain four sources");
}

void testCache() {
    const auto path = std::filesystem::temp_directory_path() / L"smtclyrics-cache-test.json";
    smtc::cache::LyricCache cache(path);
    cache.clear();
    cache.setSource("Jordan Chan 算你狠", 2);
    cache.save();
    cache.load();
    const auto source = cache.sourceFor("Jordan Chan 算你狠");
    require(source && *source == 2, "cache should persist source index");
    std::filesystem::remove(path);
}

}

int main() {
    try {
        testLrcParser();
        testConfig();
        testCache();
        std::cout << "All SMTCLyrics core tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << '\n';
        return 1;
    }
}
