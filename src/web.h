/* web.h - 网页抓书：把一个网页小说的目录页抓成整本书
 * ============================================================================
 * 用户场景：手里没有 txt，只有某个小说网站的在线阅读页。
 * 做法：给出目录页网址 → 抓目录页 → 从页面里找出所有"章节链接" → 逐章抓正文
 *       剥成纯文本 → 拼成一份完整的文本，再交给原来的 BookLoad 管线。
 *
 * 为什么"抓成文本"而不是内嵌浏览器：
 *   内嵌一个浏览器内核（IE 控件 / WebView2）会让 exe 体积和依赖暴涨，而且网页上的
 *   "下一章"按钮、广告、悬浮层都得自己处理。抓成文本后，分章、搜索、书签、
 *   位置记忆、字体配色全部天然可用 —— 与 EPUB/DOCX 的处理思路完全一致。
 *
 * 三条硬约束（都写在实现里）：
 *   1) 只做 HTTP/HTTPS GET，不带 Cookie 以外的任何状态，不解析 JS；
 *   2) 章节数、单页大小、总大小都有上限，防止用户点了一个"全书一万章"的站；
 *   3) 任何一个环节失败都要能给出中文原因，不能静默返回半本书。
 * ============================================================================ */
#ifndef NR_WEB_H
#define NR_WEB_H

#include <string>
#include <vector>
#include <windows.h>

/* 这个字符串看起来是不是网址（以 http:// 或 https:// 开头，大小写不敏感）。
 * 用来判断用户输入的是"路径"还是"网址"。 */
bool WebIsUrl(const std::wstring& s);

/* 抓一个网址的完整响应体（只处理 HTTP/HTTPS，跟随重定向）。
 * 参数 url      ：要抓的网址（宽字符，支持中文域名/路径，内部会转成 UTF-8 请求）。
 * 参数 body     ：输出，响应体原始字节（**不做编码转换**，交给 BytesToText 统一判定）。
 * 参数 finalUrl ：输出，重定向之后的最终网址（拼相对链接时要用它作基准）。
 * 参数 err      ：输出，失败原因（中文）。
 * 参数 timeoutMs：连接与读取超时（毫秒）。
 * 返回：拿到 2xx 响应返回 true。 */
bool WebFetchUrl(const std::wstring& url, std::string& body, std::wstring& finalUrl,
                 std::wstring& err, int timeoutMs = 15000);

/* 抓书进度回调。每抓完一章调用一次。
 * 参数 done / total：已完成 / 总数（目录页解析失败时 total 为 0）。
 * 参数 msg         ：给用户看的进度文字（例如"正在抓 3/120：第2章 xxx"）。
 * 返回 false 表示用户取消，抓取会尽快停下（已抓到的部分仍然会返回）。 */
typedef bool (*WebProgressFn)(void* ctx, int done, int total, const std::wstring& msg);

/* ---------------------------- 书源 ----------------------------
 * "网页小说"要求用户自己找到目录页网址；对普通用户来说这一步太难
 * （还得知道站点的目录页长什么样）。书源就是把"搜索 + 解析目录 + 抽正文"
 * 这三件事按某个站点的具体结构描述出来，用户只需要输入书名。
 *
 * 书源不是写死在代码里的：每条规则都存在 NovelReader.ini 的 [sources] 段，
 * 用户可以在「书源管理」里改、加、删（见 source.h）。程序自带一个示例源，
 * 既可以拿来用，也是写新规则时的模板。
 */

struct BookSource;      /* 定义在 source.h；这里只用引用，避免两处互相包含 */

/* 搜索结果里的一条（一本书）。 */
struct WebBookHit {
    std::wstring title;     /* 书名，例如"斗破苍穹" */
    std::wstring url;       /* 书籍详情页网址（绝对地址） */
    std::wstring desc;      /* 简介摘要，截断到几十字，列表里给用户判断是哪本 */
};

/* 用指定书源搜索（关键字会做 UTF-8 百分号编码）。
 * 与下面的 WebSourceSearch 的区别：这个明确指定"用哪个源"，
 * 界面上的书源下拉框、命令行工具都走它。 */
bool WebSourceSearchEx(const BookSource& src, const std::wstring& keyword,
                       std::vector<WebBookHit>& out, std::wstring& err, int timeoutMs = 20000);

/* 用指定书源把一本书抓成文本（bookUrl 是书籍页/目录页网址）。
 * 规则全由调用方给，所以这里不认识任何具体站点。 */
bool WebSourceFetchBookEx(const BookSource& src, const std::wstring& bookUrl,
                          std::wstring& text, std::wstring& title, std::wstring& err,
                          WebProgressFn progress = nullptr, void* ctx = nullptr,
                          const std::wstring& knownTitle = std::wstring());

/* 在书源里按书名/作者搜索。
 * 参数 keyword：关键词（中文会做 UTF-8 百分号编码）。
 * 参数 out    ：输出，按站点返回顺序排列（通常相关度从高到低）。
 * 参数 err    ：输出，失败原因（中文）。
 * 返回：至少搜到一条返回 true；一条都没有也返回 false 并给出原因。 */
bool WebSourceSearch(const std::wstring& keyword, std::vector<WebBookHit>& out,
                     std::wstring& err, int timeoutMs = 20000);

/* 从目录页网址抓整本书。
 * 参数 url      ：目录页网址（也可以是某一章的网址 —— 页面上没有章节链接时，
 *                 就把这一页当全书）。
 * 参数 text     ：输出，拼好的全书正文（宽字符；每章标题单独一行，章间空一行）。
 * 参数 title    ：输出，书名（优先取网页 <title>，否则取第一个章节名）。
 * 参数 err      ：输出，失败原因（中文）。
 * 参数 progress ：进度回调，可为 nullptr。
 * 返回：成功（至少抓到一章正文）返回 true。 */
bool WebFetchBook(const std::wstring& url, std::wstring& text, std::wstring& title,
                  std::wstring& err, WebProgressFn progress = nullptr, void* ctx = nullptr);

/* 用当前书源（SourceCurrent()）把一本书抓成文本（bookUrl 是书籍详情页）。
 * 规则全部来自书源，所以换源不用改代码；具体规则怎么起作用见 WebSourceFetchBookEx。
 * 参数 knownTitle：搜索结果里的书名，可为空（为空时从网页标题推断）。
 * 返回：成功（至少抓到一章正文）返回 true。 */
bool WebSourceFetchBook(const std::wstring& bookUrl, std::wstring& text, std::wstring& title,
                        std::wstring& err, WebProgressFn progress = nullptr, void* ctx = nullptr,
                        const std::wstring& knownTitle = std::wstring());

/* 网页抓下来的书要落盘成一个 txt（缓存目录里），这样书架、进度、书签、
 * 重新打开全都能复用现有机制。
 * 参数 title：书名（用来拼文件名，会过滤掉 Windows 文件名非法字符）。
 * 返回：完整路径，例如 %LOCALAPPDATA%\NovelReader\web\某某小说.txt。
 * 目录不存在时会自动创建。 */
std::wstring WebCachePath(const std::wstring& title);

#endif /* NR_WEB_H */
