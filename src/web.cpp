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

    /* 状态码。404 之类直接报"页面不存在"，比"抓到 0 字节"友好得多 */
    DWORD code = 0, clen = sizeof(code);
    if (HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, nullptr)) {
        if (code >= 400) {
            wchar_t buf[192];
            swprintf(buf, 192, L"服务器返回 %u，页面不存在或已被删除。", (unsigned)code);
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
 * 策略：先按系统代理设置试一次，失败再强制直连试一次。
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

    std::wstring e1;
    if (FetchOnce(u, false, body, finalUrl, e1, timeoutMs)) return true;

    /* 直连重试：代理失败时体面兜底（内网地址、本机测试服务都靠这条路） */
    std::string body2;
    std::wstring final2 = u, e2;
    if (FetchOnce(u, true, body2, final2, e2, timeoutMs)) {
        body.swap(body2);
        finalUrl = final2;
        err.clear();
        return true;
    }

    body.clear();
    err = e1;
    if (!e2.empty() && e2 != e1) { err += L"（直连也失败："; err += e2; err += L"）"; }
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
