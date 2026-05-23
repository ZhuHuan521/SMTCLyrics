#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/LrcParser.h"
#include "lyrics/LyricRepository.h"
#include "lyrics/QrcDecrypter.h"
#include "util/Encoding.h"
#include "util/Inflate.h"
#include "util/Path.h"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::wstring readIniString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    std::wstring buffer(256, L'\0');
    const DWORD chars = GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(chars);
    return buffer;
}

void writeIniString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, path.c_str());
}

std::filesystem::path findQrcFixturePath() {
    auto dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        const auto candidate = dir / L"qrckit-master" / L"src" / L"test" / L"kotlin" / L"io" / L"github" / L"proify" / L"qrckit" / L"decrypt" / L"QrcDecrypterTest.kt";
        if (std::filesystem::is_regular_file(candidate)) return candidate;
        if (!dir.has_parent_path()) break;
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool isUpperHex(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
}

std::string extractLongHexRun(std::string_view text) {
    std::size_t runStart = std::string_view::npos;
    std::size_t runLength = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && isUpperHex(text[i])) {
            if (runLength == 0) runStart = i;
            ++runLength;
            continue;
        }
        if (runLength > 1000 && runStart != std::string_view::npos) {
            return std::string(text.substr(runStart, runLength));
        }
        runStart = std::string_view::npos;
        runLength = 0;
    }
    return {};
}

void testLrcParser() {
    const auto runtimeDir = smtc::util::executableDirectory();
    const auto lrcPath = runtimeDir / L"lyrics" / L"Lo Ta-You 皇后大道東 (feat. 蔣志光).lrc";
    const auto bytes = smtc::util::readFileBytes(lrcPath);
    require(!bytes.empty(), "sample lrc should exist");

    smtc::lyrics::LrcParser parser;
    require(parser.parseBytes(bytes), "sample lrc should parse");
    auto frame = parser.frameAt(21'280, 1);
    require(frame.text.find(L"皇后大道西") != std::wstring::npos, "single-line lyric should match timestamp");
    frame = parser.frameAt(21'280, 2);
    require(frame.text.find(L"\n") != std::wstring::npos, "two-line-down mode should include next line");
    frame = parser.frameAt(24'760, 3);
    require(frame.text.find(L"\n") != std::wstring::npos, "two-line-up mode should include previous line");

    smtc::lyrics::LrcParser blankLineParser;
    require(blankLineParser.parseUtf8("[00:01.000]\n[00:02.000]first\n[00:03.000]second\n"), "blank-line lrc should parse");
    frame = blankLineParser.frameAt(1'000, 2);
    require(frame.text == L"first\nsecond", "two-line-down mode should not repeat the resolved current line after a blank timestamp");

    smtc::lyrics::LrcParser duplicateLineParser;
    require(duplicateLineParser.parseUtf8("[00:01.000]same\n[00:02.000]same\n[00:03.000]next\n"), "duplicate-line lrc should parse");
    frame = duplicateLineParser.frameAt(1'000, 2);
    require(frame.text == L"same\nnext", "two-line-down mode should skip an adjacent duplicate line when possible");

    smtc::lyrics::LrcParser krcParser;
    require(krcParser.parseUtf8("[1000,4000]<0,1000,0>A<1000,1000,0>B<2000,1000,0>C<3000,1000,0>D\n[6000,1000]<0,1000,0>E\n"), "krc lyric should parse");
    frame = krcParser.frameAt(2'500, 1);
    require(frame.text == L"ABCD", "krc tags should be stripped from displayed text");
    require(frame.highlightPercent >= 35 && frame.highlightPercent <= 40, "krc per-word progress should map to highlight percent");
    frame = krcParser.frameAt(2'500, 2);
    require(frame.text == L"ABCD\nE" && frame.highlightLine == 0, "krc two-line-down mode should highlight the current first line");
    frame = krcParser.frameAt(6'500, 3);
    require(frame.text == L"ABCD\nE" && frame.highlightLine == 1, "krc two-line-up mode should highlight the current second line");

    smtc::lyrics::LrcParser qrcParser;
    require(qrcParser.parseUtf8("[1000,2000]A(1000,500)B(1500,500)C(2000,1000)[4000,1000]D(4000,1000)\n"), "qrc lyric should parse");
    frame = qrcParser.frameAt(1'750, 1);
    require(frame.text == L"ABC", "qrc word timings should be stripped from displayed text");
    require(frame.highlightPercent >= 45 && frame.highlightPercent <= 55, "qrc absolute word timing should map to highlight percent");
    frame = qrcParser.frameAt(4'500, 3);
    require(frame.text == L"ABC\nD" && frame.highlightLine == 1, "qrc two-line-up mode should highlight the current second line");

    smtc::lyrics::LrcParser relativeQrcParser;
    require(relativeQrcParser.parseUtf8("[10000,1000]A(0,500)B(500,500)\n"), "relative qrc lyric should parse");
    frame = relativeQrcParser.frameAt(10'250, 1);
    require(frame.text == L"AB" && frame.highlightPercent >= 20 && frame.highlightPercent <= 30, "qrc relative word timing should map to highlight percent");
}

void testQrcDecrypterFixture() {
    const auto fixturePath = findQrcFixturePath();
    if (fixturePath.empty()) return;

    const auto fixture = smtc::util::readUtf8Auto(fixturePath);
    const auto encrypted = extractLongHexRun(fixture);
    if (encrypted.empty()) return;

    const auto decrypted = smtc::lyrics::decryptQrc(encrypted);
    require(decrypted.find("LyricContent") != std::string::npos, "qrc decrypter should decode the qrckit fixture");
}

void testInflateZlib() {
    const std::vector<std::uint8_t> compressed{
        0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15
    };
    const auto inflated = smtc::util::inflateZlib(compressed);
    require(std::string(inflated.begin(), inflated.end()) == "hello", "zlib inflater should decode a basic deflate stream");
}

void testConfig() {
    smtc::config::ConfigStore store(smtc::util::executableDirectory() / L"config.ini");
    const auto config = store.load();
    require(config.font.size > 0, "font size should load");
    require(config.displayMode >= 1 && config.displayMode <= 3, "display mode should be valid");
    require(config.sourcePriority.size() == 4, "source priority should contain four sources");
}

void testEnglishConfigRoundTrip() {
    const auto path = std::filesystem::temp_directory_path() / L"smtclyrics-config-english.ini";
    std::filesystem::remove(path);

    smtc::config::AppConfig config;
    config.font.name = L"Microsoft YaHei";
    config.font.size = 42;
    config.font.bold = false;
    config.font.italic = true;
    config.font.underline = true;
    config.lyricOffsetMs = -2;
    config.normal.color1 = RGB(12, 34, 56);
    config.normal.color2 = RGB(222, 111, 1);
    config.normal.border = RGB(1, 2, 3);
    config.highlight.color1 = RGB(4, 5, 6);
    config.highlight.color2 = RGB(7, 8, 9);
    config.highlight.border = RGB(10, 11, 12);
    config.sourcePriority = {4, 3, 2, 1};
    config.smtcMode = 2;
    config.smtcPollIntervalMs = 75;
    config.displayMode = 3;
    config.window = {120, 80, 1280, 160, true};

    smtc::config::ConfigStore store(path);
    store.save(config);
    const auto loaded = store.load();

    require(loaded.font.name == config.font.name, "english font name should round-trip");
    require(loaded.font.size == 42, "english font size should round-trip");
    require(!loaded.font.bold && loaded.font.italic && loaded.font.underline, "english font styles should round-trip");
    require(loaded.lyricOffsetMs == -2, "english offset should round-trip");
    require(loaded.normal.color1 == RGB(12, 34, 56), "hex color should round-trip");
    require(loaded.highlight.border == RGB(10, 11, 12), "highlight border color should round-trip");
    require(loaded.sourcePriority == std::vector<int>({4, 3, 2, 1}), "source priority should save as one-based indexes");
    require(loaded.smtcPollIntervalMs == 500, "SMTC poll interval should clamp to the safe minimum");
    require(loaded.window.hasPosition && loaded.window.left == 120 && loaded.window.top == 80, "window position should round-trip");
    require(readIniString(path, L"Lyrics", L"normalColor1") == L"#0C2238", "colors should be written as #RRGGBB");
    require(readIniString(path, L"SMTC", L"pollIntervalMs") == L"500", "SMTC poll interval should be written with the safe minimum");
    require(readIniString(path, L"Account", L"cookie", L"missing") == L"missing", "cookie account section should not be written");

    std::filesystem::remove(path);
}

void testLegacyConfigMigration() {
    const auto path = std::filesystem::temp_directory_path() / L"smtclyrics-config-legacy.ini";
    std::filesystem::remove(path);

    writeIniString(path, L"字体", L"字体名称", L"楷体");
    writeIniString(path, L"字体", L"字体大小", L"31");
    writeIniString(path, L"字体", L"字体加粗", L"真");
    writeIniString(path, L"字体", L"字体倾斜", L"假");
    writeIniString(path, L"字体", L"字体下划线", L"真");
    writeIniString(path, L"字体", L"文字颜色1", L"255");
    writeIniString(path, L"字体", L"文字颜色2", L"65535");
    writeIniString(path, L"字体", L"文字边框颜色", L"0");
    writeIniString(path, L"账号", L"cookie", L"unused-cookie");
    writeIniString(path, L"歌词", L"微调", L"5");
    writeIniString(path, L"歌词源", L"优先级1", L"0");
    writeIniString(path, L"歌词源", L"优先级2", L"1");
    writeIniString(path, L"歌词源", L"优先级3", L"2");
    writeIniString(path, L"歌词源", L"优先级4", L"3");
    writeIniString(path, L"SMTC", L"SMTC", L"2");
    writeIniString(path, L"显示方式", L"显示方式", L"2");
    writeIniString(path, L"歌词窗口", L"左边", L"33");
    writeIniString(path, L"歌词窗口", L"顶边", L"44");
    writeIniString(path, L"歌词窗口", L"宽度", L"900");
    writeIniString(path, L"歌词窗口", L"高度", L"140");

    smtc::config::ConfigStore store(path);
    const auto loaded = store.load();
    require(loaded.font.name == L"楷体", "legacy font name should load");
    require(loaded.font.size == 31, "legacy font size should load");
    require(loaded.font.bold && loaded.font.underline, "legacy booleans should load");
    require(loaded.normal.color1 == RGB(255, 0, 0), "legacy decimal color should load");
    require(loaded.sourcePriority == std::vector<int>({1, 2, 3, 4}), "legacy zero-based source priority should migrate");
    require(loaded.smtcMode == 2 && loaded.displayMode == 2, "legacy modes should load");
    require(loaded.smtcPollIntervalMs == 1000, "missing SMTC poll interval should use the default");
    require(loaded.window.hasPosition && loaded.window.left == 33 && loaded.window.top == 44, "legacy window position should load");

    store.save(loaded);
    require(readIniString(path, L"Font", L"name") == L"楷体", "migrated config should write english font key");
    require(readIniString(path, L"Lyrics", L"normalColor1") == L"#FF0000", "migrated config should write hex color");
    require(readIniString(path, L"Sources", L"priority1") == L"1", "migrated config should write one-based source priority");
    require(readIniString(path, L"字体", L"字体名称", L"missing") == L"missing", "legacy font section should be removed");
    require(readIniString(path, L"账号", L"cookie", L"missing") == L"missing", "legacy cookie should be removed");
    require(readIniString(path, L"SMTC", L"SMTC", L"missing") == L"missing", "legacy SMTC key should be removed");

    std::filesystem::remove(path);
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

    smtc::util::writeFileBytes(path, std::vector<std::uint8_t>{'{', '"', 'o', 'f', 'f', 's', 'e', 't', '"', ':', '{', '}', '}'});
    cache.load();
    require(!cache.sourceFor("missing").has_value(), "cache without source object should not assert");
    cache.setSource("missing", 3);
    require(cache.sourceFor("missing").value_or(0) == 3, "cache should recreate missing source object");

    const std::string malformedShape = R"({"source":[],"offset":"bad"})";
    smtc::util::writeFileBytes(path, std::vector<std::uint8_t>(malformedShape.begin(), malformedShape.end()));
    cache.load();
    require(!cache.sourceFor("missing").has_value(), "cache with malformed source should not assert");
    require(!cache.offsetFor("missing").has_value(), "cache with malformed offset should not assert");

    const std::string nonObjectRoot = R"(["not","an","object"])";
    smtc::util::writeFileBytes(path, std::vector<std::uint8_t>(nonObjectRoot.begin(), nonObjectRoot.end()));
    cache.load();
    cache.setOffset("missing", 42);
    require(cache.offsetFor("missing").value_or(0) == 42, "cache should recover from non-object root");

    std::filesystem::remove(path);
}

}

int main() {
    try {
        testLrcParser();
        testQrcDecrypterFixture();
        testInflateZlib();
        testConfig();
        testEnglishConfigRoundTrip();
        testLegacyConfigMigration();
        testCache();
        std::cout << "All SMTCLyrics core tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << '\n';
        return 1;
    }
}
