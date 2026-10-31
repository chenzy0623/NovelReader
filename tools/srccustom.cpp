/* srccustom.cpp —— 开发自检工具（不参与正式构建）
 *
 * 目的：验证"书源规则完全由用户配置"这件事真的成立 ——
 * 这里用一个与内置源结构完全不同的测试站（不同的标签、不同的网址形状），
 * 只靠一组正则规则把它搜出来、把目录解析出来、把正文抽出来。
 *
 * 用法：
 *   build\srccustom.exe <搜索页基础网址> <关键词>
 * 例：
 *   build\srccustom.exe http://127.0.0.1:8777/search?q={kw} 凡人
 *
 * 编译：
 *   g++ -std=gnu++17 -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -I src -municode \
 *       -o build\srccustom.exe tools\srccustom.cpp src\web.cpp src\book.cpp \
 *       src\format.cpp src\zip.cpp src\pdf.cpp src\source.cpp src\ini.cpp \
 *       -static -lwininet -luser32
 */

#include "../src/common.h"
#include "../src/web.h"
#include "../src/source.h"

#include <cstdio>
#include <string>

/* 控制台是 GBK 的，直接 wprintf 中文会被丢掉，统一转 UTF-8 再 WriteFile */
static void Say(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);

    int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string u8((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &u8[0], need, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u8.c_str(), (DWORD)(u8.size() - 1), &wrote, nullptr);
}

/* 针对测试站写的一组规则：标签名、网址形状、正文容器都与内置源不同 */
static BookSource MakeTestSource(const std::wstring& searchUrl)
{
    BookSource s;
    s.name = L"测试站（自定义规则）";
    s.search = searchUrl;                       /* 里面有 {kw} 占位 */
    s.listRe = L"<a href=\"(/book\\d+)\">([^<]+)</a></span></dt><dd>([^<]*)</dd>";
    s.tocStart = L"id=\"toc\"";                 /* 只在目录容器里找章节 */
    s.tocRe = L"<a href=\"(/c\\d+)\">([^<]+)</a>";
    s.contentRe = L"<article class=\"txt\">([\\s\\S]*?)</article>";
    s.builtin = false;
    return s;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        Say(L"用法：srccustom.exe <搜索页网址（含 {kw}）> <关键词>\n");
        return 1;
    }
    BookSource src = MakeTestSource(argv[1]);

    std::vector<WebBookHit> hits;
    std::wstring err;
    Say(L"用自定义规则搜索：%s\n", argv[2]);
    if (!WebSourceSearchEx(src, argv[2], hits, err)) {
        Say(L"搜索失败：%s\n", err.c_str());
        return 2;
    }
    Say(L"搜到 %d 条：\n", (int)hits.size());
    for (size_t i = 0; i < hits.size(); i++) {
        Say(L"  [%d] %s <%s>\n      %s\n", (int)i, hits[i].title.c_str(),
            hits[i].url.c_str(), hits[i].desc.c_str());
    }

    std::wstring text, title;
    Say(L"抓第 0 本…\n");
    if (!WebSourceFetchBookEx(src, hits[0].url, text, title, err, nullptr, nullptr, hits[0].title)) {
        Say(L"抓取失败：%s\n", err.c_str());
        return 3;
    }
    Say(L"书名=%s 字数=%d\n--- 正文 ---\n%s\n", title.c_str(), (int)text.size(), text.c_str());
    return 0;
}
