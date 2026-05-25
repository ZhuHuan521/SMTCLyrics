#include "lyrics/OnlineLyrics.h"

#include "json.hpp"
#include "lyrics/QrcDecrypter.h"
#include "util/Base64.h"
#include "util/Encoding.h"
#include "util/Inflate.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>

namespace smtc::lyrics {
namespace {

// 大多数歌词接口会检查 User-Agent，模拟浏览器可减少空响应。
std::vector<http::HttpClient::Header> browserHeaders() {
    return {
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36"},
    };
}

// QQ 音乐搜索接口需要 y.qq.com Referer。
std::vector<http::HttpClient::Header> qqHeaders() {
    auto headers = browserHeaders();
    headers.emplace_back("Referer", "https://y.qq.com/");
    return headers;
}

// QQ 歌词下载接口需要 c.y.qq.com Referer。
std::vector<http::HttpClient::Header> qqDownloadHeaders() {
    auto headers = browserHeaders();
    headers.emplace_back("Referer", "https://c.y.qq.com/");
    return headers;
}

// 网络接口返回异常时直接返回空 JSON，调用方按字段缺失处理。
nlohmann::json parseJson(const std::vector<std::uint8_t>& body) {
    try {
        return nlohmann::json::parse(std::string(body.begin(), body.end()));
    } catch (...) {
        return {};
    }
}

// 接口中的 ID 有时是字符串、有时是数字，统一转成 string。
std::string jsonString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) return std::to_string(value.get<double>());
    return {};
}

std::string jsonString(const nlohmann::json* value) {
    return value ? jsonString(*value) : std::string{};
}

const nlohmann::json* jsonAt(const nlohmann::json* value, std::string_view key) {
    // 安全访问对象字段，避免链式取值时抛异常。
    if (!value || !value->is_object()) return nullptr;
    const auto it = value->find(std::string(key));
    return it == value->end() ? nullptr : &*it;
}

const nlohmann::json* jsonAt(const nlohmann::json& value, std::string_view key) {
    return jsonAt(&value, key);
}

const nlohmann::json* jsonAt(const nlohmann::json* value, std::size_t index) {
    if (!value || !value->is_array() || index >= value->size()) return nullptr;
    return &(*value)[index];
}

bool nonEmptyArray(const nlohmann::json* value) {
    // 搜索接口常见返回结构：数组缺失或为空都代表没有结果。
    return value && value->is_array() && !value->empty();
}

std::vector<std::uint8_t> toBytes(std::string_view text) {
    // LrcParser 需要字节数组，这里不改变文本编码。
    return {text.begin(), text.end()};
}

bool isXmlNameBoundary(char ch) {
    // 简单 XML 扫描时确认标签名边界，避免 <content2> 误匹配 <content>。
    return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '>' || ch == '/';
}

std::size_t findXmlTag(std::string_view xml, std::string_view tagName, std::size_t start = 0) {
    // 只做本项目需要的轻量 XML 查找，不引入完整 XML 解析器。
    const auto needle = "<" + std::string(tagName);
    std::size_t pos = start;
    while ((pos = xml.find(needle, pos)) != std::string_view::npos) {
        const auto boundary = pos + needle.size();
        if (boundary < xml.size() && isXmlNameBoundary(xml[boundary])) {
            return pos;
        }
        pos = boundary;
    }
    return std::string_view::npos;
}

std::string extractCData(std::string_view xml, std::string_view tagName) {
    // QQ 旧歌词接口把 Base64 歌词放在 CDATA 里。
    std::size_t pos = 0;
    while ((pos = findXmlTag(xml, tagName, pos)) != std::string_view::npos) {
        const auto tagEnd = xml.find('>', pos);
        if (tagEnd == std::string_view::npos) return {};
        const auto cdataBegin = xml.find("<![CDATA[", tagEnd + 1);
        if (cdataBegin == std::string_view::npos) return {};
        const auto nextTag = xml.find('<', tagEnd + 1);
        if (nextTag != std::string_view::npos && nextTag != cdataBegin) {
            pos = tagEnd + 1;
            continue;
        }
        const auto valueBegin = cdataBegin + 9;
        const auto valueEnd = xml.find("]]>", valueBegin);
        if (valueEnd == std::string_view::npos) return {};
        return std::string(xml.substr(valueBegin, valueEnd - valueBegin));
    }
    return {};
}

std::string extractXmlAttribute(std::string_view xml, std::string_view attributeName) {
    // QRC 解密后的 XML 把真正歌词放在 LyricContent 属性中。
    std::size_t pos = 0;
    while ((pos = xml.find(attributeName, pos)) != std::string_view::npos) {
        const auto nameEnd = pos + attributeName.size();
        std::size_t scan = nameEnd;
        while (scan < xml.size() && std::isspace(static_cast<unsigned char>(xml[scan])) != 0) ++scan;
        if (scan >= xml.size() || xml[scan] != '=') {
            pos = nameEnd;
            continue;
        }
        ++scan;
        while (scan < xml.size() && std::isspace(static_cast<unsigned char>(xml[scan])) != 0) ++scan;
        if (scan >= xml.size() || (xml[scan] != '"' && xml[scan] != '\'')) return {};
        const char quote = xml[scan++];
        const auto end = xml.find(quote, scan);
        if (end == std::string_view::npos) return {};
        return std::string(xml.substr(scan, end - scan));
    }
    return {};
}

std::string extractQrcContent(std::string_view decryptedXml) {
    // 属性内容还包含 HTML 实体，取出后再解码。
    auto content = extractXmlAttribute(decryptedXml, "LyricContent");
    if (content.empty()) return {};
    return util::htmlDecodeUtf8(content);
}

std::vector<std::uint8_t> decryptKrc(const std::vector<std::uint8_t>& bytes) {
    // 酷狗 KRC：校验 krc1 头，异或固定 key，再 zlib 解压。
    constexpr std::uint8_t kHeader[] = {'k', 'r', 'c', '1'};
    constexpr std::uint8_t kKey[] = {64, 71, 97, 119, 94, 50, 116, 71, 81, 54, 49, 45, 206, 210, 110, 105};
    if (bytes.size() <= sizeof(kHeader) || std::memcmp(bytes.data(), kHeader, sizeof(kHeader)) != 0) {
        return {};
    }

    std::vector<std::uint8_t> zlibBytes(bytes.begin() + sizeof(kHeader), bytes.end());
    for (std::size_t i = 0; i < zlibBytes.size(); ++i) {
        zlibBytes[i] ^= kKey[i % std::size(kKey)];
    }

    return util::inflateZlib(zlibBytes);
}

std::string formatLrcTimestamp(double seconds) {
    // 酷我接口给秒数，转换成标准 LRC 时间戳。
    if (seconds < 0) seconds = 0;
    const auto totalMs = static_cast<long long>(seconds * 1000.0 + 0.5);
    const auto minutes = totalMs / 60000;
    const auto sec = (totalMs % 60000) / 1000;
    const auto ms = totalMs % 1000;
    std::ostringstream out;
    out << '[' << std::setw(2) << std::setfill('0') << minutes << ':'
        << std::setw(2) << std::setfill('0') << sec << '.'
        << std::setw(3) << std::setfill('0') << ms << ']';
    return out.str();
}

std::string makeUuidV4() {
    // 酷我歌词接口需要 reqId，这里生成一个随机 UUID v4。
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 255);
    unsigned char bytes[16]{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(dis(gen));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return out.str();
}

// BCrypt 句柄的 RAII 包装，避免加解密路径泄漏系统资源。
struct BCryptAlgorithmDeleter {
    void operator()(BCRYPT_ALG_HANDLE handle) const {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
};

struct BCryptHashDeleter {
    void operator()(BCRYPT_HASH_HANDLE handle) const {
        if (handle) BCryptDestroyHash(handle);
    }
};

struct BCryptKeyDeleter {
    void operator()(BCRYPT_KEY_HANDLE handle) const {
        if (handle) BCryptDestroyKey(handle);
    }
};

using BCryptAlgorithm = std::unique_ptr<void, BCryptAlgorithmDeleter>;
using BCryptHash = std::unique_ptr<void, BCryptHashDeleter>;
using BCryptKey = std::unique_ptr<void, BCryptKeyDeleter>;

std::optional<DWORD> bcryptDwordProperty(BCRYPT_ALG_HANDLE algorithm, const wchar_t* property) {
    // 读取算法对象长度、摘要长度等 DWORD 属性。
    DWORD value = 0;
    DWORD copied = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, property, reinterpret_cast<PUCHAR>(&value), sizeof(value), &copied, 0))) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint8_t> bcryptHash(std::wstring_view algorithmName, std::string_view data) {
    // 目前用于 MD5 签名；封装成通用 hash 以便复用 BCrypt 生命周期处理。
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&rawAlgorithm, std::wstring(algorithmName).c_str(), nullptr, 0))) {
        return {};
    }
    BCryptAlgorithm algorithm(rawAlgorithm);

    const auto objectLength = bcryptDwordProperty(rawAlgorithm, BCRYPT_OBJECT_LENGTH);
    const auto hashLength = bcryptDwordProperty(rawAlgorithm, BCRYPT_HASH_LENGTH);
    if (!objectLength || !hashLength) return {};

    std::vector<std::uint8_t> object(*objectLength);
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(rawAlgorithm, &rawHash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0))) {
        return {};
    }
    BCryptHash hash(rawHash);

    if (!BCRYPT_SUCCESS(BCryptHashData(rawHash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0))) {
        return {};
    }

    std::vector<std::uint8_t> digest(*hashLength);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(rawHash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        return {};
    }
    return digest;
}

std::string hexEncode(const std::vector<std::uint8_t>& bytes, bool uppercase) {
    // 网易云 EAPI 加密参数需要十六进制大写，MD5 签名需要小写。
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    if (uppercase) out << std::uppercase;
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::string md5Hex(std::string_view data) {
    // 网易云 EAPI 签名算法使用 MD5 十六进制字符串。
    return hexEncode(bcryptHash(BCRYPT_MD5_ALGORITHM, data), false);
}

std::vector<std::uint8_t> pkcs7Pad(std::string_view data, std::size_t blockSize) {
    // AES-ECB 输入需要按 16 字节块 PKCS#7 补齐。
    std::vector<std::uint8_t> padded(data.begin(), data.end());
    const auto pad = blockSize - (padded.size() % blockSize);
    padded.insert(padded.end(), pad, static_cast<std::uint8_t>(pad));
    return padded;
}

std::vector<std::uint8_t> pkcs7Unpad(std::vector<std::uint8_t> data) {
    // 解密后校验并去除 PKCS#7 padding。
    if (data.empty()) return {};
    const auto pad = data.back();
    if (pad == 0 || pad > 16 || pad > data.size()) return {};
    if (!std::all_of(data.end() - pad, data.end(), [pad](std::uint8_t value) { return value == pad; })) {
        return {};
    }
    data.resize(data.size() - pad);
    return data;
}

std::vector<std::uint8_t> aesEcbCrypt(const std::vector<std::uint8_t>& input, std::string_view key, bool encrypt) {
    // 网易云 EAPI 使用 AES-128-ECB，Windows 下用 BCrypt 实现。
    if (key.size() != 16 || input.empty() || (!encrypt && input.size() % 16 != 0)) return {};

    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&rawAlgorithm, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        return {};
    }
    BCryptAlgorithm algorithm(rawAlgorithm);

    const wchar_t chainingMode[] = BCRYPT_CHAIN_MODE_ECB;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(rawAlgorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(chainingMode)), sizeof(chainingMode), 0))) {
        return {};
    }

    const auto objectLength = bcryptDwordProperty(rawAlgorithm, BCRYPT_OBJECT_LENGTH);
    if (!objectLength) return {};

    std::vector<std::uint8_t> keyObject(*objectLength);
    BCRYPT_KEY_HANDLE rawKey = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
            rawAlgorithm,
            &rawKey,
            keyObject.data(),
            static_cast<ULONG>(keyObject.size()),
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
            static_cast<ULONG>(key.size()),
            0))) {
        return {};
    }
    BCryptKey symmetricKey(rawKey);

    ULONG outputSize = 0;
    const auto probeStatus = encrypt
        ? BCryptEncrypt(rawKey, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), nullptr, nullptr, 0, nullptr, 0, &outputSize, 0)
        : BCryptDecrypt(rawKey, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), nullptr, nullptr, 0, nullptr, 0, &outputSize, 0);
    if (!BCRYPT_SUCCESS(probeStatus) || outputSize == 0) return {};

    std::vector<std::uint8_t> output(outputSize);
    ULONG written = 0;
    const auto cryptStatus = encrypt
        ? BCryptEncrypt(rawKey, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), nullptr, nullptr, 0, output.data(), static_cast<ULONG>(output.size()), &written, 0)
        : BCryptDecrypt(rawKey, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), nullptr, nullptr, 0, output.data(), static_cast<ULONG>(output.size()), &written, 0);
    if (!BCRYPT_SUCCESS(cryptStatus)) return {};
    output.resize(written);
    return output;
}

std::vector<std::uint8_t> aesEcbEncryptPkcs7(std::string_view data, std::string_view key) {
    return aesEcbCrypt(pkcs7Pad(data, 16), key, true);
}

std::vector<std::uint8_t> aesEcbDecryptPkcs7(const std::vector<std::uint8_t>& data, std::string_view key) {
    return pkcs7Unpad(aesEcbCrypt(data, key, false));
}

std::string randomFromAlphabet(std::string_view alphabet, int length) {
    // 生成网易云匿名会话所需的随机设备字段。
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> dis(0, alphabet.size() - 1);
    std::string out;
    out.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        out.push_back(alphabet[dis(gen)]);
    }
    return out;
}

std::string randomHex(int length) {
    return randomFromAlphabet("0123456789ABCDEF", length);
}

std::string randomLowercase(int length) {
    return randomFromAlphabet("abcdefghijklmnopqrstuvwxyz", length);
}

std::string randomUppercase(int length) {
    return randomFromAlphabet("ABCDEFGHIJKLMNOPQRSTUVWXYZ", length);
}

std::string randomMacAddress() {
    std::string mac;
    for (int i = 0; i < 6; ++i) {
        if (i != 0) mac.push_back(':');
        mac += randomHex(2);
    }
    return mac;
}

std::string neteaseClientSign() {
    // 模拟桌面客户端的 clientSign cookie。
    return randomMacAddress() + "@@@" + randomUppercase(8) + "@@@@@@" + randomHex(64);
}

std::string neteaseAnonymousUsername(std::string_view deviceId) {
    // 网易云匿名注册接口需要由 deviceId 派生的 username。
    constexpr std::string_view key = "3go8&$8*3*3h0k(2)2";
    std::string xored;
    xored.reserve(deviceId.size());
    for (std::size_t i = 0; i < deviceId.size(); ++i) {
        xored.push_back(static_cast<char>(deviceId[i] ^ key[i % key.size()]));
    }
    const auto digest = bcryptHash(BCRYPT_MD5_ALGORITHM, xored);
    return util::base64Encode(std::string(deviceId) + " " + util::base64Encode(digest));
}

// 网易云请求需要一组 cookie 以及 mconfig/header 参数。
struct NeteaseSession {
    std::vector<http::HttpClient::Header> cookies;
    std::string paramsHeader;
};

std::string cookieValue(const std::vector<http::HttpClient::Header>& cookies, std::string_view name) {
    for (const auto& [key, value] : cookies) {
        if (key == name) return value;
    }
    return {};
}

void upsertCookie(std::vector<http::HttpClient::Header>& cookies, std::string name, std::string value) {
    // Set-Cookie 返回的新值覆盖已有 cookie。
    if (value.empty()) return;
    for (auto& [key, existing] : cookies) {
        if (key == name) {
            existing = std::move(value);
            return;
        }
    }
    cookies.emplace_back(std::move(name), std::move(value));
}

std::string neteaseParamsHeader(const std::vector<http::HttpClient::Header>& cookies) {
    // EAPI params 内部还要带一份 header JSON。
    nlohmann::json header;
    header["clientSign"] = cookieValue(cookies, "clientSign");
    header["os"] = cookieValue(cookies, "os");
    header["appver"] = cookieValue(cookies, "appver");
    header["deviceId"] = cookieValue(cookies, "deviceId");
    header["requestId"] = 0;
    header["osver"] = cookieValue(cookies, "osver");
    return header.dump();
}

std::string joinedCookies(const std::vector<http::HttpClient::Header>& cookies) {
    // 把内部 cookie 列表拼成 HTTP Cookie 头。
    std::string out;
    for (const auto& [name, value] : cookies) {
        if (name.empty() || value.empty()) continue;
        if (!out.empty()) out += "; ";
        out += name;
        out += '=';
        out += value;
    }
    return out;
}

std::vector<http::HttpClient::Header> neteaseHeaders(const NeteaseSession& session) {
    // 尽量贴近网易云桌面客户端请求头。
    auto headers = std::vector<http::HttpClient::Header>{
        {"accept", "*/*"},
        {"mconfig-info", "{\"IuRPVVmc3WWul9fT\":{\"version\":733184,\"appver\":\"3.1.3.203419\"}}"},
        {"origin", "orpheus://orpheus"},
        {"user-agent", "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Safari/537.36 Chrome/91.0.4472.164 NeteaseMusicDesktop/3.1.3.203419"},
        {"sec-ch-ua", "\"Chromium\";v=\"91\""},
        {"sec-ch-ua-mobile", "?0"},
        {"sec-fetch-site", "cross-site"},
        {"sec-fetch-mode", "cors"},
        {"sec-fetch-dest", "empty"},
        {"accept-encoding", "identity"},
        {"accept-language", "en-US,en;q=0.9"},
    };
    const auto cookies = joinedCookies(session.cookies);
    if (!cookies.empty()) {
        headers.emplace_back("cookie", cookies);
    }
    return headers;
}

std::string eapiPathToApiPath(std::string_view path) {
    // EAPI 签名中使用 api 路径，而实际请求使用 eapi 路径。
    std::string apiPath(path);
    const auto pos = apiPath.find("eapi");
    if (pos != std::string::npos) {
        apiPath.replace(pos, 4, "api");
    }
    return apiPath;
}

std::string neteaseEapiParams(std::string_view path, nlohmann::json params) {
    // 网易云 EAPI 参数：MD5 签名后再 AES-ECB 加密，并以 params=HEX 发送。
    constexpr std::string_view key = "e82ckenh8dichen8";
    const auto apiPath = eapiPathToApiPath(path);
    const auto paramsText = params.dump(-1, ' ', true);
    const auto sign = md5Hex("nobody" + apiPath + "use" + paramsText + "md5forencrypt");
    const auto plain = apiPath + "-36cd479b6b5-" + paramsText + "-36cd479b6b5-" + sign;
    return "params=" + hexEncode(aesEcbEncryptPkcs7(plain, key), true);
}

http::HttpResponse neteaseEapiPost(const http::HttpClient& client, std::string_view path, nlohmann::json params, const NeteaseSession& session) {
    // 统一发送网易云 EAPI 请求。
    params["e_r"] = true;
    params["header"] = session.paramsHeader;
    const auto body = neteaseEapiParams(path, std::move(params));
    return client.post("https://interface.music.163.com" + std::string(path), body, neteaseHeaders(session));
}

nlohmann::json decryptNeteaseEapiResponse(const http::HttpResponse& response) {
    // 网易云 EAPI 响应同样是 AES-ECB 加密 JSON。
    constexpr std::string_view key = "e82ckenh8dichen8";
    if (!response || response.body.size() % 16 != 0) return {};
    const auto decrypted = aesEcbDecryptPkcs7(response.body, key);
    return decrypted.empty() ? nlohmann::json{} : parseJson(decrypted);
}

std::string trimAscii(std::string_view text) {
    // 解析 HTTP 头时只需要 ASCII 空白裁剪。
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return std::string(text);
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string cookieFromSetCookie(std::string_view rawHeaders, std::string_view name) {
    // 从原始响应头中提取指定 Set-Cookie 的值。
    std::size_t lineStart = 0;
    while (lineStart <= rawHeaders.size()) {
        const auto lineEnd = rawHeaders.find("\r\n", lineStart);
        const auto line = rawHeaders.substr(lineStart, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && lowerAscii(std::string(line.substr(0, colon))) == "set-cookie") {
            auto cookie = trimAscii(line.substr(colon + 1));
            const auto prefix = std::string(name) + "=";
            if (cookie.rfind(prefix, 0) == 0) {
                const auto semicolon = cookie.find(';');
                return cookie.substr(prefix.size(), semicolon == std::string::npos ? std::string::npos : semicolon - prefix.size());
            }
        }
        if (lineEnd == std::string_view::npos) break;
        lineStart = lineEnd + 2;
    }
    return {};
}

NeteaseSession makeNeteaseSession(const http::HttpClient& client) {
    // 创建匿名网易云会话；成功时会拿到 MUSIC_A 等关键 cookie。
    static constexpr std::array<std::string_view, 4> modes{
        "MS-iCraft B760M WIFI",
        "ASUS ROG STRIX Z790",
        "MSI MAG B550 TOMAHAWK",
        "ASRock X670E Taichi",
    };
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> modeDis(0, modes.size() - 1);

    NeteaseSession session;
    session.cookies = {
        {"os", "pc"},
        {"deviceId", randomHex(50)},
        {"osver", "Microsoft-Windows-10--build-26000-64bit"},
        {"clientSign", neteaseClientSign()},
        {"channel", "netease"},
        {"mode", std::string(modes[modeDis(gen)])},
        {"appver", "3.1.3.203419"},
    };
    session.paramsHeader = neteaseParamsHeader(session.cookies);

    nlohmann::json params;
    params["username"] = neteaseAnonymousUsername(cookieValue(session.cookies, "deviceId"));
    const auto response = neteaseEapiPost(client, "/eapi/register/anonimous", std::move(params), session);
    const auto data = decryptNeteaseEapiResponse(response);
    if (!data.is_object() || jsonString(jsonAt(data, "code")) != "200") {
        return session;
    }

    upsertCookie(session.cookies, "WEVNSM", "1.0.0");
    upsertCookie(session.cookies, "NMTID", cookieFromSetCookie(response.rawHeaders, "NMTID"));
    upsertCookie(session.cookies, "MUSIC_A", cookieFromSetCookie(response.rawHeaders, "MUSIC_A"));
    upsertCookie(session.cookies, "__csrf", cookieFromSetCookie(response.rawHeaders, "__csrf"));
    upsertCookie(session.cookies, "WNMCID", randomLowercase(6) + ".0.01.0");
    session.paramsHeader = neteaseParamsHeader(session.cookies);
    return session;
}

long long monotonicSeconds() {
    // 会话缓存过期时间使用单调时钟，避免系统时间调整影响。
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

NeteaseSession cachedNeteaseSession(const http::HttpClient& client) {
    // 网易云匿名会话可复用，避免每次抓歌词都重新注册。
    static std::mutex mutex;
    static std::optional<NeteaseSession> cached;
    static long long expiresAt = 0;

    const auto now = monotonicSeconds();
    {
        std::lock_guard lock(mutex);
        if (cached && now < expiresAt) {
            return *cached;
        }
    }

    auto session = makeNeteaseSession(client);
    if (!cookieValue(session.cookies, "MUSIC_A").empty()) {
        std::lock_guard lock(mutex);
        cached = session;
        expiresAt = now + 864000;
    }
    return session;
}

std::string fetchNeteaseEapiSongId(const http::HttpClient& client, std::string_view keywordUtf8, const NeteaseSession& session) {
    // 首选新版 EAPI 搜索，结果更贴近桌面客户端。
    nlohmann::json params;
    params["limit"] = "20";
    params["offset"] = "0";
    params["keyword"] = std::string(keywordUtf8);
    params["scene"] = "NORMAL";
    params["needCorrect"] = "true";

    const auto search = decryptNeteaseEapiResponse(neteaseEapiPost(client, "/eapi/search/song/list/page", std::move(params), session));
    const auto* resources = jsonAt(jsonAt(search, "data"), "resources");
    if (!nonEmptyArray(resources)) return {};

    const auto* first = jsonAt(resources, 0);
    return jsonString(jsonAt(jsonAt(jsonAt(first, "baseInfo"), "simpleSongData"), "id"));
}

std::string fetchNeteaseLegacySongId(const http::HttpClient& client, std::string_view keywordUtf8, const std::vector<http::HttpClient::Header>& headers) {
    // 新版搜索失败时回退到旧 Web API。
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto searchUrl = "https://music.163.com/api/search/get/web?csrf_token=&hlpretag=&hlposttag=&s=" + encoded + "&type=1&offset=0&total=true&limit=10";
    const auto search = parseJson(client.get(searchUrl, headers).body);
    const auto* songs = jsonAt(jsonAt(search, "result"), "songs");
    if (!nonEmptyArray(songs)) return {};
    return jsonString(jsonAt(jsonAt(songs, 0), "id"));
}

std::vector<std::uint8_t> fetchNeteaseLegacyLrc(const http::HttpClient& client, std::string_view id, const std::vector<http::HttpClient::Header>& headers) {
    // 最后兜底读取旧版普通 LRC。
    const auto lyricUrl = "https://music.163.com/api/song/lyric?id=" + std::string(id) + "&lv=-1&kv=-1&tv=-1";
    const auto lyric = parseJson(client.get(lyricUrl, headers).body);
    const auto lrc = jsonString(jsonAt(jsonAt(lyric, "lrc"), "lyric"));
    return lrc.empty() ? std::vector<std::uint8_t>{} : toBytes(lrc);
}

}

std::string sourceName(LyricSource source) {
    // 这些短名主要用于调试或未来展示。
    switch (source) {
    case LyricSource::Local: return "local";
    case LyricSource::QQ: return "qq";
    case LyricSource::Kugou: return "kg";
    case LyricSource::Kuwo: return "kuwo";
    case LyricSource::Netease: return "wy";
    }
    return "unknown";
}

std::optional<LyricSource> sourceFromIndex(int index) {
    // 配置和缓存以 0..4/1..4 数字保存，统一转成枚举。
    switch (index) {
    case 0: return LyricSource::Local;
    case 1: return LyricSource::QQ;
    case 2: return LyricSource::Kugou;
    case 3: return LyricSource::Kuwo;
    case 4: return LyricSource::Netease;
    default: return std::nullopt;
    }
}

OnlineLyrics::OnlineLyrics(http::HttpClient client) : client_(std::move(client)) {}

std::vector<std::uint8_t> OnlineLyrics::fetch(LyricSource source, std::string_view keywordUtf8) const {
    // 分发到具体平台；本地源由 LyricRepository 处理。
    switch (source) {
    case LyricSource::QQ: return fetchQQ(keywordUtf8);
    case LyricSource::Kugou: return fetchKugou(keywordUtf8);
    case LyricSource::Kuwo: return fetchKuwo(keywordUtf8);
    case LyricSource::Netease: return fetchNetease(keywordUtf8);
    case LyricSource::Local: break;
    }
    return {};
}

std::vector<std::uint8_t> OnlineLyrics::fetchQQ(std::string_view keywordUtf8) const {
    // QQ：先搜索歌曲，再优先下载 QRC，失败后回退普通 LRC。
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto searchUrl = "https://shc.y.qq.com/soso/fcgi-bin/search_for_qq_cp?_=1657641526460&g_tk=1037878909&format=json&inCharset=utf-8&outCharset=utf-8&notice=0&platform=h5&needNewCode=1&w=" + encoded + "&zhidaqu=1&catZhida=1&t=0&flag=1&ie=utf-8&sem=1&aggr=0&perpage=20&n=20&p=1&remoteplace=txt.mqq.all";
    const auto search = parseJson(client_.get(searchUrl, qqHeaders()).body);
    const auto* list = jsonAt(jsonAt(jsonAt(search, "data"), "song"), "list");
    if (!nonEmptyArray(list)) return {};
    const auto* firstSong = jsonAt(list, 0);
    auto songmid = jsonString(jsonAt(firstSong, "songmid"));
    if (songmid.empty() || songmid == "0") {
        songmid = jsonString(jsonAt(firstSong, "mid"));
    }

    auto musicId = jsonString(jsonAt(firstSong, "songid"));
    if (musicId.empty() || musicId == "0") {
        musicId = jsonString(jsonAt(firstSong, "id"));
    }
    if (!musicId.empty() && musicId != "0") {
        // lrctype=4 返回 QRC，内容需要 3DES 解密并从 XML 属性取出。
        const auto body = "version=15&miniversion=100&lrctype=4&musicid=" + util::urlEncode(musicId);
        const auto rawXml = client_.post("https://c.y.qq.com/qqmusic/fcgi-bin/lyric_download.fcg", body, qqDownloadHeaders()).text();
        const auto encrypted = extractCData(rawXml, "content");
        if (!encrypted.empty()) {
            const auto decrypted = decryptQrc(encrypted);
            const auto qrcContent = extractQrcContent(decrypted);
            if (!qrcContent.empty()) {
                return toBytes(qrcContent);
            }
        }
    }

    // QRC 失败时使用旧接口，返回 Base64 + HTML 实体编码的 LRC。
    if (songmid.empty() || songmid == "0") return {};

    std::random_device rd;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string loginUin;
    for (int i = 0; i < 10; ++i) loginUin += static_cast<char>('0' + digit(rd));

    const auto lyricUrl = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&loginUin=" + loginUin + "&songmid=" + songmid;
    const auto lyricJson = parseJson(client_.get(lyricUrl, qqHeaders()).body);
    const auto lyricBase64 = jsonString(jsonAt(lyricJson, "lyric"));
    if (lyricBase64.empty()) return {};
    auto decoded = util::base64DecodeToString(lyricBase64);
    decoded = util::htmlDecodeUtf8(decoded);
    return toBytes(decoded);
}

std::vector<std::uint8_t> OnlineLyrics::fetchKugou(std::string_view keywordUtf8) const {
    // 酷狗：搜索歌曲 hash -> 搜索歌词候选 -> 优先 KRC，失败回退 LRC。
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto headers = browserHeaders();
    const auto searchUrl = "http://ioscdn.kugou.com/api/v3/search/song?keyword=" + encoded + "&page=1&pagesize=40&showtype=10&plat=2&version=7910&tag=1&correct=1&privilege=1&sver=5";
    const auto search = parseJson(client_.get(searchUrl, headers).body);
    const auto* info = jsonAt(jsonAt(search, "data"), "info");
    if (!nonEmptyArray(info)) return {};
    const auto* firstInfo = jsonAt(info, 0);
    const auto hash = jsonString(jsonAt(firstInfo, "hash"));
    std::string albumAudioId = jsonString(jsonAt(firstInfo, "album_audio_id"));
    const auto* group = jsonAt(firstInfo, "group");
    if (albumAudioId.empty() && nonEmptyArray(group)) {
        albumAudioId = jsonString(jsonAt(jsonAt(group, 0), "album_audio_id"));
    }
    if (hash.empty()) return {};

    const auto candidateUrl = "http://krcs.kugou.com/search?ver=1&man=no&client=pc&keyword=" + encoded + "&duration=139039&hash=" + hash + "&album_audio_id=" + albumAudioId + "&lrctxt=1";
    const auto candidates = parseJson(client_.get(candidateUrl, headers).body);
    const auto* list = jsonAt(candidates, "candidates");
    if (!nonEmptyArray(list)) return {};
    const auto* firstCandidate = jsonAt(list, 0);
    const auto id = jsonString(jsonAt(firstCandidate, "id"));
    const auto accessKey = jsonString(jsonAt(firstCandidate, "accesskey"));
    if (id.empty() || accessKey.empty()) return {};

    const auto krcUrl = "http://lyrics2.kugou.com/download?accesskey=" + accessKey + "&charset=utf8&client=pc&fmt=krc&id=" + id + "&ver=1";
    const auto krcDownload = parseJson(client_.get(krcUrl, headers).body);
    const auto krcContent = jsonString(jsonAt(krcDownload, "content"));
    if (!krcContent.empty()) {
        // KRC 是 Base64 后的加密压缩内容。
        const auto decoded = util::base64Decode(krcContent);
        if (auto decrypted = decryptKrc(decoded); !decrypted.empty()) {
            return decrypted;
        }
    }

    const auto lrcUrl = "http://lyrics2.kugou.com/download?accesskey=" + accessKey + "&charset=utf8&client=pc&fmt=lrc&id=" + id + "&ver=1";
    const auto lrcDownload = parseJson(client_.get(lrcUrl, headers).body);
    const auto lrcContent = jsonString(jsonAt(lrcDownload, "content"));
    return lrcContent.empty() ? std::vector<std::uint8_t>{} : util::base64Decode(lrcContent);
}

std::vector<std::uint8_t> OnlineLyrics::fetchKuwo(std::string_view keywordUtf8) const {
    // 酷我接口返回逐行 JSON，需要转换成标准 LRC 文本。
    const auto encoded = util::urlEncode(keywordUtf8);
    const auto headers = browserHeaders();
    const auto searchUrl = "https://kuwo.cn/search/searchMusicBykeyWord?vipver=1&client=kt&ft=music&cluster=0&strategy=2012&encoding=utf8&rformat=json&mobi=1&issubtitle=1&show_copyright_off=1&pn=0&rn=20&all=" + encoded;
    const auto search = parseJson(client_.get(searchUrl, headers).body);
    const auto* list = jsonAt(search, "abslist");
    if (!nonEmptyArray(list)) return {};
    const auto musicId = jsonString(jsonAt(jsonAt(list, 0), "DC_TARGETID"));
    if (musicId.empty()) return {};

    const auto lyricUrl = "https://kuwo.cn/openapi/v1/www/lyric/getlyric?musicId=" + musicId + "&httpsStatus=1&reqId=" + makeUuidV4() + "&plat=web_www&from=";
    const auto lyric = parseJson(client_.get(lyricUrl, headers).body);
    const auto* lrcList = jsonAt(jsonAt(lyric, "data"), "lrclist");
    if (!nonEmptyArray(lrcList)) return {};

    std::string out;
    for (const auto& item : *lrcList) {
        const auto line = jsonString(jsonAt(item, "lineLyric"));
        const auto timeText = jsonString(jsonAt(item, "time"));
        if (timeText.empty()) continue;
        try {
            out += formatLrcTimestamp(std::stod(timeText));
            out += line;
            out += "\n";
        } catch (...) {
        }
    }
    return toBytes(out);
}

std::vector<std::uint8_t> OnlineLyrics::fetchNetease(std::string_view keywordUtf8) const {
    // 网易云：EAPI 搜歌和取词，优先 YRC，失败再取 LRC/旧 API。
    const auto headers = browserHeaders();
    const auto session = cachedNeteaseSession(client_);
    auto id = fetchNeteaseEapiSongId(client_, keywordUtf8, session);
    if (id.empty()) {
        id = fetchNeteaseLegacySongId(client_, keywordUtf8, headers);
    }
    if (id.empty()) return {};

    nlohmann::json params;
    try {
        params["id"] = std::stoll(id);
    } catch (...) {
        params["id"] = id;
    }
    params["lv"] = "-1";
    params["tv"] = "-1";
    params["rv"] = "-1";
    params["yv"] = "-1";
    const auto lyric = decryptNeteaseEapiResponse(neteaseEapiPost(client_, "/eapi/song/lyric/v1", std::move(params), session));

    const auto yrc = jsonString(jsonAt(jsonAt(lyric, "yrc"), "lyric"));
    if (!yrc.empty()) {
        return toBytes(yrc);
    }

    const auto lrc = jsonString(jsonAt(jsonAt(lyric, "lrc"), "lyric"));
    if (!lrc.empty()) {
        return toBytes(lrc);
    }

    return fetchNeteaseLegacyLrc(client_, id, headers);
}

}
