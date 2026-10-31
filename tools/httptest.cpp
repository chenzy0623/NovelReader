/* httptest.cpp - 抓取链路探测工具（开发期用，不参与正式构建）
 * ============================================================================
 * 背景：无忧书城挂在 Cloudflare 后面，用 WinInet 发的请求会被 403 拦掉，
 * 而 curl（OpenSSL + 浏览器头）能拿到 200。这个工具用来试"换 WinHTTP 能不能过"。
 * 只打印状态码与前 80 字节，不动磁盘。
 * 注意：winhttp.h 与 wininet.h 在 mingw 下不能同时包含（INTERNET_SCHEME 重复定义），
 * 所以这个工具只测 WinHTTP。
 * ========================================================================== */
#include <windows.h>
#include <winhttp.h>
#include <string>

static void Say(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u8((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], n, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u8.data(), (DWORD)u8.size(), &wrote, nullptr);
}

/* 浏览器同款请求头：Cloudflare 会看这几个 Sec-Fetch-* 和 Upgrade-Insecure-Requests */
static const wchar_t* kHeaders =
    L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
    L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
    L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n"
    L"Upgrade-Insecure-Requests: 1\r\n"
    L"Sec-Fetch-Dest: document\r\n"
    L"Sec-Fetch-Mode: navigate\r\n"
    L"Sec-Fetch-Site: none\r\n"
    L"Sec-Fetch-User: ?1\r\n";

static bool TryWinHttp(const std::wstring& url)
{
    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120.0.0.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { Say(L"WinHttp: 会话创建失败 err=" + std::to_wstring(GetLastError()) + L"\n"); return false; }
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host;  uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { Say(L"WinHttp: 网址拆不开\n"); WinHttpCloseHandle(ses); return false; }

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) { Say(L"WinHttp: 连接失败\n"); WinHttpCloseHandle(ses); return false; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { Say(L"WinHttp: 请求创建失败\n"); WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false; }

    if (!WinHttpSendRequest(req, kHeaders, (DWORD)-1L, nullptr, 0, 0, 0)) {
        Say(L"WinHttp: 发送失败 err=" + std::to_wstring(GetLastError()) + L"\n");
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return false;
    }
    WinHttpReceiveResponse(req, nullptr);
    DWORD code = 0, len = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    char buf[512] = {0};
    DWORD got = 0;
    WinHttpReadData(req, buf, 400, &got);
    Say(L"WinHTTP 状态=" + std::to_wstring(code) + L" 读到字节=" + std::to_wstring(got) + L"\n");
    if (got) {
        /* 内容可能是 UTF-8 中文，按 UTF-8 转回宽字符再输出，避免乱码 */
        int wn = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, nullptr, 0);
        std::wstring w((size_t)(wn > 0 ? wn : 0), L'\0');
        if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, &w[0], wn);
        if (w.size() > 120) w = w.substr(0, 120) + L"…";
        Say(L"  内容: " + w + L"\n");
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
    return code == 200;
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring url = argc > 1 ? argv[1] : L"https://www.51shucheng.net/";
    Say(L"测试网址: " + url + L"\n");
    TryWinHttp(url);
    return 0;
}
