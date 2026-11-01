/* tools/synctest.cpp —— 开发自检工具（不随成品发布）
 *
 * 作用：假装成"手机端"，用来在没有真手机的情况下验证电脑端的 P2P 同步。
 *
 *   用法：synctest.exe <书文件> <字符偏移> <千分比> <章名>
 *
 * 它会：
 *   1) 用和电脑端完全相同的算法算出这本书的指纹（SyncBookKey）；
 *   2) 在 TCP 8245 上开一个应答器，/peer/books 返回一条记录；
 *   3) 每 2 秒往 UDP 8234 广播 "NR1|8245|phone"，让电脑端发现"手机"；
 *   4) 把收到的每个请求打印出来（用 UTF-8 写 stdout，GBK 控制台会丢中文）。
 *
 * 编译（需要带上 book.cpp 和 sync.cpp）：
 *   g++ -std=gnu++17 -O1 -DUNICODE -D_UNICODE -municode -I src -o build\synctest.exe \
 *       tools\synctest.cpp src\book.cpp src\sync.cpp src\format.cpp src\zip.cpp \
 *       src\pdf.cpp src\layout.cpp src\ini.cpp -static -lwininet -lws2_32 -liphlpapi \
 *       -lgdiplus -lshcore -lruntimeobject -lcomctl32 -lcomdlg32 -lshell32 -lole32
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <string>

#include "../src/common.h"
#include "../src/sync.h"

/* 输出到 stdout 一律走 UTF-8：中文 Windows 的控制台是 GBK，
 * 直接 wprintf 中文会被丢掉，看不到调试信息。 */
static void Say(const std::wstring& s)
{
    std::wstring line = s + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string u8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u8[0], n, nullptr, nullptr);
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u8.data(), (DWORD)u8.size(), &wrote, nullptr);
}

static const int kTcpPort = 8245;   /* 冒充手机的端口，和电脑端的 8233 区分开 */
static const int kUdpPort = 8234;

static std::string g_line;          /* 要回的记录行（已经按协议编码好） */

static std::string PctEncode(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string u8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], n, nullptr, nullptr);
    std::string out;
    for (size_t i = 0; i < u8.size(); i++)
    {
        unsigned char c = (unsigned char)u8[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '%')
        {
            char b[8];
            wsprintfA(b, "%%%02X", c);
            out += b;
        }
        else out += (char)c;
    }
    return out;
}

static DWORD WINAPI CastThread(LPVOID param)
{
    SOCKET u = *(SOCKET*)param;
    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)kUdpPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char msg[64];
    wsprintfA(msg, "NR1|%d|phone", kTcpPort);
    for (;;)
    {
        sendto(u, msg, (int)strlen(msg), 0, (sockaddr*)&to, sizeof(to));
        Sleep(2000);
    }
    return 0;
}

static void Respond(SOCKET c, int code, const char* reason, const std::string& body)
{
    char head[256];
    wsprintfA(head, "HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
              code, reason, (int)body.size());
    send(c, head, (int)strlen(head), 0);
    if (!body.empty()) send(c, body.c_str(), (int)body.size(), 0);
}

int wmain(int argc, wchar_t** argv)
{
    /* 客户端自检模式：synctest.exe --fetch 主机|端口
     * 用来单独验证 SyncFetch（电脑端用的就是同一个函数），
     * 这样即使 GUI 那边的对话框没弹出来，也能确认网络这层通不通。 */
    if (argc >= 3 && std::wstring(argv[1]) == L"--fetch")
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Say(L"WSAStartup 失败"); return 2; }
        std::vector<SyncRec> recs;
        std::wstring err;
        if (!SyncFetch(argv[2], recs, err))
        {
            Say(L"SyncFetch 失败：" + err);
            return 1;
        }
        Say(L"SyncFetch 成功，拿到 " + std::to_wstring(recs.size()) + L" 条：");
        for (size_t i = 0; i < recs.size(); i++)
        {
            Say(L"  key=" + recs[i].key + L"  title=" + recs[i].title +
                L"  off=" + std::to_wstring(recs[i].off) +
                L"  pct=" + std::to_wstring(recs[i].pct) +
                L"  ts=" + std::to_wstring(recs[i].ts) +
                L"  chapter=" + recs[i].chapter);
        }
        return 0;
    }

    if (argc < 5)
    {
        Say(L"用法：synctest.exe <书文件> <字符偏移> <千分比> <章名>");
        Say(L"      synctest.exe --fetch <主机|端口>     （只测客户端取记录）");
        return 2;
    }

    /* 1) 读同一本书、算同一个指纹 */
    Book b;
    std::wstring err;
    if (!BookLoad(b, argv[1], &err))
    {
        Say(L"打不开这本书：" + err);
        return 2;
    }
    std::wstring key = SyncBookKey(b.title, b.text);
    int off = _wtoi(argv[2]);
    int pct = _wtoi(argv[3]);
    std::wstring chapter = argv[4];

    Say(L"书：" + b.title + L"（" + std::to_wstring(b.text.size()) + L" 字符）");
    Say(L"指纹：" + key + (key.empty() ? L"  ← 太短，不参与同步！" : L""));
    Say(L"冒充手机进度：" + std::to_wstring(off) + L" 字符 / " + std::to_wstring(pct) + L"‰ / " + chapter);

    /* 2) 拼好要回的那一行 */
    char num[64];
    g_line = PctEncode(key);
    g_line += '\t';
    g_line += PctEncode(b.title);
    g_line += '\t';
    wsprintfA(num, "%d", off);
    g_line += num;
    g_line += '\t';
    wsprintfA(num, "%d", pct);
    g_line += num;
    g_line += '\t';
    wsprintfA(num, "%d", (int)time(nullptr));
    g_line += num;
    g_line += '\t';
    g_line += PctEncode(chapter);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Say(L"WSAStartup 失败"); return 2; }

    int one = 1;
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    HANDLE th = CreateThread(nullptr, 0, CastThread, &udp, 0, nullptr);
    (void)th;

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)kTcpPort);
    if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0) { Say(L"绑定 8245 失败"); return 2; }
    listen(srv, 8);
    Say(L"已开始冒充手机：TCP 8245，UDP 广播 8234，每 2 秒一次");

    for (;;)
    {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;

        char buf[4096];
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            buf[n] = 0;
            std::string req(buf);
            size_t sp1 = req.find(' ');
            size_t sp2 = req.find(' ', sp1 + 1);
            std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                                   ? req.substr(sp1 + 1, sp2 - sp1 - 1) : std::string();
            Say(L"收到请求：" + std::wstring(path.begin(), path.end()));

            if (path == "/peer/books")
                Respond(c, 200, "OK", g_line + "\n");
            else if (path == "/peer/ping")
                Respond(c, 200, "OK", "NRPEER 1\n");
            else if (path.compare(0, 11, "/peer/progress") == 0 || path == "/peer/progress")
                Respond(c, 200, "OK", "ok\n");
            else
                Respond(c, 404, "Not Found", "no\n");
        }
        closesocket(c);
    }
    return 0;
}
