#include "lyrics/TtmlParser.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smtc::lyrics {
namespace {

constexpr std::string_view kTextNodeName = "#text";

struct XmlElement {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
    // Text nodes are kept as children in document order. This is essential for
    // Apple TTML, where translated text and timing spans are interleaved.
    std::vector<XmlElement> children;
};

std::string lowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string localName(std::string_view name) {
    const auto colon = name.rfind(':');
    return lowerAscii(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

std::string decodeEntities(std::string_view value) {
    return util::htmlDecodeUtf8(value);
}

bool skipWhitespace(std::string_view xml, std::size_t& index) {
    while (index < xml.size() && std::isspace(static_cast<unsigned char>(xml[index])) != 0) ++index;
    return index < xml.size();
}

std::optional<std::string> parseName(std::string_view xml, std::size_t& index) {
    const auto begin = index;
    while (index < xml.size()) {
        const char ch = xml[index];
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_' && ch != '-' && ch != ':' && ch != '.') break;
        ++index;
    }
    if (begin == index) return std::nullopt;
    return std::string(xml.substr(begin, index - begin));
}

bool parseAttributes(std::string_view xml, std::size_t& index, std::map<std::string, std::string>& attrs) {
    for (;;) {
        if (!skipWhitespace(xml, index)) return false;
        if (xml[index] == '>' || (xml[index] == '/' && index + 1 < xml.size() && xml[index + 1] == '>')) return true;
        const auto name = parseName(xml, index);
        if (!name) return false;
        skipWhitespace(xml, index);
        if (index >= xml.size() || xml[index] != '=') return false;
        ++index;
        skipWhitespace(xml, index);
        if (index >= xml.size() || (xml[index] != '"' && xml[index] != '\'')) return false;
        const char quote = xml[index++];
        const auto end = xml.find(quote, index);
        if (end == std::string_view::npos) return false;
        attrs[lowerAscii(*name)] = decodeEntities(xml.substr(index, end - index));
        index = end + 1;
    }
}

std::size_t appendTextNode(XmlElement& parent, std::string_view encoded) {
    XmlElement textNode;
    textNode.name = std::string(kTextNodeName);
    textNode.text = decodeEntities(encoded);
    parent.children.push_back(std::move(textNode));
    return parent.children.size() - 1;
}

bool parseXmlTree(std::string_view xml, XmlElement& root) {
    std::vector<XmlElement*> stack;
    std::size_t index = 0;
    bool foundRoot = false;
    while (index < xml.size()) {
        const auto open = xml.find('<', index);
        const auto textEnd = open == std::string_view::npos ? xml.size() : open;
        if (textEnd > index && !stack.empty()) {
            appendTextNode(*stack.back(), xml.substr(index, textEnd - index));
        }
        if (open == std::string_view::npos) break;
        index = open;

        if (xml.compare(index, 4, "<!--") == 0) {
            const auto close = xml.find("-->", index + 4);
            if (close == std::string_view::npos) return false;
            index = close + 3;
            continue;
        }
        if (xml.compare(index, 9, "<![CDATA[") == 0) {
            const auto close = xml.find("]]>", index + 9);
            if (close == std::string_view::npos || stack.empty()) return false;
            appendTextNode(*stack.back(), xml.substr(index + 9, close - index - 9));
            index = close + 3;
            continue;
        }
        if (xml.compare(index, 2, "<?") == 0) {
            const auto close = xml.find("?>", index + 2);
            if (close == std::string_view::npos) return false;
            index = close + 2;
            continue;
        }
        if (xml.compare(index, 2, "<!") == 0) {
            const auto close = xml.find('>', index + 2);
            if (close == std::string_view::npos) return false;
            index = close + 1;
            continue;
        }
        if (xml.compare(index, 2, "</") == 0) {
            index += 2;
            const auto name = parseName(xml, index);
            if (!name || stack.empty() || localName(*name) != localName(stack.back()->name)) return false;
            skipWhitespace(xml, index);
            if (index >= xml.size() || xml[index] != '>') return false;
            ++index;
            stack.pop_back();
            continue;
        }

        ++index;
        const auto name = parseName(xml, index);
        if (!name) return false;
        XmlElement element;
        element.name = *name;
        if (!parseAttributes(xml, index, element.attributes)) return false;
        const bool selfClosing = index < xml.size() && xml[index] == '/';
        if (selfClosing) {
            if (index + 1 >= xml.size() || xml[index + 1] != '>') return false;
            index += 2;
        } else {
            if (index >= xml.size() || xml[index] != '>') return false;
            ++index;
        }
        if (!foundRoot) {
            root = std::move(element);
            foundRoot = true;
            if (!selfClosing) stack.push_back(&root);
        } else if (stack.empty()) {
            return false;
        } else {
            auto& parent = *stack.back();
            parent.children.push_back(std::move(element));
            if (!selfClosing) stack.push_back(&parent.children.back());
        }
    }
    return foundRoot && stack.empty();
}

std::string attributeValue(const XmlElement& element, std::string_view name) {
    const auto value = element.attributes.find(lowerAscii(name));
    return value == element.attributes.end() ? std::string{} : value->second;
}

bool hasAttribute(const XmlElement& element, std::string_view name) {
    return element.attributes.find(lowerAscii(name)) != element.attributes.end();
}

std::string roleOf(const XmlElement& element) {
    auto role = attributeValue(element, "ttm:role");
    if (role.empty()) role = attributeValue(element, "role");
    return lowerAscii(role);
}

bool isAuxiliaryLyrics(const XmlElement& element) {
    const auto role = roleOf(element);
    // Apple marks background vocals with ttm:role="x-bg" and may use other
    // x-* metadata roles. None of them belong in the primary lyric line.
    if (role.starts_with("x-") || role.find("translit") != std::string::npos ||
        role.find("pronunciation") != std::string::npos || role.find("roman") != std::string::npos) {
        return true;
    }
    const auto name = localName(element.name);
    return name == "transliteration" || name == "transcription" || name == "pronunciation";
}

std::optional<double> parseNumber(std::string_view value) {
    const auto text = std::string(util::trimUtf8(value));
    if (text.empty()) return std::nullopt;
    char* stop = nullptr;
    const double number = std::strtod(text.c_str(), &stop);
    if (!stop || *stop != '\0' || !std::isfinite(number) || number < 0) return std::nullopt;
    return number;
}

std::optional<std::int64_t> parseTimeMs(std::string_view raw) {
    const auto trimmed = util::trimUtf8(raw);
    std::string_view value(trimmed);
    if (value.empty()) return std::nullopt;

    double multiplier = 1000.0;
    if (value.ends_with("ms")) {
        value.remove_suffix(2);
        multiplier = 1.0;
    } else if (!value.empty() && value.back() == 'h') {
        value.remove_suffix(1);
        multiplier = 3'600'000.0;
    } else if (!value.empty() && value.back() == 'm') {
        value.remove_suffix(1);
        multiplier = 60'000.0;
    } else if (!value.empty() && value.back() == 's') {
        value.remove_suffix(1);
    }

    double total = 0.0;
    std::size_t cursor = 0;
    while (cursor <= value.size()) {
        const auto end = value.find(':', cursor);
        const auto token = value.substr(cursor, end == std::string_view::npos ? value.size() - cursor : end - cursor);
        const auto number = parseNumber(token);
        if (!number) return std::nullopt;
        total = total * 60.0 + *number;
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return static_cast<std::int64_t>(std::llround(total * multiplier));
}

struct Timing {
    bool valid = false;
    std::int64_t beginMs = 0;
    std::int64_t endMs = 0;
};

bool hasExplicitTiming(const XmlElement& element) {
    return hasAttribute(element, "begin") || hasAttribute(element, "end") || hasAttribute(element, "dur");
}

Timing timingFor(const XmlElement& element, const Timing& inherited, std::int64_t lineEndMs) {
    Timing result = inherited;
    const auto begin = parseTimeMs(attributeValue(element, "begin"));
    const auto end = parseTimeMs(attributeValue(element, "end"));
    const auto duration = parseTimeMs(attributeValue(element, "dur"));

    if (begin) {
        result.beginMs = *begin;
        result.valid = true;
    }
    if (end) {
        result.endMs = *end;
        result.valid = true;
    } else if (duration) {
        result.endMs = result.beginMs + *duration;
        result.valid = true;
    } else if (begin) {
        result.endMs = inherited.valid && inherited.endMs > result.beginMs ? inherited.endMs : lineEndMs;
        result.valid = true;
    }
    if (!result.valid) return result;
    if (result.endMs <= result.beginMs) result.endMs = result.beginMs + 1;
    return result;
}

bool hasVisibleText(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return std::iswspace(ch) == 0; });
}

void appendText(LrcLine& line, std::string_view utf8) {
    const auto wide = util::utf8ToWide(utf8);
    line.text.append(wide.begin(), wide.end());
}

struct ReplacementText {
    const XmlElement* element = nullptr;
    bool subtitle = false;
};

void collectContent(const XmlElement& element, const Timing& inherited, std::int64_t lineStartMs,
                    std::int64_t lineEndMs, LrcLine& line, bool wholeLineTiming, bool& hasWordTiming) {
    for (const auto& child : element.children) {
        if (child.name == kTextNodeName) {
            if (child.text.empty()) continue;
            const auto textStart = line.text.size();
            appendText(line, child.text);
            const auto textEnd = line.text.size();
            const auto timing = wholeLineTiming ? Timing{true, lineStartMs, lineEndMs} : inherited;
            if (timing.valid && textEnd > textStart &&
                hasVisibleText(std::wstring_view(line.text).substr(textStart, textEnd - textStart))) {
                const auto begin = std::max(lineStartMs, timing.beginMs);
                const auto end = std::max(begin + 1, timing.endMs);
                line.segments.push_back({.offsetMs = begin - lineStartMs,
                                         .durationMs = end - begin,
                                         .textStart = textStart,
                                         .textEnd = textEnd,
                                         .beginMs = begin,
                                         .endMs = end});
            }
            continue;
        }
        if (isAuxiliaryLyrics(child)) continue;
        if (localName(child.name) == "br") continue;

        if (!wholeLineTiming && hasExplicitTiming(child)) hasWordTiming = true;
        const auto childTiming = wholeLineTiming
            ? Timing{true, lineStartMs, lineEndMs}
            : timingFor(child, inherited, lineEndMs);
        collectContent(child, childTiming, lineStartMs, lineEndMs, line, wholeLineTiming, hasWordTiming);
    }
}

bool hasExplicitDescendantTiming(const XmlElement& element) {
    for (const auto& child : element.children) {
        if (child.name == kTextNodeName || isAuxiliaryLyrics(child)) continue;
        if (hasExplicitTiming(child) || hasExplicitDescendantTiming(child)) return true;
    }
    return false;
}
std::string localizedLanguage(const XmlElement& element) {
    return lowerAscii(attributeValue(element, "xml:lang"));
}

void collectReplacementTexts(const XmlElement& element, std::map<std::string, ReplacementText>& replacements,
                             std::string_view language = {}) {
    if (localName(element.name) == "translations") {
        for (const auto& translation : element.children) {
            if (translation.name == kTextNodeName || localName(translation.name) != "translation") continue;
            if (!language.empty()) {
                const auto actual = localizedLanguage(translation).empty()
                    ? localizedLanguage(element) : localizedLanguage(translation);
                const auto requested = lowerAscii(language);
                const auto primary = requested.substr(0, requested.find('-'));
                const auto isChinese = actual == "zh-hans" || actual == "zh-hant";
                if (actual != requested && actual != primary && !(isChinese && requested.starts_with("zh"))) continue;
            }
            const auto type = lowerAscii(attributeValue(translation, "type"));
            if (type != "replacement" && type != "subtitle") continue;
            for (const auto& text : translation.children) {
                if (text.name == kTextNodeName || localName(text.name) != "text") continue;
                const auto key = attributeValue(text, "for");
                if (key.empty()) continue;
                ReplacementText candidate{&text, type == "subtitle"};
                const auto [existing, inserted] = replacements.try_emplace(key, candidate);
                // Prefer a word-timed replacement over a subtitle that may only
                // contain a whole translated sentence without its own spans.
                if (!inserted && existing->second.subtitle && !candidate.subtitle) {
                    existing->second = candidate;
                }
            }
        }
        return;
    }
    for (const auto& child : element.children) {
        if (child.name != kTextNodeName) collectReplacementTexts(child, replacements, language);
    }
}

const XmlElement* findElement(const XmlElement& element, std::string_view wantedLocalName) {
    if (localName(element.name) == lowerAscii(wantedLocalName)) return &element;
    for (const auto& child : element.children) {
        if (child.name == kTextNodeName) continue;
        if (const auto* found = findElement(child, wantedLocalName)) return found;
    }
    return nullptr;
}

void collectParagraphs(const XmlElement& element, std::vector<const XmlElement*>& paragraphs) {
    if (localName(element.name) == "p") {
        paragraphs.push_back(&element);
        return;
    }
    for (const auto& child : element.children) {
        if (child.name != kTextNodeName) collectParagraphs(child, paragraphs);
    }
}

std::optional<std::int64_t> firstDescendantBegin(const XmlElement& element) {
    const auto ownBegin = parseTimeMs(attributeValue(element, "begin"));
    if (ownBegin) return ownBegin;
    for (const auto& child : element.children) {
        if (child.name == kTextNodeName || isAuxiliaryLyrics(child)) continue;
        if (const auto begin = firstDescendantBegin(child)) return begin;
    }
    return std::nullopt;
}

void trimLineText(LrcLine& line) {
    std::size_t begin = 0;
    std::size_t end = line.text.size();
    while (begin < end && std::iswspace(line.text[begin]) != 0) ++begin;
    while (end > begin && std::iswspace(line.text[end - 1]) != 0) --end;
    if (begin == 0 && end == line.text.size()) return;

    std::vector<LyricSegment> segments;
    for (auto segment : line.segments) {
        if (segment.textEnd <= begin || segment.textStart >= end) continue;
        segment.textStart = std::max(segment.textStart, begin) - begin;
        segment.textEnd = std::min(segment.textEnd, end) - begin;
        if (segment.textEnd > segment.textStart) segments.push_back(segment);
    }
    line.text = line.text.substr(begin, end - begin);
    line.segments = std::move(segments);
}

} // namespace

bool TtmlParser::hasSyllableTiming(std::string_view xml) {
    TtmlParser parser;
    return parser.parseUtf8(xml) && parser.hasWordTiming_;
}

bool TtmlParser::hasUsableSyllableTiming(std::string_view xml) {
    TtmlParser parser;
    return parser.parseUtf8(xml) && parser.wordTimedLineCount_ >= 2;
}

bool TtmlParser::parseUtf8(std::string_view xml) {
    lines_.clear();
    originalLines_.clear();
    translatedLines_.clear();
    hasTranslationsBlock_ = false;
    hasWordTiming_ = false;
    wordTimedLineCount_ = 0;
    if (xml.empty() || xml.size() > 8 * 1024 * 1024) return false;

    XmlElement root;
    if (!parseXmlTree(xml, root)) return false;

    std::map<std::string, ReplacementText> replacements;
    hasTranslationsBlock_ = findElement(root, "translations") != nullptr;
    collectReplacementTexts(root, replacements);
    const auto* body = findElement(root, "body");
    if (!body) return false;
    std::vector<const XmlElement*> paragraphs;
    collectParagraphs(*body, paragraphs);
    if (paragraphs.empty()) return false;

    struct ParagraphInfo {
        const XmlElement* element = nullptr;
        std::int64_t startMs = 0;
        std::int64_t endMs = 0;
    };
    std::vector<ParagraphInfo> infos;
    infos.reserve(paragraphs.size());
    for (const auto* paragraph : paragraphs) {
        ParagraphInfo info;
        info.element = paragraph;
        info.startMs = parseTimeMs(attributeValue(*paragraph, "begin")).value_or(
            firstDescendantBegin(*paragraph).value_or(0));
        info.endMs = parseTimeMs(attributeValue(*paragraph, "end")).value_or(0);
        infos.push_back(info);
    }
    for (std::size_t i = 0; i < infos.size(); ++i) {
        if (infos[i].endMs <= infos[i].startMs) {
            infos[i].endMs = i + 1 < infos.size() && infos[i + 1].startMs > infos[i].startMs
                ? infos[i + 1].startMs
                : infos[i].startMs + 1;
        }
    }

    for (const auto& info : infos) {
        LrcLine line;
        line.timeMs = info.startMs;
        line.durationMs = std::max<std::int64_t>(0, info.endMs - info.startMs);

        const auto key = attributeValue(*info.element, "itunes:key");
        const auto replacement = replacements.find(key);
        const auto* content = replacement == replacements.end() ? info.element : replacement->second.element;
        const bool wholeLineTiming = replacement != replacements.end() && replacement->second.subtitle;

        bool ignoredWordTiming = false;
        collectContent(*content, Timing{true, info.startMs, info.endMs}, info.startMs, info.endMs,
                       line, wholeLineTiming, ignoredWordTiming);
        // 判断是否真的逐字：至少两个有时间的 span。一个把整句包起来的
        // span、或译文只有整句时间，都只能是整行歌词。
        const bool lineHasWordTiming = line.segments.size() >= 2 &&
            !wholeLineTiming && hasExplicitDescendantTiming(*info.element);
        trimLineText(line);
        if (!hasVisibleText(line.text)) continue;

        std::stable_sort(line.segments.begin(), line.segments.end(), [](const LyricSegment& a, const LyricSegment& b) {
            return a.offsetMs < b.offsetMs;
        });
        if (line.durationMs <= 0 && !line.segments.empty()) {
            const auto& last = line.segments.back();
            line.durationMs = std::max<std::int64_t>(1, last.offsetMs + last.durationMs);
        }
        line.wordTiming = lineHasWordTiming;
        if (lineHasWordTiming) {
            hasWordTiming_ = true;
            ++wordTimedLineCount_;
        } else if (!line.segments.empty()) {
            // 只有整行时间，不能向渲染层声称这是真实逐字进度。
            line.segments.clear();
        }
        lines_.push_back(std::move(line));
    }

    // 保存不含本地化字幕的正文；外文歌曲只使用这份时间轴。
    std::vector<std::int64_t> originalLineTimes;
    for (const auto& info : infos) {
        LrcLine line;
        line.timeMs = info.startMs;
        line.durationMs = std::max<std::int64_t>(0, info.endMs - info.startMs);
        bool ignoredWordTiming = false;
        collectContent(*info.element, Timing{true, info.startMs, info.endMs}, info.startMs, info.endMs,
                       line, false, ignoredWordTiming);
        const bool lineHasWordTiming = line.segments.size() >= 2 && hasExplicitDescendantTiming(*info.element);
        trimLineText(line);
        if (!hasVisibleText(line.text)) continue;
        std::stable_sort(line.segments.begin(), line.segments.end(), [](const LyricSegment& a, const LyricSegment& b) {
            return a.offsetMs < b.offsetMs;
        });
        line.wordTiming = lineHasWordTiming;
        if (!line.wordTiming) line.segments.clear();
        if (originalLines_.empty()) originalLineTimes.push_back(line.timeMs);
        originalLines_.push_back(std::move(line));
    }
    std::stable_sort(originalLines_.begin(), originalLines_.end(), [](const LrcLine& a, const LrcLine& b) {
        return a.timeMs < b.timeMs;
    });

    // 语言替换字形单独保留，不能与原文正文合并。原文没有语言标记时，
    // TTML 内的替换译文可作为可选译文；外文原文优先选择中文简繁替换。
    std::map<std::string, ReplacementText> translatedReplacements;
    const auto rootLanguage = localizedLanguage(root);
    collectReplacementTexts(root, translatedReplacements, rootLanguage);
    if (translatedReplacements.empty() && !rootLanguage.empty()) {
        collectReplacementTexts(root, translatedReplacements);
    }
    if (translatedReplacements.empty()) translatedReplacements = replacements;
    if (translatedReplacements.empty()) {
        // 本地化 TTML 的 text 可能连 for 都没有；仅在确实没有真实行 key
        // 时按声明顺序编号，绝不让内部序号冒充正文的 itunes:key。
        std::vector<const XmlElement*> translationTexts;
        const auto* translations = findElement(root, "translations");
        if (translations) {
            const auto collectTexts = [&](const auto& self, const XmlElement& element) -> void {
                for (const auto& child : element.children) {
                    if (child.name == kTextNodeName) continue;
                    if (localName(child.name) == "text") translationTexts.push_back(&child);
                    else self(self, child);
                }
            };
            collectTexts(collectTexts, *translations);
        }
        for (std::size_t i = 0; i < translationTexts.size(); ++i) {
            translatedReplacements[std::to_string(i)] = ReplacementText{translationTexts[i], false};
        }
    }
    // 只有带 itunes:key 的正文才能按行匹配；无 key 的本地化 TTML 保持按声明顺序。
    const bool originalHasKeys = std::any_of(infos.begin(), infos.end(), [](const ParagraphInfo& info) {
        return !attributeValue(*info.element, "itunes:key").empty();
    });
    if (originalHasKeys && translatedReplacements.size() <= 1) {
        // 没有 itunes:key 的 translation 只有一份整篇文本；按正文行切分，
        // 再把每个切片放回对应的原始行起点，避免把整篇译文放到第一行。
        std::wstring wholeText;
        ReplacementText source;
        if (!translatedReplacements.empty()) source = translatedReplacements.begin()->second;
        bool unused = false;
        LrcLine aggregate;
        if (source.element) {
            collectContent(*source.element, Timing{true, 0, 0}, 0, 0, aggregate, source.subtitle, unused);
        }
        wholeText = aggregate.text;
        std::vector<std::wstring> pieces(originalLines_.size());
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            std::size_t next = i + 1 < pieces.size() ? wholeText.find_first_of(L"\r\n", cursor) : std::wstring::npos;
            if (next == std::wstring::npos) next = wholeText.size();
            while (next > cursor && std::iswspace(wholeText[next - 1]) != 0) --next;
            pieces[i] = wholeText.substr(cursor, next - cursor);
            while (next < wholeText.size() && std::iswspace(wholeText[next]) != 0) ++next;
            cursor = next;
        }
        if (pieces.size() == originalLines_.size() && std::any_of(pieces.begin(), pieces.end(),
                [](const std::wstring& text) { return !text.empty(); })) {
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                LrcLine line;
                line.timeMs = originalLines_[i].timeMs;
                line.durationMs = originalLines_[i].durationMs;
                line.text = pieces[i];
                translatedLines_.push_back(std::move(line));
            }
            translatedReplacements.clear();
            translatedLines_.erase(std::remove_if(translatedLines_.begin(), translatedLines_.end(),
                [](const LrcLine& line) { return line.text.empty(); }), translatedLines_.end());
        }
    }
    for (std::size_t visibleIndex = 0; visibleIndex < infos.size(); ++visibleIndex) {
        const auto& info = infos[visibleIndex];
        const auto key = attributeValue(*info.element, "itunes:key");
        auto replacement = key.empty() ? translatedReplacements.end() : translatedReplacements.find(key);
        if (key.empty()) {
            // 无 key 的正文不能与任意真实 key 误配。译文按 text 声明顺序
            // 对应可见段落；完全同序、同数量的真实 key 也按此顺序兜底。
            replacement = translatedReplacements.find(std::to_string(visibleIndex));
            if (replacement == translatedReplacements.end() && translatedReplacements.size() == infos.size()) {
                replacement = std::next(translatedReplacements.begin(), static_cast<std::ptrdiff_t>(visibleIndex));
            }
        }
        if (replacement == translatedReplacements.end()) continue;
        LrcLine line;
        line.timeMs = info.startMs;
        line.durationMs = std::max<std::int64_t>(0, info.endMs - info.startMs);
        bool ignoredWordTiming = false;
        const bool replacementWordTiming = !replacement->second.subtitle &&
            hasExplicitDescendantTiming(*replacement->second.element);
        collectContent(*replacement->second.element, Timing{true, info.startMs, info.endMs}, info.startMs,
                       info.endMs, line, replacement->second.subtitle || !replacementWordTiming, ignoredWordTiming);
        const bool lineHasWordTiming = line.segments.size() >= 2 && replacementWordTiming;
        trimLineText(line);
        line.wordTiming = lineHasWordTiming;
        if (!lineHasWordTiming) line.segments.clear();
        if (hasVisibleText(line.text)) translatedLines_.push_back(std::move(line));
    }
    std::stable_sort(translatedLines_.begin(), translatedLines_.end(), [](const LrcLine& a, const LrcLine& b) {
        return a.timeMs < b.timeMs;
    });

    std::stable_sort(lines_.begin(), lines_.end(), [](const LrcLine& a, const LrcLine& b) {
        return a.timeMs < b.timeMs;
    });
    return !lines_.empty();
}

std::vector<LrcLine> TtmlParser::originalLines() const {
    return originalLines_;
}

std::vector<LrcLine> TtmlParser::translatedLines() const {
    return translatedLines_;
}

} // namespace smtc::lyrics
