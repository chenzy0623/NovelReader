/* regextest.cpp - 开发小工具：拿 std::regex 在真实网页上试规则
 * ============================================================================
 * 为什么需要它：书源规则（结果页 / 目录 / 正文）是 std::regex 的 ECMAScript
 * 语法，和 PowerShell/.NET 的写法有细微差别（尤其惰性量词、反向引用、\s 的
 * 定义），用别的工具试出来的"能用"到程序里经常不认。这个程序用的就是
 * web.cpp 里那一套 std::regex，试通了就一定能用。
 *
 * 用法：
 *   regextest <html 文件> <规则> [组数]
 * 打印每一处匹配的第 1..组数 个捕获组。
 * ============================================================================ */
#include <windows.h>

#include <cstdio>
#include <regex>
#include <string>

static std::string ReadAll(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string out((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return out;
}

/* 参数按 UTF-16 传进来（main 用 wmain），正则和文本都转成 UTF-8 再用 std::regex
 * 匹配 —— 与 web.cpp 一致：网页字节流是 UTF-8，规则也是 UTF-8 字面量。 */
static std::string W2U(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::printf("usage: regextest <html-file> <pattern> [groups]\n");
        return 2;
    }
    std::string body = ReadAll(W2U(argv[1]).c_str());
    if (body.empty()) {
        std::printf("cannot read file\n");
        return 2;
    }
    int groups = (argc > 3) ? _wtoi(argv[3]) : 3;
    std::string pat = W2U(argv[2]);
    /* 规则里有引号时，命令行很容易被 shell 吃掉（PowerShell 会剥引号，cmd 会
     * 把空格当分隔符）。以 @ 开头表示"规则放在这个文件里"，直接读文件最省事。 */
    if (!pat.empty() && pat[0] == '@') {
        pat = ReadAll(pat.c_str() + 1);
        while (!pat.empty() && (pat.back() == '\n' || pat.back() == '\r')) pat.pop_back();
        if (pat.empty()) {
            std::printf("cannot read pattern file\n");
            return 2;
        }
    }

    std::printf("file=%zu bytes pattern=%s\n", body.size(), pat.c_str());
    try {
        std::regex re(pat, std::regex::ECMAScript);
        int shown = 0;
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re), end = std::sregex_iterator();
             it != end; ++it) {
            const std::smatch& m = *it;
            std::printf("[%d] ", shown);
            for (int g = 1; g <= groups && g < (int)m.size(); g++) {
                std::string s = m[g].str();
                if (s.size() > 70) s = s.substr(0, 70) + "…";
                std::printf("(%d=%s) ", g, s.c_str());
            }
            std::printf("\n");
            if (++shown >= 5) break;
        }
        if (shown == 0) std::printf("NO MATCH\n");
        std::printf("total=%d\n", shown);
    } catch (const std::exception& e) {
        std::printf("regex error: %s\n", e.what());
        return 3;
    }
    return 0;
}
