#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smtc::http {

struct HttpResponse {
    unsigned statusCode = 0;
    std::vector<std::uint8_t> body;
    std::string rawHeaders;

    std::string text() const {
        return {body.begin(), body.end()};
    }

    explicit operator bool() const {
        return statusCode >= 200 && statusCode < 300 && !body.empty();
    }
};

class HttpClient {
public:
    using Header = std::pair<std::string, std::string>;

    HttpResponse get(std::string_view url, const std::vector<Header>& headers = {}) const;
    HttpResponse post(std::string_view url, std::string_view body, const std::vector<Header>& headers = {}, std::string_view contentType = "application/x-www-form-urlencoded") const;

private:
    HttpResponse request(std::wstring_view method, std::string_view url, std::string_view body, const std::vector<Header>& headers, std::string_view contentType) const;
};

}
