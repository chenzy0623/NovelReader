/* source.cpp - 书源的存取与内置示例源
 * ============================================================================
 * 这里只负责"书源怎么存、怎么读、怎么增删"，真正拿规则去抓网页的代码在
 * web.cpp（WebSourceSearchEx / WebSourceFetchBookEx）。分开的原因是：
 *   - 本文件几乎不碰网络，改坏了最多是列表读不出来，不会影响抓书；
 *   - 书源是用户数据，必须能单独备份（就是 NovelReader.ini 的 [sources] 段）。
 *
 * 存储格式（写在 NovelReader.ini 里，纯文本，用户可以直接手改）：
 *   [sources]
 *   n=2
 *   cur=0
 *   s0_name=内置示例源
 *   s0_search=https://…?q={kw}
 *   s0_list=<a href="([^"]+)"…>…
 *   s0_tocstart=category-toc-list
 *   s0_toc=<a href="([^"]+)[.]html"…>([^<]+)</a>
 *   s0_content=<div[^>]*id="neirong"[^>]*>([\s\S]*?)</div>
 *   s1_name=…
 *
 * 0 号永远是内置源：SourceEnsureBuiltin 会强制把 0 号设成内置源，
 * 这样即使用户把 [sources] 整段删掉、或者手改坏了，程序启动后仍然有一个能用的源。
 * ============================================================================ */
#include "common.h"
#include "source.h"

#include <string>
#include <vector>

/* 书源个数上限。ini 里每个源要占 6 个键，16 个已经足够，
 * 再多界面上的下拉框就长得没法用了。 */
static const int kMaxSources = 16;

/* 全部书源。0 号恒为内置源（SourceEnsureBuiltin 保证）。 */
static std::vector<BookSource> s_sources;

/* 当前选中的书源下标。真实值从 ini 的 cur 读，越界时由 SourceCurrent 夹回 0。 */
static int s_current = 0;

/* 内置示例源：一个结构典型的小说站（搜索页 → 书籍页目录 → 章节页正文）。
 * 用户可以在「书源管理」里照着它改，或者直接改成自己想看的站。
 *
 * 规则里故意只用最朴素的写法，方便用户对照理解：
 *   - 用 [^"] 而不是 .*? 来取属性，避免贪婪匹配跑过头；
 *   - 用 [.] 而不是 \. ，因为在 ini 里反斜杠要写两遍，容易互相踩；
 *   - 正文用 [\s\S]*? 惰性匹配到第一个 </div>，正好是正文容器的结尾。 */
const BookSource& SourceBuiltin()
{
    static BookSource s_builtin;
    static bool s_init = false;
    if (!s_init) {
        s_builtin.name = L"内置示例源";
        s_builtin.search = L"https://www.51shucheng.net/search?q={kw}";
        /* 结果页：<a href="书籍页" title="…"><h3>书名</h3></a><p>简介</p>
         * 第 3 组（简介）整体可选：有的站没有简介，没有也不能整条匹配失败。 */
        s_builtin.listRe = L"<a href=\"([^\"]+)\"[^>]*><h3>([^<]+)</h3></a>(?:<p>([^<]*)</p>)?";
        /* 目录容器起点：先切到这一段再跑目录正则，
         * 免得把"推荐阅读""新书榜"里同样叫"第几章"的链接混进来。 */
        s_builtin.tocStart = L"category-toc-list";
        /* 目录：<li><a href="…/23716.html" title="书名 第一章 xxx">第一章 xxx</a></li> */
        s_builtin.tocRe = L"<a href=\"([^\"]+[.]html)\"[^>]*>([^<]{1,60})</a>";
        /* 正文：<div class="neirong" id="neirong"><p>…</p>…</div> */
        s_builtin.contentRe = L"<div[^>]*id=\"neirong\"[^>]*>([\\s\\S]*?)</div>";
        s_builtin.builtin = true;
        s_init = true;
    }
    return s_builtin;
}

std::vector<BookSource>& SourceList()
{
    return s_sources;
}

/* 保证 0 号是内置源。返回值表示"这次有没有真的动过列表"。 */
void SourceEnsureBuiltin()
{
    if (s_sources.empty()) {
        s_sources.push_back(SourceBuiltin());
        return;
    }
    /* 0 号存在但不是内置源（老版本 ini 里 0 号是别的源，或者被手改坏了）：
     * 直接把内置源塞回 0 号，用户自己加的那些源仍然留在后面。 */
    if (!s_sources[0].builtin || s_sources[0].name != SourceBuiltin().name) {
        BookSource keep = s_sources[0];
        (void)keep;
        s_sources[0] = SourceBuiltin();
    }
}

/* 把 [sources] 段里第 i 个源的所有键读出来。空名字视为"没有这个源"。 */
static bool SourceReadKey(int i, BookSource& out)
{
    wchar_t sec[] = L"sources";
    std::wstring prefix = L"s" + std::to_wstring(i) + L"_";

    std::wstring name = IniGetStr(sec, (prefix + L"name").c_str(), L"");
    if (name.empty()) return false;

    out.name = name;
    out.search = IniGetStr(sec, (prefix + L"search").c_str(), L"");
    out.listRe = IniGetStr(sec, (prefix + L"list").c_str(), L"");
    out.tocStart = IniGetStr(sec, (prefix + L"tocstart").c_str(), L"");
    out.tocRe = IniGetStr(sec, (prefix + L"toc").c_str(), L"");
    out.contentRe = IniGetStr(sec, (prefix + L"content").c_str(), L"");
    out.builtin = (i == 0);
    return true;
}

void SourceLoad()
{
    s_sources.clear();

    int n = IniGetInt(L"sources", L"n", 0);
    if (n > kMaxSources) n = kMaxSources;
    for (int i = 0; i < n; i++) {
        BookSource src;
        if (!SourceReadKey(i, src)) continue;
        s_sources.push_back(src);
    }

    const bool hadBuiltin = !s_sources.empty() && s_sources[0].builtin;
    SourceEnsureBuiltin();

    s_current = IniGetInt(L"sources", L"cur", 0);
    if (s_current < 0 || s_current >= (int)s_sources.size()) s_current = 0;

    /* 第一次运行（ini 里还没有 [sources] 段）就把内置源写下去：
     * 用户打开 ini 就能看到一个现成的例子，照着改最快。 */
    if (!hadBuiltin) SourceSave();
}

void SourceSave()
{
    SourceEnsureBuiltin();

    wchar_t sec[] = L"sources";
    IniSetInt(sec, L"n", (int)s_sources.size());
    IniSetInt(sec, L"cur", s_current);

    for (size_t i = 0; i < s_sources.size(); i++) {
        std::wstring prefix = L"s" + std::to_wstring(i) + L"_";
        const BookSource& src = s_sources[i];
        IniSetStr(sec, (prefix + L"name").c_str(), src.name.c_str());
        IniSetStr(sec, (prefix + L"search").c_str(), src.search.c_str());
        IniSetStr(sec, (prefix + L"list").c_str(), src.listRe.c_str());
        IniSetStr(sec, (prefix + L"tocstart").c_str(), src.tocStart.c_str());
        IniSetStr(sec, (prefix + L"toc").c_str(), src.tocRe.c_str());
        IniSetStr(sec, (prefix + L"content").c_str(), src.contentRe.c_str());
    }

    /* 删掉的源要把旧键清空，否则下次 n 变小后那些键还躺在 ini 里，
     * 用户手改 ini 时会看到一堆"幽灵书源"。
     * 只清到"上一次写进 ini 的个数"为止 —— 全清到 kMaxSources 会让新装的
     * ini 里多出 90 行空键，反而更难手改。 */
    int oldN = IniGetInt(sec, L"n", 0);
    if (oldN > kMaxSources) oldN = kMaxSources;
    for (size_t i = s_sources.size(); i < (size_t)oldN; i++) {
        std::wstring prefix = L"s" + std::to_wstring(i) + L"_";
        IniSetStr(sec, (prefix + L"name").c_str(), L"");
        IniSetStr(sec, (prefix + L"search").c_str(), L"");
        IniSetStr(sec, (prefix + L"list").c_str(), L"");
        IniSetStr(sec, (prefix + L"tocstart").c_str(), L"");
        IniSetStr(sec, (prefix + L"toc").c_str(), L"");
        IniSetStr(sec, (prefix + L"content").c_str(), L"");
    }
}

int SourceCurrent()
{
    if (s_sources.empty()) return 0;
    if (s_current < 0 || s_current >= (int)s_sources.size()) s_current = 0;
    return s_current;
}

void SourceSetCurrent(int idx)
{
    if (idx < 0 || idx >= (int)s_sources.size()) return;
    s_current = idx;
    IniSetInt(L"sources", L"cur", s_current);
}

int SourceAdd()
{
    if ((int)s_sources.size() >= kMaxSources) return -1;
    BookSource src;
    src.name = L"新书源";
    src.builtin = false;
    s_sources.push_back(src);
    SourceSave();
    return (int)s_sources.size() - 1;
}

bool SourceDelete(int idx)
{
    if (idx <= 0 || idx >= (int)s_sources.size()) return false;
    if (s_sources[idx].builtin) return false;    /* 内置源只能改不能删 */
    s_sources.erase(s_sources.begin() + idx);
    if (s_current >= (int)s_sources.size()) s_current = 0;
    SourceSave();
    return true;
}
