/* loadtest.cpp - 开发用自检：加载 txt，打印编码/段落/章节/搜索命中情况（不随成品发布） */
#include "../src/common.h"
#include <cstdio>

static void Out(const std::wstring& s)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string u((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], need, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u.data(), (DWORD)u.size(), &wrote, nullptr);
}

static void Ln(const std::wstring& s)
{
    Out(s + L"\n");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { Ln(L"usage: loadtest <file.txt> [keyword]"); return 2; }
    Book b;
    std::wstring err;
    ULONGLONG t0 = GetTickCount64();
    bool ok = BookLoad(b, argv[1], &err);
    ULONGLONG t1 = GetTickCount64();
    if (!ok) { Ln(L"FAIL: " + err); return 1; }

    wchar_t buf[256];
    wsprintfW(buf, L"ok  %llu ms  chars=%d  paras=%d  chapters=%d",
              (unsigned long long)(t1 - t0), (int)b.text.size(), b.ParaCount(), (int)b.chapters.size());
    Ln(buf);
    Ln(L"title: " + b.title);

    int n = (int)b.chapters.size() < 8 ? (int)b.chapters.size() : 8;
    for (int i = 0; i < n; i++) {
        wchar_t t[400];
        wsprintfW(t, L"  ch[%d] para=%d off=%d  ", i, b.chapters[(size_t)i].para, b.chapters[(size_t)i].off);
        std::wstring line = t;
        line += BookChapterTitle(b, i);
        Ln(line);
    }
    if (b.chapters.size() > 8) {
        wchar_t t[400];
        wsprintfW(t, L"  ... last ch[%d] para=%d  ", (int)b.chapters.size() - 1, b.chapters.back().para);
        std::wstring line = t;
        line += BookChapterTitle(b, (int)b.chapters.size() - 1);
        Ln(line);
    }

    /* 段落前 3 行样例 */
    for (int p = 0; p < 3 && p < b.ParaCount(); p++) {
        std::wstring s = BookParaText(b, p);
        if (s.size() > 60) s = s.substr(0, 60) + L"…";
        wchar_t t[64];
        wsprintfW(t, L"  para[%d] len=%d: ", p, (int)BookParaText(b, p).size());
        Ln(std::wstring(t) + s);
    }

    if (argc >= 3) {
        std::vector<Hit> hits;
        ULONGLONG s0 = GetTickCount64();
        BookSearch(b, argv[2], hits, 4000);
        ULONGLONG s1 = GetTickCount64();
        wsprintfW(buf, L"search '%s': %d hits in %llu ms", argv[2], (int)hits.size(),
                  (unsigned long long)(s1 - s0));
        Ln(buf);
        for (int i = 0; i < 5 && i < (int)hits.size(); i++) {
            const Hit& h = hits[(size_t)i];
            std::wstring ctx = b.text.substr((size_t)h.off, (size_t)h.len);
            wchar_t t[256];
            wsprintfW(t, L"  hit[%d] off=%d ch=%d len=%d '", i, h.off, h.chapter, h.len);
            Ln(std::wstring(t) + ctx + L"'");
        }
    }
    return 0;
}
