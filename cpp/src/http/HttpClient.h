#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smtc::http {

// WinHTTP 请求结果：状态码、响应体和原始响应头。
struct HttpResponse {
    unsigned statusCode = 0;
    std::vector<std::uint8_t> body;
    std::string rawHeaders;

    // 在线歌词接口大多返回文本/JSON/XML，直接把 body 视作字节串。
    std::string text() const {
        return {body.begin(), body.end()};
    }

    // 简化成功判断：2xx 且响应体非空。
    explicit operator bool() const {
        return statusCode >= 200 && statusCode < 300 && !body.empty();
    }
};

// 最小封装的同步 HTTP 客户端，供在线歌词源调用。
class HttpClient {
public:
    using Header = std::pair<std::string, std::string>;

    // GET/POST 都接受 UTF-8 URL 和 UTF-8 请求头。
    HttpResponse get(std::string_view url, const std::vector<Header>& headers = {}) const;
    HttpResponse post(std::string_view url, std::string_view body, const std::vector<Header>& headers = {}, std::string_view contentType = "application/x-www-form-urlencoded") const;

private:
    // 统一请求实现：负责拆 URL、建 WinHTTP 句柄、发送请求和收集响应。
    HttpResponse request(std::wstring_view method, std::string_view url, std::string_view body, const std::vector<Header>& headers, std::string_view contentType) const;
};

}
