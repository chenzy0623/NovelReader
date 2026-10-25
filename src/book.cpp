/* book.cpp - 打开 txt：编码识别、正文规范化、段落/章节索引、全文搜索
 *
 * 这是整个程序唯一碰"字节"的地方。数据流：
 *
 *     磁盘文件
 *        │ ReadFile 整块读进 std::vector<char>（上限 256MB）
 *        ▼
 *     DecodeBytes  ── 判断编码（BOM / UTF-16LE 探测 / UTF-8 / GB18030 / GBK / ANSI）
 *        │           解码成 std::wstring（UTF-16），此后全程序都用宽字符
 *        ▼
 *     Normalize    ── 统一换行、合并连续空行、tab 换空格、去掉控制字符和 BOM
 *        │
 *        ├─► paraOff[]   段落起点表（末尾多一个哨兵：全文字符数）
 *        └─► chapters[]  章节表（逐段跑 IsChapterLine，命中就记一条）
 *
 * 为什么内部全用 UTF-16：Windows 的 GDI 文本绘制、剪贴板、窗口标题都是
 * 宽字符接口，统一之后就不用到处编码转换了。代价是内存约等于原文件的 2 倍
 * （GBK 书转 UTF-16 后其实差不多，UTF-8 中文会翻倍），15MB 的书约 15~30MB，可接受。
 *
 * 性能取舍：打开时一次性算完段落表和章节表（都是线性扫描，很快），
 * 而"每段有多少视觉行"这种排版信息不在这里算——那是 view.cpp 的懒排版，
 * 界面上滚到哪才算到哪。
 */
#include "common.h"
/* 多格式入口：BookLoad 里按扩展名分派到 format.cpp（epub/docx/html/md → 纯文本） */
#include "format.h"
/* PDF 入口：pdf.cpp 自己解析 PDF 结构，抽出带段落换行的纯文本 */
#include "pdf.h"
#include <cwchar>
#include <cstring>
#include <algorithm>

/* 当前打开的书。整个程序只有一本（单文档），所以直接放全局。 */
Book g_book;

/* ================= 编码识别 ================= */

/* 严格校验一段字节是不是合法的 UTF-8，顺便告诉调用方"里面有没有多字节字符"。
 *
 * 为什么要自己校验而不是直接交给 MultiByteToWideChar：
 *   Windows 的 CP_UTF8 转换很宽容，遇到非法字节会塞替换符（U+FFFD）而不报错，
 *   于是 GBK 文本也会被"成功"转成 UTF-8 的一堆乱码。必须先自己判，
 *   GBK 书才轮得到后面的 GB18030 分支，不会全军覆没成乱码。
 *
 * 判定规则（RFC 3629）：
 *   1 字节 0xxxxxxx                     —— ASCII
 *   2 字节 110xxxxx 10xxxxxx            —— 码位 >= 0x80（否则就是超长编码，非法）
 *   3 字节 1110xxxx 10xxxxxx 10xxxxxx   —— 码位 >= 0x800
 *   4 字节 11110xxx ...                 —— 码位 >= 0x10000 且 <= 0x10FFFF
 *   另外代理区 U+D800~U+DFFF 单独出现也是非法的（UTF-8 里不该有）
 *
 * 参数：p/n 是字节和长度；asciiOnly 出参——整段全是 ASCII 时为 true
 *      （调用方可以走更快的逐字节拷贝，不用调 MultiByteToWideChar）。
 * 返回：合法 true；任何一处非法立刻返回 false（不需要看完整个文件）。 */
static bool IsValidUtf8(const unsigned char* p, size_t n, bool* asciiOnly)
{
    size_t i = 0;
    bool multi = false;          /* 是否出现过非 ASCII 字符 */
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }      /* ASCII，直接跳过 */
        int len;
        unsigned int cp;
        /* 从首字节的掩码位看出后面还跟几个字节，并取出码位高几位 */
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else return false;                    /* 10xxxxxx / 11111xxx 都不能当首字节 */
        if (i + (size_t)len > n) return false;        /* 末尾被截断 */
        for (int k = 1; k < len; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) return false;    /* 后续字节必须是 10xxxxxx */
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        /* 反超长编码：能用更短编码表示的码位在这里一律拒绝 */
        if (len == 2 && cp < 0x80)        return false;
        if (len == 3 && cp < 0x800)       return false;
        if (len == 4 && cp < 0x10000)     return false;
        if (cp > 0x10FFFF)                return false;   /* 超出 Unicode 范围 */
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;   /* 代理区不该出现在 UTF-8 里 */
        multi = true;
        i += (size_t)len;
    }
    if (asciiOnly) *asciiOnly = !multi;
    return true;
}

/* 按代码页把字节转换成 UTF-16 宽字符串，追加到 out 末尾（不清空 out）。
 * cp 可以是 CP_UTF8(65001)、936(GBK)、54936(GB18030)、CP_ACP(系统 ANSI) 等。
 * 返回 false 表示解码失败（非法字节序列），调用方会再试别的代码页。 */
static bool MbToWide(UINT cp, const char* p, int n, std::wstring& out)
{
    if (n <= 0) return true;
    int need = MultiByteToWideChar(cp, 0, p, n, nullptr, 0);   /* 先问需要多少宽字符 */
    if (need <= 0) return false;
    size_t old = out.size();
    out.resize(old + (size_t)need);
    int got = MultiByteToWideChar(cp, 0, p, n, &out[old], need);
    if (got <= 0) { out.resize(old); return false; }           /* 失败则回滚，保持 out 不变 */
    out.resize(old + (size_t)got);
    return true;
}

/* 把 UTF-16 的双字节流（16LE / 16BE）转成 wchar_t 序列，追加到 out 末尾。
 * 注意：
 *   - 调用前 BOM 已由调用方跳过，这里不管 BOM；
 *   - wchar_t 在 Windows 上就是 16 位，所以一个"码元"对应一个 wchar_t，
 *     代理对（emoji 等）会自然地被拆成两个 wchar_t，正好符合 Win32 的习惯；
 *   - 奇数长度时最后一个字节被丢弃（n/2 向下取整）。 */
static void Wide16ToWStr(const unsigned char* p, size_t n, bool bigEndian, std::wstring& out)
{
    size_t m = n / 2;
    size_t old = out.size();
    out.resize(old + m);
    for (size_t i = 0; i < m; i++) {
        unsigned char lo, hi;
        if (bigEndian) { hi = p[2 * i]; lo = p[2 * i + 1]; }
        else           { lo = p[2 * i]; hi = p[2 * i + 1]; }
        out[old + i] = (wchar_t)(((unsigned)hi << 8) | lo);
    }
}

/* 字节 → UTF-16 文本。识别顺序是"从最可靠的特征往最不可靠的猜"：
 *
 *   1. BOM：UTF-8(EF BB BF) / UTF-16LE(FF FE) / UTF-16BE(FE FF) —— 有 BOM 就不用猜
 *   2. 无 BOM 的 UTF-16LE：半数以上奇数位字节是 0x00（中文在 UTF-16LE 里高位多为 0）
 *   3. 严格合法的 UTF-8 且含中文 —— 中文小说里 UTF-8 很常见
 *   4. GB18030(54936) → GBK(936) → 系统 ANSI(CP_ACP) —— 国内老书的绝对主流
 *
 * 为什么 GB18030 放在 GBK 前面：GB18030 是 GBK 的超集，用同一个代码页
 * 解码 GBK 文本不会出错，而且能多认四种字节的扩展汉字。
 * 最后一定要落到 CP_ACP 兜底，宁可出几个乱码字，也不要整本书打不开。 */
static std::wstring DecodeBytes(const std::vector<char>& buf)
{
    std::wstring out;
    const unsigned char* p = (const unsigned char*)buf.data();
    size_t n = buf.size();
    if (n == 0) return out;

    /* 1. 有 BOM：UTF-8 / UTF-16LE / UTF-16BE */
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        MbToWide(CP_UTF8, (const char*)p + 3, (int)(n - 3), out);   /* 跳过 3 字节 BOM */
        return out;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        Wide16ToWStr(p + 2, n - 2, false, out);                     /* UTF-16LE */
        return out;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        Wide16ToWStr(p + 2, n - 2, true, out);                      /* UTF-16BE */
        return out;
    }

    /* 2. 无 BOM 的 UTF-16LE：中文每个字的第二个字节基本都是 0x00。
     *    判据：全部奇数位字节里有 3/4 以上是 0，就认为它是 UTF-16LE。
     *    要求文件长度是偶数且大于 64 字节，避免短文件被误判。
     *    注意必须先于 UTF-8 判断：带 0x00 的字节流不是合法 UTF-8，顺序反了也不影响，
     *    但放前面能省一次全文件扫描。 */
    if (n > 64 && (n % 2) == 0) {
        size_t zeros = 0;
        for (size_t i = 1; i < n; i += 2) if (p[i] == 0) zeros++;
        if (zeros * 4 >= (n / 2) * 3) { Wide16ToWStr(p, n, false, out); return out; }
    }

    /* 3. 纯 UTF-8（含 ASCII）*/
    bool asciiOnly = true;
    if (IsValidUtf8(p, n, &asciiOnly)) {
        /* 全 ASCII 时逐字节扩充更省事（没有多字节序列要处理） */
        if (asciiOnly) { out.reserve(n); for (size_t i = 0; i < n; i++) out.push_back((wchar_t)(unsigned char)buf[i]); return out; }
        MbToWide(CP_UTF8, (const char*)p, (int)n, out);
        return out;
    }

    /* 4. GB18030 / GBK / 系统 ANSI
     * 每试一个代码页前先 clear()，否则失败时 out 里会残留半截乱码。 */
    if (MbToWide(54936, (const char*)p, (int)n, out)) return out;
    out.clear();
    if (MbToWide(936, (const char*)p, (int)n, out)) return out;
    out.clear();
    MbToWide(CP_ACP, (const char*)p, (int)n, out);
    return out;
}

/* ================= 文本规范化 ================= */

/* 判断是不是"空白"：半角空格、制表符、回车、换行，外加全角空格(0x3000)
 * 和不间断空格(0x00A0)——中文小说里这两种"看不见的空格"特别多，
 * 不处理的话章节标题会带着它们，比对时永远匹配不上。 */
static inline bool IsSpaceW(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000 || c == 0x00A0;
}

/* 把解码后的原文整理成"适合显示和索引"的形式，逐字符重建一遍字符串。
 * 处理的脏数据（都是实际 txt 里常见的）：
 *   - \r\n 和单独的 \r → 统一成 \n（否则段落切分会多出一堆空段）
 *   - 连续多个空行 → 合并成一个（有些书每段之间空两行，看着很散）
 *   - 段落末的尾随空格 → 删掉（不影响阅读，还能让段落长度计算准确）
 *   - 行首空行 → 直接丢掉（prevNl 初值为 true 就是干这个的）
 *   - \t → 两个半角空格（制表符在自绘排版里没法对齐，换成空格更稳）
 *   - 控制字符、BOM(0xFEFF)、零宽空格(0x200B)、方向标记(0x200E/0x200F) → 删掉
 *
 * 注意：这里**不**去掉每行行首的空格。中文小说常用"两个全角空格"表示缩进，
 * 这正是排版想要的，去掉反而难看。 */
static void Normalize(std::wstring& s)
{
    std::wstring o;
    o.reserve(s.size() + 16);
    bool prevNl = true;   /* 起始视作换行，去掉开头空行 */
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'\r') {
            /* \r\n 当成一个字符处理，顺手把 \n 吃掉 */
            if (i + 1 < s.size() && s[i + 1] == L'\n') i++;
            c = L'\n';
        }
        if (c == L'\n') {
            /* 先削掉这一行末尾的空格/全角空格 */
            while (!o.empty() && (o.back() == L' ' || o.back() == L'\t' || o.back() == 0x3000)) o.pop_back();
            if (prevNl) continue;              /* 多个空行合并成一个 */
            o.push_back(L'\n');
            prevNl = true;
            continue;
        }
        if (c == L'\t') { o.push_back(L' '); o.push_back(L' '); prevNl = false; continue; }
        if (c == 0x00A0) c = L' ';             /* 不间断空格统一成普通空格 */
        /* 控制字符 + 各种不可见字符一律丢弃 */
        if (c < 0x20 || c == 0x7F || c == 0xFEFF || c == 0x200B || c == 0x200E || c == 0x200F) continue;
        o.push_back(c);
        prevNl = false;
    }
    /* 去掉结尾的换行，避免最后多出一个空段落 */
    while (!o.empty() && o.back() == L'\n') o.pop_back();
    /* 去掉每行行首多余空格对阅读无碍，保留原样 */
    s.swap(o);
}

/* ================= 分章 ================= */

/* 这个字符能不能当"章节号"用：阿拉伯数字，或者中文数字/大写金额数字。
 * 用 wcschr 在常量串里查，比写一大堆 if 清楚。 */
static bool IsNumChar(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return true;
    static const wchar_t* t = L"零一二三四五六七八九十百千万亿两〇壹贰叁肆伍陆柒捌玖拾佰仟";
    return wcschr(t, c) != nullptr;
}

/* 判断 [off, off+len) 这一段（一行）是不是章节标题。
 *
 * 三道关，全过才认：
 *   ① 长度 <= 60 字，且不含句号"。"（正文句子几乎一定带句号；标题基本不带）
 *      含逗号时还要求逗号出现在前 40 字内，避免把"标题＋正文挤在一行"的段落认成标题
 *   ② 命中三类写法之一：
 *      a) 特殊标题词：序章 / 序言 / 楔子 / 引子 / 前言 / 后记 / 尾声 / 终章 /
 *         番外 / 外传 / 正文 / 大结局 / 结局 / 附录 / 完本感言 / 作品相关
 *         —— 只要行首是这些词就算（"番外 一"、"楔子"都能中）
 *      b) 英文：Chapter 1 / CHAPTER 12 / chapter.3（Chapter 后允许空格/点/冒号）
 *      c) 中文：第 + 数字 + 单位字（章节回卷集篇幕话）
 *         数字之间允许夹空格（"第 12 章"这种排版很常见）
 *   ③ 对容易误判的单位字做排除：
 *      "第三回合"（回+合）、"第三回到家/第三回到"、"第三节课"、"第几节目"、
 *      "第二篇章/第几篇幅"——这些都更像正文，不能当章节。
 *
 * 出参：*oOff / *oLen 返回**去掉首尾空白后**的标题范围（章节名显示时要用）。
 * 返回：是章节标题返回 true。 */
static bool IsChapterLine(const std::wstring& t, int off, int len, int* oOff, int* oLen)
{
    int s = off, e = off + len;
    while (s < e && IsSpaceW(t[s])) s++;              /* 跳过行首空白 */
    while (e > s && IsSpaceW(t[e - 1])) e--;          /* 去掉行尾空白 */
    int L = e - s;
    if (L <= 0 || L > 60) return false;               /* 太长的不可能是标题 */
    for (int i = s; i < e; i++) {
        if (t[i] == L'。') return false;                 /* 正文句子通常有句号 */
        if (t[i] == L'，' && (i - s) > 40) return false; /* 长句里才有逗号，标题不会 */
    }

    static const wchar_t* units = L"章节回卷集篇幕话";
    const wchar_t* str = t.c_str() + s;

    /* 3 类特殊标题 */
    static const wchar_t* kws[] = {
        L"序章", L"序言", L"楔子", L"引子", L"前言", L"后记", L"尾声", L"终章",
        L"番外", L"外传", L"正文", L"大结局", L"结局", L"附录", L"完本感言", L"作品相关"
    };
    for (size_t k = 0; k < sizeof(kws) / sizeof(kws[0]); k++) {
        size_t kl = wcslen(kws[k]);
        if ((size_t)L >= kl && wcsncmp(str, kws[k], kl) == 0) { *oOff = s; *oLen = L; return true; }
    }
    /* 英文写法：Chapter 1 / CHAPTER 12 */
    if (L >= 8 && (wcsncmp(str, L"Chapter", 7) == 0 || wcsncmp(str, L"CHAPTER", 7) == 0 || wcsncmp(str, L"chapter", 7) == 0)) {
        int i = 7;
        while (i < L && (str[i] == L' ' || str[i] == L'.' || str[i] == L':')) i++;   /* 跳过分隔符 */
        if (i < L && str[i] >= L'0' && str[i] <= L'9') { *oOff = s; *oLen = L; return true; }
    }
    /* 第X章 / 第X节 / 第X回 ... */
    if (str[0] == L'第') {
        int i = 1, cnt = 0;
        /* 吃掉所有数字字符（中间允许空格，空格不算数字） */
        while (i < L && (IsNumChar(str[i]) || str[i] == L' ')) { if (str[i] != L' ') cnt++; i++; }
        if (cnt > 0 && i < L) {                 /* 至少要有一个数字，且后面还有单位字 */
            wchar_t u = str[i];
            if (u && wcschr(units, u)) {
                /* 排除 "第三回合" 这类误判：单位字后面紧跟 合/会/答 等多字词 */
                if (i + 1 < L) {
                    wchar_t nx = str[i + 1];
                    if (u == L'回' && (nx == L'合' || nx == L'到' || nx == L'来' || nx == L'去' || nx == L'家')) return false;
                    if (u == L'节' && (nx == L'课' || nx == L'目' || nx == L'省')) return false;
                    if (u == L'篇' && nx == L'幅') return false;
                }
                *oOff = s; *oLen = L;
                return true;
            }
        }
    }
    return false;
}

/* ================= 载入 ================= */

/* 从完整路径里取文件名（不含目录）。找不到分隔符就当整串是文件名。 */
static std::wstring FileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

/* 打开并索引一本书。这是 book.cpp 的主入口，界面通过 UiOpenFile 调用。
 *
 * 步骤：读文件 → 判编码解码 → 规范化 → 建段落表 → 建章节表 → 取书名
 *
 * 参数：
 *   b     要填充的 Book（进来先清空，失败时保持空，避免界面显示半本书）
 *   path  文件绝对路径
 *   err   失败原因（中文，可直接弹给用户）；传 nullptr 表示不关心
 * 返回：成功 true。
 *
 * 注意几个上限：文件 > 256MB 直接拒绝（再大就该考虑分卷了），
 * 空文件也拒绝并给出明确提示。
 * 共享方式用 FILE_SHARE_READ|WRITE，这样别的程序正在写这本书时也能打开。 */
bool BookLoad(Book& b, const std::wstring& path, std::wstring* err)
{
    b.path = path;
    b.title.clear();
    b.text.clear();
    b.paraOff.clear();
    b.chapters.clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法打开文件（可能不存在或没有读取权限）";
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); if (err) *err = L"读取文件大小失败"; return false; }
    if (sz.QuadPart <= 0) { CloseHandle(h); if (err) *err = L"文件是空的"; return false; }
    if (sz.QuadPart > (LONGLONG)256 * 1024 * 1024) { CloseHandle(h); if (err) *err = L"文件超过 256MB，暂不支持"; return false; }

    /* 一次把文件读进内存：按大小预分配，再以 1MB 为单位循环读，
     * 避免单次 ReadFile 在超大文件上被截断。 */
    std::vector<char> buf;
    buf.resize((size_t)sz.QuadPart);
    size_t got = 0;
    while (got < buf.size()) {
        DWORD rd = 0;
        DWORD want = (DWORD)std::min<size_t>(buf.size() - got, 1u << 20);
        if (!ReadFile(h, &buf[got], want, &rd, nullptr) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got == 0) { if (err) *err = L"读取文件失败"; return false; }
    buf.resize(got);              /* 实际读到的可能比文件大小少（文件被截断等） */

    /* ---- 多格式分派（规则见 format.h）----
     * ① zip 容器（.epub / .docx）：原始字节直接交给 FormatFromZip 抽文字，
     *    它内部用 zip.cpp 解压，抽出来已经是宽字符正文；
     * ② 标记型文本（.html / .htm / .md …）：先按普通文本解码（编码自动识别），
     *    再剥掉标签 / Markdown 记号；
     * ③ 其它（.txt 或任意扩展名）：什么都不做，走原来的纯文本管线。
     * 三条路径最后都汇到同一份"纯文本 + '\n' 分段的字符串"上，所以后面的段落表、
     * 章节识别、全文搜索、位置记忆一行都不用改。
     */
    std::wstring w;
    if (PdfIsPdf(path)) {
        /* ---- PDF：抽文字 ----
         * PDF 是页面描述语言，不是"文本文件"：要自己扫对象 → 解字体 ToUnicode
         * → 解文本算子，才能拿到字。抽出来同样是"纯文本 + '\n' 分段"，所以
         * 分章、搜索、书签、进度记忆全都不用改。
         */
        std::wstring perr;
        if (!PdfExtractText((const uint8_t*)buf.data(), buf.size(), w, perr)) {
            if (err) *err = perr.empty() ? L"这份 PDF 里没有可显示的正文" : perr;
            return false;
        }
    } else if (FormatIsZipContainer(path)) {
        std::wstring ferr;
        if (!FormatFromZip(path, buf.data(), buf.size(), w, ferr)) {
            if (err) *err = ferr.empty() ? L"这个文件里没有可显示的正文" : ferr;
            return false;
        }
    } else {
        w = DecodeBytes(buf);
        /* 只有解码成功才去剥标记：解码失败时下面统一报"编码不支持"，别在这里重复处理 */
        if (!w.empty() && FormatIsMarkup(path)) {
            std::wstring plain = FormatStripMarkup(path, w);
            if (!plain.empty()) w.swap(plain);
        }
    }
    /* 字节缓冲已经没用了，立刻释放：打开 100MB 的书时这一步能省一半峰值内存 */
    buf.clear();
    buf.shrink_to_fit();
    if (w.empty()) { if (err) *err = L"文件内容无法识别为文本（编码不支持或格式里没有正文）"; return false; }

    Normalize(w);
    if (w.empty()) { if (err) *err = L"文件没有可显示的文本"; return false; }
    b.text.swap(w);               /* 换进 Book，省一次整串拷贝 */

    /* 段落表：先在 0 处放起点，然后每遇到一个 '\n' 就把"下一段的起点"记下来，
     * 最后再压入全文字符数作为哨兵。于是：
     *     第 i 段 = text[paraOff[i], paraOff[i+1])，末尾含那个 '\n'
     * 这样段落数 = paraOff.size() - 1，最后一段也不用特判。 */
    b.paraOff.reserve(1024);
    b.paraOff.push_back(0);
    for (size_t i = 0; i < b.text.size(); i++) {
        if (b.text[i] == L'\n') b.paraOff.push_back((uint32_t)(i + 1));
    }
    b.paraOff.push_back((uint32_t)b.text.size());

    /* 章节表：逐段判断是不是章节标题。段落数可能上万，但这一段只是
     * 每段做几次字符比较，整本书耗时在毫秒级，打开时一次算完最省事
     * （代价是换书要重新扫，但换书本来就不频繁）。 */
    int n = b.ParaCount();
    b.chapters.reserve(256);
    for (int p = 0; p < n; p++) {
        int a = (int)b.paraOff[p];
        int z = (int)b.paraOff[p + 1];
        int len = z - a;
        if (len > 0 && b.text[z - 1] == L'\n') len--;   /* 段末的 '\n' 不算内容 */
        if (len <= 0) continue;                        /* 空段落跳过 */
        int to = 0, tl = 0;
        if (IsChapterLine(b.text, a, len, &to, &tl)) {
            Chapter c; c.para = p; c.off = to; c.len = tl;
            b.chapters.push_back(c);
        }
    }

    /* 书名 = 文件名去掉扩展名。不解析 txt 里的"书名："行，
     * 因为各书格式太杂，文件名反而更可靠。
     * 例外：只有 .txt 才去扩展名，.epub/.docx/.html/.md 保留 —— 和首页书架
     * （ini.cpp 的 PosSaveMeta）用同一条规则，否则标题栏写"张三"、书架写"张三.epub"，
     * 同一本书在两处显示不同名字，看着像 bug。 */
    std::wstring fn = FileNameOf(path);
    size_t dot = fn.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        std::wstring ext = fn.substr(dot);
        for (size_t q = 0; q < ext.size(); q++) ext[q] = (wchar_t)towlower(ext[q]);
        if (ext == L".txt") fn = fn.substr(0, dot);
    }
    b.title = fn;
    return true;
}

/* ================= 查询工具 ================= */

/* 字符偏移 → 段落序号。
 * 做法：在段落起点表里 upper_bound 找第一个 > off 的起点，它的前一个就是答案。
 * 例：paraOff = [0, 10, 25, 40]，off = 17 → upper_bound 得到 25 的位置(下标 2)，
 *     减 1 得 1，即第 1 段（区间 [10,25)），正确。
 * 边界：off <= 0 返回 0；off >= 全文长度返回最后一段，保证调用方拿到的
 * 一定是合法段号（不需要再判越界）。 */
int BookCharToPara(const Book& b, int off)
{
    if (b.paraOff.empty()) return 0;
    if (off <= 0) return 0;
    if (off >= (int)b.text.size()) return b.ParaCount() - 1;
    std::vector<uint32_t>::const_iterator it =
        std::upper_bound(b.paraOff.begin(), b.paraOff.end(), (uint32_t)off);
    int idx = (int)(it - b.paraOff.begin()) - 1;
    if (idx < 0) idx = 0;
    if (idx >= b.ParaCount()) idx = b.ParaCount() - 1;
    return idx;
}

/* 段落 → 所属章节下标。章节表的 para 是递增的，所以二分找"最后一个起点 <= para"的章节。
 * 返回 -1 表示这一段在任何章节标题之前（书名、作者、简介那些开篇文字）。
 * 状态栏"第几章"、目录高亮、上下章跳转都靠它。 */
int BookChapterAtPara(const Book& b, int para)
{
    if (b.chapters.empty()) return -1;
    int lo = 0, hi = (int)b.chapters.size() - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (b.chapters[mid].para <= para) { ans = mid; lo = mid + 1; }   /* 记下来再往右找 */
        else hi = mid - 1;
    }
    return ans;
}

/* 这一段本身是不是章节标题段（不是"属于第几章"，而是"就是标题那一行"）。
 * 排版时用它决定这一行是否用大号加粗的标题样式。 */
bool BookIsChapterPara(const Book& b, int para)
{
    if (b.chapters.empty()) return false;
    int lo = 0, hi = (int)b.chapters.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (b.chapters[mid].para == para) return true;
        if (b.chapters[mid].para < para) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

/* 取章节标题文本（原样、已去首尾空格）。目录列表、状态栏第二段用。
 * chIdx 越界或该章节数据异常时返回空串，调用方不用判空指针。 */
std::wstring BookChapterTitle(const Book& b, int chIdx)
{
    if (chIdx < 0 || chIdx >= (int)b.chapters.size()) return L"";
    const Chapter& c = b.chapters[chIdx];
    if (c.off < 0 || c.len <= 0) return L"";
    return b.text.substr((size_t)c.off, (size_t)c.len);
}

/* 取第 para 段的文本（去掉段末换行）。
 * 注意这是"逻辑段落"，不是视觉行——排版换行是 view.cpp 的事。
 * 搜索结果的片段展示、复制、排版都从这里取原文。 */
std::wstring BookParaText(const Book& b, int para)
{
    if (para < 0 || para >= b.ParaCount()) return L"";
    int a = (int)b.paraOff[para];
    int z = (int)b.paraOff[para + 1];
    if (z > a && b.text[z - 1] == L'\n') z--;    /* 段末换行不算内容 */
    if (z <= a) return L"";
    return b.text.substr((size_t)a, (size_t)(z - a));
}

/* ================= 搜索 ================= */

/* 搜索用的"小写化"：只处理 ASCII 的 A-Z。
 * 为什么不调 CharLowerW / towlower：中文没有大小写，而为每个字符调一次
 * Unicode 大小写映射在几百万字符的全文上明显更慢；小说里要忽略大小写的
 * 基本只有英文字母和数字，这样最划算。 */
static inline wchar_t Lc(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c + 32);
    return c;
}

/* 从 from 开始向后找第一处 needle。
 * 返回：命中的字符偏移；找不到返回 -1。
 * 出参 hitLen：命中长度（= needle 长度，留给调用方做高亮宽度）。
 * 实现：朴素逐位置比较 + 首字符预筛。对小说这种纯文本足够快
 * （15MB 全书扫一遍在几十毫秒级），不值得上 KMP/BM。
 * 大小写：只有 ASCII 字母不敏感，中文原样比较。 */
int BookFindNext(const Book& b, const std::wstring& needle, int from, int* hitLen)
{
    if (hitLen) *hitLen = 0;
    if (needle.empty() || b.text.empty()) return -1;
    const wchar_t* t = b.text.c_str();
    size_t n = b.text.size(), m = needle.size();
    if (m > n) return -1;
    if (from < 0) from = 0;
    const wchar_t* nd = needle.c_str();
    wchar_t first = Lc(nd[0]);
    for (size_t i = (size_t)from; i + m <= n; i++) {
        if (Lc(t[i]) != first) continue;          /* 首字符不同直接跳过，省掉大部分比较 */
        size_t k = 1;
        for (; k < m; k++) if (Lc(t[i + k]) != Lc(nd[k])) break;
        if (k == m) { if (hitLen) *hitLen = (int)m; return (int)i; }
    }
    return -1;
}

/* 全文搜索，把每处命中追加到 out。
 *
 * 参数：
 *   needle  关键词（用户输入，原样；不做 trim，因为空格也可能是要搜的内容）
 *   out     结果数组，进来先 clear
 *   cap     最多收多少条（界面传 4000）。为什么要有上限：搜"的"这种字
 *           一本书几万处，全都塞进列表框会卡死界面，而且用户也不会翻到第 5000 条。
 * 每条结果的 chapter 字段预先算好，界面直接显示"第12章 …"而不用再查。
 * 步进：命中后从 p+len 继续，避免"哈哈哈"搜"哈哈"时重叠计数。 */
void BookSearch(const Book& b, const std::wstring& needle, std::vector<Hit>& out, int cap)
{
    out.clear();
    if (needle.empty() || b.text.empty()) return;
    int from = 0, hl = 0;
    while ((int)out.size() < cap) {
        int p = BookFindNext(b, needle, from, &hl);
        if (p < 0) break;
        Hit h; h.off = p; h.len = hl; h.chapter = BookChapterAtPara(b, BookCharToPara(b, p));
        out.push_back(h);
        from = p + (hl > 0 ? hl : 1);
    }
}

/* ============================================================================
 * 下面三个小函数是给 web.cpp（网页抓书）用的"桥"。
 * 为什么要在这里加，而不是让 web.cpp 自己实现一份：
 *   - BytesToText 必须和本文件里的 DecodeBytes 用**完全同一套编码判定**，
 *     否则会出现"网页上显示正常、存到本地打开是乱码"这种最难查的问题；
 *   - BookLooksLikeChapterTitle 必须和分章逻辑用同一个判定，
 *     否则抓下来的 chapter 链接会被切成"假章节"或者一章都切不出来。
 * 都是薄薄一层包装，不引入新逻辑。
 * ========================================================================== */

/* 任意字节流（HTTP 响应体、剪贴板里的 HTML 等）→ 宽字符串。
 * 内部就是 DecodeBytes，所以 BOM / UTF-16LE / UTF-8 / GB18030 全部照样认。
 * 参数 s：原始字节。返回：解码后的文本（失败也尽量返回能看的东西，不抛异常）。 */
std::wstring BytesToText(const std::string& s)
{
    std::vector<char> v(s.begin(), s.end());
    return DecodeBytes(v);
}

/* 这一行（段落）看起来是不是"章节标题"。给网页抓取用：网页目录里的
 * <a>第12章 xxx</a> 链接文字，要用同一套规则判断，才能保证抓下来的目录
 * 与本程序的分章结果一致。
 * 参数 line：去掉首尾空白之后的整行文本。返回：是章节标题返回 true。 */
bool BookLooksLikeChapterTitle(const std::wstring& line)
{
    if (line.empty()) return false;
    int o1 = 0, o2 = 0;
    return IsChapterLine(line, 0, (int)line.size(), &o1, &o2);
}