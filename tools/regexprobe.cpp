// regexprobe.cpp —— 开发自检：书源规则是用户自己填的正则，万一写了灾难性回溯，
// 会不会把界面线程卡死？（只用来测行为，不进正式构建）
#include <cstdio>
#include <string>
#include <regex>
#include <chrono>
#include <windows.h>

static double Try(const wchar_t* pattern, const std::wstring& text, int maxHits)
{
    auto t0 = std::chrono::steady_clock::now();
    int hits = 0;
    try
    {
        std::wregex re(pattern, std::regex::ECMAScript | std::regex::optimize);
        std::wsregex_iterator it(text.begin(), text.end(), re), end;
        for (; it != end && hits < maxHits; ++it) hits++;
    }
    catch (const std::regex_error&)
    {
        wprintf(L"  (regex_error)\n");
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    wprintf(L"  命中=%d 用时=%lld ms\n", hits, (long long)ms);
    return (double)ms;
}

int wmain()
{
    std::wstring evil(120000, L'a');
    evil += L'b';                      /* (a+)+$ 面对"很多 a 加一个 b"就是经典回溯爆炸 */
    wprintf(L"文本长度=%zu\n", evil.size());

    wprintf(L"1) 灾难性回溯 (a+)+$\n");       Try(L"(a+)+$", evil, 100);
    wprintf(L"2) 灾难性回溯 (a|a)*$\n");      Try(L"(a|a)*$", evil, 100);
    wprintf(L"3) 正常规则 <a href=\"([^\"]+)\">\n");
    std::wstring page = L"<a href=\"/c1.html\">第一章</a>";
    for (int i = 0; i < 20000; i++) page += page.substr(0, 40);
    Try(L"<a href=\"([^\"]+)\">", page, 200);
    wprintf(L"4) 嵌套量词 (\\w+\\s*)+$\n");   Try(L"(\\w+\\s*)+$", std::wstring(20000, L'a') + L'!', 100);
    return 0;
}