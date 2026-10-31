/* web.cpp - 网页抓书（HTTP/HTTPS GET + 目录解析 + 逐章抓取）
 * ============================================================================
 * 这个文件只干一件事：把一个"在线阅读页"变成一份能交给 BookLoad 的纯文本。
 *
 * 流程（每一步都在下面有对应函数）：
 *   1. WebFetchUrl      抓目录页（WinInet，跟随重定向，原始字节不转码）
 *   2. BytesToText      按与打开本地文件完全相同的规则解码（BOM/UTF-8/GBK 自动识别）
 *   3. ExtractLinks     从 HTML 里抠出 <a href> 链接与链接文字
 *   4. 过滤             只要"看起来像章节标题"的链接（与分章用同一判定）
 *   5. 逐章 WebFetchUrl + FormatHtmlToText，按目录顺序拼起来
 *   6. WebCachePath 落盘成 UTF-8 txt，交给 UiOpenFile 打开
 *
 * 为什么不用 DOM/正则库：exe 要保持几百 KB、零第三方依赖。网页的 <a> 结构足够简单，
 * 一个状态机扫一遍就够，而且不会因为页面里出现畸形标签就整页解析失败。
 * ============================================================================ */

#include "web.h"
#include "format.h"
#include "common.h"      /* BytesToText / BookLooksLikeChapterTitle（实现在 book.cpp） */

#include <wininet.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>

/* ---------------- 上限：防止被"全书一万章"的站点拖死 ---------------- */

static const int    kMaxChapters   = 3000;         /* 最多抓多少章 */
static const int    kMaxPageBytes  = 8 * 1024 * 1024;  /* 单页最大 8 MB */
static const size_t kMaxTotalChars = 40u * 1000 * 1000; /* 全文最多 4000 万字符 */
static const int    kMaxErrInARow  = 5;            /* 连续失败这么多次就放弃 */

/* ---------------- 编码转换（只在本文件用，所以不放进公共头） ---------------- */

/* ---------------- 字符串小工具 ---------------- */

/* 去首尾空白（含全角空格、\r\n\t）。网页里链接文字常带换行和缩进。 */
static std::wstring TrimW(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b) {
        wchar_t c = s[a];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000 || c == 0xA0) a++;
        else break;
    }
    while (b > a) {
        wchar_t c = s[b - 1];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000 || c == 0xA0) b--;
        else break;
    }
    return s.substr(a, b - a);
}

/* ASCII 小写化一个字符（只用于比较标签名/属性名，中文原样返回）。 */
static wchar_t LowerW(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    return c;
}

/* 在 hay 里从 from 开始找子串 needle（ASCII 大小写不敏感）。找不到返回 npos。 */
static size_t FindCI(const std::wstring& hay, const std::wstring& needle, size_t from)
{
    if (needle.empty() || hay.size() < needle.size()) return std::wstring::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); i++) {
        size_t k = 0;
        for (; k < needle.size(); k++) {
            if (LowerW(hay[i + k]) != LowerW(needle[k])) break;
        }
        if (k == needle.size()) return i;
    }
    return std::wstring::npos;
}

/* 把 HTML 文本片段里的 &nbsp; 之类最常见实体换成字符，并去掉内部标签。
 * 只处理链接文字，所以不需要完整实体表（完整版在 format.cpp 的 DecodeEntities 里，
 * 那是给正文用的）。 */
static std::wstring LinkTextToPlain(const std::wstring& raw)
{
    std::wstring out;
    bool inTag = false;
    for (size_t i = 0; i < raw.size(); i++) {
        wchar_t c = raw[i];
        if (c == L'<') { inTag = true; continue; }
        if (c == L'>') { inTag = false; continue; }
        if (inTag) continue;
        if (c == L'&') {
            size_t semi = raw.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 8) {
                std::wstring ent = raw.substr(i + 1, semi - i - 1);
                if (ent == L"nbsp") { out += L' '; i = semi; continue; }
                if (ent == L"amp")  { out += L'&'; i = semi; continue; }
                if (ent == L"lt")   { out += L'<'; i = semi; continue; }
                if (ent == L"gt")   { out += L'>'; i = semi; continue; }
                if (ent == L"quot") { out += L'"'; i = semi; continue; }
                if (ent == L"#12288") { out += L' '; i = semi; continue; }
            }
        }
        out += c;
    }
    return TrimW(out);
}

/* ---------------- 网址处理 ---------------- */

/* 是不是网址。只看开头，容忍前后空白。 */
bool WebIsUrl(const std::wstring& s)
{
    std::wstring t = TrimW(s);
    if (t.size() < 8) return false;
    return FindCI(t, L"http://", 0) == 0 || FindCI(t, L"https://", 0) == 0;
}

/* 从网址里取出 "scheme://host[:port]" 这一段（不含路径）。用于拼绝对地址与判同站。 */
static std::wstring UrlOrigin(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return std::wstring();
    size_t start = p + 3;
    size_t e = url.find_first_of(L"/?#", start);
    if (e == std::wstring::npos) e = url.size();
    std::wstring scheme = url.substr(0, p);
    for (size_t i = 0; i < scheme.size(); i++) scheme[i] = LowerW(scheme[i]);
    return scheme + L"://" + url.substr(start, e - start);
}

/* 取网址的目录部分（去掉 ?query 和 #fragment，再砍掉最后一个 '/' 之后的东西）。 */
static std::wstring UrlDir(const std::wstring& url)
{
    std::wstring u = url;
    size_t q = u.find_first_of(L"?#");
    if (q != std::wstring::npos) u = u.substr(0, q);
    size_t sl = u.rfind(L'/');
    if (sl == std::wstring::npos) return u + L"/";
    /* "http://a.com" 这种没有路径的情况，rfind('/') 会落在 "//" 上，要补一个斜杠 */
    if (sl + 1 == u.size()) return u;
    return u.substr(0, sl + 1);
}

/* 规范化路径里的 "." 和 ".."（只处理路径部分，scheme://host 原样保留）。 */
static std::wstring NormalizePathDots(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    size_t pathStart = url.find(L'/', p + 3);
    if (pathStart == std::wstring::npos) return url;
    std::wstring head = url.substr(0, pathStart);
    std::wstring path = url.substr(pathStart);

    std::vector<std::wstring> segs;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(L'/', i);
        if (j == std::wstring::npos) j = path.size();
        std::wstring seg = path.substr(i, j - i);
        if (seg == L".") {
            /* 忽略 */
        } else if (seg == L"..") {
            if (!segs.empty()) segs.pop_back();
        } else if (!seg.empty()) {
            segs.push_back(seg);
        }
        if (j == path.size()) break;
        i = j + 1;
    }
    std::wstring out = head;
    for (size_t k = 0; k < segs.size(); k++) { out += L'/'; out += segs[k]; }
    if (!segs.empty() && path.size() && path[path.size() - 1] == L'/') out += L'/';
    if (segs.empty()) out += L'/';
    return out;
}

/* 相对链接 → 绝对链接。
 * 处理四种写法：完整网址、协议相对 "//host/x"、根相对 "/x"、普通相对 "x/y"。
 * 认不出来的一律返回空串（调用方会跳过这条链接）。 */
static std::wstring UrlResolve(const std::wstring& base, const std::wstring& href)
{
    std::wstring h = TrimW(href);
    if (h.empty()) return std::wstring();
    if (h[0] == L'#') return std::wstring();                  /* 页内锚点没有意义 */
    if (FindCI(h, L"http://", 0) == 0 || FindCI(h, L"https://", 0) == 0) return h;
    if (FindCI(h, L"javascript:", 0) == 0 || FindCI(h, L"mailto:", 0) == 0) return std::wstring();

    std::wstring origin = UrlOrigin(base);
    if (origin.empty()) return std::wstring();

    if (h.size() >= 2 && h[0] == L'/' && h[1] == L'/') return origin.substr(0, origin.find(L"://") + 1) + h;
    if (h[0] == L'/') return origin + h;

    std::wstring u = UrlDir(base) + h;
    return NormalizePathDots(u);
}

/* ---------------- HTML 解析 ---------------- */

/* 从 HTML 里取出 <title> 的文字，作为书名。没有就返回空串。 */
static std::wstring ExtractTitle(const std::wstring& html)
{
    size_t a = FindCI(html, L"<title", 0);
    if (a == std::wstring::npos) return std::wstring();
    size_t gt = html.find(L'>', a);
    if (gt == std::wstring::npos) return std::wstring();
    size_t b = FindCI(html, L"</title", gt);
    if (b == std::wstring::npos) return std::wstring();
    std::wstring t = LinkTextToPlain(html.substr(gt + 1, b - gt - 1));
    /* 站名常见后缀 "_小说站名" / "- 某某网"，砍掉，只留书名 */
    const wchar_t* seps[] = { L"_", L" - ", L"|", L"－", L"—" };
    for (int i = 0; i < 5; i++) {
        size_t pos = t.find(seps[i]);
        if (pos != std::wstring::npos && pos >= 2) { t = TrimW(t.substr(0, pos)); break; }
    }
    return t;
}

/* 一条链接：绝对地址 + 链接文字。 */
struct RawLink {
    std::wstring url;
    std::wstring text;
};

/* 扫一遍 HTML，抠出所有 <a href="...">文字</a>。
 * 用状态机而不是正则：畸形 HTML（少闭合、属性不引号）也不会把整页搞崩。 */
static void ExtractLinks(const std::wstring& html, const std::wstring& base,
                         std::vector<RawLink>& out)
{
    size_t i = 0;
    const size_t n = html.size();
    while (i + 2 < n) {
        size_t a = FindCI(html, L"<a", i);
        if (a == std::wstring::npos) break;
        /* 必须是 <a 后面跟空白或 '>'，否则 <abbr>/<area> 会误命中 */
        if (a + 2 < n && html[a + 2] != L' ' && html[a + 2] != L'\t' && html[a + 2] != L'\r'
            && html[a + 2] != L'\n' && html[a + 2] != L'>') { i = a + 2; continue; }

        size_t tagEnd = html.find(L'>', a);
        if (tagEnd == std::wstring::npos) break;
        std::wstring tag = html.substr(a, tagEnd - a + 1);

        /* href="..." / href='...' / href=xxx 三种写法都认 */
        std::wstring href;
        size_t hp = FindCI(tag, L"href", 0);
        if (hp != std::wstring::npos && hp + 4 < tag.size() && tag[hp + 4] == L'=') {
            size_t v = hp + 5;
            if (v < tag.size() && (tag[v] == L'"' || tag[v] == L'\'')) {
                wchar_t q = tag[v];
                size_t e = tag.find(q, v + 1);
                if (e != std::wstring::npos) href = tag.substr(v + 1, e - v - 1);
            } else {
                size_t e = tag.find_first_of(L" \t\r\n>", v);
                href = tag.substr(v, (e == std::wstring::npos ? tag.size() : e) - v);
            }
        }

        size_t close = FindCI(html, L"</a", tagEnd);
        if (close == std::wstring::npos) break;

        if (!href.empty()) {
            std::wstring abs = UrlResolve(base, href);
            if (!abs.empty()) {
                RawLink lk;
                lk.url = abs;
                lk.text = LinkTextToPlain(html.substr(tagEnd + 1, close - tagEnd - 1));
                out.push_back(lk);
            }
        }
        i = close + 3;
    }
}

/* 把一堆链接过滤成"章节列表"：文字像章节名、同站、长度合理、去掉重复与导航链接。
 * 排序保持页面顺序 —— 目录页的顺序就是章节顺序。 */
static void FilterChapters(const std::vector<RawLink>& in, const std::wstring& base,
                           std::vector<RawLink>& out)
{
    std::wstring origin = UrlOrigin(base);
    std::vector<std::wstring> seen;
    for (size_t i = 0; i < in.size(); i++) {
        const RawLink& lk = in[i];
        if (lk.text.empty() || lk.text.size() > 48) continue;
        if (!BookLooksLikeChapterTitle(lk.text)) continue;
        /* 同站过滤：跨站的"推荐阅读"链接不是这本书的章节 */
        if (!origin.empty() && UrlOrigin(lk.url) != origin) continue;
        bool dup = false;
        for (size_t k = 0; k < seen.size(); k++) if (seen[k] == lk.url) { dup = true; break; }
        if (dup) continue;
        seen.push_back(lk.url);
        out.push_back(lk);
        if ((int)out.size() >= kMaxChapters) break;
    }
}

/* ---------------- WinInet 抓取 ---------------- */

/* 两个会话句柄：一个走系统代理（PRECONFIG），一个强制直连（DIRECT）。
 * 为什么要两份：公司/校园网常配代理，而本机测试或内网地址又不该走代理。
 * 先用系统配置试一次，失败再直连试一次，两条路都失败才报错 —— 用户不用懂代理。 */
static HINTERNET s_inetProxy  = nullptr;
static HINTERNET s_inetDirect = nullptr;

static HINTERNET InetHandle(bool direct)
{
    HINTERNET& slot = direct ? s_inetDirect : s_inetProxy;
    if (slot) return slot;
    slot = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) NovelReader/1.0",
                         direct ? INTERNET_OPEN_TYPE_DIRECT : INTERNET_OPEN_TYPE_PRECONFIG,
                         nullptr, nullptr, 0);
    return slot;
}

/* 进程退出前不用特意关，系统会回收；但抓书结束时关掉更干净（也便于再抓一次时重建） */
static void InetCloseAll()
{
    if (s_inetProxy) { InternetCloseHandle(s_inetProxy); s_inetProxy = nullptr; }
    if (s_inetDirect) { InternetCloseHandle(s_inetDirect); s_inetDirect = nullptr; }
}

/* ---------------- curl.exe 兜底 ----------------
 * 为什么需要这一层：有些站点（例如无忧书城）挂在 Cloudflare 后面，
 * 会按 TLS 指纹把 Windows 自带的 Schannel（WinInet / WinHTTP 都用它）判成机器人，
 * 直接回 403 加一张 "Just a moment..." 的 JS 挑战页。而 Windows 10 1803 以后
 * 系统自带 C:\Windows\System32\curl.exe（用 OpenSSL，指纹和浏览器接近），
 * 同一个网址它能正常拿到 200。所以 WinInet 两条路都失败时，调一次系统 curl.exe 兜底：
 * 既不增加任何第三方依赖，也不用把几十 MB 的浏览器内核塞进这个 500 KB 的小程序。
 * 找不到 curl.exe 就如实报错，让用户知道是这台机器的限制。 */
static bool FetchOnceCurl(const std::wstring& url, std::string& body, std::wstring& finalUrl,
                          std::wstring& err, int timeoutMs)
{
    body.clear();
    finalUrl = url;

    wchar_t curl[MAX_PATH] = {0};
    if (SearchPathW(nullptr, L"curl.exe", nullptr, MAX_PATH, curl, nullptr) == 0) {
        err = L"系统里找不到 curl.exe（Windows 10 1803 以后自带），换台机器或升级系统再试。";
        return false;
    }

    /* 临时文件放 %TEMP%\NovelReaderCurl\，抓完立刻删，不留垃圾 */
    wchar_t tmpDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring dir = std::wstring(tmpDir) + L"NovelReaderCurl";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\f%08x.html", (unsigned)GetTickCount());
    std::wstring outPath = dir + name;

    int ms = timeoutMs > 0 ? timeoutMs : 15000;
    wchar_t sec[16];
    swprintf(sec, 16, L"%d", (ms + 999) / 1000);

    /* -s 静默、-L 跟跳转、--compressed 让 curl 自己解 gzip、-o 落文件。
     * UA 与请求头都照 Chrome 抄：有的站点（例如走 Cloudflare 的）会拿
     * 请求头缺项 + TLS 指纹做机器人判定，头越像浏览器，被拦的概率越低。 */
    std::wstring cmd = L"\"";
    cmd += curl;
    cmd += L"\" -s -L --compressed --max-time ";
    cmd += sec;
    cmd += L" -A \"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
           L"(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36\"";
    cmd += L" -H \"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
           L"image/avif,image/webp,*/*;q=0.8\"";
    cmd += L" -H \"Accept-Language: zh-CN,zh;q=0.9,en;q=0.5\"";
    cmd += L" -H \"Upgrade-Insecure-Requests: 1\"";
    cmd += L" -H \"Sec-Fetch-Dest: document\" -H \"Sec-Fetch-Mode: navigate\"";
    cmd += L" -H \"Sec-Fetch-Site: none\" -H \"Sec-Fetch-User: ?1\"";
    cmd += L" -o \"";
    cmd += outPath;
    cmd += L"\" \"";
    cmd += url;
    cmd += L"\"";

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    /* CreateProcessW 要可写缓冲区，拷贝一份命令行 */
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        err = L"启动 curl.exe 失败。";
        return false;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)ms + 8000);
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* 读回文件内容（大小上限和 WinInet 那条路一致，防止一页过大吃内存） */
    std::string data;
    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        sz.QuadPart = 0;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (LONGLONG)kMaxPageBytes) {
            data.resize((size_t)sz.QuadPart);
            DWORD got = 0;
            if (ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr)) data.resize(got);
            else data.clear();
        }
        CloseHandle(f);
    }
    DeleteFileW(outPath.c_str());

    if (data.empty()) {
        err = L"curl.exe 没抓到内容（网络不通，或网站要求人工验证）。";
        return false;
    }

    /* Cloudflare 的 JS 挑战页：内容拿回来了，但不是正文。
     * 触发原因几乎都是"短时间请求太密"，所以提示里直接告诉用户等一会儿，
     * 免得他以为程序坏了、反复点。 */
    if (data.find("Just a moment") != std::string::npos ||
        data.find("cf-browser-verification") != std::string::npos) {
        err = L"站点触发人机验证（Cloudflare），一般是请求太快被限流，等一两分钟再试。";
        return false;
    }

    body.swap(data);
    return true;
}

/* 取网址的"主机名"（小写），用来记住"这个站只有 curl 抓得动" */
static std::wstring HostLowerOf(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return std::wstring();
    size_t start = p + 3;
    size_t end = url.find(L'/', start);
    if (end == std::wstring::npos) end = url.size();
    std::wstring host = url.substr(start, end - start);
    size_t at = host.find(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);
    for (size_t i = 0; i < host.size(); i++) host[i] = (wchar_t)towlower(host[i]);
    return host;
}

/* 已经确认过"WinInet 抓不动、curl 可以"的站点，下次直接走 curl，省掉两次 403 的往返 */
static std::vector<std::wstring> s_curlOnlyHosts;

static bool CurlOnlyHost(const std::wstring& host)
{
    for (size_t i = 0; i < s_curlOnlyHosts.size(); i++)
        if (s_curlOnlyHosts[i] == host) return true;
    return false;
}

/* 一趟抓取。direct 决定是否绕过代理。结果与错误都通过引用返回。 */
static bool FetchOnce(const std::wstring& u, bool direct, std::string& body, std::wstring& finalUrl,
                      std::wstring& err, int timeoutMs)
{
    HINTERNET inet = InetHandle(direct);
    if (!inet) { err = L"无法初始化网络组件（WinInet）。"; return false; }

    DWORD to = (DWORD)(timeoutMs > 0 ? timeoutMs : 15000);
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    DWORD to2 = 10000;
    InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT, &to2, sizeof(to2));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    HINTERNET h = InternetOpenUrlW(
        inet, u.c_str(),
        L"Accept: text/html,application/xhtml+xml,*/*\r\n"
        L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.5\r\n",
        (DWORD)-1L,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION,
        0);
    if (!h) {
        DWORD e = GetLastError();
        wchar_t buf[192];
        /* 常见错误码翻成人话。12029 连不上 / 12007 域名解析失败 / 12002 超时
         * 是最常见的三种；其余把错误码原样给出来，方便用户搜。 */
        if (e == 12029)      err = L"连不上这个网站（连接被拒绝）。";
        else if (e == 12007) err = L"域名解析失败，检查网址是否写对、网络是否正常。";
        else if (e == 12002) err = L"请求超时（网络太慢或对方没响应）。";
        else { swprintf(buf, 192, L"打开网址失败（错误码 %u）。", (unsigned)e); err = buf; }
        return false;
    }

    /* 跟随重定向后的最终地址：拼相对链接必须用它，否则二级目录会算错 */
    wchar_t real[2048];
    DWORD rl = sizeof(real) / sizeof(real[0]);
    if (InternetQueryOptionW(h, INTERNET_OPTION_URL, real, &rl) && rl >= 2) finalUrl = real;

    /* 状态码。404 之类直接报"页面不存在"，比"抓到 0 字节"友好得多；
     * 403/429 是另一回事——服务器上有这一页，但这次不给你（机器人判定或限流），
     * 如实说清楚，用户知道等一会儿再试就行。 */
    DWORD code = 0, clen = sizeof(code);
    if (HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, nullptr)) {
        if (code >= 400) {
            wchar_t buf[224];
            if (code == 403) {
                lstrcpynW(buf, L"服务器拒绝了这次请求（403），通常是防采集或机器人验证。", 224);
            } else if (code == 429) {
                lstrcpynW(buf, L"服务器说请求太频繁（429 限流），等一两分钟再试。", 224);
            } else {
                swprintf(buf, 224, L"服务器返回 %u，页面不存在或已被删除。", (unsigned)code);
            }
            err = buf;
            InternetCloseHandle(h);
            return false;
        }
    }

    char tmp[8192];
    DWORD got = 0;
    while (InternetReadFile(h, tmp, sizeof(tmp), &got) && got > 0) {
        if (body.size() + got > (size_t)kMaxPageBytes) {
            err = L"这一页太大了（超过 8 MB），已中止。";
            InternetCloseHandle(h);
            return false;
        }
        body.append(tmp, got);
    }
    InternetCloseHandle(h);

    if (body.empty()) { err = L"服务器返回了空内容（可能是需要登录或跳转的页面）。"; return false; }
    return true;
}

/* 抓一个网址的响应体。见 web.h 的说明。
 * 策略：已知只有 curl 抓得动的站点直接走 curl；其余先按系统代理设置试一次 WinInet，
 * 失败再强制直连试一次，两条都失败再用系统 curl.exe 兜底（Cloudflare 那类 TLS 指纹拦截）。
 * 为什么不在失败后清空 body 接着用：body 里可能已经有半页数据，
 * 直连重试必须从干净的字符串开始，否则会把两页的内容拼在一起。 */
bool WebFetchUrl(const std::wstring& url, std::string& body, std::wstring& finalUrl,
                 std::wstring& err, int timeoutMs)
{
    body.clear();
    finalUrl = url;
    err.clear();

    std::wstring u = TrimW(url);
    if (!WebIsUrl(u)) { err = L"网址必须以 http:// 或 https:// 开头。"; return false; }

    /* 之前踩过坑的站点：直接用 curl，省掉两次注定 403 的往返（抓整本时这点开销很可观） */
    std::wstring host = HostLowerOf(u);
    if (!host.empty() && CurlOnlyHost(host)) {
        std::string bc;
        std::wstring fc = u, ec;
        if (FetchOnceCurl(u, bc, fc, ec, timeoutMs)) {
            body.swap(bc);
            finalUrl = fc;
            return true;
        }
    }

    std::wstring e1;
    if (FetchOnce(u, false, body, finalUrl, e1, timeoutMs)) return true;

    /* 被反爬挡下（403/429）时，直连重试几乎必然还是同样的结果，
     * 少发一次请求对站点、对我们都更友好 —— 直接跳到最后那道 curl。 */
    const bool blocked = (e1.find(L"403") != std::wstring::npos || e1.find(L"429") != std::wstring::npos);

    /* 直连重试：代理失败时体面兜底（内网地址、本机测试服务都靠这条路） */
    std::string body2;
    std::wstring final2 = u, e2;
    if (!blocked) {
        if (FetchOnce(u, true, body2, final2, e2, timeoutMs)) {
            body.swap(body2);
            finalUrl = final2;
            err.clear();
            return true;
        }
    }

    /* 最后一道：系统 curl.exe。成功就记住这个站，后面每一页都直接走它 */
    std::string body3;
    std::wstring final3 = u, e3;
    if (FetchOnceCurl(u, body3, final3, e3, timeoutMs)) {
        body.swap(body3);
        finalUrl = final3;
        err.clear();
        if (!host.empty() && !CurlOnlyHost(host)) s_curlOnlyHosts.push_back(host);
        return true;
    }

    /* curl 也被挡：说明这个站的 WinInet 那条路一定不通，记住它，
     * 下次直接走 curl，不再浪费两次请求（限流的环境下这很关键）。 */
    if (blocked && !host.empty() && !CurlOnlyHost(host)) s_curlOnlyHosts.push_back(host);

    body.clear();
    err = e1;
    if (!e2.empty() && e2 != e1) { err += L"（直连也失败："; err += e2; err += L"）"; }
    if (!e3.empty()) { err += L"（curl 备份通道："; err += e3; err += L"）"; }
    return false;
}
/* 清理一章的正文：去掉与章节标题重复的首行，以及"上一章/下一章/目录"这类导航行。
 * 为什么必须去掉标题重复行：网页的 <h1>第1章 xxx</h1> 会和目录里的链接文字一模一样，
 * 两行连续出现就会被分章逻辑各认成一章 —— 用户会看到"每章都出现两次"的目录。
 * 导航行同理：它们是页面装饰，混进正文会让阅读和搜索都很碍眼。 */
static std::wstring CleanChapterBody(const std::wstring& body, const std::wstring& title)
{
    std::vector<std::wstring> lines;
    size_t i = 0;
    while (i <= body.size()) {
        size_t j = body.find(L'\n', i);
        if (j == std::wstring::npos) j = body.size();
        lines.push_back(body.substr(i, j - i));
        if (j == body.size()) break;
        i = j + 1;
    }

    /* 导航行白名单：整行只有这些字就直接丢 */
    const wchar_t* junk[] = { L"上一章", L"下一章", L"上一页", L"下一页", L"目录", L"返回目录",
                              L"章节目录", L"加入书签", L"投推荐票", L"返回列表", L"上一节",
                              L"下一节", L"回目录", L"首页", L"上一章 下一章" };
    const int junkN = 15;

    std::wstring out;
    bool first = true;
    for (size_t k = 0; k < lines.size(); k++) {
        std::wstring line = TrimW(lines[k]);
        if (line.empty()) {
            if (!out.empty() && out[out.size() - 1] != L'\n') out += L'\n';
            continue;
        }
        /* 导航/装饰行先判掉，而且**不能因此消耗掉 first 标志**：
         * 真实现场是 <div class="nav">目录</div> 出现在 <h1> 之前，
         * 如果先判标题重复，第一行是"目录"、second 行才是 <h1>，
         * 结果重复的章节标题就被保留下来了（用户看到目录里每章出现两次）。 */
        bool isJunk = false;
        for (int q = 0; q < junkN; q++) {
            if (line == junk[q]) { isJunk = true; break; }
        }
        if (isJunk) continue;

        /* 第一条真正的内容行如果是页面标题（与章节名相同或包含它），跳过 */
        if (first) {
            if (line == title) { continue; }
            if (line.size() <= title.size() + 8 && line.find(title) != std::wstring::npos) { continue; }
            first = false;
        }
        out += line;
        out += L'\n';
    }
    return TrimW(out);
}
/* ---------------- 缓存文件 ---------------- */

/* 把书名变成合法文件名：去掉 \ / : * ? " < > | 和控制字符，压掉连续空格，
 * 太长就截断（Windows 单段文件名上限 255，这里留足余量）。 */
static std::wstring SanitizeFileName(const std::wstring& in)
{
    std::wstring out;
    for (size_t i = 0; i < in.size() && out.size() < 100; i++) {
        wchar_t c = in[i];
        if (c < 32) continue;
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') continue;
        if (c == L' ' && !out.empty() && out[out.size() - 1] == L' ') continue;
        out += c;
    }
    out = TrimW(out);
    /* 结尾的点和空格在 Windows 上是非法的（"abc." 会被截断），一并去掉 */
    while (!out.empty() && (out[out.size() - 1] == L'.' || out[out.size() - 1] == L' ')) out.erase(out.size() - 1);
    if (out.empty()) out = L"网页小说";
    return out;
}

/* 缓存目录：%LOCALAPPDATA%\NovelReader\web。用 LocalAppData 而不是 exe 目录，
 * 是因为 exe 可能放在 Program Files（不可写）。目录不存在就创建。 */
static std::wstring WebCacheDir()
{
    std::wstring dir;
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir = buf;
    else dir = L".";
    /* 两级目录都要建：CreateDirectoryW 不会自动创建父目录，
     * 第一次运行时 %LOCALAPPDATA%\NovelReader 还不存在，
     * 直接建 ...\NovelReader\web 会失败（这就是"抓到了内容但写文件失败"的根因）。 */
    std::wstring base = dir + L"\\NovelReader";
    CreateDirectoryW(base.c_str(), nullptr);           /* 已存在会返回 FALSE，无所谓 */
    dir = base + L"\\web";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring WebCachePath(const std::wstring& title)
{
    std::wstring dir = WebCacheDir();
    std::wstring name = SanitizeFileName(title.empty() ? std::wstring(L"网页小说") : title);
    return dir + L"\\" + name + L".txt";
}

/* ---------------- 抓整本书 ---------------- */

/* 主流程。参数含义见 web.h。返回 false 时 err 一定有内容。
 * 每一步失败都会**带上"抓到第几章"的上下文**：用户看到"第 47 章抓取失败"比看到
 * "网络错误"有用得多（可以手动补那几章）。 */
bool WebFetchBook(const std::wstring& url, std::wstring& text, std::wstring& title,
                  std::wstring& err, WebProgressFn progress, void* ctx)
{
    text.clear();
    title.clear();
    err.clear();

    if (!WebIsUrl(url)) { err = L"这不像一个网址（要以 http:// 或 https:// 开头）。"; return false; }

    /* 1) 目录页 */
    std::string body;
    std::wstring base = url, e1;
    if (progress && !progress(ctx, 0, 0, L"正在打开目录页…")) { err = L"已取消。"; return false; }
    if (!WebFetchUrl(url, body, base, e1)) { err = std::wstring(L"打不开目录页：") + e1; return false; }

    std::wstring html = BytesToText(body);      /* 与打开本地文件同一套编码判定 */
    std::wstring pageTitle = ExtractTitle(html);
    body.clear();
    body.shrink_to_fit();

    /* 2) 找章节链接 */
    std::vector<RawLink> all;
    ExtractLinks(html, base, all);
    std::vector<RawLink> chapters;
    FilterChapters(all, base, chapters);

    bool single = chapters.size() < 2;      /* 少于两章就当单页小说 */
    if (single) {
        /* 单页：把目录页本身当全书。很多"某某小说最新章节"页就是一章一页，
         * 用户直接贴章节页也能读。 */
        text = FormatHtmlToText(html);
        if (TrimW(text).empty()) { err = L"这一页里没找到正文（可能是需要登录、或者是纯 JS 渲染的页面）。"; return false; }
        title = pageTitle.empty() ? std::wstring(L"网页小说") : pageTitle;
        html.clear();
        if (progress) progress(ctx, 1, 1, L"已抓取这一页");
        return true;
    }

    /* 3) 逐章抓 */
    int total = (int)chapters.size();
    int done = 0, errInARow = 0;
    for (int i = 0; i < total; i++) {
        if (text.size() > kMaxTotalChars) {
            /* 超上限就停，但已经抓到的部分照样给用户（比报错丢掉全书好） */
            err.clear();
            break;
        }
        if (progress) {
            wchar_t m[256];
            swprintf(m, 256, L"正在抓 %d/%d：%s", i + 1, total, chapters[i].text.c_str());
            if (!progress(ctx, done, total, m)) { err = L"已取消，已抓到的部分仍然保留。"; break; }
        }

        std::string cb;
        std::wstring cbase = chapters[i].url, e2;
        if (!WebFetchUrl(chapters[i].url, cb, cbase, e2)) {
            errInARow++;
            if (errInARow >= kMaxErrInARow) {
                err = std::wstring(L"连续 ") + std::to_wstring(kMaxErrInARow) + L" 章抓取失败，已停止。";
                break;
            }
            continue;               /* 单章失败跳过，尽量把书抓完 */
        }
        errInARow = 0;

        std::wstring chtml = BytesToText(cb);
        std::wstring ctext = FormatHtmlToText(chtml);
        chtml.clear();

        /* 章节标题单独占一行：这样 BookLoad 的"第 N 章"规则能认出章，
         * 目录、上下章跳转、进度百分比全都自动正确。 */
        text += chapters[i].text;
        text += L"\n";
        text += CleanChapterBody(ctext, chapters[i].text);
        text += L"\n\n";
        done++;
        if (progress) progress(ctx, done, total, L"");
    }

    if (TrimW(text).empty()) {
        err = L"目录页里找到了章节链接，但正文都没抓到（可能需要登录或正文由 JS 加载）。";
        return false;
    }

    InetCloseAll();     /* 抓完就关掉网络会话，下次抓书重建（也避免占着连接不放） */

    /* 书名：优先网页标题（去掉站名后缀），没有就用第一章的名字 */
    title = pageTitle.empty() ? chapters[0].text : pageTitle;
    if (title.size() > 60) title = title.substr(0, 60);
    return true;
}

/* ======================= 内置书源：无忧书城 51shucheng =======================
 * 为什么要有"书源"：通用的「打开网页小说」要求用户自己找到目录页网址，
 * 这一步对普通读者太难（还得看懂站点结构）。书源把"搜索 → 选书 → 抓全书"
 * 做成一条龙，用户只需要输入书名。
 *
 * 站点结构（2026-10 实测，页面全是 UTF-8）：
 *   搜索页  /search?q=<关键词>
 *     <div class="search_result"><ul>
 *       <li><a href="https://www.51shucheng.net/wangluo/doupocangqiong" title="…">
 *             <h3>斗破苍穹</h3></a><p>简介…</p></li>…
 *     </ul></div>
 *   详情页  <h1>斗破苍穹</h1> + <ul class="category-toc-list">
 *             <li><a href="…/23716.html" title="…">第一章 陨落的天才</a></li>…
 *   正文页  <div class="neirong" id="neirong"><p>…</p>…</div>
 *           这个 div 一结束就是广告和"下一章"导航，所以正文只取到第一个 </div>。
 *
 * 站点给的域名是 .com，实际 301 到 .net；这里统一改写成 .net，省一次跳转，
 * 也让用户从搜索结果里粘 .com 的链接时照样能用。
 * ========================================================================= */

/* 取标签里的某个属性值（href / title 这类）。支持 href="x" / href='x' / href=x。 */
static std::wstring TagAttr(const std::wstring& tag, const wchar_t* name)
{
    size_t p = FindCI(tag, name, 0);
    if (p == std::wstring::npos) return std::wstring();
    size_t eq = p + std::wcslen(name);
    /* 跳过 name 与 '=' 之间的空白 */
    while (eq < tag.size() && (tag[eq] == L' ' || tag[eq] == L'\t')) eq++;
    if (eq >= tag.size() || tag[eq] != L'=') return std::wstring();
    size_t v = eq + 1;
    while (v < tag.size() && (tag[v] == L' ' || tag[v] == L'\t')) v++;
    if (v >= tag.size()) return std::wstring();
    if (tag[v] == L'"' || tag[v] == L'\'') {
        wchar_t q = tag[v];
        size_t e = tag.find(q, v + 1);
        if (e == std::wstring::npos) return std::wstring();
        return tag.substr(v + 1, e - v - 1);
    }
    size_t e = tag.find_first_of(L" \t\r\n>", v);
    if (e == std::wstring::npos) e = tag.size();
    return tag.substr(v, e - v);
}

/* 把宽字符串按 UTF-8 做百分号编码（中文关键词要用它拼搜索网址）。 */
static std::wstring UrlEncodeU8(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u8((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], n, nullptr, nullptr);

    static const wchar_t* kHex = L"0123456789ABCDEF";
    std::wstring out;
    for (size_t i = 0; i < u8.size(); i++) {
        unsigned char c = (unsigned char)u8[i];
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out += (wchar_t)c;
        } else {
            out += L'%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

/* 域名归一：51shucheng.com → 51shucheng.net（站点自己就这么跳） */
static std::wstring SourceHostFix(const std::wstring& url)
{
    std::wstring s = url;
    const std::wstring from = L"51shucheng.com";
    const std::wstring to   = L"51shucheng.net";
    size_t p = FindCI(s, from, 0);
    if (p != std::wstring::npos) s.replace(p, from.size(), to);
    return s;
}

/* 逐行去掉首尾空白。网页正文习惯用 &nbsp; 缩进，解出来是 U+00A0（不换行空格）
 * 或全角空格 U+3000，普通 trim 去不掉，这里三种一起处理。 */
static std::wstring SourceCleanParagraphs(const std::wstring& t)
{
    std::wstring out;
    size_t i = 0;
    while (i <= t.size()) {
        size_t j = t.find(L'\n', i);
        if (j == std::wstring::npos) j = t.size();
        size_t a = i, b = j;
        while (a < b && (t[a] == L' ' || t[a] == L'\t' || t[a] == (wchar_t)0x00A0 || t[a] == (wchar_t)0x3000)) a++;
        while (b > a && (t[b - 1] == L' ' || t[b - 1] == L'\t' || t[b - 1] == (wchar_t)0x00A0 || t[b - 1] == (wchar_t)0x3000)) b--;
        if (b > a) {
            if (!out.empty()) out += L'\n';
            out.append(t, a, b - a);
        }
        if (j >= t.size()) break;
        i = j + 1;
    }
    return out;
}

/* 从搜索结果区里抠出每一本书。传入的 sub 已经限定在 search_result 到 related_search 之间，
 * 所以这里遇到的 <li> 都是书，不会混进"相关搜索"。 */
static void SourceCollectHits(const std::wstring& sub, std::vector<WebBookHit>& out)
{
    size_t i = 0;
    while (i + 3 < sub.size()) {
        size_t li = FindCI(sub, L"<li", i);
        if (li == std::wstring::npos) break;
        size_t liEnd = sub.find(L'>', li);
        if (liEnd == std::wstring::npos) break;
        size_t close = FindCI(sub, L"</li", liEnd);
        if (close == std::wstring::npos) break;
        std::wstring chunk = sub.substr(liEnd + 1, close - liEnd - 1);
        i = close + 4;

        size_t a = FindCI(chunk, L"<a", 0);
        if (a == std::wstring::npos) continue;
        size_t aEnd = chunk.find(L'>', a);
        if (aEnd == std::wstring::npos) continue;
        std::wstring href = TagAttr(chunk.substr(a, aEnd - a + 1), L"href");
        if (href.empty()) continue;
        std::wstring abs = UrlResolve(L"https://www.51shucheng.net/", href);
        if (abs.empty()) continue;

        /* 书名优先取 <h3>（结果卡片里书名的位置），没有就用链接文字 */
        std::wstring title;
        size_t h3 = FindCI(chunk, L"<h3", 0);
        if (h3 != std::wstring::npos) {
            size_t h3e = chunk.find(L'>', h3);
            if (h3e != std::wstring::npos) {
                size_t h3c = FindCI(chunk, L"</h3", h3e);
                if (h3c != std::wstring::npos)
                    title = LinkTextToPlain(chunk.substr(h3e + 1, h3c - h3e - 1));
            }
        }
        if (title.empty()) {
            size_t ac = FindCI(chunk, L"</a", aEnd);
            if (ac != std::wstring::npos)
                title = LinkTextToPlain(chunk.substr(aEnd + 1, ac - aEnd - 1));
        }
        if (title.empty()) continue;

        /* 简介：第一个 <p>…</p>，太长就截断（列表里只用来分辨是哪本书） */
        std::wstring desc;
        size_t p = FindCI(chunk, L"<p", 0);
        if (p != std::wstring::npos) {
            size_t pe = chunk.find(L'>', p);
            if (pe != std::wstring::npos) {
                size_t pc = FindCI(chunk, L"</p", pe);
                if (pc != std::wstring::npos)
                    desc = LinkTextToPlain(chunk.substr(pe + 1, pc - pe - 1));
            }
        }
        if (desc.size() > 60) { desc.resize(60); desc += L"…"; }

        WebBookHit h;
        h.title = title;
        h.url = abs;
        h.desc = desc;
        out.push_back(h);
    }
}

bool WebSourceSearch(const std::wstring& keyword, std::vector<WebBookHit>& out,
                     std::wstring& err, int timeoutMs)
{
    out.clear();
    err.clear();

    std::wstring kw = TrimW(keyword);
    if (kw.empty()) { err = L"请先在搜索框里输入书名或作者。"; return false; }

    std::wstring url = L"https://www.51shucheng.net/search?q=" + UrlEncodeU8(kw);
    std::string body;
    std::wstring base = url, e1;
    if (!WebFetchUrl(url, body, base, e1, timeoutMs)) {
        err = std::wstring(L"连不上无忧书城：") + e1;
        return false;
    }
    std::wstring html = BytesToText(body);
    body.clear();
    body.shrink_to_fit();

    /* 结果区边界：从 class="search_result" 到"相关搜索"之前。
     * 不这么切的话，页面底部的相关搜索词也会被当成书抓进来。 */
    size_t pos = FindCI(html, L"class=\"search_result\"", 0);
    if (pos == std::wstring::npos) {
        err = L"没看懂搜索页的结构（站点可能改版了）。可以用「文件 → 打开网页小说」直接贴目录页网址。";
        return false;
    }
    size_t end = FindCI(html, L"related_search", pos);
    std::wstring sub = html.substr(pos, (end == std::wstring::npos ? html.size() : end) - pos);
    SourceCollectHits(sub, out);

    if (out.empty()) {
        err = L"没搜到这本书。换个书名试试，或者只输入书名的前几个字。";
        return false;
    }
    InetCloseAll();     /* 搜完就放掉网络会话，抓书时会重新建 */
    return true;
}

/* 从详情页解析出有序的章节列表。
 * 优先用站点的目录容器 category-toc-list（顺序就是站点顺序，且不含"上一章/推荐"）；
 * 找不到就退回通用规则（ExtractLinks + FilterChapters），站点小改版也不至于整个失效。 */
static void SourceCollectToc(const std::wstring& html, const std::wstring& base,
                             std::vector<RawLink>& out)
{
    size_t pos = FindCI(html, L"category-toc-list", 0);
    if (pos == std::wstring::npos) {
        std::vector<RawLink> all;
        ExtractLinks(html, base, all);
        FilterChapters(all, base, out);
        return;
    }
    size_t ulEnd = FindCI(html, L"</ul", pos);
    std::wstring sub = html.substr(pos, (ulEnd == std::wstring::npos ? html.size() : ulEnd) - pos);

    size_t i = 0;
    while (i + 3 < sub.size()) {
        size_t li = FindCI(sub, L"<li", i);
        if (li == std::wstring::npos) break;
        size_t liEnd = sub.find(L'>', li);
        if (liEnd == std::wstring::npos) break;
        size_t close = FindCI(sub, L"</li", liEnd);
        if (close == std::wstring::npos) break;
        std::wstring chunk = sub.substr(liEnd + 1, close - liEnd - 1);
        i = close + 4;

        size_t a = FindCI(chunk, L"<a", 0);
        if (a == std::wstring::npos) continue;
        size_t aEnd = chunk.find(L'>', a);
        if (aEnd == std::wstring::npos) continue;
        std::wstring href = TagAttr(chunk.substr(a, aEnd - a + 1), L"href");
        size_t ac = FindCI(chunk, L"</a", aEnd);
        if (href.empty() || ac == std::wstring::npos) continue;
        std::wstring text = LinkTextToPlain(chunk.substr(aEnd + 1, ac - aEnd - 1));
        if (text.empty()) continue;
        std::wstring abs = UrlResolve(base, href);
        if (abs.empty()) continue;

        RawLink lk;
        lk.url = abs;
        lk.text = text;
        out.push_back(lk);
        if ((int)out.size() >= kMaxChapters) break;    /* 上限保护，见 web.h 的说明 */
    }
}

/* 抽正文：只取 div#neirong 里的内容，到第一个 </div> 为止。
 * 这样广告、字号工具栏、"下一章 ›"导航一个都不会进正文 ——
 * 通用的 FormatHtmlToText 是整页剥标签，这些脏东西会混进来。 */
static std::wstring SourceChapterText(const std::wstring& html)
{
    size_t p = FindCI(html, L"id=\"neirong\"", 0);
    if (p == std::wstring::npos) p = FindCI(html, L"class=\"neirong\"", 0);
    if (p == std::wstring::npos) return std::wstring();
    size_t gt = html.find(L'>', p);
    if (gt == std::wstring::npos) return std::wstring();
    size_t e = FindCI(html, L"</div>", gt);
    if (e == std::wstring::npos) e = html.size();
    std::wstring inner = html.substr(gt + 1, e - gt - 1);
    return SourceCleanParagraphs(FormatHtmlToText(inner));
}

bool WebSourceFetchBook(const std::wstring& bookUrl, std::wstring& text, std::wstring& title,
                        std::wstring& err, WebProgressFn progress, void* ctx,
                        const std::wstring& knownTitle)
{
    text.clear();
    title.clear();
    err.clear();

    std::wstring url = SourceHostFix(TrimW(bookUrl));
    if (!WebIsUrl(url)) { err = L"这不像一个网址（要以 http:// 或 https:// 开头）。"; return false; }

    /* 1) 详情页：拿章节列表 + 书名 */
    if (progress && !progress(ctx, 0, 0, L"正在打开书籍页…")) { err = L"已取消。"; return false; }
    std::string body;
    std::wstring base = url, e1;
    if (!WebFetchUrl(url, body, base, e1)) { err = std::wstring(L"打不开书籍页：") + e1; return false; }
    std::wstring html = BytesToText(body);
    body.clear();
    body.shrink_to_fit();

    std::vector<RawLink> chapters;
    SourceCollectToc(html, base, chapters);
    if (chapters.empty()) {
        err = L"这一页里没找到章节列表。可能不是书籍详情页，试试用「打开网页小说」贴目录页网址。";
        return false;
    }

    /* 书名：调用方给的最干净（搜索结果里的书名），其次详情页 <h1>，最后网页标题 */
    if (!knownTitle.empty()) title = TrimW(knownTitle);
    if (title.empty()) {
        size_t h1 = FindCI(html, L"<h1", 0);
        if (h1 != std::wstring::npos) {
            size_t ge = html.find(L'>', h1);
            if (ge != std::wstring::npos) {
                size_t hc = FindCI(html, L"</h1", ge);
                if (hc != std::wstring::npos)
                    title = TrimW(LinkTextToPlain(html.substr(ge + 1, hc - ge - 1)));
            }
        }
    }
    if (title.empty()) title = ExtractTitle(html);
    if (title.empty()) title = L"网页小说";
    if (title.size() > 60) title.resize(60);
    html.clear();

    /* 2) 逐章抓正文。一章失败就跳过（站点的个别章经常抽风），
     *    连续失败太多才整体放弃 —— 与通用抓书同一套策略。
     *    每章之间故意歇一下：无忧书城挂在 Cloudflare 后面，短时间连发太快会被限流
     *    （429 / JS 挑战页），歇 250ms 后基本不再触发；遇到失败还会退避重试两次。 */
    const int total = (int)chapters.size();
    int done = 0, errInARow = 0;
    for (int i = 0; i < total; i++) {
        if (text.size() > kMaxTotalChars) break;
        if (i > 0) Sleep(250);
        if (progress) {
            wchar_t msg[256];
            swprintf(msg, 256, L"正在抓 %d/%d：%s", i + 1, total, chapters[i].text.c_str());
            if (!progress(ctx, done, total, msg)) { err = L"已取消，已抓到的部分仍然保留。"; break; }
        }

        std::string cb;
        std::wstring cbase = chapters[i].url, e2;
        bool ok = false;
        /* 最多试 3 次，中间退避 2 秒 / 5 秒：限流往往是几秒钟的事 */
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            if (attempt > 0) Sleep(attempt == 1 ? 2000 : 5000);
            ok = WebFetchUrl(chapters[i].url, cb, cbase, e2);
        }
        if (!ok) {
            if (++errInARow >= kMaxErrInARow) { err = L"连续 5 章抓取失败，已停止。"; break; }
            continue;
        }
        errInARow = 0;

        std::wstring chtml = BytesToText(cb);
        std::wstring ctext = SourceChapterText(chtml);
        if (ctext.empty()) ctext = SourceCleanParagraphs(FormatHtmlToText(chtml));   /* 兜底 */
        chtml.clear();

        /* 章节标题单独一行：BookLoad 的"第 N 章"规则靠它分章，
         * 目录 / 上下章 / 进度百分比全都自动正确。 */
        text += chapters[i].text;
        text += L"\n";
        text += ctext;
        text += L"\n\n";
        done++;
        if (progress) progress(ctx, done, total, L"");
    }

    InetCloseAll();
    if (TrimW(text).empty()) {
        err = L"章节列表找到了，但正文一个字都没抓到（可能需要登录，或者正文由脚本动态加载）。";
        return false;
    }
    return true;
}
