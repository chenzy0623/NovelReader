/* update.cpp - 检查更新（说明见 update.h）
 * ============================================================================
 * 这里刻意把"解析 JSON"和"联网"分开：
 *   UpdateParseReleaseJson 是纯函数，tests\ 里能直接喂字符串断言；
 *   UpdateCheck 只负责把网址抓下来再交给它。
 * 这样即使将来换 API（GitHub → 自建接口），需要重新验证的也只有前者。
 *
 * 为什么用"手抠字符串"而不是完整 JSON 解析器：
 *   GitHub 的 release 响应有几十个字段、嵌套好几层，而我们只要三个字符串。
 *   sourcejson.cpp 里已经有一个小解析器，但它是给书源文件用的、在另一个编译单元；
 *   为了三个字段再链接一份没必要 —— 这里的查找逻辑总共 20 行，且对"字段顺序、
 *   多余空白、转义"都做了最少限度的处理。
 * ============================================================================ */
#include "update.h"

#include "version.h"
#include "web.h"        /* WebFetchUrl */

#include <string>
#include <vector>

/* release 仓库地址。改仓库名只改这里。 */
static const wchar_t* kRepo = L"chenzy0623/NovelReader";

int VersionParse(const wchar_t* text)
{
    if (!text) return -1;
    const wchar_t* p = text;
    /* 跳过前缀（"v"、"version "、"release-" 之类），找到第一个数字 */
    while (*p && !(*p >= L'0' && *p <= L'9')) p++;
    if (!*p) return -1;

    int part[3] = {0, 0, 0};
    int idx = 0;
    bool any = false;
    while (*p && idx < 3) {
        if (*p >= L'0' && *p <= L'9') {
            /* 防止 "18446744073709551616" 这种超长数字把 int 撑爆 */
            if (part[idx] < 100000) part[idx] = part[idx] * 10 + (int)(*p - L'0');
            any = true;
            p++;
        } else if (*p == L'.' || *p == L'_' || *p == L'-') {
            idx++;
            p++;
        } else {
            break;
        }
    }
    if (!any) return -1;
    /* "1.8" 当 1.8.0；"1" 当 1.0.0 */
    return part[0] * 10000 + part[1] * 100 + part[2];
}

/* 从 json 里找 "key": "value" 的字符串值。找不到返回空串。
 * 只处理转义了引号的情况（GitHub 的 url/tag 里不会出现别的转义）。 */
static std::wstring JsonStringField(const std::wstring& json, const wchar_t* key)
{
    std::wstring pat = L"\"";
    pat += key;
    pat += L"\"";
    size_t p = json.find(pat);
    if (p == std::wstring::npos) return std::wstring();
    p = json.find(L':', p + pat.size());
    if (p == std::wstring::npos) return std::wstring();
    p = json.find(L'"', p + 1);
    if (p == std::wstring::npos) return std::wstring();
    p++;
    std::wstring out;
    while (p < json.size()) {
        wchar_t c = json[p];
        if (c == L'"') return out;
        if (c == L'\\' && p + 1 < json.size()) {
            wchar_t e = json[p + 1];
            if (e == L'"') { out.push_back(L'"'); p += 2; continue; }
            if (e == L'\\') { out.push_back(L'\\'); p += 2; continue; }
            if (e == L'n') { out.push_back(L'\n'); p += 2; continue; }
            if (e == L'r') { out.push_back(L'\r'); p += 2; continue; }
            if (e == L't') { out.push_back(L'\t'); p += 2; continue; }
            if (e == L'u' && p + 5 < json.size()) {
                /* \uXXXX：只处理基本拉丁范围以外也照收，够用了 */
                unsigned v = 0;
                bool bad = false;
                for (int k = 0; k < 4; k++) {
                    wchar_t h = json[p + 2 + (size_t)k];
                    unsigned d;
                    if (h >= L'0' && h <= L'9') d = (unsigned)(h - L'0');
                    else if (h >= L'a' && h <= L'f') d = (unsigned)(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') d = (unsigned)(h - L'A' + 10);
                    else { bad = true; break; }
                    v = (v << 4) | d;
                }
                if (!bad) out.push_back((wchar_t)v);
                p += 6;
                continue;
            }
            p += 2;
            continue;
        }
        out.push_back(c);
        p++;
    }
    return std::wstring();
}

bool UpdateParseReleaseJson(const std::wstring& json, UpdateInfo& out)
{
    out = UpdateInfo();
    if (json.empty()) {
        out.err = L"服务器返回了空内容。";
        return false;
    }
    /* API 限流 / 仓库不存在时返回的是 {"message": "..."} */
    if (json.find(L"\"tag_name\"") == std::wstring::npos) {
        std::wstring msg = JsonStringField(json, L"message");
        if (!msg.empty()) {
            out.err = L"GitHub 说：" + msg;
        } else {
            out.err = L"返回内容里没有版本号（可能不是 release 接口，或者仓库还没发过 release）。";
        }
        return false;
    }

    out.tag = JsonStringField(json, L"tag_name");
    out.url = JsonStringField(json, L"html_url");
    out.name = JsonStringField(json, L"name");
    out.latest = VersionParse(out.tag.c_str());
    if (out.url.empty()) {
        /* html_url 正常情况下一定有；万一没有就用 releases 页面兜底 */
        out.url = std::wstring(L"https://github.com/") + kRepo + L"/releases";
    }
    if (out.latest < 0) {
        out.err = L"版本号看不懂：" + out.tag;
        return false;
    }
    out.ok = true;
    return true;
}

UpdateInfo UpdateCheck(int timeoutMs)
{
    UpdateInfo info;
    std::wstring url = std::wstring(L"https://api.github.com/repos/") + kRepo + L"/releases/latest";

    std::string body;
    std::wstring finalUrl, err;
    if (!WebFetchUrl(url, body, finalUrl, err, timeoutMs)) {
        info.err = err.empty() ? std::wstring(L"连不上 GitHub。") : err;
        return info;
    }

    /* body 是 UTF-8 字节：转成宽字符再解析（tag/url 都是 ASCII，但 name 可能有中文） */
    std::wstring text;
    if (!body.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
        if (n > 0) {
            text.resize((size_t)n);
            MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), &text[0], n);
        }
    }
    UpdateParseReleaseJson(text, info);
    return info;
}
