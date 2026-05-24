#include "http/HttpClient.h"

#include "util/Encoding.h"

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <string>

namespace smtc::http {
namespace {

struct InternetHandleDeleter {
    void operator()(HINTERNET handle) const {
        if (handle) WinHttpCloseHandle(handle);
    }
};
using InternetHandle = std::unique_ptr<void, InternetHandleDeleter>;

std::wstring headersToWide(const std::vector<HttpClient::Header>& headers, std::string_view contentType, bool hasBody) {
    std::wstring result;
    for (const auto& [name, value] : headers) {
        result += util::utf8ToWide(name);
        result += L": ";
        result += util::utf8ToWide(value);
        result += L"\r\n";
    }
    if (hasBody && !contentType.empty()) {
        result += L"Content-Type: ";
        result += util::utf8ToWide(contentType);
        result += L"\r\n";
    }
    return result;
}

std::wstring componentToString(const URL_COMPONENTS& components, const wchar_t* ptr, DWORD length) {
    if (!ptr || length == 0) return {};
    return {ptr, ptr + length};
}

}

HttpResponse HttpClient::get(std::string_view url, const std::vector<Header>& headers) const {
    return request(L"GET", url, {}, headers, {});
}

HttpResponse HttpClient::post(std::string_view url, std::string_view body, const std::vector<Header>& headers, std::string_view contentType) const {
    return request(L"POST", url, body, headers, contentType);
}

HttpResponse HttpClient::request(std::wstring_view method, std::string_view url, std::string_view body, const std::vector<Header>& headers, std::string_view contentType) const {
    HttpResponse response;
    const auto wideUrl = util::utf8ToWide(url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
        return response;
    }

    const auto host = componentToString(components, components.lpszHostName, components.dwHostNameLength);
    auto path = componentToString(components, components.lpszUrlPath, components.dwUrlPathLength);
    path += componentToString(components, components.lpszExtraInfo, components.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    InternetHandle session(WinHttpOpen(L"SMTCLyrics/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return response;

    InternetHandle connect(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connect) return response;

    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle req(WinHttpOpenRequest(connect.get(), std::wstring(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req) return response;

    const DWORD timeout = 10000;
    WinHttpSetTimeouts(req.get(), timeout, timeout, timeout, timeout);

    const auto headerText = headersToWide(headers, contentType, !body.empty());
    LPVOID bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<LPVOID>(const_cast<char*>(body.data()));
    const DWORD bodySize = static_cast<DWORD>(body.size());
    const BOOL sent = WinHttpSendRequest(
        req.get(),
        headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
        headerText.empty() ? 0 : static_cast<DWORD>(headerText.size()),
        bodyPtr,
        bodySize,
        bodySize,
        0);
    if (!sent || !WinHttpReceiveResponse(req.get(), nullptr)) {
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = status;

    DWORD rawHeaderSize = 0;
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &rawHeaderSize, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && rawHeaderSize > 0) {
        std::wstring rawHeaders(static_cast<std::size_t>(rawHeaderSize / sizeof(wchar_t)), L'\0');
        if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, rawHeaders.data(), &rawHeaderSize, WINHTTP_NO_HEADER_INDEX)) {
            rawHeaders.resize(wcslen(rawHeaders.c_str()));
            response.rawHeaders = util::wideToUtf8(rawHeaders);
        }
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &available) || available == 0) break;
        const auto oldSize = response.body.size();
        response.body.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(req.get(), response.body.data() + oldSize, available, &read)) break;
        response.body.resize(oldSize + read);
    }

    return response;
}

}
