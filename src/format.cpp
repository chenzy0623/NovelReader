/* format.cpp - 多格式支持：EPUB / DOCX / HTML / Markdown → 纯文本
 * ============================================================================
 * 为什么统一"拍平"成纯文本：
 *   本程序的排版（换行、分章、滚动锚点）、全文搜索、位置记忆全部只认一种输入：
 *   「一段一段的宽字符文本」（book.cpp 里的 Book）。与其为每种格式再写一套渲染与
 *   定位逻辑，不如在打开文件的第一步就把它们转换成同一种纯文本 —— 后面的代码一行
 *   都不用改，而且 ebook 里那些"第 3 章 xxx"的标题照样能被分章规则认出来。
 *
 * 三条路径，入口在 FormatIsZipContainer / FormatIsMarkup 里按扩展名分派：
 *   1) .epub / .docx —— zip 容器，必须拿原始字节，靠 zip.cpp 解压出 XML/XHTML；
 *   2) .html/.htm/.xhtml/.md/.markdown —— 先按普通文本解码，再剥标记；
 *   3) 其它（.txt 或任何扩展名）—— 原样走 book.cpp 的老管线。
 *
 * 解析 XML 为什么手写字符串扫描而不是引入 XML 库：
 *   ① 程序体积是硬指标（成品 ~330 KB，一个 XML 库就翻倍）；
 *   ② 这里只需要"按固定标签名抠出少数几个属性/文本"，不需要通用 XML 语义；
 *   ③ 小说文本不可能有极端嵌套，扫描器对畸形输入只要"尽量别崩"就够了。
 *   凡是解析失败的地方一律走"回退路径"或返回明确错误，绝不静默产出半截内容。
 * ============================================================================ */
#include "format.h"
#include "zip.h"

#include <cwctype>
#include <algorithm>

/* ---------------- 小工具 ---------------- */

/* 取小写扩展名（含点），例如 "D:\\a\\B.EPUB" -> L".epub"。没有扩展名返回空串。
 * 用 towlower 逐字符转，而不是 CompareStringOrdinal，是因为这里只需做"是不是 epub"这种
 * 简单比较，而且 NTFS 不区分大小写、用户手上的文件名大小写五花八门。 */
static std::wstring ExtOf(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos) return L"";
    if (sep != std::wstring::npos && dot < sep) return L"";   /* 点在目录名里，不算扩展名 */
    std::wstring e = path.substr(dot);
    for (size_t i = 0; i < e.size(); i++) e[i] = (wchar_t)towlower(e[i]);
    return e;
}

/* UTF-8 → 宽字符。失败（非法 UTF-8）返回空串 —— 调用方都把空串当"这段内容没用"处理。 */
static std::wstring U8ToW(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/* 宽字符 → UTF-8（只在上层需要把 wstring 塞回 std::string 时用；目前没用到，
 * 保留是因为 epub 的 href 与文件名比较时可能需要，删掉会误报未使用函数警告，
 * 所以这里干脆不写 —— 见文件末尾说明）。 */

/* 把 XML/HTML 实体还原成字符：&amp; &lt; &gt; &quot; &apos; &nbsp; &#123; &#x1F;。
 * 不认识的实体原样保留（宁可显示 &foo; 也不要吞掉正文）。
 * 参数 s：待解码串；返回解码后的串。 */
static std::wstring DecodeEntities(const std::wstring& s)
{
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != L'&') { o += s[i++]; continue; }
        size_t semi = s.find(L';', i + 1);
        /* 实体名最长不过 10 个字符（&#xFFFF; 也就 8 个），找不到 ';' 或者隔太远就当普通 '&' */
        if (semi == std::wstring::npos || semi - i > 12) { o += s[i++]; continue; }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        if (!ent.empty() && ent[0] == L'#') {
            /* 数字实体：&#123 或 &#x1F。超出 BMP 的码点（>0xFFFF）这里不处理 ——
             * 小说里几乎不会出现，真要遇到就当普通字符输出，不影响可读性。 */
            unsigned long v = 0;
            bool ok = true;
            if (ent.size() > 2 && (ent[1] == L'x' || ent[1] == L'X')) {
                for (size_t k = 2; k < ent.size(); k++) {
                    wchar_t c = ent[k];
                    int d;
                    if (c >= L'0' && c <= L'9') d = c - L'0';
                    else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
                    else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
                    else { ok = false; break; }
                    v = v * 16 + (unsigned long)d;
                }
            } else {
                for (size_t k = 1; k < ent.size(); k++) {
                    wchar_t c = ent[k];
                    if (c < L'0' || c > L'9') { ok = false; break; }
                    v = v * 10 + (unsigned long)(c - L'0');
                }
            }
            if (ok && v > 0 && v <= 0xFFFF) o += (wchar_t)v;
            else o += s.substr(i, semi - i + 1);   /* 解不出来就原样保留 */
        } else {
            /* 命名实体表：中文小说（尤其网页转来的）里最常见的是引号、破折号、省略号、间隔号。
             * 表里没有的原样保留 —— 宁可显示 &foo; 也不要吞掉正文。
             */
            static const struct { const wchar_t* name; wchar_t ch; } kNamed[] = {
                { L"amp",    L'&' },       { L"lt",     L'<' },
                { L"gt",     L'>' },       { L"quot",   L'"' },
                { L"apos",   L'\'' },      { L"nbsp",   L' ' },
                { L"ldquo",  L'\u201C' },  { L"rdquo",  L'\u201D' },
                { L"lsquo",  L'\u2018' },  { L"rsquo",  L'\u2019' },
                { L"laquo",  L'\u00AB' },  { L"raquo",  L'\u00BB' },
                { L"mdash",  L'\u2014' },  { L"ndash",  L'\u2013' },
                { L"hellip", L'\u2026' },  { L"middot", L'\u00B7' },
                { L"bull",   L'\u2022' },  { L"deg",    L'\u00B0' },
                { L"times",  L'\u00D7' },  { L"divide", L'\u00F7' },
                { L"copy",   L'\u00A9' },  { L"reg",    L'\u00AE' },
                { L"trade",  L'\u2122' },  { L"ensp",   L' ' },
                { L"emsp",   L' ' },       { L"thinsp", L' ' },
                { L"shy",    L'\u00AD' }
            };
            wchar_t hit = 0;
            bool found = false;
            for (size_t k = 0; k < sizeof(kNamed) / sizeof(kNamed[0]); k++) {
                if (ent == kNamed[k].name) { hit = kNamed[k].ch; found = true; break; }
            }
            if (found) o += hit;
            else o += s.substr(i, semi - i + 1);
        }
        i = semi + 1;
    }
    return o;
}

/* 忽略大小写地比较 [pos, pos+len) 是否等于 ASCII 小写串 lit。
 * 用于在扫描器里认标签名（<P> 和 <p> 都要认）。 */
static bool MatchAtCI(const std::wstring& s, size_t pos, const wchar_t* lit)
{
    size_t n = wcslen(lit);
    if (pos + n > s.size()) return false;
    for (size_t k = 0; k < n; k++) {
        wchar_t a = (wchar_t)towlower(s[pos + k]);
        if (a != lit[k]) return false;
    }
    return true;
}

/* 从 zip 里按名字取一段内容（自动处理 deflate / stored）。
 * 参数 za：已加载的 zip；参数 name：条目全名（区分大小写，zip 规范如此）。
 * 返回 true 表示取到了，out 里是解压后的字节。 */
static bool ReadEntry(ZipArchive& za, const std::string& name, std::string& out)
{
    if (!za.Ok()) return false;
    if (za.Find(name) < 0) return false;
    return za.ReadName(name, out);
}

/* 把 URL 里的 %XX 还原、并把反斜杠统一成正斜杠、去掉 #片段。
 * epub 的 href 是相对 URL，常见 "text/ch1.xhtml"、"ch%201.xhtml"，个别书还有 "../"。
 * 参数 href：原始 href；返回整理后的相对路径（仍是斜杠风格）。 */
static std::string NormalizeHref(const std::string& href)
{
    std::string s = href;
    size_t hash = s.find('#');
    if (hash != std::string::npos) s = s.substr(0, hash);
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = 0, lo = 0;
            char a = s[i + 1], b = s[i + 2];
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            hi = hex(a); lo = hex(b);
            if (hi >= 0 && lo >= 0) { o += (char)(hi * 16 + lo); i += 2; continue; }
        }
        o += (s[i] == '\\') ? '/' : s[i];
    }
    return o;
}

/* 把 base（一个 zip 内的目录路径，例如 "OEBPS/"）与相对路径 rel 拼起来，并处理 "../"。
 * 参数 base：以 '/' 结尾的目录（可为空串表示根）；参数 rel：相对路径。
 * 返回：相对 zip 根的完整条目名（例如 "OEBPS/text/ch1.xhtml"）。 */
static std::string JoinPath(const std::string& base, const std::string& rel)
{
    std::string full;
    /* rel 是绝对路径（以 '/' 开头）时直接当根相对 */
    if (!rel.empty() && rel[0] == '/') full = rel.substr(1);
    else full = base + rel;

    /* 逐段处理 "./" 与 "../"，用栈式的做法：按 '/' 切段，遇到 ".." 弹一段 */
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= full.size()) {
        size_t e = full.find('/', i);
        if (e == std::string::npos) e = full.size();
        std::string seg = full.substr(i, e - i);
        if (!seg.empty() && seg != ".") {
            if (seg == "..") { if (!segs.empty()) segs.pop_back(); }
            else segs.push_back(seg);
        }
        i = e + 1;
    }
    std::string o;
    for (size_t k = 0; k < segs.size(); k++) {
        if (k) o += '/';
        o += segs[k];
    }
    return o;
}

/* ---------------- HTML / XHTML 去标签 ---------------- */

/* 这个标签是不是"块级"（应该在前后断开成新段落）。
 * 对小说来说，真正影响阅读的只有 p / div / br / h1..h6 / li / tr / hr / section / article。
 * 表格的 td 用空格而不是换行，否则一行表格会被拆成一堆两字段落。 */
static bool IsBlockTag(const std::wstring& name)
{
    static const wchar_t* kBlocks[] = {
        L"p", L"div", L"br", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6",
        L"li", L"tr", L"hr", L"section", L"article", L"blockquote",
        L"pre", L"table", L"ul", L"ol", L"dd", L"dt", L"figure", L"figcaption", L"body"
    };
    for (size_t i = 0; i < sizeof(kBlocks) / sizeof(kBlocks[0]); i++)
        if (name == kBlocks[i]) return true;
    return false;
}

/* 剥掉 HTML/XHTML 标签，返回纯文本。参数 in 是解码后的原始标记文本。
 * 扫描器是一个状态机，只认四种状态：
 *   ① 普通文本 —— 收集到 out，遇到 '<' 转 ②；
 *   ② 标签 —— 读到 '>' 结束，取标签名；块级标签补一个 '\n'；
 *   ③ 注释 <!-- --> —— 整段丢掉（很多书的头部有大段版权注释）；
 *   ④ <script> / <style> —— 一直丢到对应的结束标签（里面的 JS/CSS 绝不能进正文）。
 * 为什么不检查标签配对完整性：HTML 天生不严格，缺一个 </p> 很常见，
 * 我们的目标是"文字别丢、别多出垃圾"，不是校验合法性。
 * 边界：文件末尾还没闭合的标签/注释一律丢弃（已经收集的正文不受影响）。
 */
static std::wstring StripHtml(const std::wstring& in)
{
    std::wstring out;
    out.reserve(in.size() / 2);
    size_t i = 0;
    bool lastWasNewline = true;     /* 用来压缩连续空行：开头不要空行，中间最多留一个 */

    auto addText = [&](const std::wstring& t) {
        if (t.empty()) return;
        out += t;
        lastWasNewline = false;
    };
    auto addBreak = [&]() {
        if (!lastWasNewline) { out += L'\n'; lastWasNewline = true; }
    };

    while (i < in.size()) {
        wchar_t c = in[i];
        if (c != L'<') {
            /* ---- 状态 ①：普通文本，攒到下一个 '<' 为止 ---- */
            size_t e = in.find(L'<', i);
            if (e == std::wstring::npos) e = in.size();
            addText(DecodeEntities(in.substr(i, e - i)));
            i = e;
            continue;
        }

        /* ---- 状态 ③：注释 ---- */
        if (MatchAtCI(in, i, L"<!--")) {
            size_t e = in.find(L"-->", i + 4);
            i = (e == std::wstring::npos) ? in.size() : e + 3;
            continue;
        }
        /* ---- 状态 ④：整段丢弃的标签（script / style / head / title） ----
         * script、style 里是 JS / CSS，进正文就是垃圾；
         * head 里全是 meta / link，本来也不该显示；
         * title 尤其要跳：epub 的每章 xhtml 都有 <title>第一章 初入宗门</title>，
         * 只要不跳，就会和正文里的 <h1>第一章 初入宗门</h1> 重复一遍，
         * 章节目录里随即冒出成对的重复条目（这个 bug 真出现过）。
         * 做法：找到对应的结束标签，整段跳到它后面；找不到结束标签就丢到文件末尾。
         */
        {
            static const wchar_t* kSkipOpen[]  = { L"<script", L"<style", L"<head", L"<title" };
            static const wchar_t* kSkipClose[] = { L"</script", L"</style", L"</head", L"</title" };
            int hit = -1;
            for (int t = 0; t < 4; t++) {
                if (MatchAtCI(in, i, kSkipOpen[t])) { hit = t; break; }
            }
            if (hit >= 0) {
                size_t found = std::wstring::npos;
                /* 从 '<' 之后一格起逐位试探结束标签（大小写不敏感）；
                 * 留出 8 格余量是因为最短的结束标签 "</head" 也有 6 个字符 */
                for (size_t e = i + 1; e + 8 < in.size(); e++) {
                    if (MatchAtCI(in, e, kSkipClose[hit])) { found = e; break; }
                }
                if (found == std::wstring::npos) { i = in.size(); break; }
                size_t gt2 = in.find(L'>', found);
                i = (gt2 == std::wstring::npos) ? in.size() : gt2 + 1;
                continue;
            }
        }

        /* ---- 状态 ②：普通标签 ---- */
        size_t gt = in.find(L'>', i);
        if (gt == std::wstring::npos) { i = in.size(); break; }
        std::wstring tag = in.substr(i, gt - i + 1);
        i = gt + 1;

        /* 取标签名：跳过 '<'、可选的 '/'、再读到空白或 '/' 或 '>' */
        size_t k = 1;
        bool closing = false;
        if (k < tag.size() && tag[k] == L'/') { closing = true; k++; }
        size_t nameStart = k;
        while (k < tag.size() && !iswspace(tag[k]) && tag[k] != L'/' && tag[k] != L'>') k++;
        std::wstring name = tag.substr(nameStart, k - nameStart);
        for (size_t q = 0; q < name.size(); q++) name[q] = (wchar_t)towlower(name[q]);

        if (name == L"td" || name == L"th") { addText(L" "); continue; }   /* 表格单元格之间留个空格 */
        if (IsBlockTag(name)) addBreak();                                  /* 块级标签：断段 */
        (void)closing;   /* 块级标签开/闭我们都断段，所以这里用不上 closing，显式忽略 */
    }
    return out;
}

/* ---------------- Markdown 去标记 ---------------- */

/* 去掉行内标记：**粗体** __粗体__ *斜体* _斜体_ `代码` ~~删除~~ 以及 [文字](链接)。
 * 参数 s：一行文本；返回去掉标记后的一行。
 * 只处理"肯定不影响文字内容"的记号；像缩进代码块、表格这类一律按普通文本保留 —— 
 * 小说用 Markdown 存的话几乎只有标题和分隔线，过度处理反而容易吃字。 */
static std::wstring StripInlineMd(const std::wstring& s)
{
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        wchar_t c = s[i];
        /* 链接 [文字](链接) → 只留文字；图片 ![说明](路径) → 只留说明 */
        if (c == L'[' || (c == L'!' && i + 1 < s.size() && s[i + 1] == L'[')) {
            size_t lb = (c == L'!') ? i + 1 : i;
            size_t rb = s.find(L']', lb);
            if (rb != std::wstring::npos && rb + 1 < s.size() && s[rb + 1] == L'(') {
                size_t rp = s.find(L')', rb + 1);
                if (rp != std::wstring::npos) {
                    o += s.substr(lb + 1, rb - lb - 1);
                    i = rp + 1;
                    continue;
                }
            }
        }
        /* 成对的强调记号直接跳过；单个 '*' '_' 也可能是正文（例如 "a*b"），
         * 但小说里 '*' 极少作正文，跳过更省事 */
        if (c == L'*' || c == L'`' || c == L'~') { i++; continue; }
        if (c == L'_') {
            /* "_" 只在两侧都是空白/边界时跳过，避免把 snake_case 单词拆坏 */
            bool leftOk = (o.empty() || iswspace(o[o.size() - 1]));
            bool rightOk = (i + 1 >= s.size() || iswspace(s[i + 1]));
            if (leftOk || rightOk) { i++; continue; }
        }
        /* 反斜杠转义：\* 之类的下一个字符当字面量 */
        if (c == L'\\' && i + 1 < s.size()) { o += s[i + 1]; i += 2; continue; }
        o += c;
        i++;
    }
    return o;
}

/* 按行处理 Markdown：去掉标题井号、引用符、列表符号、代码围栏、水平分隔线。
 * 参数 in：Markdown 原文；返回纯文本（段落之间 '\n' 分隔）。 */
/* 供 web.cpp 复用：把一段 HTML 剥成纯文本（含实体解码、script/style/head 丢弃）。
 * 网页抓书时每个章节页都要走这一步，所以直接复用这里的 StripHtml，
 * 不再另写一份 —— 否则"看网页"和"看 HTML 文件"两种入口的排版会不一致。 */
std::wstring FormatHtmlToText(const std::wstring& html)
{
    return StripHtml(html);
}
static std::wstring StripMarkdown(const std::wstring& in)
{
    std::wstring out;
    out.reserve(in.size() / 2);
    size_t i = 0;
    while (i <= in.size()) {
        size_t e = in.find(L'\n', i);
        if (e == std::wstring::npos) e = in.size();
        std::wstring line = in.substr(i, e - i);
        i = e + 1;

        /* 行首归一：去掉 \r 与首尾空白（原来的缩进在纯文本里没有意义） */
        while (!line.empty() && (line.back() == L'\r' || iswspace(line.back()))) line.pop_back();
        size_t s0 = 0;
        while (s0 < line.size() && iswspace(line[s0])) s0++;
        line = line.substr(s0);

        /* 代码围栏 ``` 本身不输出（里面的内容保留） */
        if (line.size() >= 3 && line[0] == L'`' && line[1] == L'`' && line[2] == L'`') continue;

        /* 纯分隔线 --- / *** / ___ ：丢掉整行，它在纯文本里没有任何意义 */
        bool onlyRule = !line.empty();
        for (size_t k = 0; k < line.size(); k++) {
            wchar_t c = line[k];
            if (c != L'-' && c != L'*' && c != L'_' && !iswspace(c)) { onlyRule = false; break; }
        }
        if (onlyRule && line.find_first_of(L"-*_") != std::wstring::npos) continue;

        /* 标题：开头的 # （1~6 个）+ 空格 */
        size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == L'#') hashes++;
        if (hashes >= 1 && hashes <= 6 && hashes < line.size() && iswspace(line[hashes])) {
            line = line.substr(hashes);
            while (!line.empty() && iswspace(line[0])) line = line.substr(1);
        }
        /* 引用：开头的 '>' 可以有好几层 */
        while (!line.empty() && line[0] == L'>') {
            line = line.substr(1);
            while (!line.empty() && iswspace(line[0])) line = line.substr(1);
        }
        /* 列表符号：- * + 或 1. / 1) */
        if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'*' || line[0] == L'+')
            && iswspace(line[1])) {
            line = line.substr(1);
            while (!line.empty() && iswspace(line[0])) line = line.substr(1);
        } else {
            size_t d = 0;
            while (d < line.size() && line[d] >= L'0' && line[d] <= L'9') d++;
            if (d > 0 && d + 1 < line.size() && (line[d] == L'.' || line[d] == L')')
                && iswspace(line[d + 1])) {
                line = line.substr(d + 1);
                while (!line.empty() && iswspace(line[0])) line = line.substr(1);
            }
        }

        out += StripInlineMd(line);
        out += L'\n';
    }
    return out;
}

/* ---------------- DOCX ---------------- */

/* 从 docx 的 word/document.xml 里抽段落文本。
 * 参数 xml：解压出来的 document.xml（UTF-8 解码后的宽字符）。
 * 返回：纯文本，每个 <w:p> 一行。
 * 只认三类标签，其余一律跳过（docx 的 XML 里绝大部分是样式/属性，与我们无关）：
 *   <w:p>...</w:p>  一个段落 → 结尾换行
 *   <w:t>文字</w:t> 一段可见文字 → 原样收集（注意要解码 XML 实体）
 *   <w:br/> <w:cr/> 软换行 → 换行；<w:tab/> → 制表位变一个空格
 * 为什么靠 "<w:" 前缀判断：docx 的命名空间前缀理论上可以变（w: 只是约定），
 * 但 Word 自己生成的文件永远是 w:，python-docx、LibreOffice 也都是 w:，
 * 兼容极端情况不值得让解析器复杂一倍。
 */
static std::wstring DocxText(const std::wstring& xml)
{
    std::wstring out;
    out.reserve(xml.size() / 3);
    size_t i = 0;
    bool inP = false;

    while (i < xml.size()) {
        if (xml[i] != L'<') { i++; continue; }        /* 标签外的空白一律不要 */

        size_t gt = xml.find(L'>', i);
        if (gt == std::wstring::npos) break;
        std::wstring tag = xml.substr(i, gt - i + 1);
        i = gt + 1;

        /* ---- 段落边界 ---- */
        if (MatchAtCI(tag, 0, L"<w:p>") || MatchAtCI(tag, 0, L"<w:p ")) {
            if (!inP) {
                /* 新段落开始：前面已经有一行就换行；开头不留空行 */
                if (!out.empty() && out.back() != L'\n') out += L'\n';
                inP = true;
            }
            continue;
        }
        if (MatchAtCI(tag, 0, L"</w:p")) {
            if (!out.empty() && out.back() != L'\n') out += L'\n';
            inP = false;
            continue;
        }
        /* ---- 软换行 / 制表 ---- */
        if (MatchAtCI(tag, 0, L"<w:br") || MatchAtCI(tag, 0, L"<w:cr")) { out += L'\n'; continue; }
        if (MatchAtCI(tag, 0, L"<w:tab")) { out += L' '; continue; }

        /* ---- 可见文字 <w:t ...>文字</w:t> ---- */
        if (MatchAtCI(tag, 0, L"<w:t>") || MatchAtCI(tag, 0, L"<w:t ")) {
            size_t e = xml.find(L"</w:t>", i);
            if (e == std::wstring::npos) break;
            out += DecodeEntities(xml.substr(i, e - i));
            i = e + 6;
            continue;
        }
        /* 其它标签（<w:rPr>、<w:pPr>、<w:drawing>…）直接跳过标签本身即可 */
    }
    return out;
}

/* ---------------- EPUB ---------------- */

/* 从 container.xml 里找 OPF 的路径（<rootfile full-path="...">）。
 * 返回 OPF 在 zip 里的完整条目名；找不到返回空串。 */
static std::string EpubFindOpf(const std::string& containerXml)
{
    std::string s = containerXml;
    size_t p = s.find("full-path");
    while (p != std::string::npos) {
        size_t q = s.find('=', p);
        if (q == std::string::npos) break;
        q++;
        while (q < s.size() && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r' || s[q] == '\n')) q++;
        if (q < s.size() && (s[q] == '"' || s[q] == '\'')) {
            char quote = s[q++];
            size_t e = s.find(quote, q);
            if (e != std::string::npos && e > q) return s.substr(q, e - q);
        }
        p = s.find("full-path", p + 1);
    }
    return std::string();
}

/* 解析 OPF：拿 manifest 的 (id -> href) 与 spine 的 id 顺序。
 * 参数 opf：OPF 文本（UTF-8）。
 * 参数 hrefById：输出，manifest 里 id → href。
 * 参数 spineIds：输出，spine 里 itemref 的 idref 顺序。
 * 返回：解析到多少个 XHTML 条目（0 表示这份 OPF 不可用，调用方走兜底）。 */
static int EpubParseOpf(const std::string& opf,
                        std::vector<std::pair<std::string, std::string>>& hrefById,
                        std::vector<std::string>& spineIds)
{
    /* ---- manifest：<item id="x" href="y" media-type="z" .../> ---- */
    size_t i = 0;
    while ((i = opf.find("<item", i)) != std::string::npos) {
        size_t gt = opf.find('>', i);
        if (gt == std::string::npos) break;
        std::string tag = opf.substr(i, gt - i + 1);
        i = gt + 1;
        /* 用同一套属性抠取逻辑，只是这里是窄字符串：为了复用 AttrOf（宽字符版），
         * 先把标签转成宽字符再转回来太绕，这里直接写个窄串版本的最小实现。 */
        auto attr = [&tag](const char* name) -> std::string {
            size_t n = strlen(name);
            for (size_t k = 0; k + n < tag.size(); k++) {
                bool eq = true;
                for (size_t m = 0; m < n; m++) {
                    char a = tag[k + m];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (a != name[m]) { eq = false; break; }
                }
                if (!eq) continue;
                if (k > 0 && !isspace((unsigned char)tag[k - 1])) continue;
                size_t q = k + n;
                while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
                if (q >= tag.size() || tag[q] != '=') continue;
                q++;
                while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
                if (q >= tag.size()) return std::string();
                if (tag[q] == '"' || tag[q] == '\'') {
                    char quote = tag[q++];
                    size_t e = tag.find(quote, q);
                    if (e == std::string::npos) return std::string();
                    return tag.substr(q, e - q);
                }
                size_t e = q;
                while (e < tag.size() && !isspace((unsigned char)tag[e]) && tag[e] != '>') e++;
                return tag.substr(q, e - q);
            }
            return std::string();
        };
        std::string id = attr("id");
        std::string href = attr("href");
        std::string mt = attr("media-type");
        if (id.empty() || href.empty()) continue;
        /* 只要 XHTML 内容（图片、字体、样式表都跳过） */
        bool isXhtml = (mt.find("xhtml") != std::string::npos)
                    || (mt.find("html") != std::string::npos)
                    || (href.size() > 6 && href.compare(href.size() - 6, 6, ".xhtml") == 0)
                    || (href.size() > 5 && href.compare(href.size() - 5, 5, ".html") == 0)
                    || (href.size() > 4 && href.compare(href.size() - 4, 4, ".htm") == 0);
        if (!isXhtml) continue;
        hrefById.push_back(std::make_pair(id, href));
    }

    /* ---- spine：<itemref idref="x"/> 的顺序就是阅读顺序 ---- */
    size_t sp = opf.find("<spine");
    if (sp != std::string::npos) {
        size_t spe = opf.find("</spine>", sp);
        std::string body = opf.substr(sp, (spe == std::string::npos ? opf.size() : spe + 8) - sp);
        size_t k = 0;
        while ((k = body.find("<itemref", k)) != std::string::npos) {
            size_t gt = body.find('>', k);
            if (gt == std::string::npos) break;
            std::string tag = body.substr(k, gt - k + 1);
            k = gt + 1;
            size_t q = tag.find("idref");
            if (q == std::string::npos) continue;
            q += 5;
            while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
            if (q >= tag.size() || tag[q] != '=') continue;
            q++;
            while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
            if (q >= tag.size()) continue;
            char quote = tag[q];
            if (quote != '"' && quote != '\'') continue;
            q++;
            size_t e = tag.find(quote, q);
            if (e == std::string::npos) continue;
            spineIds.push_back(tag.substr(q, e - q));
        }
    }
    return (int)hrefById.size();
}

/* zip 容器入口：.epub / .docx 分别处理。
 * 参数 err 里写的中文原因会直接弹给用户，所以要能看懂（"这个文件里没有正文"之类）。
 */
bool FormatFromZip(const std::wstring& path, const char* bytes, size_t len,
                   std::wstring& text, std::wstring& err)
{
    text.clear();
    err.clear();
    std::wstring ext = ExtOf(path);
    if (!bytes || len == 0) { err = L"文件是空的。"; return false; }

    ZipArchive za;
    /* Load 只读中央目录，不整体解压 —— 几十 MB 的 docx/epub 也能瞬间打开 */
    if (!za.Load((const uint8_t*)bytes, len)) {
        err = L"这个文件不像有效的 EPUB / DOCX（压缩包结构损坏，或用了不支持的 ZIP64/加密）。";
        return false;
    }

    if (ext == L".docx") {
        std::string xml;
        /* 正文在 word/document.xml；少数生成器会带前缀或放在别处，这里再兜一次底 */
        if (!ReadEntry(za, "word/document.xml", xml)) {
            int idx = za.Find("word/document.xml");
            if (idx < 0) {
                /* 兜底：找任何以 document.xml 结尾的条目 */
                for (size_t k = 0; k < za.Count(); k++) {
                    const std::string& nm = za.At(k).name;
                    if (nm.size() >= 12 && nm.compare(nm.size() - 12, 12, "document.xml") == 0) {
                        if (za.ReadIndex(k, xml)) break;
                    }
                }
            }
            if (xml.empty()) { err = L"这个 docx 里找不到正文（word/document.xml）。"; return false; }
        }
        text = DocxText(U8ToW(xml));
        if (text.empty()) { err = L"这个 docx 的正文是空的（可能整篇都是图片）。"; return false; }
        return true;
    }

    if (ext != L".epub") {
        err = L"不支持的压缩格式。";
        return false;
    }

    /* ---- EPUB：container.xml → OPF → spine 顺序读 XHTML ---- */
    std::string container;
    if (!ReadEntry(za, "META-INF/container.xml", container)) {
        err = L"这个 epub 里没有 META-INF/container.xml，不是标准 EPUB。";
        return false;
    }
    std::string opfName = EpubFindOpf(container);
    if (opfName.empty()) { err = L"这个 epub 的 container.xml 里没有指向 OPF 的路径。"; return false; }

    std::string opf;
    if (!ReadEntry(za, opfName, opf)) {
        /* OPF 路径是纯 ASCII 的相对路径，直接按 UTF-8 转宽字符拼进提示里（拼不出来就不拼） */
        std::wstring hint = L"读不到 epub 的 OPF 文件";
        std::wstring wname = U8ToW(opfName);
        if (!wname.empty()) { hint += L"（"; hint += wname; hint += L"）"; }
        hint += L"。";
        err = hint;
        return false;
    }

    /* OPF 所在目录：所有 manifest 里的 href 都是相对它解析的 */
    std::string opfDir;
    {
        size_t slash = opfName.find_last_of('/');
        if (slash != std::string::npos) opfDir = opfName.substr(0, slash + 1);
    }

    std::vector<std::pair<std::string, std::string>> hrefById;
    std::vector<std::string> spineIds;
    int nItems = EpubParseOpf(opf, hrefById, spineIds);
    if (nItems <= 0) { err = L"这个 epub 的 OPF 里没有可读的 XHTML 条目。"; return false; }

    /* 组装最终要读的条目顺序：spine 优先（作者排的阅读顺序），
     * spine 缺失或全是无效 id 时退回 manifest 顺序（至少内容不丢）。 */
    std::vector<std::string> order;
    for (size_t k = 0; k < spineIds.size(); k++) {
        for (size_t m = 0; m < hrefById.size(); m++) {
            if (hrefById[m].first == spineIds[k]) {
                order.push_back(JoinPath(opfDir, NormalizeHref(hrefById[m].second)));
                break;
            }
        }
    }
    if (order.empty()) {
        for (size_t m = 0; m < hrefById.size(); m++)
            order.push_back(JoinPath(opfDir, NormalizeHref(hrefById[m].second)));
    }

    /* 逐章读取并去标签；章与章之间用两个换行分隔，这样"第 N 章"标题自然成为独立段落，
     * book.cpp 的分章规则能直接认出来。 */
    std::wstring all;
    int okCount = 0;
    for (size_t k = 0; k < order.size(); k++) {
        std::string body;
        if (!ReadEntry(za, order[k], body)) continue;   /* 单章缺失不影响整本，跳过即可 */
        std::wstring w = U8ToW(body);
        if (w.empty()) continue;
        std::wstring plain = StripHtml(w);
        if (plain.empty()) continue;
        if (!all.empty()) all += L"\n\n";
        all += plain;
        okCount++;
    }
    if (okCount == 0) { err = L"这个 epub 解出来的正文是空的。"; return false; }
    text = all;
    return true;
}

/* ---------------- 对外分派 ---------------- */

/* 扩展名 → 是否 zip 容器 */
bool FormatIsZipContainer(const std::wstring& path)
{
    std::wstring e = ExtOf(path);
    return e == L".epub" || e == L".docx";
}

/* 扩展名 → 是否标记型文本 */
bool FormatIsMarkup(const std::wstring& path)
{
    std::wstring e = ExtOf(path);
    return e == L".html" || e == L".htm" || e == L".xhtml"
        || e == L".md" || e == L".markdown" || e == L".mht" || e == L".mhtml";
}

/* 标记型文本 → 纯文本。按扩展名在 HTML 与 Markdown 两套规则之间选一套；
 * .mht/.mhtml 按 HTML 处理（它们只是带 MIME 头的 HTML，正文里照样有标签）。 */
std::wstring FormatStripMarkup(const std::wstring& path, const std::wstring& in)
{
    std::wstring e = ExtOf(path);
    if (e == L".md" || e == L".markdown") return StripMarkdown(in);
    return StripHtml(in);
}
