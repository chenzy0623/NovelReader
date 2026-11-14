/* crashdump.cpp - 崩溃转储（说明见 crashdump.h）
 * ============================================================================
 * 实现要点：
 *   - SetUnhandledExceptionFilter 是进程级的"最后一道网"，覆盖主线程和所有工作线程；
 *   - MiniDumpNormal 只存栈和少量内存，几百 KB；加 WithThreadInfo 才能看到全部线程；
 *   - 写盘前把"转储里不能包含敏感信息"这件事想清楚：小说正文在内存里，正常迷你转储
 *     只包含栈和寄存器，不会把整本书带走，所以不做额外处理；
 *   - 崩溃时内存状态已不可信，所以这里只用栈上缓冲 + Win32 API，不碰 std::string
 *     （除了路径拼接，那是出问题也只会缺个文件名）。实际上用 std::wstring 拼路径是
 *     安全的：堆通常还没坏，坏的是逻辑状态。为可读性还是用 wstring。
 * ============================================================================ */
#include "crashdump.h"

#include "version.h"

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>   /* ShellExecuteW */
#include <shlobj.h>     /* SHCreateDirectoryExW */

#include <string>
#include <vector>

static LONG s_inHandler = 0;   /* 防重入：转储过程中再次崩溃就直接放过 */

std::wstring CrashDumpDir()
{
    wchar_t buf[MAX_PATH * 2];
    buf[0] = 0;
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::wstring();
    std::wstring dir(buf);
    dir += L"\\NovelReader\\crashes";
    return dir;
}

/* 只保留最近 keep 份 .dmp（文件名带时间戳，字典序就是时间序）。 */
static void PruneOld(const std::wstring& dir, int keep)
{
    std::wstring pat = dir + L"\\crash-*.dmp";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if ((int)names.size() <= keep) return;
    /* 文件名形如 crash-20261114-221500-1234.dmp，升序即从旧到新 */
    for (size_t i = 0; i + (size_t)keep < names.size(); i++) {
        std::wstring p = dir + L"\\" + names[i];
        DeleteFileW(p.c_str());
        std::wstring t = p;
        t.resize(t.size() - 4);
        t += L".txt";
        DeleteFileW(t.c_str());
    }
}

static void WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &s[0], n, nullptr, nullptr);
        DWORD wrote = 0;
        char bom[3] = {(char)0xEF, (char)0xBB, (char)0xBF};
        WriteFile(h, bom, 3, &wrote, nullptr);
        WriteFile(h, s.data(), (DWORD)s.size(), &wrote, nullptr);
    }
    CloseHandle(h);
}

/* 路径里不能有 ':' 之类的字符，时间戳统一用数字和 '-'。 */
static void StampNow(wchar_t* out, size_t cch)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(out, cch, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&s_inHandler, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;

    std::wstring dir = CrashDumpDir();
    if (dir.empty()) return EXCEPTION_EXECUTE_HANDLER;

    /* 目录可能还不存在：逐级建（SHCreateDirectoryExW 会连带建父目录） */
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    wchar_t stamp[32];
    StampNow(stamp, 32);
    wchar_t name[MAX_PATH];
    swprintf(name, MAX_PATH, L"crash-%s-%lu", stamp, (unsigned long)GetCurrentProcessId());

    std::wstring dmpPath = dir + L"\\" + name + L".dmp";
    std::wstring txtPath = dir + L"\\" + name + L".txt";

    DWORD code = 0;
    const void* addr = nullptr;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = ep->ExceptionRecord->ExceptionAddress;
    }

    bool dumped = false;
    HANDLE file = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                                             MiniDumpWithDataSegs);
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                   ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(file);
        if (!dumped) DeleteFileW(dmpPath.c_str());
    }

    /* 摘要：用户能直接复制粘贴到 issue 里的那种 */
    wchar_t summary[1024];
    swprintf(summary, 1024,
             L"阅读器崩溃报告\r\n"
             L"版本：%s\r\n"
             L"时间：%s\r\n"
             L"进程：%lu\r\n"
             L"异常码：0x%08X\r\n"
             L"异常地址：%p\r\n"
             L"系统：%s\r\n"
             L"转储文件：%s\r\n",
             NR_VERSION_STR, stamp, (unsigned long)GetCurrentProcessId(),
             (unsigned)code, addr, L"(见 dmp)", dumped ? dmpPath.c_str() : L"(写入失败)");
    WriteTextFile(txtPath, summary);

    PruneOld(dir, 5);

    std::wstring msg = L"阅读器遇到了一个错误，马上就退出。\r\n\r\n";
    if (dumped) {
        msg += L"崩溃信息已保存到：\r\n";
        msg += dmpPath;
        msg += L"\r\n\r\n";
    } else {
        msg += L"（崩溃转储没能写出来，可能目录没有写权限。）\r\n\r\n";
    }
    msg += L"把这个文件发给开发者，就能定位问题。\r\n\r\n现在打开所在文件夹吗？";

    if (MessageBoxW(nullptr, msg.c_str(), L"出错了", MB_YESNO | MB_ICONERROR) == IDYES) {
        std::wstring arg = L"/select,\"" + dmpPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }

    return EXCEPTION_EXECUTE_HANDLER;   /* 交给系统默认处理（进程退出） */
}

void CrashDumpInstall()
{
    SetUnhandledExceptionFilter(CrashFilter);
}
