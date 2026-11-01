/* sync.cpp —— 电脑 ↔ 手机的阅读进度同步（P2P，无固定服务端）
 *
 * 结构分四块：
 *   1. 百分号编解码 + 记录行格式（协议的最小单元）
 *   2. 书的指纹（两边算出同一个 key 才算"同一本书"）
 *   3. 网络：UDP 广播发现 + TCP 监听（HTTP 应答）
 *   4. 对外的读/写记录函数（供 ui.cpp 调用）
 *
 * 线程模型：UDP 发现线程、广播线程、TCP 接收线程各一个，都是死循环 + 停止标志，
 * SyncStop 里置标志再关套接字把它们踢出来（和 server.cpp 的做法一致）。
 * 所有对 ini 的读写都发生在 TCP 线程或界面线程里，Winsock 不参与，因此不需要
 * 额外的锁；ini 的写入本来就有系统级的串行化。
 */
#include "sync.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include "common.h"

namespace
{
/* ============================ 1. 编解码 ================================== */

/* 只有这三个字符会破坏"一行一条记录 / 制表符分字段"的格式，其余原样输出。
 * 中文在 UTF-8 里是多字节，百分号编码按字节做，所以中文会被编成一串 %XX —— 这是
 * 故意便宜的代价（记录很短），换来的是解析逻辑简单到不用写转义状态机。 */
std::string PctEncode(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string u8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], n, nullptr, nullptr);

    std::string out;
    out.reserve(u8.size() + 16);
    for (size_t i = 0; i < u8.size(); i++)
    {
        unsigned char c = (unsigned char)u8[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '%')
        {
            char buf[8];
            wsprintfA(buf, "%%%02X", c);
            out += buf;
        }
        else
        {
            out += (char)c;
        }
    }
    return out;
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::wstring PctDecode(const std::string& s)
{
    std::string u8;
    u8.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            int h = HexVal(s[i + 1]), l = HexVal(s[i + 2]);
            if (h >= 0 && l >= 0) { u8 += (char)((h << 4) | l); i += 2; continue; }
        }
        u8 += s[i];
    }
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &w[0], n);
    return w;
}

/* 纯 UTF-8 字节 → 宽字符。参数已经从 URL 里解出来了，这里不再处理 %XX */
std::wstring Utf8ToW(const std::string& u8)
{
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &w[0], n);
    return w;
}

/* 一条记录编成一行：key<TAB>title<TAB>off<TAB>pct<TAB>ts<TAB>chapter */
std::string RecToLine(const SyncRec& r)
{
    std::string line = PctEncode(r.key);
    line += '\t';
    line += PctEncode(r.title);
    line += '\t';
    char num[64];
    wsprintfA(num, "%d", r.off);
    line += num;
    line += '\t';
    wsprintfA(num, "%d", r.pct);
    line += num;
    line += '\t';
    wsprintfA(num, "%d", r.ts);
    line += num;
    line += '\t';
    line += PctEncode(r.chapter);
    return line;
}

/* 解析一行；字段不够就返回 false（截断的请求不应该让程序崩掉）。 */
bool LineToRec(const std::string& line, SyncRec& r)
{
    std::vector<std::string> f;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] == '\t') { f.push_back(cur); cur.clear(); }
        else if (line[i] != '\r') cur += line[i];
    }
    f.push_back(cur);
    if (f.size() < 6) return false;

    r.key = PctDecode(f[0]);
    r.title = PctDecode(f[1]);
    r.off = atoi(f[2].c_str());
    r.pct = atoi(f[3].c_str());
    r.ts = atoi(f[4].c_str());
    r.chapter = PctDecode(f[5]);
    return !r.key.empty();
}

/* ============================ 2. 书的指纹 ================================ */

/* FNV-1a 64 位：一行代码就能解释的哈希，不需要任何依赖。
 * 只用它做"这是不是同一本书"的判等，不做安全用途，所以碰撞概率够低就行。 */
unsigned long long Fnv64(const std::wstring& s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned int c = (unsigned int)s[i];
        h ^= (unsigned long long)(c & 0xFF);
        h *= 1099511628211ULL;
        h ^= (unsigned long long)((c >> 8) & 0xFF);
        h *= 1099511628211ULL;
    }
    return h;
}

/* ============================ 3. 网络 ==================================== */

const int kTcpPort = 8233;          /* 同步对话端口（1.1.0 的网页服务是 8231，避开） */
const int kUdpPort = 8234;          /* 广播发现端口 */

volatile LONG g_stopFlag = 0;
bool g_running = false;
SOCKET g_tcp = INVALID_SOCKET;
SOCKET g_udp = INVALID_SOCKET;
HANDLE g_thTcp = nullptr;
HANDLE g_thUdp = nullptr;
HANDLE g_thCast = nullptr;

/* 对端信息：广播里收到的最后一台设备 */
struct Peer
{
    std::wstring ip;
    int port = 0;
    std::wstring name;
    DWORD seen = 0;      /* GetTickCount，用来判断"还活着吗" */
    bool valid = false;
};
Peer g_peer;
/* 本机的会话 id：广播里带上它，收到自己的包就忽略。
 * 不加这个的话，自己发的广播会被自己的监听收到，程序会把自己当成"手机"去连自己，
 * 于是"两边进度一样"、永远不弹同步提示（这个坑实测踩过一次）。 */
std::string g_selfId;

std::wstring IpToW(const sockaddr_in& a)
{
    unsigned char* b = (unsigned char*)&a.sin_addr;
    wchar_t buf[64];
    wsprintfW(buf, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

/* 发一个小包给广播地址。UDP 广播不需要对端先存在，所以这里失败也不报错 ——
 * 家里路由器偶尔会丢广播包，下一轮 2 秒后就补上了。 */
void CastOnce()
{
    if (g_udp == INVALID_SOCKET) return;
    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)kUdpPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    /* 报文：NR1|tcp端口|设备名。设备名让用户在对端列表里能认出来。 */
    char msg[160];
    wsprintfA(msg, "NR1|%d|pc|%s", kTcpPort, g_selfId.c_str());
    sendto(g_udp, msg, (int)strlen(msg), 0, (sockaddr*)&to, sizeof(to));
}

DWORD WINAPI CastThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_stopFlag, 0, 0) == 0)
    {
        CastOnce();
        Sleep(2000);
    }
    return 0;
}

DWORD WINAPI UdpThread(LPVOID)
{
    char buf[512];
    while (InterlockedCompareExchange(&g_stopFlag, 0, 0) == 0)
    {
        sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(g_udp, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0)
        {
            if (InterlockedCompareExchange(&g_stopFlag, 0, 0) != 0) break;
            Sleep(20);
            continue;
        }
        buf[n] = 0;

        /* 只认自己人的报文，别把局域网里别的广播当同伴 */
        if (strncmp(buf, "NR1|", 4) != 0) continue;
        int port = 0;
        char who[64] = {0};
        char sid[64] = {0};
        int got = sscanf(buf + 4, "%d|%63[^|]|%63s", &port, who, sid);
        if (got < 2) continue;
        if (port <= 0 || port > 65535) continue;

        /* 自己的广播：忽略（否则会把自己当成对端） */
        if (got >= 3 && g_selfId == sid) continue;


        /* 同一台机器的多条网卡会重复广播，按 IP 覆盖即可 */
        g_peer.ip = IpToW(from);
        g_peer.port = port;
        g_peer.name = who[0] ? L"手机" : L"设备";
        g_peer.seen = GetTickCount();
        g_peer.valid = true;
    }
    return 0;
}

/* 读一整段 HTTP 请求（头 + 可选 body）。最多读 1 MB，防止有人乱发把内存吃光。 */
std::string ReadRequest(SOCKET c)
{
    std::string req;
    char buf[2048];
    int total = 0;
    while (total < 1024 * 1024)
    {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        total += n;

        /* 头结束在空行；再看 Content-Length 决定要不要继续读 body */
        size_t headEnd = req.find("\r\n\r\n");
        if (headEnd == std::string::npos) continue;

        size_t clPos = req.find("Content-Length:");
        if (clPos == std::string::npos) clPos = req.find("content-length:");
        if (clPos == std::string::npos) break;

        int cl = atoi(req.c_str() + clPos + 15);
        if (cl <= 0) break;
        if (req.size() >= headEnd + 4 + (size_t)cl) break;
    }
    return req;
}

void SendAll(SOCKET c, const std::string& s)
{
    size_t sent = 0;
    while (sent < s.size())
    {
        int n = send(c, s.c_str() + sent, (int)(s.size() - sent), 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

void Respond(SOCKET c, int code, const char* reason, const std::string& body, const char* type)
{
    char head[512];
    wsprintfA(head,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %d\r\n"
              "Connection: close\r\n\r\n",
              code, reason, type, (int)body.size());
    SendAll(c, head);
    SendAll(c, body);
}

/* 把对端推过来的记录写进 ini 的 [sync] 段：键是 "k" + 书指纹。
 * 存到全局段而不是"当前这本书的段"，是因为推过来的时候电脑这边可能开着别的书，
 * 甚至没开书；等下次打开那本书时再用指纹去查。 */
void StorePeerRec(const SyncRec& r)
{
    /* 对端数据是不可信输入（8233 无鉴权，局域网内都能打）：key[64] / val[512]
     * 用 wsprintfW 拼对端可控字符串且无边界检查，超长 key/chapter 直接写爆栈。
     * 指纹固定 16 个十六进制字符，章名截到 400 字足够显示。 */
    std::wstring k = r.key.substr(0, 32);
    std::wstring ch = r.chapter.substr(0, 400);
    wchar_t key[64];
    wsprintfW(key, L"k%s", k.c_str());
    wchar_t val[512];
    wsprintfW(val, L"%d|%d|%d|%s", r.off, r.pct, r.ts, ch.c_str());
    IniSetStr(L"sync", key, val);
    /* 书名单独存一份：万一这台机器上没有这本书，也能在提示里说出是哪本 */
    wsprintfW(key, L"t%s", k.c_str());
    IniSetStr(L"sync", key, r.title);
}

/* 处理一个请求。返回后由调用方关闭套接字。 */
void HandleOne(SOCKET c)
{
    std::string req = ReadRequest(c);
    if (req.empty()) return;

    bool isPost = (req.compare(0, 4, "POST") == 0);
    /* 取请求行里的路径（"GET /peer/ping HTTP/1.1"） */
    size_t sp1 = req.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                           ? req.substr(sp1 + 1, sp2 - sp1 - 1)
                           : std::string();

    if (path == "/peer/ping")
    {
        Respond(c, 200, "OK", "NRPEER 1\n", "text/plain");
        return;
    }

    if (path == "/peer/books")
    {
        /* 把本机"读到过的所有书"都列出来（书架上的书手机也能看到），
         * 一行一本；手机端按 ts 倒序显示。 */
        std::vector<SyncRec> all;
        SyncLocalRecords(all);
        std::string body;
        for (size_t i = 0; i < all.size(); i++)
        {
            body += RecToLine(all[i]);
            body += '\n';
        }
        Respond(c, 200, "OK", body, "text/plain; charset=utf-8");
        return;
    }

    /* "/peer/book" 一共 10 个字符，compare 的 count 必须写 10：
     * 写成 11 会去和 10 长度的字面量比"11 个字符"，长度不等永远不成立。 */
    if (path.compare(0, 10, "/peer/book") == 0)
    {
        /* ?key=xxxx  —— 手机端手上一本书都没有时，用它把正文要过去。
         * 这里必须老老实实解析查询串：path 是 "/peer/book?key=abc"，
         * 早先直接 substr(12) 会把 "ey=abc" 当指纹，导致永远 404。 */
        std::string want;
        size_t qm = path.find('?');
        if (qm != std::string::npos)
        {
            std::string query = path.substr(qm + 1);
            size_t kp = query.find("key=");
            if (kp != std::string::npos)
            {
                size_t s = kp + 4;
                size_t e = query.find('&', s);
                want = query.substr(s, (e == std::string::npos) ? std::string::npos : e - s);
            }
        }
        std::wstring key = Utf8ToW(want), title, text;
        /* 先试"当前打开的书"，对不上再按指纹去书架里找对应文件现读一份，
         * 这样手机上可以读到电脑这边任意一本记录过的书。 */
        if (key.empty() || !SyncServeBook(key, title, text))
        {
            Respond(c, 404, "Not Found", "no such book\n", "text/plain");
            return;
        }
        std::string body;
        /* 首行要带真实进度：原来 off/pct/ts 写死 0，手机把这条当"电脑的进度"存下来，
         * 于是手机永远从第 1 页开始读（ReaderActivity 里 intent 的 off 成了死代码）。 */
        SyncRec headRec;
        headRec.key = key;
        headRec.title = title;
        {
            std::vector<SyncRec> localAll;
            SyncLocalRecords(localAll);
            for (size_t i = 0; i < localAll.size(); i++) {
                if (localAll[i].key == key) { headRec = localAll[i]; break; }
            }
        }
        body += RecToLine(headRec) + "\n";
        /* 正文单独放在第二行之后，保持第一行仍是记录格式 */
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (n > 0)
        {
            std::string u8((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &u8[0], n, nullptr, nullptr);
            body += u8;
        }
        Respond(c, 200, "OK", body, "text/plain; charset=utf-8");
        return;
    }

    if (isPost && path == "/peer/progress")
    {
        size_t headEnd = req.find("\r\n\r\n");
        std::string body = (headEnd == std::string::npos) ? std::string() : req.substr(headEnd + 4);
        SyncRec r;
        if (LineToRec(body, r))
        {
            StorePeerRec(r);
            Respond(c, 200, "OK", "ok\n", "text/plain");
        }
        else
        {
            Respond(c, 400, "Bad Request", "bad record\n", "text/plain");
        }
        return;
    }

    Respond(c, 404, "Not Found", "not found\n", "text/plain");
}

DWORD WINAPI TcpThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_stopFlag, 0, 0) == 0)
    {
        SOCKET c = accept(g_tcp, nullptr, nullptr);
        if (c == INVALID_SOCKET)
        {
            if (InterlockedCompareExchange(&g_stopFlag, 0, 0) != 0) break;
            Sleep(20);
            continue;
        }
        DWORD tv = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        HandleOne(c);
        closesocket(c);
    }
    return 0;
}

/* 简单的 HTTP 客户端：连过去、发请求、把整个响应读回来。
 * 只用于本协议（对端是我们自己的程序，不会分块传输），所以不做 chunked 解析。 */
bool HttpOnce(const std::wstring& host, int port, const std::string& request,
              std::string& body, std::wstring& err)
{
    char hostA[128];
    WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, hostA, sizeof(hostA), nullptr, nullptr);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = L"创建套接字失败"; return false; }

    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = inet_addr(hostA);
    if (to.sin_addr.s_addr == INADDR_NONE)
    {
        closesocket(s);
        err = L"对端地址不对";
        return false;
    }

    DWORD tv = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(s, (sockaddr*)&to, sizeof(to)) != 0)
    {
        closesocket(s);
        err = L"连不上对端（手机可能已经退出同步，或者被防火墙挡了）";
        return false;
    }

    SendAll(s, request);

    std::string resp;
    char buf[4096];
    for (;;)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, (size_t)n);
        if (resp.size() > 64 * 1024 * 1024) break;   /* 防御：单次最多 64 MB */
    }
    closesocket(s);

    size_t headEnd = resp.find("\r\n\r\n");
    if (headEnd == std::string::npos) { err = L"对端没有回应"; return false; }

    /* 只要 200。404 之类当成"这本书对面没有"处理，由上层决定怎么说 */
    if (resp.compare(0, 12, "HTTP/1.1 200") != 0)
    {
        err = L"对端返回了错误（可能版本不一致）";
        return false;
    }
    body = resp.substr(headEnd + 4);
    return true;
}

} // namespace

/* ============================ 4. 对外接口 ================================ */

std::wstring SyncBookKey(const std::wstring& title, const std::wstring& text)
{
    /* 太短的书（只有一两行）不参与同步：指纹不稳，跳位置也没意义 */
    if (text.size() < 64) return std::wstring();

    std::wstring seed = title;
    seed += L'|';
    wchar_t num[32];
    wsprintfW(num, L"%u", (unsigned)text.size());
    seed += num;
    seed += L'|';
    seed.append(text, 0, text.size() < 4096 ? text.size() : 4096);

    unsigned long long h = Fnv64(seed);
    wchar_t hex[32];
    wsprintfW(hex, L"%08x%08x", (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFFULL));
    return hex;
}

bool SyncLocalRecord(SyncRec& r)
{
    if (!g_book.Ok()) return false;
    r.key = SyncBookKey(g_book.title, g_book.text);
    if (r.key.empty()) return false;

    /* 进度以 ini 里的 off/ts 为准（那是真正落盘的阅读位置），章名和百分比也是
     * 保存进度时一起写进去的，所以这里只读不重算，避免"界面上的位置"和
     * "存下来的位置"不一致时算出两套数字。 */
    std::wstring sec = PosSectionFor(g_book.path);
    r.title = g_book.title;
    r.off = IniGetInt(sec.c_str(), L"off", 0);
    r.pct = IniGetInt(sec.c_str(), L"pct", 0);
    r.ts = IniGetInt(sec.c_str(), L"ts", 0);
    r.chapter = IniGetStr(sec.c_str(), L"chname", L"");
    return true;
}

/* 本机记录里的一本书（从 ini 的 f* 段直接读出来的原始信息） */
struct LocalEntry
{
    std::wstring key;      /* 指纹（保存进度时写下的 skey） */
    std::wstring path;     /* 书文件路径 */
    std::wstring title;    /* 书名 */
    std::wstring chapter;  /* 上次读到的章名 */
    int off = 0;
    int pct = 0;
    int ts = 0;
};

/* 枚举本机"读到过"的书：靠 ini 里每个 f* 段记下的 skey / path / off。
 *
 * 为什么要单独存一个 skey：指纹要把正文前 4096 字算进去，而 /peer/books
 * 每秒都可能被手机拉一次，不可能每次都把十几本书重新读盘算一遍指纹。
 * 所以保存阅读进度时顺手把指纹写进 ini，这里只做纯文本枚举。 */
void EnumLocalEntries(std::vector<LocalEntry>& out)
{
    out.clear();
    std::wstring ini = IniFilePath();
    if (ini.empty()) return;

    std::vector<wchar_t> buf(65536, 0);
    DWORD n = GetPrivateProfileSectionNamesW(&buf[0], (DWORD)buf.size(), ini.c_str());
    if (n == 0) return;

    for (DWORD i = 0; i < n;)
    {
        std::wstring sec = &buf[i];
        i += (DWORD)sec.size() + 1;
        if (sec.size() < 2 || sec[0] != L'f') continue;   /* 只认 f* 进度段 */

        LocalEntry e;
        e.key = IniGetStr(sec.c_str(), L"skey", L"");
        e.path = IniGetStr(sec.c_str(), L"path", L"");
        if (e.key.empty() || e.path.empty()) continue;    /* 老记录没有指纹，跳过 */

        e.title = IniGetStr(sec.c_str(), L"title", L"");
        e.off = IniGetInt(sec.c_str(), L"off", 0);
        e.pct = IniGetInt(sec.c_str(), L"pct", 0);
        e.ts = IniGetInt(sec.c_str(), L"ts", 0);
        e.chapter = IniGetStr(sec.c_str(), L"chname", L"");
        out.push_back(e);
    }

    /* 手机上按"最近读的排前面"更好用 */
    std::stable_sort(out.begin(), out.end(),
                     [](const LocalEntry& a, const LocalEntry& b) { return a.ts > b.ts; });
}

void SyncLocalRecords(std::vector<SyncRec>& out)
{
    out.clear();
    std::vector<LocalEntry> all;
    EnumLocalEntries(all);

    SyncRec cur;
    bool hasCur = SyncLocalRecord(cur);
    for (size_t i = 0; i < all.size(); i++)
    {
        /* 当前这本用内存里的实时位置覆盖 ini 里的（可能还没落盘） */
        if (hasCur && all[i].key == cur.key) { out.push_back(cur); continue; }
        SyncRec r;
        r.key = all[i].key;
        r.title = all[i].title;
        r.off = all[i].off;
        r.pct = all[i].pct;
        r.ts = all[i].ts;
        r.chapter = all[i].chapter;
        out.push_back(r);
    }
    if (hasCur)
    {
        bool found = false;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].key == cur.key) { found = true; break; }
        if (!found) out.push_back(cur);
    }
}

/* 按指纹取一本书的正文：
 * 1) 就是当前打开的书 → 直接用内存里的，省一次读盘；
 * 2) 否则从 ini 记录里找到文件路径，现读一份给它。
 * 这样手机上能读电脑这边任意一本读过（记录还在）的书，不必先在电脑上打开。 */
bool SyncServeBook(const std::wstring& key, std::wstring& title, std::wstring& text)
{
    title.clear();
    text.clear();
    if (key.empty()) return false;

    std::wstring ck, ct, cx;
    if (SyncCurrentBook(ck, ct, cx) && ck == key)
    {
        title = ct;
        text = cx;
        return true;
    }

    std::vector<LocalEntry> all;
    EnumLocalEntries(all);
    for (size_t i = 0; i < all.size(); i++)
    {
        if (all[i].key != key) continue;
        Book tmp;
        std::wstring err;
        if (!BookLoad(tmp, all[i].path, &err)) return false;
        title = tmp.title.empty() ? all[i].title : tmp.title;
        text.swap(tmp.text);
        return !text.empty();
    }
    return false;
}

bool SyncCurrentBook(std::wstring& key, std::wstring& title, std::wstring& text)
{
    if (!g_book.Ok()) return false;
    key = SyncBookKey(g_book.title, g_book.text);
    if (key.empty()) return false;
    title = g_book.title;
    text = g_book.text;
    return true;
}

/* 读 [sync] 段里对端留下的记录 */
bool SyncPeerRecord(const std::wstring& key, SyncRec& out)
{
    if (key.empty()) return false;
    wchar_t k[64];
    wsprintfW(k, L"k%s", key.c_str());
    std::wstring val = IniGetStr(L"sync", k, L"");
    if (val.empty()) return false;

    /* 格式："off|pct|ts|chapter"（chapter 里可能没有 '|'，所以只切前三段） */
    int off = 0, pct = 0, ts = 0;
    std::wstring rest;
    if (swscanf(val.c_str(), L"%d|%d|%d|", &off, &pct, &ts) != 3) return false;
    size_t p = 0;
    for (int i = 0; i < 3; i++)
    {
        size_t q = val.find(L'|', p);
        if (q == std::wstring::npos) return false;
        p = q + 1;
    }
    rest = val.substr(p);

    wchar_t tk[64];
    wsprintfW(tk, L"t%s", key.c_str());
    out.key = key;
    out.title = IniGetStr(L"sync", tk, L"");
    out.chapter = rest;
    out.off = off;
    out.pct = pct;
    out.ts = ts;
    return true;
}

bool SyncStart(std::wstring& err)
{
    if (g_running) return true;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        err = L"初始化网络失败";
        return false;
    }

    /* ---- TCP 监听：等对端来问 ---- */
    g_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_tcp == INVALID_SOCKET)
    {
        WSACleanup();
        err = L"创建同步端口失败";
        return false;
    }
    int one = 1;
    setsockopt(g_tcp, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)kTcpPort);
    if (bind(g_tcp, (sockaddr*)&a, sizeof(a)) != 0)
    {
        closesocket(g_tcp);
        g_tcp = INVALID_SOCKET;
        WSACleanup();
        err = L"同步端口 8233 被占用（可能是另一个 NovelReader 还开着）";
        return false;
    }
    if (listen(g_tcp, 8) != 0)
    {
        closesocket(g_tcp);
        g_tcp = INVALID_SOCKET;
        WSACleanup();
        err = L"监听失败";
        return false;
    }

    /* ---- UDP：广播 + 收广播 ---- */
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET)
    {
        closesocket(g_tcp);
        g_tcp = INVALID_SOCKET;
        WSACleanup();
        err = L"创建发现端口失败";
        return false;
    }
    setsockopt(g_udp, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    setsockopt(g_udp, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));

    sockaddr_in u;
    ZeroMemory(&u, sizeof(u));
    u.sin_family = AF_INET;
    u.sin_addr.s_addr = htonl(INADDR_ANY);
    u.sin_port = htons((unsigned short)kUdpPort);
    if (bind(g_udp, (sockaddr*)&u, sizeof(u)) != 0)
    {
        /* 发现端口用不上不算致命错误：用户还能手填地址，所以只提示不退出 */
        closesocket(g_udp);
        g_udp = INVALID_SOCKET;
    }

    /* 生成本次会话的随机 id（16 个十六进制字符），只用于"别把自己的广播当对端"。
     * 这里不用 rand()：C 运行时不播种 rand 时两个进程会拿到同一串序列，id 撞了就
     * 互相忽略、永远发现不了对方。用时间 + 进程号混一个 LCG 更可靠。 */
    unsigned int seed = (unsigned int)GetTickCount()
                        ^ ((unsigned int)GetCurrentProcessId() << 16)
                        ^ (unsigned int)GetTickCount64();
    static const char* const kHexDigits = "0123456789abcdef";
    g_selfId.clear();
    for (int i = 0; i < 16; i++)
    {
        seed = seed * 1103515245u + 12345u;
        g_selfId += kHexDigits[(seed >> 16) & 0xF];
    }

    InterlockedExchange(&g_stopFlag, 0);
    g_thTcp = CreateThread(nullptr, 0, TcpThread, nullptr, 0, nullptr);
    g_thCast = CreateThread(nullptr, 0, CastThread, nullptr, 0, nullptr);
    if (g_udp != INVALID_SOCKET) g_thUdp = CreateThread(nullptr, 0, UdpThread, nullptr, 0, nullptr);

    if (!g_thTcp)
    {
        SyncStop();
        err = L"创建同步线程失败";
        return false;
    }
    g_running = true;
    return true;
}

void SyncStop()
{
    if (!g_running)
    {
        /* 起动到一半失败时也可能走到这里：套接字和线程要照样收干净 */
        InterlockedExchange(&g_stopFlag, 1);
        if (g_tcp != INVALID_SOCKET) { closesocket(g_tcp); g_tcp = INVALID_SOCKET; }
        if (g_udp != INVALID_SOCKET) { closesocket(g_udp); g_udp = INVALID_SOCKET; }
        if (g_thTcp) { WaitForSingleObject(g_thTcp, 3000); CloseHandle(g_thTcp); g_thTcp = nullptr; }
        if (g_thUdp) { WaitForSingleObject(g_thUdp, 3000); CloseHandle(g_thUdp); g_thUdp = nullptr; }
        if (g_thCast) { WaitForSingleObject(g_thCast, 3000); CloseHandle(g_thCast); g_thCast = nullptr; }
        return;
    }

    /* 先置标志再关套接字：阻塞在 accept/recvfrom 上的线程会立刻返回并退出循环，
     * 顺序反了线程会以为只是"临时失败"，接着用一个已经关掉的句柄再试一次。 */
    InterlockedExchange(&g_stopFlag, 1);
    if (g_tcp != INVALID_SOCKET) { closesocket(g_tcp); g_tcp = INVALID_SOCKET; }
    if (g_udp != INVALID_SOCKET) { closesocket(g_udp); g_udp = INVALID_SOCKET; }

    if (g_thTcp) { WaitForSingleObject(g_thTcp, 3000); CloseHandle(g_thTcp); g_thTcp = nullptr; }
    if (g_thUdp) { WaitForSingleObject(g_thUdp, 3000); CloseHandle(g_thUdp); g_thUdp = nullptr; }
    if (g_thCast) { WaitForSingleObject(g_thCast, 3000); CloseHandle(g_thCast); g_thCast = nullptr; }

    g_running = false;
    g_peer.valid = false;
    g_selfId.clear();
    WSACleanup();
}

bool SyncRunning() { return g_running; }

std::wstring SyncPeerUrl()
{
    if (!g_running || !g_peer.valid) return std::wstring();
    /* 15 秒没再收到广播就当它离线了（手机锁屏时广播会停） */
    if (GetTickCount() - g_peer.seen > 15000) return std::wstring();
    wchar_t buf[128];
    wsprintfW(buf, L"%s|%d", g_peer.ip.c_str(), g_peer.port);
    std::wstring s = buf;
    return s;
}

std::vector<std::wstring> SyncPeers()
{
    std::vector<std::wstring> out;
    if (!g_running || !g_peer.valid) return out;
    DWORD age = GetTickCount() - g_peer.seen;
    if (age > 15000) return out;

    wchar_t buf[256];
    wsprintfW(buf, L"%s:%d  %s  %u 秒前", g_peer.ip.c_str(), g_peer.port,
              g_peer.name.c_str(), (unsigned)(age / 1000));
    out.push_back(buf);
    return out;
}

bool SyncFetch(const std::wstring& baseUrl, std::vector<SyncRec>& out, std::wstring& err)
{
    /* baseUrl 形如 "192.168.1.23|8233"（也接受 "192.168.1.23:8233"） */
    std::wstring host;
    int port = kTcpPort;
    std::wstring u = baseUrl;
    size_t bar = u.find(L'|');
    if (bar == std::wstring::npos) bar = u.find(L':');
    if (bar == std::wstring::npos) { host = u; }
    else { host = u.substr(0, bar); port = _wtoi(u.c_str() + bar + 1); }

    if (host.empty()) { err = L"没有可用的对端地址"; return false; }

    std::string req = "GET /peer/books HTTP/1.1\r\nHost: novelreader\r\n\r\n";
    std::string body;
    if (!HttpOnce(host, port, req, body, err)) return false;

    /* 一行一条；空行忽略 */
    size_t start = 0;
    while (start < body.size())
    {
        size_t nl = body.find('\n', start);
        std::string line = body.substr(start, (nl == std::string::npos ? body.size() : nl) - start);
        start = (nl == std::string::npos) ? body.size() : nl + 1;
        if (line.empty() || line == "\r") continue;

        SyncRec r;
        if (LineToRec(line, r)) out.push_back(r);
    }
    return true;
}

bool SyncPush(const std::wstring& baseUrl, const SyncRec& rec, std::wstring& err)
{
    std::wstring host;
    int port = kTcpPort;
    std::wstring u = baseUrl;
    size_t bar = u.find(L'|');
    if (bar == std::wstring::npos) bar = u.find(L':');
    if (bar == std::wstring::npos) { host = u; }
    else { host = u.substr(0, bar); port = _wtoi(u.c_str() + bar + 1); }
    if (host.empty()) { err = L"没有可用的对端地址"; return false; }

    std::string body = RecToLine(rec);
    char head[256];
    wsprintfA(head,
              "POST /peer/progress HTTP/1.1\r\n"
              "Host: novelreader\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %d\r\n\r\n",
              (int)body.size());
    std::string resp;
    return HttpOnce(host, port, std::string(head) + body, resp, err);
}
