#include "applemusic/AppleMusicInternalProvider.h"
#include "cache/Cache.h"
#include "config/Config.h"
#include "lyrics/AppleMusicLyrics.h"
#include "lyrics/LrcParser.h"
#include "lyrics/LyricRepository.h"
#include "lyrics/TtmlParser.h"
#include "lyrics/QrcDecrypter.h"
#include "ui/ControlWindow.h"
#include "util/Base64.h"
#include "util/Encoding.h"
#include "util/Inflate.h"
#include "util/Path.h"

#include <cctype>
#include <filesystem>
#include <fstream>
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

std::vector<std::uint8_t> toBytes(std::string_view text) {
    return {text.begin(), text.end()};
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
    require(frame.highlightPercent >= 3750 && frame.highlightPercent <= 4250, "krc per-word progress should map to fixed point highlight percent");
    frame = krcParser.frameAt(2'500, 2);
    require(frame.text == L"ABCD\nE" && frame.highlightLine == 0, "krc two-line-down mode should highlight the current first line");
    frame = krcParser.frameAt(6'500, 3);
    require(frame.text == L"ABCD\nE" && frame.highlightLine == 1, "krc two-line-up mode should highlight the current second line");

    smtc::lyrics::LrcParser qrcParser;
    require(qrcParser.parseUtf8("[1000,2000]A(1000,500)B(1500,500)C(2000,1000)[4000,1000]D(4000,1000)\n"), "qrc lyric should parse");
    frame = qrcParser.frameAt(1'750, 1);
    require(frame.text == L"ABC", "qrc word timings should be stripped from displayed text");
    require(frame.highlightPercent >= 4750 && frame.highlightPercent <= 5250, "qrc absolute word timing should map to fixed point highlight percent");
    frame = qrcParser.frameAt(4'500, 3);
    require(frame.text == L"ABC\nD" && frame.highlightLine == 1, "qrc two-line-up mode should highlight the current second line");

    smtc::lyrics::LrcParser relativeQrcParser;
    require(relativeQrcParser.parseUtf8("[10000,1000]A(0,500)B(500,500)\n"), "relative qrc lyric should parse");
    frame = relativeQrcParser.frameAt(10'250, 1);
    require(frame.text == L"AB" && frame.highlightPercent >= 2250 && frame.highlightPercent <= 2750, "qrc relative word timing should map to fixed point highlight percent");

    smtc::lyrics::LrcParser yrcParser;
    require(yrcParser.parseUtf8("[1000,3000](1000,500,0)A(1500,500,0)B[2000,0](2000,1000,0)C\n"), "netease yrc lyric should parse");
    frame = yrcParser.frameAt(1'750, 1);
    require(frame.text == L"ABC", "yrc word timings should be stripped from displayed text");
    require(frame.highlightPercent >= 4750 && frame.highlightPercent <= 5250, "yrc absolute word timing should map to fixed point highlight percent");
}

void testLyricRepositoryLocalPriority() {
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-local-priority";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-local-priority-cache.json";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);

    const auto keyword = std::wstring(L"Song - Artist");
    const auto krcPath = dir / L"Song - Artist.krc";
    const auto lrcPath = dir / L"Song - Artist.lrc";
    require(smtc::util::writeFileBytes(krcPath, toBytes("[1000,2000]<0,1000,0>A<1000,1000,0>B\n")), "should write sample krc");
    require(smtc::util::writeFileBytes(lrcPath, toBytes("[00:01.000]fallback\n")), "should write sample lrc");

    smtc::cache::LyricCache cache(cachePath);
    smtc::lyrics::LyricRepository repository(dir, cache);
    std::filesystem::path matchedPath;
    const auto bytes = repository.loadLocal(keyword, &matchedPath);

    require(!bytes.empty(), "repository should load local lyric");
    require(matchedPath.extension() == L".krc", "repository should prefer local krc over lrc");

    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}

void testLyricRepositoryLocalFallback() {
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-local-fallback";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-local-fallback-cache.json";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);

    const auto keyword = std::wstring(L"Song - Artist");
    const auto krcPath = dir / L"Song - Artist.krc";
    const auto qrcPath = dir / L"Song - Artist.qrc";
    const auto lrcPath = dir / L"Song - Artist.lrc";
    require(smtc::util::writeFileBytes(krcPath, toBytes("not a lyric")), "should write invalid krc");
    require(smtc::util::writeFileBytes(qrcPath, toBytes("still not a lyric")), "should write invalid qrc");
    require(smtc::util::writeFileBytes(lrcPath, toBytes("[00:01.000]fallback\n")), "should write fallback lrc");

    smtc::cache::LyricCache cache(cachePath);
    smtc::lyrics::LyricRepository repository(dir, cache);
    std::filesystem::path matchedPath;
    const auto bytes = repository.loadLocal(keyword, &matchedPath);

    require(!bytes.empty(), "repository should fall back to local lrc");
    require(matchedPath.extension() == L".lrc", "repository should use lrc when krc and qrc fail to parse");

    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}

void testAutomaticLoadDoesNotRecordSource() {
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-auto-no-cache";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-auto-no-cache.json";
    const std::wstring keyword = L"Song - Artist";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);
    require(smtc::util::writeFileBytes(dir / L"Song - Artist.lrc", toBytes("[00:01.00]line\n")),
            "should write automatic-load fixture");

    smtc::cache::LyricCache cache(cachePath);
    cache.load();
    smtc::lyrics::LyricRepository repository(dir, cache);
    const auto result = repository.loadForKeyword(keyword, smtc::config::AppConfig{});
    require(result.source == smtc::lyrics::LyricSource::Local && !result.recordSource,
            "automatic loads must not mark their source for cache persistence");
    require(!cache.sourceFor("Song - Artist").has_value(),
            "automatic loads must not write a source entry to cache.json");
    require(!std::filesystem::exists(cachePath),
            "automatic loads must not create or save cache.json");

    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}

void testSpecificBase64WrappedLocalKrc() {
    const auto runtimeDir = smtc::util::executableDirectory();
    const auto lyricsDir = runtimeDir / L"lyrics";
    const auto path = lyricsDir / L"忘了你，忘了我 - 王杰.krc";
    if (!std::filesystem::is_regular_file(path)) return;

    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-specific-krc-cache.json";
    std::filesystem::remove(cachePath);
    smtc::cache::LyricCache cache(cachePath);
    smtc::lyrics::LyricRepository repository(lyricsDir, cache);

    std::filesystem::path matchedPath;
    const auto bytes = repository.loadLocal(L"忘了你，忘了我 - 王杰", &matchedPath);
    require(!bytes.empty(), "specific local krc should load");
    require(matchedPath.filename() == path.filename(), "specific local krc should match requested file");

    smtc::lyrics::LrcParser parser;
    require(parser.parseBytes(bytes), "specific local krc should parse after repository preprocessing");
    require(!parser.lines().empty(), "specific local krc should produce lyric lines");

    std::filesystem::remove(cachePath);
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

void testTtmlParser() {
    const std::string ttml = R"(<?xml version="1.0" encoding="UTF-8"?>
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata">
  <body><div>
    <p begin="00:00:01.250" end="00:00:03.500" itunes:key="1"><span begin="1.250" end="2.000">逐</span><span begin="2.000" end="3.500">字</span></p>
    <p begin="00:00:04.000" end="00:00:05.000"><span begin="4s" end="4.5s">甲&amp;乙</span></p>
  </div></body>
</tt>)";
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "TTML should parse");
    require(parser.lines().size() == 2, "TTML should produce two lines");
    require(parser.lines()[0].timeMs == 1250 && parser.lines()[0].text == L"逐字", "TTML should preserve line and nested spans");
    require(parser.lines()[0].segments.size() == 2 && parser.lines()[0].segments[1].offsetMs == 750,
            "TTML should preserve millisecond sibling timing");
    require(parser.lines()[1].text == L"甲&乙",
            "TTML should support seconds with suffix and entities");
}

void testTtmlSelfClosingElements() {
    const std::string ttml = std::string(R"self(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata"><head><metadata><ttm:agent type="person" xml:id="v1"/><translations/></metadata></head><body><div><p begin="1.000" end="2.000"><span begin="1.000" end="1.500">逐</span><br/><span begin="1.500" end="2.000">字</span></p></div></body></tt>)self");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "TTML with self-closing metadata and br elements should parse");
    require(parser.lines().size() == 1 && parser.lines()[0].text == L"逐字",
            "self-closing metadata elements must not discard the lyric body");
    require(parser.lines()[0].segments.size() == 2 &&
            parser.lines()[0].segments[1].offsetMs == 500,
            "self-closing br elements must not break sibling word timing");
}

void testTtmlLineOnlyLyrics() {
    const std::string ttml = std::string(R"lines(<tt xmlns="http://www.w3.org/ns/ttml" itunes:timing="Line" xmlns:itunes="http://music.apple.com/lyric-ttml-internal"><body><div><p begin="17.262" end="20.491">粗糙的手</p><p begin="23.956" end="30.911">雖然彈不出 那好的節奏</p></div></body></tt>)lines");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "line-only Apple TTML should parse");
    require(parser.lines().size() == 2 && parser.lines()[0].text == L"粗糙的手",
            "line-only Apple TTML should retain direct p text");
    require(parser.lines()[0].durationMs == 3229, "line-only Apple TTML should preserve line timing");
}
void testTtmlReplacementLanguage() {
    const std::string ttml = std::string(R"replacement(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:itunes="http://music.apple.com/lyric-ttml-internal" itunes:timing="Word"><head><metadata><translations><translation type="replacement" xml:lang="zh-Hans"><text for="L1"><span begin="14.6" end="15.1">是</span><span begin="15.1" end="15.8">寂</span> <span begin="17.9" end="19.6">慢慢占领</span></text></translation></translations></metadata></head><body><div><p begin="14.6" end="21.3" itunes:key="L1"><span begin="14.6" end="15.1">是</span><span begin="15.1" end="15.8">寂</span> <span begin="17.9" end="19.6">慢慢佔領</span></p></div></body></tt>)replacement");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "replacement TTML should parse");
    require(parser.lines().size() == 1 && parser.lines()[0].text == L"是寂 慢慢占领",
            "replacement translation should change displayed text");
    require(smtc::lyrics::TtmlParser::hasSyllableTiming(ttml), "replacement should be detected as word-timed TTML");
    const std::string bg = std::string(R"bg(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="2"><span begin="1" end="1.5">main</span><span ttm:role="x-bg" xmlns:ttm="http://www.w3.org/ns/ttml#metadata"><span begin="1" end="2">background</span></span></p></div></body></tt>)bg");
    smtc::lyrics::TtmlParser bgParser;
    require(bgParser.parseUtf8(bg) && bgParser.lines()[0].text == L"main", "background lyrics should not be shown as the main lyric");
}

void testTtmlReplacementUsesItsOwnTimeline() {
    const std::string ttml = std::string(R"replacement(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:itunes="http://music.apple.com/lyric-ttml-internal" xmlns:ttm="http://www.w3.org/ns/ttml#metadata" itunes:timing="Word"><head><metadata><translations><translation type="replacement" xml:lang="zh-Hans"><text for="L1"><span begin="10.000" end="10.100">在</span><span begin="10.100" end="10.500">无穷无尽</span><span begin="10.500" end="10.700">之</span><span begin="10.700" end="10.900">间</span> <span ttm:role="x-bg"><span begin="10.000" end="10.200">(在)</span></span></text><text for="L2"><span begin="12.000" end="12.200">和</span><span begin="12.200" end="12.400">和</span></text></translation></translations></metadata></head><body><div><p begin="10.000" end="10.900" itunes:key="L1"><span begin="10.000" end="10.100">在</span><span begin="10.100" end="10.500">無窮無盡</span><span begin="10.500" end="10.700">之</span><span begin="10.700" end="10.900">間</span> <span ttm:role="x-bg"><span begin="10.000" end="10.200">(在)</span></span></p><p begin="12.000" end="12.400" itunes:key="L2"><span begin="12.000" end="12.200">和</span><span begin="12.200" end="12.400">和</span></p></div></body></tt>)replacement");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "replacement TTML with simplified/traditional text should parse");
    require(parser.lines().size() == 2, "replacement TTML should produce both lines");

    const auto& first = parser.lines()[0];
    require(first.timeMs == 10'000 && first.text == L"在无穷无尽之间",
            "replacement text and background lyric filtering should match the displayed line");
    require(first.segments.size() == 4,
            "replacement timing must come from the replacement text, not by matching original characters");
    require(smtc::lyrics::hasWordLevelTiming(toBytes(ttml)), "replacement spans should survive parsing as a word-level source");
    require(first.segments[1].offsetMs == 100 && first.segments[1].durationMs == 400,
            "replacement span millisecond timing should be preserved");
    require(first.text.substr(first.segments[1].textStart, first.segments[1].textEnd - first.segments[1].textStart) == L"无穷无尽",
            "replacement span text ranges must point at the translated glyphs");

    const auto& second = parser.lines()[1];
    require(second.segments.size() == 2 && second.segments[0].textStart == 0 && second.segments[1].textStart == 1,
            "repeated characters must not create duplicate or overlapping highlight ranges");
    require(smtc::lyrics::TtmlParser::hasSyllableTiming(ttml),
            "replacement spans should mark TTML as word-timed");
    require(smtc::lyrics::TtmlParser::hasUsableSyllableTiming(ttml),
            "two word-timed replacement lines should be considered usable");
}
void testTtmlEnglishSpanSpacing() {
    const std::string ttml = std::string(R"english(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="3.070" end="6.066" itunes:key="L1"><span begin="3.070" end="3.483">Once</span> <span begin="3.483" end="3.888">upon</span> <span begin="3.888" end="4.026">a</span> <span begin="4.026" end="4.464">time,</span></p></div></body></tt>)english");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "English Apple TTML should parse");
    require(parser.lines().size() == 1 && parser.lines()[0].text == L"Once upon a time,",
            "Apple TTML should retain the literal spaces between timed word spans");
    require(parser.lines()[0].segments.size() == 4,
            "Apple TTML separators should not become independent highlight segments");

    const std::string contiguous = std::string(R"contiguous(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="2"><span begin="1" end="1.5">call</span><span begin="1.5" end="2">ing</span></p></div></body></tt>)contiguous");
    smtc::lyrics::TtmlParser contiguousParser;
    require(contiguousParser.parseUtf8(contiguous) && contiguousParser.lines()[0].text == L"calling",
            "adjacent word spans should not introduce spaces that are absent from the TTML");
}
void testTtmlLineOnlyWithLocalizations() {
    const std::string ttml = std::string(R"line(<tt xmlns="http://www.w3.org/ns/ttml"><head><metadata><translations><translation xml:lang="zh-Hans" type="replacement"><text for="L1">原文</text></translation></translations></metadata></head><body><div><p begin="1" end="2" itunes:key="L1">原文</p></div></body></tt>)line");
    require(!smtc::lyrics::TtmlParser::hasSyllableTiming(ttml),
            "line-only TTML should not be classified as word-timed because its translation contains spans");
    const std::string transliteration = std::string(R"translit(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata"><head><metadata><translations><translation type="transliteration" xml:lang="ja"><text for="L1"><span begin="1" end="2">romaji</span></text></translation></translations></metadata></head><body><div><p begin="1" end="2" itunes:key="L1"><span begin="1" end="1.5">歌</span><span begin="1.5" end="2">詞</span></p><p begin="2" end="3" itunes:key="L2"><span begin="2" end="2.5">歌</span><span begin="2.5" end="3">詞</span></p></div></body></tt>)translit");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(transliteration) && parser.lines()[0].text == L"歌詞" && parser.lines()[0].segments.size() == 2,
            "transliteration text must be discarded while retaining the original word timing");
    const std::string inlineTransliteration = std::string(R"inline(<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata"><body><div><p begin="1" end="2"><span begin="1" end="2">歌詞</span><span ttm:role="x-transliteration"><span begin="1" end="2">kashi</span></span></p></div></body></tt>)inline");
    smtc::lyrics::TtmlParser inlineParser;
    require(inlineParser.parseUtf8(inlineTransliteration) && inlineParser.lines()[0].text == L"歌詞",
            "inline transliteration roles must not be appended to the displayed lyric");
}

void testAppleMusicLanguageSelection() {
    require(std::string(smtc::lyrics::kAppleMusicLyricsLanguages[0]) == "zh-Hans-CN" &&
            std::string(smtc::lyrics::kAppleMusicLyricsLanguages[1]) == "zh-Hant-TW" &&
            std::string(smtc::lyrics::kAppleMusicLyricsLanguages[2]) == "zh-Hant-HK" &&
            smtc::lyrics::kAppleMusicLyricsLanguages[3].empty(),
            "Apple Music should try Simplified Chinese, Taiwan, Hong Kong, then original");
    require(smtc::lyrics::appleMusicLyricQuery("zh-Hans-CN") == "?extend=ttmlLocalizations&l%5Blyrics%5D=zh-Hans-CN&l%5Bscript%5D=zh-Hans",
            "Apple Music should derive zh-Hans script from the simplified Chinese language");
    require(smtc::lyrics::appleMusicLyricQuery("zh-Hant-TW") == "?extend=ttmlLocalizations&l%5Blyrics%5D=zh-Hant-TW&l%5Bscript%5D=zh-Hant",
            "Apple Music should derive zh-Hant script from the Taiwan language variant");
    require(smtc::lyrics::appleMusicLyricQuery("") == "?extend=ttmlLocalizations",
            "Apple Music should omit language and script parameters when requesting the original lyrics");
    const std::string response = std::string(R"apple({"data":[{"attributes":{"ttml":"<tt><body><div><p begin=\"1\" end=\"3\"><span begin=\"1\" end=\"2\">原</span><span begin=\"2\" end=\"3\">文</span></p><p begin=\"3\" end=\"5\"><span begin=\"3\" end=\"4\">歌</span><span begin=\"4\" end=\"5\">词</span></p></div></body></tt>","ttmlLocalizations":{"zh-Hans-CN":"<tt><body><div><p begin=\"1\" end=\"3\"><span begin=\"1\" end=\"2\">简</span><span begin=\"2\" end=\"3\">体</span></p><p begin=\"3\" end=\"5\"><span begin=\"3\" end=\"4\">歌</span><span begin=\"4\" end=\"5\">词</span></p></div></body></tt>","zh-Hant":"<tt><body><div><p begin=\"1\" end=\"3\">繁</p><p begin=\"3\" end=\"5\">体</p></div></body></tt>"}}}]})apple");
    const auto selected = smtc::lyrics::appleMusicTtmlForLanguage(response, "zh-Hans-CN");
    require(!selected.empty(), "Apple Music should select the requested simplified Chinese TTML");
    require(smtc::lyrics::appleMusicTtmlForLanguage(response, "zh-Hant-TW").find("繁") != std::string::npos,
            "Apple Music should keep the Traditional Chinese translation when its timing is line-only");
    require(!smtc::lyrics::appleMusicTtmlForLanguage(response, "").empty(),
            "Apple Music should use original TTML after all Chinese variants are unavailable");
    require(smtc::lyrics::appleMusicTtmlForLanguage(response, "ja-JP").empty(),
            "Apple Music should use original TTML rather than an unrelated localization");
}
void testWordLevelTimingDetection() {
    const std::string lineLrc = "[00:01.00]first line\n[00:02.00]second line\n";
    require(!smtc::lyrics::hasWordLevelTiming(toBytes(lineLrc)), "plain LRC must remain a line-level fallback");
    const std::string wordTtml = R"word(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="3"><span begin="1" end="2">A</span><span begin="2" end="3">B</span></p><p begin="3" end="5"><span begin="3" end="4">C</span><span begin="4" end="5">D</span></p></div></body></tt>)word";
    require(smtc::lyrics::hasWordLevelTiming(toBytes(wordTtml)), "two lines of timed spans should be word-level");
}


void testLineOnlyLyricsDoNotDrawSweep() {
    const std::string ttml = R"line(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="3"><span begin="1" end="3">Line one</span></p><p begin="3" end="5"><span begin="3" end="5">Line two</span></p></div></body></tt>)line";
    smtc::lyrics::LrcParser parser;
    require(parser.parseUtf8(ttml), "whole-line TTML should parse");
    require(!parser.frameAt(2000, 1).drawWordHighlight, "line-only lyrics must not draw a sweep");
    const std::string word = R"word(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="3"><span begin="1" end="2">A</span><span begin="2" end="3">B</span></p><p begin="3" end="5"><span begin="3" end="4">C</span><span begin="4" end="5">D</span></p></div></body></tt>)word";
    smtc::lyrics::LrcParser wordParser;
    require(wordParser.parseUtf8(word) && wordParser.frameAt(1500, 1).drawWordHighlight, "word-timed lyrics should retain per-glyph highlighting");
}


void testAppleMusicRepositoryFallback() {
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-fallback";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-fallback-cache.json";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);
    smtc::config::AppConfig config;
    config.sourcePriority = {5, 2, 1, 3, 4};
    smtc::cache::LyricCache cache(cachePath);
    smtc::lyrics::LyricRepository repository(dir, cache);
    const auto result = repository.loadForKeyword(L"Keyword", config, true);
    require(result.source != smtc::lyrics::LyricSource::AppleMusic, "missing Apple Music credentials should fall back");
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}


void testTtmlTimingClassification() {
    const std::string headOnly = std::string(R"head(<tt xmlns="http://www.w3.org/ns/ttml"><head><metadata><translations><translation type="replacement" xml:lang="zh-Hans"><text for="L1"><span begin="1" end="2">简体</span></text></translation></translations></metadata></head><body><div><p begin="1" end="2" itunes:key="L1">原文</p></div></body></tt>)head");
    require(!smtc::lyrics::TtmlParser::hasSyllableTiming(headOnly),
            "head-only translation spans must not classify a line-only TTML as word-timed");
    const std::string bodyTimed = std::string(R"body(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="2"><span begin="1" end="1.5">逐</span><span begin="1.5" end="2">字</span></p></div></body></tt>)body");
    require(smtc::lyrics::TtmlParser::hasSyllableTiming(bodyTimed), "body word spans should mark TTML as word-timed");
}

void testAppleMusicLineOnlyFallback() {
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-line-fallback";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-line-fallback-cache.json";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);
    smtc::config::AppConfig config;
    config.sourcePriority = {5};
    smtc::cache::LyricCache cache(cachePath);
    smtc::lyrics::LyricRepository repository(dir, cache);
    const std::string appleLine = std::string(R"line(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1" end="2">Apple line</p></div></body></tt>)line");
    require(!smtc::lyrics::TtmlParser::hasSyllableTiming(appleLine),
            "line-only Apple TTML should be marked as a fallback candidate");
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}

void testLineOnlyAppleNeverOverridesConfiguredWordSource() {
    // A cached line-only Apple result must not prevent the repository from trying later word sources.
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-cache-priority";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-cache-priority.json";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);
    smtc::cache::LyricCache cache(cachePath);
    cache.load();
    cache.setSource("Song - Artist", 5);
    cache.save();
    smtc::lyrics::LyricRepository repository(dir, cache);
    smtc::config::AppConfig config;
    config.sourcePriority = {5, 1, 2, 3, 4};
    const auto result = repository.loadForKeyword(L"Song - Artist", config);
    require(result.source != smtc::lyrics::LyricSource::AppleMusic,
            "Cached line-only Apple TTML must not override a lower-priority word source");
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}

void testManualSourceSwitchSkipsLocalLyrics() {
    const auto dir = std::filesystem::temp_directory_path() / L"smtclyrics-manual-source-switch";
    const auto cachePath = std::filesystem::temp_directory_path() / L"smtclyrics-manual-source-switch.json";
    const auto lyricsPath = dir / L"Song - Artist.lrc";
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
    std::filesystem::create_directories(dir);
    require(smtc::util::writeFileBytes(lyricsPath, toBytes("[00:01.00]first line\n[00:02.00]second line\n")),
            "should write line-only local lyric fixture");
    smtc::cache::LyricCache cache(cachePath);
    smtc::lyrics::LyricRepository repository(dir, cache);
    smtc::config::AppConfig config;
    config.sourcePriority = {5, 1};
    const auto result = repository.loadNextOnlineSource(L"Song - Artist", config);
    require(result.source == smtc::lyrics::LyricSource::Local && result.lrcBytes.empty(),
            "manual source switching must bypass local lyrics and online sources without network access");
    std::filesystem::remove_all(dir);
    std::filesystem::remove(cachePath);
}

void testTtmlPreciseMillisecondTiming() {
    const std::string ttml = std::string(R"ms(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="1.000" end="2.000"><span begin="1.000" end="1.001">A</span><span begin="1.001" end="2.000">B</span></p></div></body></tt>)ms");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml) && parser.lines()[0].segments[0].durationMs == 1,
            "TTML decimal-second timestamps should retain millisecond precision");
}

void testTtmlSungGapAndScaledHighlight() {
    const std::string ttml = std::string(R"gap(<tt xmlns="http://www.w3.org/ns/ttml"><body><div>
      <p begin="10.000" end="14.000"><span begin="10.000" end="10.200">一</span><span begin="10.200" end="10.400">二</span> <span begin="13.000" end="13.500">三</span></p>
      <p begin="14.000" end="15.000"><span begin="14.000" end="14.100">A</span></p>
    </div></body></tt>)gap");
    smtc::lyrics::TtmlParser ttmlParser;
    require(ttmlParser.parseUtf8(ttml), "TTML with a sung gap should parse");
    const auto& segments = ttmlParser.lines()[0].segments;
    require(segments.size() == 3 && segments[2].offsetMs == 3000,
            "TTML segment dur should not extend into a subsequent unsung phrase");
    smtc::lyrics::LrcParser gapRenderer;
    require(gapRenderer.parseUtf8(ttml), "gap TTML should be renderable");
    const auto gapStart = gapRenderer.frameAt(10100, 1);
    require(gapStart.highlightPercent >= 0 && gapStart.highlightPercent < 1500,
            "first phrase highlight should only cover its own glyphs");
    const auto gapFrame = gapRenderer.frameAt(12000, 1);
    require(gapFrame.highlightPercent >= 4000 && gapFrame.highlightPercent <= 10000,
            "unsung gaps must never light a later lyric phrase early");
}

void testAppleMusicLineTimingIsNotPresentedAsWordTiming() {
    const std::string ttml = std::string(R"line(<tt xmlns="http://www.w3.org/ns/ttml" itunes:timing="Word" xmlns:itunes="http://music.apple.com/lyric-ttml-internal"><body><div><p begin="1.000" end="3.000"><span begin="1.000" end="3.000">整句字幕</span></p><p begin="3.000" end="5.000"><span begin="3.000" end="5.000">第二句字幕</span></p></div></body></tt>)line");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "nominal Word TTML with one span per line should still display");
    require(parser.lines().size() == 2, "whole-line Apple lyrics should retain both lines");
    require(parser.lines()[0].segments.empty(), "a whole-line span must not be exposed as word-level timing");
    require(!smtc::lyrics::hasWordLevelTiming({ttml.begin(), ttml.end()}),
            "a nominal Word response with whole-line spans must trigger lyric-source fallback");
}

void testTtmlInheritedTimingAndDocumentOrder() {
    const std::string ttml = R"ord(<tt xmlns="http://www.w3.org/ns/ttml"><body><div begin="1.000"><p begin="1.000" end="2.000"><span begin="1.000" end="1.500">A</span><span begin="1.500" end="2.000">B</span></p><p begin="2.000" end="3.000">Line</p></div></body></tt>)ord";
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "TTML inherited timing should parse");
    require(parser.lines().size() == 2 && parser.lines()[0].timeMs == 1000 && parser.lines()[1].timeMs == 2000,
            "TTML should preserve document order and inherited paragraph timing");
}

void testTtmlSubtitleWithoutSpansKeepsLineTiming() {
    const std::string ttml = std::string(R"subtitle(<tt xmlns="http://www.w3.org/ns/ttml" itunes:timing="Word"><head><metadata><translations><translation type="subtitle" xml:lang="zh-Hans"><text for="L1">你好</text><text for="L2">再见</text></translation></translations></metadata></head><body><div><p begin="1.250" end="2.750" itunes:key="L1"><span begin="1.250" end="2.000">Hello</span></p><p begin="3.000" end="4.000" itunes:key="L2"><span begin="3.000" end="4.000">Bye</span></p></div></body></tt>)subtitle");
    smtc::lyrics::TtmlParser parser;
    require(parser.parseUtf8(ttml), "subtitle translation without spans should parse");
    require(parser.lines().size() == 2 && parser.lines()[0].text == L"你好",
            "subtitle translations should be shown instead of untranslated source text");
    require(parser.lines()[0].durationMs == 1500,
            "unspanned subtitle should inherit its paragraph timing");
    require(!smtc::lyrics::hasWordLevelTiming(toBytes(ttml)),
            "subtitle-only translations must remain a line-level fallback");
    const std::string both = std::string(R"both(<tt xmlns="http://www.w3.org/ns/ttml"><head><metadata><translations><translation type="subtitle" xml:lang="zh-Hans"><text for="L1">整句副标题</text></translation><translation type="replacement" xml:lang="zh-Hans"><text for="L1"><span begin="1.250" end="1.600">你</span><span begin="1.600" end="2.000">好</span></text></translation></translations></metadata></head><body><div><p begin="1.250" end="2.750" itunes:key="L1"><span begin="1.250" end="1.600">Hello</span><span begin="1.600" end="2.000">there</span></p><p begin="3.000" end="4.000" itunes:key="L2"><span begin="3.000" end="3.500">Bye</span><span begin="3.500" end="4.000">now</span></p></div></body></tt>)both");
    smtc::lyrics::TtmlParser bothParser;
    require(bothParser.parseUtf8(both) && bothParser.lines()[0].text == L"你好" && bothParser.lines()[0].segments.size() == 2,
            "replacement text should win over a subtitle localization for the same line");
}
void testAppleMusicParserAndVersionMatching() {
    const std::string json = std::string(R"aapp({
      "data":[
        {"id":"123","attributes":{"name":"Song","artistName":"Artist","durationInMillis":180000,"isrc":"ISRC1"}},
        {"id":"456","attributes":{"name":"Song (Live)","artistName":"Artist","durationInMillis":180000,"isrc":"ISRC2"}}
      ]
    })aapp");
    const auto candidates = smtc::lyrics::parseAppleMusicTrackCandidates(json);
    require(candidates.size() == 2 && candidates[0].catalogId == "123", "Apple Music response should map catalog candidates");
    require(smtc::lyrics::appleMusicRecordingMatches("Song", "Artist", json, 180000), "Apple Music should accept a matching recording");
    require(!smtc::lyrics::appleMusicRecordingMatches("Song", "Artist", std::string(R"live({"data":[{"id":"456","attributes":{"name":"Song (Live)","artistName":"Artist","durationInMillis":180000}}]})live"), 180000),
            "Apple Music should reject a live version unless requested");
    require(!smtc::lyrics::appleMusicRecordingMatches("Song", "Wrong Artist", std::string(R"single({"data":[{"id":"123","attributes":{"name":"Song","artistName":"Artist","durationInMillis":180000}}]})single"), 180000), "Apple Music should reject mismatched artist");
    require(smtc::lyrics::normalizedCatalogId(" 123456 ") && !smtc::lyrics::normalizedCatalogId("12a"), "Apple Music catalog IDs should be numeric");
    smtc::lyrics::AppleMusicTokenSettings settings;
    const auto result = smtc::lyrics::AppleMusicLyricsClient{}.fetch("Song - Artist", {}, settings);
    require(result.status == smtc::lyrics::AppleMusicStatus::AuthRequired && result.ttml.empty(),
            "Apple Music should require user-provided credentials, not search anonymously");
}

void testAppleMusicAccountStorefront() {
    const std::string response = std::string(R"storefront({"data":[{"id":"tr","type":"storefronts"}]})storefront");
    require(smtc::lyrics::parseAppleMusicAccountStorefront(response) == std::optional<std::string>("tr"),
            "Apple Music account storefront should be read from /me/storefront");
    require(!smtc::lyrics::parseAppleMusicAccountStorefront(R"({"data":[]})"),
            "Apple Music should reject a missing account storefront");
}
void testAppleMusicStorefrontIsRuntimeOnly() {
    const auto path = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-storefront-runtime.ini";
    std::filesystem::remove(path);
    smtc::config::AppConfig initial;
    const std::wstring emptyStorefront;
    initial.appleMusicStorefront = emptyStorefront;
    smtc::config::ConfigStore(path).save(initial);
    const auto config = smtc::config::ConfigStore(path).load();
    require(config.appleMusicStorefront.empty(), "fixture should start without a persisted storefront");
    require(smtc::lyrics::AppleMusicLyricsClient::validStorefront("") &&
            smtc::lyrics::AppleMusicLyricsClient::validStorefront("tr") &&
            smtc::lyrics::AppleMusicLyricsClient::validStorefront("usa"),
            "empty and two/three-letter storefronts should be valid");
    require(!smtc::lyrics::AppleMusicLyricsClient::validStorefront("tr1") &&
            !smtc::lyrics::AppleMusicLyricsClient::validStorefront("toolong"),
            "invalid storefront must not overwrite the user's typed value");
    smtc::config::AppConfig saved;
    saved.appleMusicStorefront = L"jp";
    smtc::config::ConfigStore(path).save(saved);
    require(smtc::config::ConfigStore(path).load().appleMusicStorefront == L"jp",
            "manual storefront should still be persisted only by a deliberate save");
    std::filesystem::remove(path);
}

void testAppleMusicBridgeDynamicIidRequest() {
    smtc::applemusic::bridge::SnapshotRequest request;
    constexpr GUID empty{};
    require(IsEqualGUID(request.playerIid, empty), "new snapshot request should start without dynamic IID");
    require(sizeof(request) == 112, "snapshot request should keep a stable six-pipe-protocol layout");

    request.playerIid = {0xF807B158, 0xB2E7, 0x5CF7, {0xA5, 0x12, 0x8B, 0x0B, 0xE7, 0x67, 0xDF, 0x1B}};
    request.nowPlayingItemIid = {0x2DC5F65C, 0x28F6, 0x58AA, {0x87, 0xCF, 0xE4, 0xA7, 0xDF, 0x9B, 0x9A, 0x4B}};
    require(!IsEqualGUID(request.playerIid, empty), "dynamic player IID should occupy the request field");
    require(!IsEqualGUID(request.nowPlayingItemIid, empty), "dynamic now-playing IID should occupy the request field");
}

void testAppleMusicInternalStateMapping() {
    smtc::applemusic::AppleMusicInternalSnapshot snapshot;
    snapshot.name = L"Song";
    snapshot.artist = L"Artist";
    snapshot.album = L"Album";
    snapshot.currentPosition100ns = 42LL * 10000000;
    snapshot.duration100ns = 180LL * 10000000;
    snapshot.playing = true;

    auto state = smtc::applemusic::mapAppleMusicSnapshotToMediaState(snapshot);
    require(state.valid, "Apple Music internal mapping should be valid with a title");
    require(state.title == L"Song" && state.artist == L"Artist" && state.album == L"Album", "Apple Music internal metadata should map");
    require(state.positionMs == 42000 && state.durationMs == 180000, "Apple Music TimeSpan values should map to milliseconds");
    require(state.playing, "Apple Music playing flag should map");

    snapshot.currentPosition100ns = 200LL * 10000000;
    state = smtc::applemusic::mapAppleMusicSnapshotToMediaState(snapshot);
    require(state.positionMs == 180000, "Apple Music position should clamp to duration");

    snapshot.name.clear();
    snapshot.subtitle = L"Fallback";
    state = smtc::applemusic::mapAppleMusicSnapshotToMediaState(snapshot);
    require(state.valid && state.title == L"Fallback", "Apple Music mapping should fall back to subtitle");

    snapshot.subtitle.clear();
    state = smtc::applemusic::mapAppleMusicSnapshotToMediaState(snapshot);
    require(!state.valid, "Apple Music mapping should be invalid without title text");

    snapshot.name = L"Song";
    snapshot.subtitle = L"Artist From Subtitle \u2014 Album From Subtitle";
    snapshot.artist.clear();
    snapshot.album.clear();
    state = smtc::applemusic::mapAppleMusicSnapshotToMediaState(snapshot);
    require(state.artist == L"Artist From Subtitle" && state.album == L"Album From Subtitle", "Apple Music subtitle should fill missing artist and album");
}

void testAppleMusicStartupBridgeConnectionPolicy() {
    require(smtc::playback::bridgePipeMissing(ERROR_FILE_NOT_FOUND),
            "ERROR_FILE_NOT_FOUND (0x80070002) should trigger the bridge load path");
    require(!smtc::playback::bridgePipeMissing(ERROR_ACCESS_DENIED),
            "a permission error must not be treated as a missing bridge");
}

void testConfig() {
    smtc::config::ConfigStore store(smtc::util::executableDirectory() / L"config.ini");
    const auto config = store.load();
    require(config.font.size > 0, "font size should load");
    require(config.displayMode >= 1 && config.displayMode <= 3, "display mode should be valid");
    require(config.sourcePriority.size() == 5, "source priority should contain five sources");
}
void testAppleMusicConfigCredentials() {
    const auto path = std::filesystem::temp_directory_path() / L"smtclyrics-applemusic-config.ini";
    std::filesystem::remove(path);
    smtc::config::AppConfig config;
    config.appleMusicDeveloperToken = L"Bearer eyJ-user+developer/token==";
    config.appleMusicMediaUserToken = L"0.Avks-user+token/with=padding==";
    config.appleMusicStorefront = L"us";
    config.sourcePriority = {5, 2, 1, 3, 4};
    smtc::config::ConfigStore store(path);
    store.save(config);
    const auto loaded = store.load();
    require(loaded.appleMusicDeveloperToken == config.appleMusicDeveloperToken, "Apple Music developer Token should round-trip");
    require(loaded.appleMusicMediaUserToken == config.appleMusicMediaUserToken, "Apple Music Media-User-Token should round-trip");
    require(loaded.appleMusicStorefront == L"us", "Apple Music storefront should round-trip");
    require(readIniString(path, L"AppleMusic", L"developerToken") == config.appleMusicDeveloperToken,
            "Apple Music developer Token should be stored in local config.ini");
    require(readIniString(path, L"AppleMusic", L"mediaUserToken") == config.appleMusicMediaUserToken,
            "Apple Music Media-User-Token should be stored in local config.ini");
    require(loaded.sourcePriority == std::vector<int>({5, 2, 1, 3, 4}), "five-source priority should preserve Apple Music position");
    std::filesystem::remove(path);
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
    config.sourcePriority = {5, 4, 3, 2, 1};
    config.smtcMode = 3;
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
    require(loaded.sourcePriority == std::vector<int>({5, 4, 3, 2, 1}), "source priority should save as one-based indexes");
    require(loaded.smtcMode == 3, "Apple Music internal mode should round-trip");
    require(loaded.smtcPollIntervalMs == 500, "SMTC poll interval should clamp to the safe minimum");
    require(loaded.window.hasPosition && loaded.window.left == 120 && loaded.window.top == 80, "window position should round-trip");
    require(readIniString(path, L"Lyrics", L"normalColor1") == L"#0C2238", "colors should be written as #RRGGBB");
    require(readIniString(path, L"SMTC", L"pollIntervalMs") == L"500", "SMTC poll interval should be written with the safe minimum");
    require(readIniString(path, L"SMTC", L"mode") == L"3", "Apple Music internal mode should be written");
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
    require(loaded.sourcePriority == std::vector<int>({1, 2, 3, 4, 5}), "legacy zero-based source priority should migrate");
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
    cache.setOffset("Jordan Chan 算你狠", -125);
    cache.save();
    cache.load();
    const auto source = cache.sourceFor("Jordan Chan 算你狠");
    require(source && *source == 2, "cache should persist source index");
    const auto offset = cache.offsetFor("Jordan Chan 算你狠");
    require(offset && *offset == -125, "cache should persist song offset");

    const auto savedBytes = smtc::util::readFileBytes(path);
    const std::string savedText(savedBytes.begin(), savedBytes.end());
    const auto key = smtc::util::base64Encode("Jordan Chan 算你狠");
    const auto firstKey = savedText.find(key);
    require(firstKey != std::string::npos, "cache should store song under a base64 key");
    require(savedText.find(key, firstKey + 1) == std::string::npos, "cache should store one key per song");
    require(savedText.find("\"songs\"") != std::string::npos, "cache should store per-song objects");
    require(savedText.find("\"source\":{") == std::string::npos, "cache should not write legacy source map");
    require(savedText.find("\"offset\":{") == std::string::npos, "cache should not write legacy offset map");

    cache.removeSource("Jordan Chan 算你狠");
    require(!cache.sourceFor("Jordan Chan 算你狠").has_value(), "cache should remove only source field");
    require(cache.offsetFor("Jordan Chan 算你狠").value_or(0) == -125, "cache should keep offset when source is removed");

    smtc::util::writeFileBytes(path, std::vector<std::uint8_t>{'{', '"', 'o', 'f', 'f', 's', 'e', 't', '"', ':', '{', '}', '}'});
    cache.load();
    require(!cache.sourceFor("missing").has_value(), "cache without songs object should not assert");
    cache.setSource("missing", 3);
    require(cache.sourceFor("missing").value_or(0) == 3, "cache should recreate missing songs object");

    const std::string malformedShape = R"({"songs":[],"source":{"legacy":1},"offset":{"legacy":2}})";
    smtc::util::writeFileBytes(path, std::vector<std::uint8_t>(malformedShape.begin(), malformedShape.end()));
    cache.load();
    require(!cache.sourceFor("missing").has_value(), "cache with malformed songs should not assert");
    require(!cache.offsetFor("missing").has_value(), "cache with malformed songs should not return offset");

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
        testLyricRepositoryLocalPriority();
        testLyricRepositoryLocalFallback();
        testAutomaticLoadDoesNotRecordSource();
        testSpecificBase64WrappedLocalKrc();
        testQrcDecrypterFixture();
        testInflateZlib();
        testConfig();
        testTtmlParser();
        testTtmlSelfClosingElements();
        testTtmlLineOnlyLyrics();
        testTtmlReplacementLanguage();
        testTtmlEnglishSpanSpacing();
        testTtmlLineOnlyWithLocalizations();
        testAppleMusicLanguageSelection();
        testAppleMusicParserAndVersionMatching();
        testAppleMusicAccountStorefront();
        testAppleMusicStorefrontIsRuntimeOnly();
        testAppleMusicConfigCredentials();
        testAppleMusicRepositoryFallback();
        testWordLevelTimingDetection();
        testLineOnlyLyricsDoNotDrawSweep();
        testLineOnlyAppleNeverOverridesConfiguredWordSource();
        testManualSourceSwitchSkipsLocalLyrics();
        testAppleMusicLineOnlyFallback();
        testTtmlPreciseMillisecondTiming();
        testTtmlSungGapAndScaledHighlight();
        testAppleMusicLineTimingIsNotPresentedAsWordTiming();
        testTtmlInheritedTimingAndDocumentOrder();
        testTtmlTimingClassification();
        testTtmlSubtitleWithoutSpansKeepsLineTiming();
        testTtmlReplacementUsesItsOwnTimeline();
        testAppleMusicBridgeDynamicIidRequest();
        testAppleMusicInternalStateMapping();
        testAppleMusicStartupBridgeConnectionPolicy();
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
