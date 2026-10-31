/* srctest.cpp - 书源（51shucheng）自检工具：开发期用，不参与正式构建
 * ============================================================================
 * 用法：
 *   srctest.exe <关键词>            只搜索，打印结果列表
 *   srctest.exe <关键词> <序号>     搜索后抓第 <序号> 本（从 0 开始）的整本，打印统计
 *   srctest.exe --url <书籍页网址>  直接按网址抓整本（测 .com → .net 改写）
 *
 * 为什么要这个工具：GUI 里下载整本要几分钟，调试解析规则时用命令行几秒就能看到
 * "搜到几本 / 章节数 / 正文字数 / 前几段长什么样"，比截图快得多。
 * 输出走 UTF-8 + WriteFile：本机控制台是 GBK，wprintf 打中文会丢字。
 * ========================================================================== */
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/common.h"
#include "../src/web.h"

/* 按 UTF-8 写一段文字到标准输出（不受控制台代码页影响） */
static void Say(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u8((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], n, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u8.data(), (DWORD)u8.size(), &wrote, nullptr);
}

static void SayLine(const std::wstring& s) { Say(s + L"\n"); }

/* 进度回调：每 10 章打一行，避免刷屏。
 * g_limit > 0 时抓到第 g_limit 章就返回 false —— 相当于"取消"，
 * WebSourceFetchBook 会把已抓到的部分留着并正常返回，方便快速验证解析规则。 */
static int g_limit = 0;

static bool Progress(void*, int done, int total, const std::wstring& msg)
{
    if (!msg.empty()) {
        SayLine(L"  " + msg);
        /* 在这里判上限：WebSourceFetchBook 只在"开始抓 N/M"这一步看返回值，
         * 抓完一章后的那次空消息回调返回值是被忽略的，判在那儿没用。 */
        if (g_limit > 0 && done >= g_limit) {
            SayLine(L"  达到测试上限，停止抓取。");
            return false;
        }
        return true;
    }
    if (total > 0 && (done % 10 == 0 || done == total)) {
        wchar_t b[128];
        swprintf(b, 128, L"  进度 %d/%d", done, total);
        SayLine(b);
    }
    if (g_limit > 0 && done >= g_limit) {
        SayLine(L"  达到测试上限，停止抓取。");
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        SayLine(L"用法: srctest.exe <关键词> [序号]   或   srctest.exe --url <书籍页网址>");
        return 1;
    }

    /* 直接按网址抓：用来验证 .com → .net 改写和详情页解析 */
    if (std::wstring(argv[1]) == L"--url") {
        if (argc < 3) { SayLine(L"缺网址"); return 1; }
        std::wstring text, title, err;
        SayLine(L"抓取中…");
        DWORD t0 = GetTickCount();
        bool ok = WebSourceFetchBook(argv[2], text, title, err, Progress, nullptr);
        DWORD ms = GetTickCount() - t0;
        if (!ok) { SayLine(L"失败：" + err); return 2; }
        wchar_t b[256];
        swprintf(b, 256, L"OK 书名=%s 字数=%d 用时=%ums", title.c_str(), (int)text.size(), (unsigned)ms);
        SayLine(b);
        return 0;
    }

    /* 搜索 */
    std::vector<WebBookHit> hits;
    std::wstring err;
    SayLine(std::wstring(L"搜索：") + argv[1]);
    if (!WebSourceSearch(argv[1], hits, err)) {
        SayLine(L"搜索失败：" + err);
        return 2;
    }
    wchar_t b[128];
    swprintf(b, 128, L"共 %d 条结果：", (int)hits.size());
    SayLine(b);
    for (size_t i = 0; i < hits.size(); i++) {
        swprintf(b, 128, L"  [%d] ", (int)i);
        SayLine(std::wstring(b) + hits[i].title + L"  <" + hits[i].url + L">");
        if (!hits[i].desc.empty()) SayLine(L"       " + hits[i].desc);
    }

    if (argc >= 3) {
        int idx = _wtoi(argv[2]);
        if (argc >= 4) g_limit = _wtoi(argv[3]);
        if (idx < 0 || idx >= (int)hits.size()) { SayLine(L"序号越界"); return 1; }
        std::wstring text, title;
        SayLine(L"抓取第 " + std::to_wstring(idx) + L" 本…");
        DWORD t0 = GetTickCount();
        bool ok = WebSourceFetchBook(hits[idx].url, text, title, err, Progress, nullptr, hits[idx].title);
        DWORD ms = GetTickCount() - t0;
        if (!ok) { SayLine(L"失败：" + err); return 2; }
        swprintf(b, 128, L"OK 书名=%s 字数=%d 用时=%ums", title.c_str(), (int)text.size(), (unsigned)ms);
        SayLine(b);
        /* 打前 3 行，肉眼确认章节标题与正文没混进导航 */
        SayLine(L"--- 正文开头 ---");
        int lines = 0;
        size_t i = 0;
        while (i < text.size() && lines < 3) {
            size_t j = text.find(L'\n', i);
            if (j == std::wstring::npos) j = text.size();
            std::wstring line = text.substr(i, j - i);
            if (line.size() > 60) line = line.substr(0, 60) + L"…";
            SayLine(L"  " + line);
            lines++;
            if (j >= text.size()) break;
            i = j + 1;
        }
    }
    return 0;
}
