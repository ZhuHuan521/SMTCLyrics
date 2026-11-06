#include "lyrics/TtmlParser.h"

#include "util/Encoding.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string_view>

namespace smtc::lyrics {
namespace {

struct XmlElement {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
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

bool parseXmlTree(std::string_view xml, XmlElement& root) {
    std::vector<XmlElement*> stack;
    std::size_t index = 0;
    bool foundRoot = false;
    while (index < xml.size()) {
        const auto open = xml.find('<', index);
        const auto textEnd = open == std::string_view::npos ? xml.size() : open;
        if (textEnd > index && !stack.empty()) {
            stack.back()->text += decodeEntities(xml.substr(index, textEnd - index));
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
            stack.back()->text.append(xml.substr(index + 9, close - index - 9));
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
            if (!name || stack.empty() || lowerAscii(*name) != lowerAscii(stack.back()->name)) return false;
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
        const bool selfClosing = index < xml.size() && xml[index] == '/' ;
        if (index >= xml.size() || xml[index] != '>') return false;
        ++index;
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
        if (selfClosing && foundRoot && stack.empty() && index < xml.size()) {
            // Root is the only element in this TTML format; ignore trailing whitespace.
        }
    }
    return foundRoot && stack.empty();
}

std::string attributeValue(const XmlElement& element, std::string_view name) {
    const auto value = element.attributes.find(lowerAscii(name));
    return value == element.attributes.end() ? std::string{} : value->second;
}

std::optional<double> parseTimeSeconds(std::string_view value) {
    value = util::trimUtf8(value);
    if (value.empty()) return std::nullopt;
    double multiplier = 1000.0;
    if (value.size() >= 2 && value.substr(value.size() - 2) == "ms") {
        value.remove_suffix(2);
        multiplier = 1.0;
    } else if (!value.empty() && value.back() == 'h') {
        value.remove_suffix(1);
        multiplier = 3600000.0;
    } else if (!value.empty() && value.back() == 'm') {
        value.remove_suffix(1);
        multiplier = 60000.0;
    } else if (!value.empty() && value.back() == 's') {
        value.remove_suffix(1);
    }
    double totalSeconds = 0;
    std::size_t cursor = 0;
    while (cursor <= value.size()) {
        const auto end = value.find(':', cursor);
        const auto token = value.substr(cursor, end == std::string_view::npos ? value.size() - cursor : end - cursor);
        char* stop = nullptr;
        const std::string tokenText(token);
        const double number = std::strtod(tokenText.c_str(), &stop);
        if (!stop || *stop != '\0' || number < 0) return std::nullopt;
        totalSeconds = totalSeconds * 60.0 + number;
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return totalSeconds * multiplier / 1000.0;
}

std::int64_t timeMs(const std::string& value) {
    const auto seconds = parseTimeSeconds(value);
    return seconds ? static_cast<std::int64_t>(*seconds * 1000.0 + 0.5) : 0;
}

struct TimedText {
    std::string text;
    std::int64_t beginMs = 0;
    std::int64_t endMs = 0;
    bool hasTiming = false;
};

void collectTimedText(const XmlElement& element, const TimedText& inherited, std::vector<TimedText>& output) {
    TimedText current = inherited;
    if (current.hasTiming && element.children.empty()) {
        output.push_back(current);
        return;
    }
    for (const auto& child : element.children) {
        TimedText next = current;
        const auto begin = attributeValue(child, "begin");
        const auto end = attributeValue(child, "end");
        if (!begin.empty() || !end.empty()) {
            next.beginMs = begin.empty() ? current.endMs : timeMs(begin);
            next.endMs = end.empty() ? next.beginMs + std::max<std::int64_t>(1, current.endMs - next.beginMs) : timeMs(end);
            next.hasTiming = true;
        }
        if (!child.text.empty() && child.children.empty()) {
            TimedText piece = next;
            piece.text = child.text;
            output.push_back(std::move(piece));
        } else {
            collectTimedText(child, next, output);
        }
    }
}

}

bool TtmlParser::hasSyllableTiming(std::string_view xml) {
    // Word timing is only exposed by <p> children. Head metadata may contain
    // timed translations even when the body has no word-timed spans.
    const auto body = xml.find("<body");
    if (body == std::string_view::npos) return false;
    const auto bodyEnd = xml.find("</body>", body);
    if (bodyEnd == std::string_view::npos) return false;
    const auto bodyText = xml.substr(body, bodyEnd - body);
    std::size_t search = 0;
    while ((search = bodyText.find("<p", search)) != std::string_view::npos) {
        const auto openEnd = bodyText.find('>', search);
        if (openEnd == std::string_view::npos) return false;
        const auto close = bodyText.find("</p>", openEnd);
        if (close == std::string_view::npos) return false;
        const auto paragraph = bodyText.substr(openEnd + 1, close - openEnd - 1);
        std::size_t span = 0;
        while ((span = paragraph.find("<span", span)) != std::string_view::npos) {
            const auto spanEnd = paragraph.find('>', span);
            if (spanEnd == std::string_view::npos) return false;
            const auto tag = paragraph.substr(span, spanEnd - span + 1);
            if (tag.find("ttm:role=\"x-bg\"") == std::string_view::npos &&
                (tag.find(" begin=") != std::string_view::npos || tag.find(" end=") != std::string_view::npos)) {
                return true;
            }
            span = spanEnd + 1;
        }
        search = close + 4;
    }
    return false;
}
bool TtmlParser::parseUtf8(std::string_view xml) {
    lines_.clear();
    if (xml.empty() || xml.size() > 8 * 1024 * 1024) return false;

    struct Element {
        std::string name;
        std::map<std::string, std::string> attr;
        std::string text;
        std::vector<Element> children;
    };
    auto isNameChar = [](char ch) {
        const auto uch = static_cast<unsigned char>(ch);
        return std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == ':' || ch == '.';
    };
    auto parseName = [&](std::size_t& cursor, std::string& out) {
        const auto begin = cursor;
        while (cursor < xml.size() && isNameChar(xml[cursor])) ++cursor;
        if (cursor == begin) return false;
        out.assign(xml.substr(begin, cursor - begin));
        return true;
    };
    auto decode = [](std::string_view encoded) {
        std::string result;
        result.reserve(encoded.size());
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] != '&') {
                result.push_back(encoded[i]);
                continue;
            }
            const auto semi = encoded.find(';', i + 1);
            if (semi == std::string_view::npos) {
                result.push_back('&');
                continue;
            }
            const auto token = encoded.substr(i + 1, semi - i - 1);
            if (token == "amp") result.push_back('&');
            else if (token == "lt") result.push_back('<');
            else if (token == "gt") result.push_back('>');
            else if (token == "quot") result.push_back('"');
            else if (token == "apos") result.push_back('\'');
            else if (!token.empty() && token[0] == '#') {
                long codepoint = 0;
                try {
                    codepoint = token[1] == 'x' || token[1] == 'X'
                        ? std::stol(std::string(token.substr(2)), nullptr, 16)
                        : std::stol(std::string(token.substr(1)), nullptr, 10);
                } catch (...) {
                    codepoint = 0;
                }
                if (codepoint > 0 && codepoint <= 0x10ffff) {
                    std::wstring wide(1, static_cast<wchar_t>(codepoint));
                    result += util::wideToUtf8(wide);
                }
            } else {
                result.append(encoded.substr(i, semi - i + 1));
            }
            i = semi;
        }
        return result;
    };

    Element root;
    std::vector<Element*> stack;
    bool foundRoot = false;
    std::size_t cursor = 0;
    while (cursor < xml.size()) {
        const auto open = xml.find('<', cursor);
        const auto textEnd = open == std::string_view::npos ? xml.size() : open;
        if (textEnd > cursor && !stack.empty() && textEnd <= xml.size()) {
            stack.back()->text += decode(xml.substr(cursor, textEnd - cursor));
        }
        if (open == std::string_view::npos) break;
        cursor = open;
        if (xml.compare(cursor, 4, "<!--") == 0) {
            const auto end = xml.find("-->", cursor + 4);
            if (end == std::string_view::npos) return false;
            cursor = end + 3;
            continue;
        }
        if (xml.compare(cursor, 9, "<![CDATA[") == 0) {
            const auto end = xml.find("]]>", cursor + 9);
            if (end == std::string_view::npos || stack.empty()) return false;
            stack.back()->text.append(xml.substr(cursor + 9, end - cursor - 9));
            cursor = end + 3;
            continue;
        }
        if (xml.compare(cursor, 2, "<?") == 0) {
            const auto end = xml.find("?>", cursor + 2);
            if (end == std::string_view::npos) return false;
            cursor = end + 2;
            continue;
        }
        if (xml.compare(cursor, 2, "<!") == 0) {
            const auto end = xml.find('>', cursor + 2);
            if (end == std::string_view::npos) return false;
            cursor = end + 1;
            continue;
        }
        if (xml.compare(cursor, 2, "</") == 0) {
            cursor += 2;
            std::string closeName;
            if (!parseName(cursor, closeName) || stack.empty() || localName(closeName) != localName(stack.back()->name)) return false;
            while (cursor < xml.size() && std::isspace(static_cast<unsigned char>(xml[cursor])) != 0) ++cursor;
            if (cursor >= xml.size() || xml[cursor] != '>') return false;
            ++cursor;
            stack.pop_back();
            continue;
        }

        ++cursor;
        Element element;
        if (!parseName(cursor, element.name)) return false;
        bool selfClosing = false;
        for (;;) {
            while (cursor < xml.size() && std::isspace(static_cast<unsigned char>(xml[cursor])) != 0) ++cursor;
            if (cursor >= xml.size()) return false;
            if (xml[cursor] == '>') {
                ++cursor;
                break;
            }
            if (xml[cursor] == '/' && cursor + 1 < xml.size() && xml[cursor + 1] == '>') {
                cursor += 2;
                selfClosing = true;
                break;
            }
            std::string attrName;
            if (!parseName(cursor, attrName)) return false;
            while (cursor < xml.size() && std::isspace(static_cast<unsigned char>(xml[cursor])) != 0) ++cursor;
            if (cursor >= xml.size() || xml[cursor] != '=') return false;
            ++cursor;
            while (cursor < xml.size() && std::isspace(static_cast<unsigned char>(xml[cursor])) != 0) ++cursor;
            if (cursor >= xml.size() || (xml[cursor] != '"' && xml[cursor] != '\'')) return false;
            const char quote = xml[cursor++];
            const auto valueEnd = xml.find(quote, cursor);
            if (valueEnd == std::string_view::npos) return false;
            element.attr[lowerAscii(attrName)] = decode(xml.substr(cursor, valueEnd - cursor));
            cursor = valueEnd + 1;
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
    if (!foundRoot || !stack.empty()) return false;

    auto attr = [](const Element& element, const char* key) {
        const auto it = element.attr.find(lowerAscii(key));
        return it == element.attr.end() ? std::string{} : it->second;
    };
    auto parseTime = [](const std::string& raw) -> std::int64_t {
        const auto value = util::trimUtf8(raw);
        if (value.empty()) return 0;
        double factor = 1000.0;
        std::string number = value;
        if (number.size() >= 2 && number.substr(number.size() - 2) == "ms") {
            number.resize(number.size() - 2);
            factor = 1.0;
        } else if (!number.empty() && (number.back() == 's' || number.back() == 'm' || number.back() == 'h')) {
            if (number.back() == 'h') factor = 3600000.0;
            else if (number.back() == 'm') factor = 60000.0;
            number.pop_back();
        }
        double seconds = 0.0;
        std::size_t cursor = 0;
        while (cursor <= number.size()) {
            const auto end = number.find(':', cursor);
            const auto part = number.substr(cursor, end == std::string::npos ? number.size() - cursor : end - cursor);
            try {
                const auto value = std::stod(part);
                if (value < 0 || !std::isfinite(value)) return 0;
                seconds = seconds * 60.0 + value;
            } catch (...) {
                return 0;
            }
            if (end == std::string::npos) break;
            cursor = end + 1;
        }
        // use rounded time from the shared parser to keep all spans on the same clock
        return static_cast<std::int64_t>(seconds * factor + 0.5);
    };

    std::map<std::string, std::string> replacementText;
    const auto translations = xml.find("<translations>");
    if (translations != std::string_view::npos) {
        const auto translationsEnd = xml.find("</translations>", translations);
        if (translationsEnd != std::string_view::npos) {
            const std::string_view translationBlock = xml.substr(translations, translationsEnd - translations);
            std::size_t translationStart = 0;
            while ((translationStart = translationBlock.find("<translation ", translationStart)) != std::string_view::npos) {
                const auto openEnd = translationBlock.find('>', translationStart);
                if (openEnd == std::string_view::npos) break;
                const auto close = translationBlock.find("</translation>", openEnd);
                if (close == std::string_view::npos) break;
                const auto tag = translationBlock.substr(translationStart, openEnd - translationStart + 1);
                const auto isReplacement = tag.find("type=\"replacement\"") != std::string_view::npos;
                if (isReplacement) {
                    const std::string_view replacementBlock = translationBlock.substr(openEnd + 1, close - openEnd - 1);
                    std::size_t textStart = 0;
                    while ((textStart = replacementBlock.find("<text", textStart)) != std::string_view::npos) {
                        const auto textOpen = replacementBlock.find('>', textStart);
                        if (textOpen == std::string_view::npos) break;
                        const auto textClose = replacementBlock.find("</text>", textOpen);
                        if (textClose == std::string_view::npos) break;
                        const auto textTag = replacementBlock.substr(textStart, textOpen - textStart + 1);
                        const auto keyStart = textTag.find("for");
                        if (keyStart != std::string_view::npos) {
                            const auto keyFrom = textTag.find('"', keyStart);
                            const auto keyTo = textTag.find('"', keyFrom + 1);
                            if (keyTo != std::string_view::npos) {
                                const std::string_view content = replacementBlock.substr(textOpen + 1, textClose - textOpen - 1);
                                std::string plain;
                                std::size_t contentCursor = 0;
                                while (contentCursor < content.size()) {
                                    const auto tagBegin = content.find('<', contentCursor);
                                    plain += content.substr(contentCursor, tagBegin == std::string_view::npos ? content.size() - contentCursor : tagBegin - contentCursor);
                                    if (tagBegin == std::string_view::npos) break;
                                    const auto tagEnd = content.find('>', tagBegin);
                                    if (tagEnd == std::string_view::npos) break;
                                    contentCursor = tagEnd + 1;
                                }
                                const std::string key = std::string(textTag.substr(keyFrom + 1, keyTo - keyFrom - 1));
                                replacementText[key] = decode(plain);
                            }
                        }
                        textStart = textClose + 7;
                    }
                }
                translationStart = close + 14;
            }
        }
    }
    struct Part {
        std::string text;
        std::int64_t begin = 0;
        std::int64_t end = 0;
        bool timed = false;
        bool separator = false;
    };
    auto replacementFor = [&](std::string_view key) -> const std::string* {
        const auto found = replacementText.find(std::string(key));
        return found == replacementText.end() ? nullptr : &found->second;
    };
    const auto isWhitespace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };

    // Element::text 与 children 是分开保存的，原始 XML 的混合内容顺序在这里无法
    // 直接恢复。Apple 的 span 之间只有空格，因此解出 span 后按原文补齐分隔空格。
    std::function<void(const Element&)> visit;
    visit = [&](const Element& element) {
        if (localName(element.name) == "p") {
            LrcLine line;
            line.timeMs = parseTime(attr(element, "begin"));
            const auto endTime = parseTime(attr(element, "end"));
            const auto lineEnd = endTime > line.timeMs ? endTime : line.timeMs + 1;
            line.durationMs = lineEnd - line.timeMs;
            std::vector<Part> parts;
            const auto lineKey = attr(element, "itunes:key");
            const auto* replacement = replacementFor(lineKey);
            if (replacement) {
                const auto startOffset = line.text.size();
                line.text += util::utf8ToWide(*replacement);
                std::function<void(const Element&)> replacementTiming;
                replacementTiming = [&](const Element& node) {
                    const auto begin = attr(node, "begin");
                    const auto stop = attr(node, "end");
                    if ((!begin.empty() || !stop.empty()) && !node.text.empty() && node.children.empty()) {
                        const auto start = begin.empty() ? line.timeMs : parseTime(begin);
                        const auto finish = stop.empty() ? lineEnd : parseTime(stop);
                        const auto text = util::utf8ToWide(node.text);
                        std::size_t position = 0;
                        while ((position = line.text.find(text, position)) != std::wstring::npos) {
                            line.segments.push_back({.offsetMs = start - line.timeMs,
                                .durationMs = std::max<std::int64_t>(1, finish - start),
                                .textStart = startOffset + position, .textEnd = startOffset + position + text.size()});
                            position += text.size();
                        }
                        return;
                    }
                    for (const auto& child : node.children) replacementTiming(child);
                };
                replacementTiming(element);
                if (!line.text.empty()) lines_.push_back(std::move(line));
                return;
            } else {
                std::vector<Part> timedParts;
                std::vector<bool> spanStartsSpace;
                std::function<void(const Element&)> findTimed;
                findTimed = [&](const Element& node) {
                    const auto name = localName(node.name);
                    if (attr(node, "ttm:role") == "x-bg" || name == "br") return;
                    const auto begin = attr(node, "begin");
                    const auto stop = attr(node, "end");
                    if ((!begin.empty() || !stop.empty()) && !node.text.empty()) {
                        if (node.children.empty()) {
                            const auto start = begin.empty() ? line.timeMs : parseTime(begin);
                            const auto finish = stop.empty() ? lineEnd : parseTime(stop);
                            timedParts.push_back(Part{node.text, start, std::max(start + 1, finish), true});
                            return;
                        }
                        for (const auto& child : node.children) findTimed(child);
                        return;
                    }
                    for (const auto& child : node.children) findTimed(child);
                };
                findTimed(element);
                if (!timedParts.empty()) {
                    // Element::text loses the original mixed-content order. Locate each
                    // timed span in source and preserve only the literal spaces directly
                    // before it; do not infer word boundaries from timing gaps.
                    std::size_t spanSearch = 0;
                    for (const auto& timedPart : timedParts) {
                        std::size_t tagStart = xml.find("<span", spanSearch);
                        bool startsWithSpace = false;
                        while (tagStart != std::string_view::npos) {
                            const auto openEnd = xml.find('>', tagStart);
                            if (openEnd == std::string_view::npos) break;
                            const auto close = xml.find("</span>", openEnd);
                            if (close == std::string_view::npos) break;
                            const auto candidateText = decode(xml.substr(openEnd + 1, close - openEnd - 1));
                            if (candidateText == timedPart.text) {
                                std::size_t literalStart = tagStart;
                                while (literalStart > 0 && xml[literalStart - 1] != '>') {
                                    if (xml[literalStart - 1] == ' ' || xml[literalStart - 1] == '\t') {
                                        startsWithSpace = true;
                                        break;
                                    }
                                    --literalStart;
                                }
                                spanSearch = tagStart + 5;
                                break;
                            }
                            spanSearch = tagStart + 5;
                            tagStart = xml.find("<span", spanSearch);
                        }
                        spanStartsSpace.push_back(startsWithSpace);
                    }
                    for (std::size_t i = 0; i < timedParts.size(); ++i) {
                        // The replacement translation has no XML inter-span text to inspect,
                        // so its separator list is empty. Add only source spans whose raw
                        // source contains a literal separator; never infer by timing gaps.
                        if (i == 0 && spanStartsSpace.empty()) {
                            parts.push_back(timedParts[i]);
                            continue;
                        }
                        if (i < spanStartsSpace.size() && spanStartsSpace[i]) {
                            parts.push_back(Part{" ", line.timeMs, lineEnd, false, true});
                        }
                        parts.push_back(timedParts[i]);
                    }
                } else if (!element.text.empty()) {
                    parts.push_back(Part{element.text, line.timeMs, lineEnd, false});
                }
            }
            for (const auto& part : parts) {
                if (part.text.empty()) continue;
                const auto offset = line.text.size();
                line.text += util::utf8ToWide(part.text);
                if (part.separator && !line.segments.empty()) line.segments.back().textEnd = line.text.size();
                if (part.timed) {
                    line.segments.push_back({.offsetMs = part.begin - line.timeMs,
                                             .durationMs = std::max<std::int64_t>(1, part.end - part.begin),
                                             .textStart = offset, .textEnd = line.text.size()});
                }
            }
            if (!line.text.empty()) lines_.push_back(std::move(line));
            return;
        }
        for (const auto& child : element.children) visit(child);
    };
    visit(root);
    std::stable_sort(lines_.begin(), lines_.end(), [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
    return !lines_.empty();
}

}
