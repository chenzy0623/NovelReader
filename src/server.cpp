/* server.cpp - 局域网手机阅读服务（HTTP）
 *
 * 设计的一句话说明：
 *   电脑端开一个极小的 HTTP 服务，手机浏览器连上来就能看同一批书；手机读到的
 *   位置直接写进电脑上的 NovelReader.ini（和电脑端共用同一段 [pos]），所以"同步"
 *   不是靠网络协议，而是靠"两边读同一份进度文件"这件事本身。
 *
 * 实现取向：
 *   1. 不引入任何第三方库，只用 Winsock2 + 手写 HTML。
 *   2. 单线程 accept 循环 + 单线程处理请求。家里最多两三个手机，够用；
 *      省掉线程池和锁带来的复杂度。
 *   3. 一次请求读一本书（带一次缓存），分页按"字符窗口"切，边界对齐到段落，
 *      这样手机上翻页不会把一段话从中间切断。
 *   4. 所有页面都是 UTF-8，深色底，字号大，按钮大，适合手机拇指操作。
 *
 * 线程与共享状态：
 *   服务工作在独立线程里，但它读 ini（ShelfItems / IniGetStr）和写 ini
 *   （PosSaveMeta）都是走 ini.cpp 的公开接口；ini 的读写本来就加了自己的
 *   临界区，所以这里不再额外加锁。
 */
#include "server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include <ctime>


namespace {

const int kDefaultPort = 8231;   // 默认端口：8231 不常见，撞车概率低
const int kPageChars = 1400;     // 手机上"一屏"大概的字数（可自适应）
const int kMaxReq = 8192;        // 单个请求头上限，防止恶意大包
/* App 用的 JSON 接口的 Content-Type。Android 的 HttpURLConnection 看到这个头
 * 才会按 UTF-8 解码，否则中文会被当成 ISO-8859-1 变成问号。 */
const char* const kJsonType = "application/json; charset=utf-8";

SOCKET g_listen = INVALID_SOCKET;
int g_port = 0;
bool g_running = false;
HANDLE g_thread = nullptr;
volatile LONG g_stopFlag = 0;

// ---- 进度（手机端最近一次保存）------------------------------------------
std::wstring g_phonePath;
int g_phoneOff = 0;
std::wstring g_phoneChapter;
int g_phonePct = 0;

// ---- 当前打开的书（只缓存最近一本，省内存）-------------------------------
Book g_srvBook;
std::wstring g_srvBookPath;

// 简单的宽窄串转换（服务端只用到 UTF-8）
std::string ToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// HTML 转义：正文里可能出现 < > & 引号，不转义会被浏览器当成标签
std::string Esc(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '&':  o += "&amp;";  break;
        case '<':  o += "&lt;";   break;
        case '>':  o += "&gt;";   break;
        case '"':  o += "&quot;"; break;
        default:   o += c;        break;
        }
    }
    return o;
}

std::string EscW(const std::wstring& s) { return Esc(ToUtf8(s)); }

// 发送全部数据（send 可能只发一部分）
bool SendAll(SOCKET s, const char* p, size_t n)
{
    while (n > 0)
    {
        int k = send(s, p, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

// 回一个完整的 HTTP 响应
void Respond(SOCKET s, int code, const char* reason, const std::string& body,
             const char* ctype = "text/html; charset=utf-8")
{
    char hdr[512];
    wsprintfA(hdr,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %u\r\n"
              "Cache-Control: no-store\r\n"
              "Connection: close\r\n"
              "\r\n",
              code, reason, ctype, (unsigned)body.size());
    if (!SendAll(s, hdr, strlen(hdr))) return;
    if (!body.empty()) SendAll(s, body.data(), body.size());
}

// 页面公共头部/尾部：移动端 viewport、深色配色、大字号
std::string PageHead(const std::string& title)
{
    std::string h;
    h += "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1\">";
    h += "<title>" + Esc(title) + "</title><style>";
    h += "html{-webkit-text-size-adjust:100%}";
    h += "body{margin:0;padding:16px 18px 90px;background:#18191a;color:#c8cacd;";
    h += "font:20px/1.85 'Microsoft YaHei',system-ui,-apple-system,sans-serif}";
    h += "h1{font-size:26px;color:#e8c07a;margin:6px 0 18px}";
    h += "h2{font-size:22px;color:#e8c07a;margin:26px 0 12px}";
    h += "p{margin:0 0 1.1em;text-indent:2em}";
    h += "a{color:#7fb0e8;text-decoration:none}";
    h += ".card{display:block;padding:14px 16px;margin:0 0 12px;border-radius:12px;";
    h += "background:#232527;color:#d6d8da}";
    h += ".t{font-size:21px;font-weight:700;color:#e6e8ea}";
    h += ".m{font-size:16px;color:#8d9196;margin-top:6px}";
    h += ".pct{float:right;color:#e8c07a;font-weight:700}";
    h += ".bar{position:fixed;left:0;right:0;bottom:0;display:flex;gap:10px;padding:10px 14px;";
    h += "background:#101112;border-top:1px solid #2a2c2f}";
    h += ".bar a,.bar span{flex:1;text-align:center;padding:14px 0;border-radius:10px;";
    h += "background:#2b2e31;color:#d6d8da;font-size:19px;font-weight:600}";
    h += ".bar span.dim{opacity:.4}";
    h += ".tip{color:#8d9196;font-size:15px;margin:18px 0}";
    h += "</style></head><body>";
    return h;
}

std::string PageTail()
{
    return "<div class=\"tip\">进度已同步到电脑 · <a href=\"/\">书架</a></div></body></html>";
}

// ---- 打开书（带单本缓存）-------------------------------------------------
bool ServerOpenBook(const std::wstring& path, std::wstring& err)
{
    if (g_srvBook.Ok() && g_srvBookPath == path) return true;
    g_srvBook = Book();
    g_srvBookPath.clear();
    std::wstring e;
    if (!BookLoad(g_srvBook, path, &e)) { err = e; return false; }
    g_srvBookPath = path;
    return true;
}

// ---- 书架页 --------------------------------------------------------------
std::string PageShelf()
{
    std::vector<ShelfItem> items = ShelfItems();
    std::string b = PageHead(ToUtf8(L"小说阅读器"));
    b += "<h1>书架</h1>";
    if (items.empty())
    {
        b += "<p class=\"tip\">电脑上还没打开过任何书。先在电脑上打开一本，再刷新这里。</p>";
    }
    else
    {
        for (size_t i = 0; i < items.size(); i++)
        {
            const ShelfItem& it = items[i];
            char buf[64];
            b += "<a class=\"card\" href=\"/book?id=";
            wsprintfA(buf, "%u", (unsigned)i);
            b += buf;
            b += "\"><div class=\"t\">" + EscW(it.title);
            wsprintfA(buf, "%.1f%%", (double)it.pct / 10.0);
            b += "<span class=\"pct\">" + std::string(buf) + "</span></div>";
            b += "<div class=\"m\">";
            if (it.missing) b += "文件已不在 · ";
            if (!it.chapter.empty()) b += EscW(it.chapter);
            else b += "还没开始读";
            if (it.chn > 0)
            {
                wsprintfA(buf, "  ·  共 %d 章", it.chn);
                b += buf;
            }
            b += "</div></a>";
        }
    }
    b += PageTail();
    return b;
}

// ---- 书的分章目录 --------------------------------------------------------
std::string PageBook(int id, std::wstring& err)
{
    std::vector<ShelfItem> items = ShelfItems();
    if (id < 0 || id >= (int)items.size()) { err = L"没有这本书"; return std::string(); }
    const ShelfItem& it = items[(size_t)id];
    if (!ServerOpenBook(it.path, err)) return std::string();

    std::string b = PageHead(ToUtf8(g_srvBook.title));
    b += "<h1>" + EscW(g_srvBook.title) + "</h1>";
    // "继续阅读"：定位到上次的进度
    char buf[128];
    int off = (int)((long long)it.pct * g_srvBook.text.size() / 1000);
    wsprintfA(buf, "/read?id=%d&off=%d", id, off);
    b += "<a class=\"card\" href=\"";
    b += buf;
    b += "\"><div class=\"t\">继续阅读</div><div class=\"m\">";
    b += it.chapter.empty() ? "从头开始" : EscW(it.chapter);
    b += "</div></a>";

    if (!g_srvBook.chapters.empty())
    {
        b += "<h2>目录</h2>";
        for (size_t i = 0; i < g_srvBook.chapters.size(); i++)
        {
            wsprintfA(buf, "/read?id=%d&off=%d", id, g_srvBook.chapters[i].off);
            b += "<a class=\"card\" href=\"";
            b += buf;
            b += "\"><div class=\"t\" style=\"font-size:18px;font-weight:400\">";
            b += EscW(BookChapterTitle(g_srvBook, (int)i));
            b += "</div></a>";
        }
    }
    b += PageTail();
    return b;
}

// 把偏移对齐到段落段首（避免从半句话开始读）
int AlignToPara(const Book& bk, int off)
{
    if (!bk.Ok() || bk.text.empty()) return 0;
    if (off <= 0) return 0;
    if (off >= (int)bk.text.size()) off = (int)bk.text.size() - 1;
    int p = BookCharToPara(bk, off);
    return (int)bk.paraOff[(size_t)p];
}

// ---- 阅读页 --------------------------------------------------------------
std::string PageRead(int id, int off, std::wstring& err)
{
    std::vector<ShelfItem> items = ShelfItems();
    if (id < 0 || id >= (int)items.size()) { err = L"没有这本书"; return std::string(); }
    const ShelfItem& it = items[(size_t)id];
    if (!ServerOpenBook(it.path, err)) return std::string();

    const std::wstring& txt = g_srvBook.text;
    int total = (int)txt.size();
    off = AlignToPara(g_srvBook, off);
    if (off < 0) off = 0;
    if (off >= total) off = total > 0 ? total - 1 : 0;

    // 一页取 kPageChars 个字，尽量在段落结尾收住
    int end = off + kPageChars;
    if (end > total) end = total;
    if (end < total)
    {
        int p = BookCharToPara(g_srvBook, end);
        // 往后最多再看 2 段，找离 end 最近的段首
        int limit = p + 2;
        if (limit > g_srvBook.ParaCount() - 1) limit = g_srvBook.ParaCount() - 1;
        if (p < limit) end = (int)g_srvBook.paraOff[(size_t)limit];
    }
    if (end <= off) end = (off + kPageChars < total) ? off + kPageChars : total;

    std::wstring seg = txt.substr((size_t)off, (size_t)(end - off));

    // 段落：跳过空行，段落之间空一行
    std::string body;
    size_t pos = 0;
    while (pos <= seg.size())
    {
        size_t nl = seg.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos) ? seg.substr(pos) : seg.substr(pos, nl - pos);
        // 去掉首尾空白（手机上不需要行首缩进以外的空白）
        size_t a = 0, z = line.size();
        while (a < z && (line[a] == L' ' || line[a] == L'\t' || line[a] == L'\r')) a++;
        while (z > a && (line[z - 1] == L' ' || line[z - 1] == L'\t' || line[z - 1] == L'\r')) z--;
        std::wstring t = line.substr(a, z - a);
        if (!t.empty()) body += "<p>" + EscW(t) + "</p>";
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }

    // 记进度：写 ini，电脑端 Ctrl+G 就能跳过来
    int para = BookCharToPara(g_srvBook, off);
    int ch = BookChapterAtPara(g_srvBook, para);
    std::wstring chName = (ch >= 0) ? BookChapterTitle(g_srvBook, ch) : std::wstring();
    int pct = total > 0 ? (int)((long long)off * 1000 / total) : 0;
    PosSaveMeta(g_srvBook.path, off, chName, ch, (int)g_srvBook.chapters.size(), pct);
    g_phonePath = g_srvBook.path;
    g_phoneOff = off;
    g_phoneChapter = chName;
    g_phonePct = pct;

    std::string b = PageHead(ToUtf8(g_srvBook.title));
    b += "<h1 style=\"font-size:22px\">" + EscW(g_srvBook.title) + "</h1>";
    if (!chName.empty()) b += "<h2>" + EscW(chName) + "</h2>";
    b += body;

    // 底部导航：上一页 / 下一页 / 目录
    char buf[256];
    b += "<div class=\"bar\">";
    if (off > 0)
    {
        int prev = off - kPageChars;
        if (prev < 0) prev = 0;
        wsprintfA(buf, "<a href=\"/read?id=%d&off=%d\">上一页</a>", id, prev);
        b += buf;
    }
    else
    {
        b += "<span class=\"dim\">上一页</span>";
    }
    wsprintfA(buf, "<a href=\"/book?id=%d\">目录</a>", id);
    b += buf;
    if (end < total)
    {
        wsprintfA(buf, "<a href=\"/read?id=%d&off=%d\">下一页</a>", id, end);
        b += buf;
    }
    else
    {
        b += "<span class=\"dim\">已读完</span>";
    }
    b += "</div>";

    wsprintfA(buf, "<div class=\"tip\">%.1f%% · %d/%d 字</div>",
              (double)pct / 10.0, off, total);
    b += buf;
    b += PageTail();
    return b;
}

// ---- 查询串解析 ----------------------------------------------------------
int QueryInt(const std::string& q, const char* key, int defVal)
{
    std::string pat = std::string(key) + "=";
    size_t p = q.find(pat);
    if (p == std::string::npos) return defVal;
    p += pat.size();
    int v = 0;
    bool any = false;
    while (p < q.size() && q[p] >= '0' && q[p] <= '9')
    {
        v = v * 10 + (q[p] - '0');
        any = true;
        p++;
        if (v > 100000000) break;
    }
    return any ? v : defVal;
}

// ---- 手机 App 用的 JSON 接口 ----------------------------------------------
// 这组接口是给 Android 客户端（android/ 目录，纯 Java）用的，返回 UTF-8 JSON。
// 为什么另开一套而不是让 App 去解析 HTML：手机端只要"书目 / 目录 / 一页正文 /
// 上报进度"四件事，JSON 比抓 <p> 稳得多，也不用跟着网页改版活着。
//
// 注意：上报进度故意做成 GET（/api/report?id=0&off=1234）。HandleOne 只读到
// 请求头就分发，不解析 body；GET 带查询串最省事，局域网里也没什么安全问题。

/* 把宽字符串转成 JSON 字符串字面量（含引号），转义 " \ 和换行/制表。
 * 中文直接以 UTF-8 原样放进去 —— JSON 允许，体积也最小。 */
std::string JsonStr(const std::wstring& s)
{
    std::string u = ToUtf8(s);
    std::string o;
    o.reserve(u.size() + 2);
    o += '"';
    for (size_t i = 0; i < u.size(); i++)
    {
        unsigned char c = (unsigned char)u[i];
        if (c == '"')       o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20)  { char b[8]; wsprintfA(b, "\\u%04x", (unsigned)c); o += b; }
        else                o += (char)c;
    }
    o += '"';
    return o;
}

/* 数字转字符串的小工具（wsprintfA 的 %d 够用，但包一层省得每处都写 buf）。 */
std::string Num(int v) { char b[32]; wsprintfA(b, "%d", v); return std::string(b); }

/* 取一页的起止偏移：从 off 起最多 kPageChars 个字，并尽量在下一段段首收住。
 * 网页版和 App 共用这段规则，保证同一位置两边看到的是同一页。 */
void CalcPageRange(const Book& bk, int off, int* pEnd)
{
    int total = (int)bk.text.size();
    int end = off + kPageChars;
    if (end > total) end = total;
    if (end < total)
    {
        int p = BookCharToPara(bk, end);
        int limit = p + 2;
        if (limit > bk.ParaCount() - 1) limit = bk.ParaCount() - 1;
        if (p < limit) end = (int)bk.paraOff[(size_t)limit];
    }
    if (end <= off) end = (off + kPageChars < total) ? off + kPageChars : total;
    *pEnd = end;
}

/* 把一段正文里的换行压成空格，得到"一页纯文本"。App 用一个 TextView 显示，
 * 段落之间用空行分开 —— 不用 HTML，也就没有标签转义问题。 */
std::wstring PlainFromSeg(const std::wstring& seg)
{
    std::wstring out;
    out.reserve(seg.size() + 16);
    size_t pos = 0;
    while (pos <= seg.size())
    {
        size_t nl = seg.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos) ? seg.substr(pos) : seg.substr(pos, nl - pos);
        size_t a = 0, z = line.size();
        while (a < z && (line[a] == L' ' || line[a] == L'\t' || line[a] == L'\r')) a++;
        while (z > a && (line[z - 1] == L' ' || line[z - 1] == L'\t' || line[z - 1] == L'\r')) z--;
        if (z > a)
        {
            if (!out.empty()) out += L'\n';
            out.append(line, a, z - a);
        }
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    return out;
}

/* 记一次"手机读到这儿"。除了照常写 off（电脑端 Ctrl+G 能跳过来），
 * 还另存 poff / ppct / phts：
 *   电脑端自己的进度键是 off + ts，手机的是 poff + phts，
 *   两个时间戳比大小，谁读得更晚就听谁的 —— 这样两边交替读也不会互相踩。 */
void SavePhoneProgress(const Book& bk, int off)
{
    int total = (int)bk.text.size();
    int para = BookCharToPara(bk, off);
    int ch = BookChapterAtPara(bk, para);
    std::wstring chName = (ch >= 0) ? BookChapterTitle(bk, ch) : std::wstring();
    int pct = total > 0 ? (int)((long long)off * 1000 / total) : 0;

    PosSaveMeta(bk.path, off, chName, ch, (int)bk.chapters.size(), pct);

    std::wstring sec = PosSectionFor(bk.path);
    IniSetInt(sec.c_str(), L"poff", off);
    IniSetInt(sec.c_str(), L"ppct", pct);
    IniSetInt(sec.c_str(), L"phts", (int)time(nullptr));

    g_phonePath = bk.path;
    g_phoneOff = off;
    g_phoneChapter = chName;
    g_phonePct = pct;
}

// GET /api/shelf —— 书架（最多 10 本，和电脑端一致）
std::string ApiShelf()
{
    /* 先刷一遍书架：手机每翻一页都会 /api/report 写进度，而内存里的书架快照
     * 是启动时建的，不刷的话手机上看到的百分比永远停在打开程序那一刻。
     * ShelfRefresh 只读 ini 的几个键，开销可以忽略。 */
    ShelfRefresh();
    std::vector<ShelfItem> items = ShelfItems();
    std::string b = "{\"ok\":1,\"count\":";
    b += Num((int)items.size());
    b += ",\"books\":[";
    for (size_t i = 0; i < items.size(); i++)
    {
        if (i) b += ',';
        b += "{\"id\":" + Num((int)i);
        b += ",\"title\":" + JsonStr(items[i].title);
        b += ",\"chapter\":" + JsonStr(items[i].chapter);
        /* pct 是千分数；没读过（-1）时给 0，免得手机端显示 "-0.1%" */
        b += ",\"pct\":" + Num(items[i].pct < 0 ? 0 : items[i].pct);
        b += ",\"chn\":" + Num(items[i].chn);
        b += ",\"missing\":" + Num(items[i].missing ? 1 : 0);
        b += '}';
    }
    b += "]}";
    return b;
}

// GET /api/chapters?id=0 —— 目录
std::string ApiChapters(int id, std::wstring& err)
{
    std::vector<ShelfItem> items = ShelfItems();
    if (id < 0 || id >= (int)items.size()) { err = L"没有这本书"; return std::string(); }
    if (!ServerOpenBook(items[(size_t)id].path, err)) return std::string();

    std::string b = "{\"ok\":1,\"title\":" + JsonStr(g_srvBook.title);
    b += ",\"chapters\":[";
    const std::vector<Chapter>& chs = g_srvBook.chapters;
    for (size_t i = 0; i < chs.size(); i++)
    {
        if (i) b += ',';
        b += "{\"i\":" + Num((int)i);
        b += ",\"off\":" + Num((int)chs[i].off);
        b += ",\"title\":" + JsonStr(BookChapterTitle(g_srvBook, (int)i));
        b += '}';
    }
    b += "]}";
    return b;
}

// GET /api/page?id=0&off=1234 —— 一页正文（纯文本）
std::string ApiPage(int id, int off, std::wstring& err)
{
    std::vector<ShelfItem> items = ShelfItems();
    if (id < 0 || id >= (int)items.size()) { err = L"没有这本书"; return std::string(); }
    if (!ServerOpenBook(items[(size_t)id].path, err)) return std::string();

    const Book& bk = g_srvBook;
    int total = (int)bk.text.size();
    if (total <= 0) { err = L"这本书是空的"; return std::string(); }
    off = AlignToPara(bk, off);
    if (off < 0) off = 0;
    if (off >= total) off = total - 1;

    int end = off;
    CalcPageRange(bk, off, &end);
    std::wstring plain = PlainFromSeg(bk.text.substr((size_t)off, (size_t)(end - off)));

    int para = BookCharToPara(bk, off);
    int ch = BookChapterAtPara(bk, para);
    std::wstring chName = (ch >= 0) ? BookChapterTitle(bk, ch) : std::wstring();
    int pct = (int)((long long)off * 1000 / total);

    int prev = off - kPageChars;
    if (prev < 0) prev = 0;
    int next = end;
    if (next >= total) next = -1;          /* -1 = 到头了，App 据此禁用"下一页" */

    std::string b = "{\"ok\":1";
    b += ",\"id\":" + Num(id);
    b += ",\"off\":" + Num(off);
    b += ",\"next\":" + Num(next);
    b += ",\"prev\":" + Num(prev);
    b += ",\"pct\":" + Num(pct);
    b += ",\"chapter\":" + JsonStr(chName);
    b += ",\"title\":" + JsonStr(bk.title);
    b += ",\"text\":" + JsonStr(plain);
    b += '}';
    return b;
}

// GET /api/report?id=0&off=1234 —— 手机上报"读到这儿"，顺手把进度写回 ini
std::string ApiReport(int id, int off, std::wstring& err)
{
    std::vector<ShelfItem> items = ShelfItems();
    if (id < 0 || id >= (int)items.size()) { err = L"没有这本书"; return std::string(); }
    if (!ServerOpenBook(items[(size_t)id].path, err)) return std::string();

    const Book& bk = g_srvBook;
    int total = (int)bk.text.size();
    if (total <= 0) { err = L"这本书是空的"; return std::string(); }
    off = AlignToPara(bk, off);
    if (off < 0) off = 0;
    if (off >= total) off = total - 1;
    SavePhoneProgress(bk, off);

    int pct = (int)((long long)off * 1000 / total);
    return "{\"ok\":1,\"off\":" + Num(off) + ",\"pct\":" + Num(pct) + "}";
}

// GET /api/ping —— App 用来测试"地址填对没有"
std::string ApiPing()
{
    std::string b = "{\"ok\":1,\"app\":\"NovelReader\",\"port\":" + Num(g_port);
    b += ",\"book\":" + JsonStr(g_book.Ok() ? g_book.title : std::wstring());
    b += ",\"total\":" + Num(g_book.Ok() ? (int)ShelfItems().size() : 0);
    b += '}';
    return b;
}

// ---- 处理一个请求 --------------------------------------------------------
void HandleOne(SOCKET c)
{
    std::string req;
    char buf[2048];
    // 读请求头，直到 \r\n\r\n。除了前面设的单次 recv 超时，这里再加一个总时长上限：
    // 否则攻击者每 4.9 秒滴一个字节，就能无限占住这个单线程 accept 循环，服务整体卡死。
    const DWORD reqStart = GetTickCount();
    while (req.size() < (size_t)kMaxReq)
    {
        if (GetTickCount() - reqStart > 5000) return;
        int k = recv(c, buf, sizeof(buf), 0);
        if (k <= 0) break;
        req.append(buf, (size_t)k);
        if (req.find("\r\n\r\n") != std::string::npos) break;
    }
    if (req.size() < 4) return;

    // 第一行：GET /path?query HTTP/1.1
    size_t sp1 = req.find(' ');
    if (sp1 == std::string::npos) return;
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return;
    std::string method = req.substr(0, sp1);
    std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "GET" && method != "HEAD") { Respond(c, 405, "Method Not Allowed", "no"); return; }

    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) { path = target.substr(0, qm); query = target.substr(qm + 1); }

    std::wstring err;
    std::string body;

    if (path == "/" || path == "/index.html")
    {
        body = PageShelf();
        Respond(c, 200, "OK", body);
    }
    else if (path == "/book")
    {
        body = PageBook(QueryInt(query, "id", -1), err);
        if (body.empty()) Respond(c, 404, "Not Found", "<h1>打开失败</h1><p>" + EscW(err) + "</p>");
        else Respond(c, 200, "OK", body);
    }
    else if (path == "/read")
    {
        body = PageRead(QueryInt(query, "id", -1), QueryInt(query, "off", 0), err);
        if (body.empty()) Respond(c, 404, "Not Found", "<h1>打开失败</h1><p>" + EscW(err) + "</p>");
        else Respond(c, 200, "OK", body);
    }
    else if (path == "/favicon.ico")
    {
        Respond(c, 404, "Not Found", "", "image/x-icon");
    }
    /* ---- App 用的 JSON 接口 ---- */
    else if (path == "/api/ping")
    {
        Respond(c, 200, "OK", ApiPing(), kJsonType);
    }
    else if (path == "/api/shelf")
    {
        Respond(c, 200, "OK", ApiShelf(), kJsonType);
    }
    else if (path == "/api/chapters")
    {
        body = ApiChapters(QueryInt(query, "id", -1), err);
        if (body.empty()) Respond(c, 404, "Not Found", "{\"ok\":0,\"err\":" + JsonStr(err) + "}", kJsonType);
        else Respond(c, 200, "OK", body, kJsonType);
    }
    else if (path == "/api/page")
    {
        body = ApiPage(QueryInt(query, "id", -1), QueryInt(query, "off", 0), err);
        if (body.empty()) Respond(c, 404, "Not Found", "{\"ok\":0,\"err\":" + JsonStr(err) + "}", kJsonType);
        else Respond(c, 200, "OK", body, kJsonType);
    }
    else if (path == "/api/report")
    {
        body = ApiReport(QueryInt(query, "id", -1), QueryInt(query, "off", 0), err);
        if (body.empty()) Respond(c, 404, "Not Found", "{\"ok\":0,\"err\":" + JsonStr(err) + "}", kJsonType);
        else Respond(c, 200, "OK", body, kJsonType);
    }
    else
    {
        Respond(c, 404, "Not Found", "<h1>404</h1><p><a href=\"/\">回书架</a></p>");
    }
}

// ---- accept 循环 ---------------------------------------------------------
DWORD WINAPI AcceptThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_stopFlag, 0, 0) == 0)
    {
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET)
        {
            // 监听套接字被关掉时 accept 会失败，直接退出循环
            if (InterlockedCompareExchange(&g_stopFlag, 0, 0) != 0) break;
            Sleep(20);
            continue;
        }
        // 给请求设个超时，避免半开连接把线程卡死
        DWORD tv = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        HandleOne(c);
        closesocket(c);
    }
    return 0;
}

// 枚举本机 IPv4 地址（跳过回环、未连接的网卡、以及各种虚拟网卡）
//
// 为什么要挑：Windows 上常见的 WSL / Hyper-V / VirtualBox 会额外冒出一堆
// 172.x 网卡，用户照着填到手机上永远连不上。这里按"像不像家里的内网"排序，
// 把 192.168 / 10. 这类地址排前面，虚拟网卡直接跳过。
bool LooksVirtualAdapter(const std::wstring& name, const std::wstring& desc)
{
    static const wchar_t* kW[] = { L"wsl", L"hyper-v", L"vethernet", L"virtualbox",
                                   L"vmware", L"loopback", L"vpn", L"tap-", L"tun" };
    std::wstring s = name + L" " + desc;
    for (size_t i = 0; i < s.size(); i++) s[i] = (wchar_t)towlower(s[i]);
    for (const wchar_t* k : kW)
        if (s.find(k) != std::wstring::npos) return true;
    return false;
}

int IpRank(unsigned char a, unsigned char b)
{
    if (a == 192 && b == 168) return 0;   // 家用路由最常见
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 2;
    return 3;
}

void CollectIPv4(std::vector<std::wstring>& out)
{
    ULONG size = 16 * 1024;
    std::vector<uint8_t> bufbuf(size);
    IP_ADAPTER_ADDRESSES* addrs = (IP_ADAPTER_ADDRESSES*)bufbuf.data();
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW)
    {
        bufbuf.resize(size);
        addrs = (IP_ADAPTER_ADDRESSES*)bufbuf.data();
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) return;

    struct Cand { std::wstring url; int rank; };
    std::vector<Cand> cands;
    for (IP_ADAPTER_ADDRESSES* a = addrs; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring nm = a->FriendlyName ? a->FriendlyName : L"";
        std::wstring ds = a->Description ? a->Description : L"";
        if (LooksVirtualAdapter(nm, ds)) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next)
        {
            sockaddr_in* sa = (sockaddr_in*)u->Address.lpSockaddr;
            if (!sa || sa->sin_family != AF_INET) continue;
            unsigned char* b = (unsigned char*)&sa->sin_addr;
            if (b[0] == 127 || b[0] == 0) continue;
            char ip[64];
            wsprintfA(ip, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            std::wstring url = L"http://";
            url += FromUtf8(ip);
            wchar_t port[16];
            wsprintfW(port, L":%d/", g_port);
            url += port;
            Cand c;
            c.url = url;
            c.rank = IpRank(b[0], b[1]);
            cands.push_back(c);
        }
    }
    // 稳定的插入排序：rank 小的在前
    for (size_t i = 1; i < cands.size(); i++)
    {
        Cand key = cands[i];
        size_t j = i;
        while (j > 0 && cands[j - 1].rank > key.rank) { cands[j] = cands[j - 1]; j--; }
        cands[j] = key;
    }
    for (size_t i = 0; i < cands.size(); i++) out.push_back(cands[i].url);
}

} // namespace

// ========================= 对外接口 =======================================

bool ServerStart(int port, std::wstring& err)
{
    if (g_running) return true;
    if (port <= 0 || port > 65535) port = kDefaultPort;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        err = L"初始化网络失败（WSAStartup）";
        return false;
    }

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET)
    {
        err = L"创建监听套接字失败";
        WSACleanup();
        return false;
    }

    // 允许重用地址，避免上次没退干净导致端口占用
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 监听所有网卡，手机才能连上
    addr.sin_port = htons((unsigned short)port);

    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        // 端口被占用：自动往后找 5 个
        bool ok = false;
        for (int p = port + 1; p <= port + 5 && p < 65535; p++)
        {
            addr.sin_port = htons((unsigned short)p);
            if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) == 0) { port = p; ok = true; break; }
        }
        if (!ok)
        {
            err = L"端口被占用，换一个端口试试";
            closesocket(g_listen);
            g_listen = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
    }

    if (listen(g_listen, 8) != 0)
    {
        err = L"监听失败";
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    g_port = port;
    InterlockedExchange(&g_stopFlag, 0);
    g_thread = CreateThread(nullptr, 0, AcceptThread, nullptr, 0, nullptr);
    if (!g_thread)
    {
        err = L"创建服务线程失败";
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    g_running = true;
    return true;
}

bool ServerRunning() { return g_running; }

/* 关掉服务（1.7.0：给"手机同步"开关用）。
 *
 * 关的顺序有讲究：
 *  1. 先立停止标志 —— 接收线程的循环条件看的是它，置位后线程处理完手上
 *     这一个请求就会退出，不会再去 accept 新连接；
 *  2. 再关监听套接字 —— 阻塞在 accept 上的线程会被唤醒（accept 返回
 *     INVALID_SOCKET），从而立刻跳出循环，不必等 5 秒超时；
 *  3. 最后等线程退出并 WSACleanup，避免线程还在用 ws2_32 的句柄。
 * 顺序反过来的话（先关套接字再置标志），线程会以为只是"临时失败"，
 * Sleep(20) 之后再 accept 一个已经关掉的句柄。 */
void ServerStop()
{
    if (!g_running) return;

    InterlockedExchange(&g_stopFlag, 1);

    if (g_listen != INVALID_SOCKET)
    {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_thread)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    g_running = false;
    g_port = 0;
    WSACleanup();
}

int ServerPort() { return g_running ? g_port : 0; }

std::vector<std::wstring> ServerUrls()
{
    std::vector<std::wstring> out;
    if (!g_running) return out;
    CollectIPv4(out);
    return out;
}

bool ServerLastPhoneProgress(std::wstring& path, int& off, std::wstring& chapter, int& pct)
{
    if (g_phonePath.empty()) return false;
    path = g_phonePath;
    off = g_phoneOff;
    chapter = g_phoneChapter;
    pct = g_phonePct;
    return true;
}

/* 从 ini 里读某本书的"手机读到哪"：poff / ppct / phts。
 * 和内存里的 g_phone* 不同，这三个键是**跨进程**活着的 —— 电脑关掉再开，
 * 也能知道上次是手机读到哪儿、什么时候读的。 */
bool ServerPhoneProgress(const std::wstring& path, int* off, int* pct, int* ts)
{
    if (path.empty()) return false;
    std::wstring sec = PosSectionFor(path);
    int o = IniGetInt(sec.c_str(), L"poff", -1);
    int t = IniGetInt(sec.c_str(), L"phts", 0);
    if (o < 0 || t <= 0) return false;
    if (off) *off = o;
    if (pct) *pct = IniGetInt(sec.c_str(), L"ppct", 0);
    if (ts)  *ts = t;
    return true;
}
