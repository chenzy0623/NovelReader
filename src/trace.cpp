/* trace.cpp - 调试跟踪日志（只有调试版才编进来，正式版完全没有这段代码）
 *
 * 用途：排查"某条命令到底有没有执行""滚动间隔是多少"这类问题时，
 * 在代码里撒 NrTrace(...) 调用，运行时就会往 exe 同目录的 nr_trace.log
 * 追加一行日志（UTF-8，带毫秒时间戳），每条立刻落盘，就算程序随后崩了也能留下现场。
 *
 * 为什么正式版没有它：
 *   common.h 里 NR_TRACE 未定义时，NrTrace 被定义成一个空宏，
 *   这些调用连字符串常量都不会进最终二进制，所以既不影响体积也不拖性能。
 *
 * 怎么用：
 *   编译 build_dbg.bat 得到 build\NovelReader_dbg.exe（带 -DNR_TRACE），
 *   在源码里写 NrTrace(L"AUTO tick speed=%d off=%d", speed, off);
 *   跑完看 build\nr_trace.log 即可。
 *
 * 注意：底层用的是 wvsprintfW（不要和 CRT 的 vswprintf 混淆），
 * 它属于 User32 的老式 wsprintf 家族，**不支持 %llu / %I64d**，
 * 也不支持浮点 %f；只支持 %d %u %s %c %x 这类基础格式。
 * 遇到 64 位整数请自己拆成两个 32 位或者转成字符串。
 */
#include "common.h"
#include <cstdarg>
#include <cstdio>

#ifdef NR_TRACE

/* 写一条日志。参数是 printf 风格的宽字符格式串（受 wvsprintfW 的限制，见文件头）。
 * 任何一步失败都静默返回——调试日志绝对不能影响主流程。 */
void NrTrace(const wchar_t* fmt, ...)
{
    /* 单条日志上限 2000 字符，超了会被截断（wvsprintfW 不做边界检查，
     * 所以缓冲区必须给得比实际需要的宽裕些） */
    wchar_t buf[2000];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);   /* wsprintf 子集，不支持 %llu，够用 */
    va_end(ap);

    std::wstring line = buf;
    {
        /* 每行前缀一个毫秒级时间戳（从系统启动算，GetTickCount 约 49 天回绕一次）。
         * 有了它就能算"两次 AUTO tick 之间隔了多少毫秒"，用来验证定时器间隔。 */
        wchar_t ts[32];
        wsprintfW(ts, L"[%u] ", (unsigned)GetTickCount());
        line = std::wstring(ts) + line;
    }
    line += L"\r\n";

    /* 日志文件统一用 UTF-8（不带 BOM）落盘，用记事本 / VS Code 都能正常看中文。
     * 先问需要多少字节，再按需分配，避免写死缓冲区。 */
    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string u((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u[0], need, nullptr, nullptr);

    /* 日志路径：exe 所在目录\nr_trace.log。只算一次（static），之后复用。
     * 放在 exe 旁边而不是 %TEMP%，是为了调试时一眼就能找到。 */
    static std::wstring path;                 /* 首次调用时确定日志路径 */
    if (path.empty()) {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring s(exe, n);
        size_t k = s.find_last_of(L"\\/");
        if (k != std::wstring::npos) s = s.substr(0, k);
        path = s + L"\\nr_trace.log";
    }

    /* 每写一条就"打开→追加→关闭"：
     * 慢一点（调试版无所谓），但保证进程被强杀时日志不会丢在缓冲区里。 */
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, u.data(), (DWORD)u.size(), &wrote, nullptr);
    CloseHandle(h);
}

#endif /* NR_TRACE */
