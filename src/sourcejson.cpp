/* sourcejson.cpp - 书源的 JSON 导入 / 导出（格式说明见 sourcejson.h）
 * ============================================================================
 * 这里有三块东西，为了"单元测试能脱离窗口跑"全都不依赖 ui.cpp / ini.cpp：
 *   1. UTF-8 ⇄ UTF-16 转换（每个文件都有一份本地小实现，这里也不例外，
 *      免得为了两个函数把 common.h 拖进来）；
 *   2. 一个只有 200 行的递归下降 JSON 解析器 —— 够用就行，不追求完整实现；
 *   3. 读写文件的薄封装（CreateFileW + 循环 WriteFile）。
 *
 * 解析器为什么不用第三方库：仓库至今零外部依赖（静态链接、单 exe），
 * 为一个只有 7 个字符串字段的格式引入 nlohmann/json 不值得。
 *
 * 解析器为什么"宽容"：书源文件是人手抄、论坛帖子复制、别的阅读器改格式来的，
 * 报错太严只会让人放弃。所以：顶层可以是数组、键名有别名、未知字段跳过、
 * 数字/布尔/null 都能当未知值跳过。真正非法的结构才报错，并带字符位置。
 * ============================================================================ */
#include "sourcejson.h"

#include <windows.h>

#include <string>
#include <vector>

namespace {

/* ---------------- 编码转换 ---------------- */

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string u((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, nullptr, nullptr);
    return u;
}

/* 去掉首尾空白（含全角空格 U+3000 —— 从网页复制规则时经常带着它）。 */
std::wstring TrimW(const std::wstring& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e) {
        wchar_t c = s[b];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000) b++;
        else break;
    }
    while (e > b) {
        wchar_t c = s[e - 1];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000) e--;
        else break;
    }
    return s.substr(b, e - b);
}

/* ---------------- JSON 解析器 ---------------- */

/* 单文件上限。书源规则一条最多几 KB，8MB 已经是"是不是传错文件了"的量级，
 * 设上限是为了不让一个坏文件把内存吃光。 */
const int kMaxJsonBytes = 8 * 1024 * 1024;

/* 递归深度上限：防止畸形输入（[[[[…）把栈打穿。正常的书源文件深度不超过 4。 */
const int kMaxDepth = 32;

struct Parser {
    const std::wstring& s;
    size_t i = 0;
    std::wstring err;

    explicit Parser(const std::wstring& text) : s(text) {}

    bool eof() const { return i >= s.size(); }
    wchar_t peek() const { return i < s.size() ? s[i] : 0; }

    void skipWs()
    {
        while (i < s.size()) {
            wchar_t c = s[i];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') { i++; continue; }
            /* 容忍 BOM 出现在文件开头或结构之间（有人用记事本另存会带一个） */
            if (c == 0xFEFF) { i++; continue; }
            break;
        }
    }

    bool fail(const wchar_t* what)
    {
        if (err.empty()) {
            err = std::wstring(what) + L"（第 " + std::to_wstring((unsigned long long)(i + 1)) + L" 个字符处）";
        }
        return false;
    }

    /* 解析一个 JSON 字符串（当前位置必须是引号）。转义、\uXXXX、代理对都在这里处理。 */
    bool parseString(std::wstring& out)
    {
        out.clear();
        if (peek() != L'"') return fail(L"这里应该是一个字符串");
        i++;    /* 跳过开引号 */
        while (i < s.size()) {
            wchar_t c = s[i];
            if (c == L'"') { i++; return true; }
            if ((unsigned)c < 0x20) return fail(L"字符串里有未转义的控制字符");
            if (c != L'\\') { out.push_back(c); i++; continue; }
            /* 反斜杠转义 */
            i++;
            if (i >= s.size()) return fail(L"字符串以反斜杠结尾");
            wchar_t e = s[i++];
            switch (e) {
                case L'"':  out.push_back(L'"');  break;
                case L'\\': out.push_back(L'\\'); break;
                case L'/':  out.push_back(L'/');  break;
                case L'b':  out.push_back(L'\b'); break;
                case L'f':  out.push_back(L'\f'); break;
                case L'n':  out.push_back(L'\n'); break;
                case L'r':  out.push_back(L'\r'); break;
                case L't':  out.push_back(L'\t'); break;
                case L'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    /* 代理对：高位后面必须跟 \uDC00..\uDFFF，拼成一个码点 */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 1 < s.size() && s[i] == L'\\' && s[i + 1] == L'u') {
                            i += 2;
                            unsigned lo = 0;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                unsigned full = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                if (full <= 0xFFFF) out.push_back((wchar_t)full);
                                else {
                                    /* 目标平台 wchar_t 是 16 位：再拆回代理对 */
                                    full -= 0x10000;
                                    out.push_back((wchar_t)(0xD800 + (full >> 10)));
                                    out.push_back((wchar_t)(0xDC00 + (full & 0x3FF)));
                                }
                                break;
                            }
                            /* 后面不是低位代理：把刚才吃掉的 \u 当普通字符吐回去 */
                            out.push_back((wchar_t)cp);
                            break;
                        }
                        out.push_back((wchar_t)cp);
                        break;
                    }
                    out.push_back((wchar_t)cp);
                    break;
                }
                default:
                    return fail(L"字符串里有不认识的转义（只支持 \\\" \\\\ \\/ \\b \\f \\n \\r \\t \\uXXXX）");
            }
        }
        return fail(L"字符串没有闭合（文件是不是被截断了？）");
    }

    bool hex4(unsigned& out)
    {
        if (i + 4 > s.size()) return fail(L"\\u 后面要跟 4 位十六进制");
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
            wchar_t c = s[i + (size_t)k];
            unsigned d;
            if (c >= L'0' && c <= L'9') d = (unsigned)(c - L'0');
            else if (c >= L'a' && c <= L'f') d = (unsigned)(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') d = (unsigned)(c - L'A' + 10);
            else return fail(L"\\u 后面要跟 4 位十六进制");
            v = (v << 4) | d;
        }
        i += 4;
        out = v;
        return true;
    }

    /* 跳过任意一个 JSON 值（不认识、也不需要的字段走这里）。 */
    bool skipValue(int depth)
    {
        if (depth > kMaxDepth) return fail(L"JSON 嵌套太深");
        skipWs();
        wchar_t c = peek();
        if (c == L'"') { std::wstring tmp; return parseString(tmp); }
        if (c == L'{' || c == L'[') {
            wchar_t close = (c == L'{') ? L'}' : L']';
            i++;
            skipWs();
            if (peek() == close) { i++; return true; }
            for (;;) {
                skipWs();
                if (c == L'{') {
                    std::wstring key;
                    if (!parseString(key)) return false;
                    skipWs();
                    if (peek() != L':') return fail(L"对象里缺少冒号");
                    i++;
                }
                if (!skipValue(depth + 1)) return false;
                skipWs();
                if (peek() == L',') { i++; continue; }
                if (peek() == close) { i++; return true; }
                return fail(L"对象/数组里的元素之间缺少逗号，或者没有闭合");
            }
        }
        /* 数字 / true / false / null：吃到下一个分隔符为止 */
        size_t start = i;
        while (i < s.size()) {
            wchar_t d = s[i];
            if (d == L',' || d == L'}' || d == L']' || d == L' ' || d == L'\t' || d == L'\r' || d == L'\n') break;
            i++;
        }
        if (i == start) return fail(L"这里应该是一个 JSON 值");
        return true;
    }
};

/* 键名小写化（只处理 ASCII 大写，键名本来就只可能是英文）。 */
std::wstring NormKey(const std::wstring& k)
{
    std::wstring low;
    low.reserve(k.size());
    for (size_t n = 0; n < k.size(); n++) {
        wchar_t c = k[n];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        low.push_back(c);
    }
    return low;
}

/* 键名归一：把常见的几种写法映射到统一的字段号。认不出来返回 -1（跳过该字段）。 */
int FieldOf(const std::wstring& rawKey)
{
    std::wstring low = NormKey(rawKey);
    if (low == L"name" || low == L"title" || low == L"bookname") return 0;
    if (low == L"search" || low == L"searchurl" || low == L"url" || low == L"searchpage") return 1;
    if (low == L"list" || low == L"listre" || low == L"searchre" || low == L"searchregex" ||
        low == L"listregex" || low == L"resultre") return 2;
    if (low == L"tocstart" || low == L"toc_start" || low == L"toccontainer" || low == L"liststart") return 3;
    if (low == L"toc" || low == L"tocre" || low == L"chapterre" || low == L"tocregex" ||
        low == L"chaptersre" || low == L"tocregex") return 4;
    if (low == L"content" || low == L"contentre" || low == L"bodyre" || low == L"textre" ||
        low == L"contentregex") return 5;
    return -1;
}

/* 外层容器键：{"sources":[…]}/{"books":[…]}/{"sites":[…]}/{"srcs":[…]}。
 * 注意 "list" 不在这里 —— 它已经被 FieldOf 当成 listRe（结果页正则）了。 */
bool IsSourceListKey(const std::wstring& rawKey)
{
    std::wstring low = NormKey(rawKey);
    return low == L"sources" || low == L"books" || low == L"sites" || low == L"srcs";
}

/* 解析一个"书源对象"（当前位置必须是 '{'）。
 * 顺带支持对象里再嵌一个 sources 数组（有些人习惯写成 { "sources": [...] }）。 */
bool ParseSourceArray(Parser& p, std::vector<BookSource>& out);   /* 前置声明：下面会互相调用 */

bool ParseSourceObject(Parser& p, std::vector<BookSource>& out)
{
    if (p.peek() != L'{') return p.fail(L"书源应该是一个 JSON 对象（{ … }）");
    if (out.size() > 512) return p.fail(L"书源太多了（超过 512 条，先确认文件没弄错）");
    p.i++;
    BookSource src;
    std::vector<BookSource> nested;
    bool hasAny = false;

    p.skipWs();
    if (p.peek() == L'}') {
        p.i++;
    } else {
        for (;;) {
            p.skipWs();
            std::wstring key;
            if (!p.parseString(key)) return false;
            p.skipWs();
            if (p.peek() != L':') return p.fail(L"对象里的键后面缺少冒号");
            p.i++;
            p.skipWs();

            const int f = FieldOf(key);
            if (f >= 0) {
                /* 值必须是字符串。写成数字/对象也不炸，当空字符串处理（宽容优先）。 */
                std::wstring val;
                if (p.peek() == L'"') {
                    if (!p.parseString(val)) return false;
                } else {
                    if (!p.skipValue(1)) return false;
                }
                val = TrimW(val);
                switch (f) {
                    case 0: src.name = val;      break;
                    case 1: src.search = val;    break;
                    case 2: src.listRe = val;    break;
                    case 3: src.tocStart = val;  break;
                    case 4: src.tocRe = val;     break;
                    case 5: src.contentRe = val; break;
                    default: break;
                }
                hasAny = true;
            } else if (IsSourceListKey(key)) {
                /* 嵌套写法：{ "sources": [ … ] }。键名对上了但值不是数组，
                 * 说明文件确实写错了（常见于手改 JSON 少了一层方括号），
                 * 这时候报错比"静默得到 0 条"有用。 */
                if (p.peek() != L'[') return p.fail(L"sources 应该是一个数组（[ … ]）");
                if (!ParseSourceArray(p, nested)) return false;
            } else {
                if (!p.skipValue(1)) return false;
            }

            p.skipWs();
            if (p.peek() == L',') { p.i++; continue; }
            if (p.peek() == L'}') { p.i++; break; }
            return p.fail(L"对象里的字段之间缺少逗号，或者没有闭合");
        }
    }

    /* 空对象 / 只有无关字段的对象：跳过，不算错误（文件里常带 "app"/"version" 之类的元信息）。 */
    if (hasAny && !src.name.empty()) {
        src.builtin = false;
        out.push_back(src);
    }
    for (size_t n = 0; n < nested.size(); n++) out.push_back(nested[n]);
    return true;
}

bool ParseSourceArray(Parser& p, std::vector<BookSource>& out)
{
    if (p.peek() != L'[') return p.fail(L"书源列表应该是一个数组（[ … ]）");
    p.i++;
    p.skipWs();
    if (p.peek() == L']') { p.i++; return true; }
    for (;;) {
        p.skipWs();
        if (p.peek() == L'{') {
            if (!ParseSourceObject(p, out)) return false;
        } else {
            /* 数组里混进字符串之类的东西：跳过，不因为一条脏数据毁掉整个文件 */
            if (!p.skipValue(1)) return false;
        }
        p.skipWs();
        if (p.peek() == L',') { p.i++; continue; }
        if (p.peek() == L']') { p.i++; return true; }
        return p.fail(L"数组里的元素之间缺少逗号，或者没有闭合");
    }
}

/* ---------------- 文件读写 ---------------- */

bool FileErr(std::wstring& err, const wchar_t* what, DWORD code)
{
    err = std::wstring(what) + L"（错误码 " + std::to_wstring((unsigned long long)code) + L"）";
    return false;
}

bool EnsureParentDir(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return true;
    std::wstring dir = path.substr(0, slash);
    /* 逐级创建：D:\a\b → 先 D:\a 再 D:\a\b。已存在时 CreateDirectoryW 报
     * ERROR_ALREADY_EXISTS，那不是错误。 */
    for (size_t n = 1; n <= dir.size(); n++) {
        if (n != dir.size() && dir[n] != L'\\' && dir[n] != L'/') continue;
        std::wstring part = dir.substr(0, n);
        /* 只有 "D:" 这种纯盘符不能建（CreateDirectoryW 会失败）；以前写成
         * part.size() >= 2，把 "D:\a" 也跳过了，于是带子目录的导出路径
         * 一个目录都建不出来。 */
        if (part.size() == 2 && part[1] == L':') continue;
        CreateDirectoryW(part.c_str(), nullptr);
    }
    return true;
}

bool ReadAllBytes(const std::wstring& path, std::string& out, std::wstring& err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return FileErr(err, L"打不开文件", GetLastError());

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        return FileErr(err, L"读不到文件长度", e);
    }
    if (sz.QuadPart > (LONGLONG)kMaxJsonBytes) {
        CloseHandle(h);
        err = L"文件太大了（超过 8 MB），这多半不是书源文件。";
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    size_t got = 0;
    while (got < out.size()) {
        DWORD want = (DWORD)((out.size() - got) > 0x10000 ? 0x10000 : (out.size() - got));
        DWORD read = 0;
        if (!ReadFile(h, &out[got], want, &read, nullptr)) {
            DWORD e = GetLastError();
            CloseHandle(h);
            return FileErr(err, L"读文件失败", e);
        }
        if (read == 0) break;
        got += read;
    }
    CloseHandle(h);
    out.resize(got);
    return true;
}

bool WriteAllBytes(const std::wstring& path, const std::string& data, std::wstring& err)
{
    EnsureParentDir(path);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return FileErr(err, L"写不了这个文件", GetLastError());
    size_t put = 0;
    while (put < data.size()) {
        DWORD want = (DWORD)((data.size() - put) > 0x10000 ? 0x10000 : (data.size() - put));
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + put, want, &wrote, nullptr)) {
            DWORD e = GetLastError();
            CloseHandle(h);
            return FileErr(err, L"写文件失败", e);
        }
        put += wrote;
        if (wrote == 0) break;
    }
    CloseHandle(h);
    return true;
}

/* 字节 → 宽字符：认 UTF-8（带/不带 BOM）、UTF-16LE/BE（带 BOM）；
 * 认不出来按 UTF-8 硬解，解不动再退回系统 ANSI 代码页（老版本记事本存的 GBK 书源文件）。 */
std::wstring BytesToWide(const std::string& raw)
{
    const unsigned char* p = (const unsigned char*)raw.data();
    size_t n = raw.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        return Utf8ToW(raw.substr(3));
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        std::wstring w((n - 2) / 2, L'\0');
        if (!w.empty()) memcpy(&w[0], p + 2, w.size() * 2);
        return w;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        std::wstring w((n - 2) / 2, L'\0');
        for (size_t k = 0; k < w.size(); k++) {
            w[k] = (wchar_t)((p[2 + k * 2] << 8) | p[3 + k * 2]);
        }
        return w;
    }
    /* 没有 BOM：先按 UTF-8 严格解一次，失败再按 ANSI */
    if (!raw.empty()) {
        int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
        if (need > 0) {
            std::wstring w((size_t)need, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &w[0], need);
            return w;
        }
        int need2 = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        if (need2 > 0) {
            std::wstring w((size_t)need2, L'\0');
            MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &w[0], need2);
            return w;
        }
    }
    return Utf8ToW(raw);
}

}  /* namespace */

/* ---------------- 对外接口 ---------------- */

std::wstring SourceJsonQuote(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 8);
    out.push_back(L'"');
    for (size_t n = 0; n < s.size(); n++) {
        wchar_t c = s[n];
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if ((unsigned)c < 0x20) {
                    /* 其余控制字符走 \u00XX，保证输出一定是合法 JSON */
                    static const wchar_t* kHex = L"0123456789abcdef";
                    out += L"\\u00";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring SourceExportJson(const std::vector<BookSource>& list, bool includeBuiltin)
{
    /* 先数一下真正要导出的条数：内置源在 0 号，可按 includeBuiltin 跳过。 */
    size_t count = 0;
    for (size_t n = 0; n < list.size(); n++) {
        if (list[n].name.empty()) continue;
        if (!includeBuiltin && list[n].builtin) continue;
        count++;
    }

    std::wstring j;
    j += L"{\n";
    j += L"  \"app\": \"NovelReader\",\n";
    j += L"  \"type\": \"book-sources\",\n";
    j += L"  \"version\": 1,\n";
    j += L"  \"count\": " + std::to_wstring((unsigned long long)count) + L",\n";
    j += L"  \"sources\": [\n";

    size_t written = 0;
    for (size_t n = 0; n < list.size(); n++) {
        const BookSource& src = list[n];
        if (src.name.empty()) continue;
        if (!includeBuiltin && src.builtin) continue;
        j += L"    {\n";
        j += L"      \"name\": " + SourceJsonQuote(src.name) + L",\n";
        j += L"      \"search\": " + SourceJsonQuote(src.search) + L",\n";
        j += L"      \"list\": " + SourceJsonQuote(src.listRe) + L",\n";
        j += L"      \"tocStart\": " + SourceJsonQuote(src.tocStart) + L",\n";
        j += L"      \"toc\": " + SourceJsonQuote(src.tocRe) + L",\n";
        j += L"      \"content\": " + SourceJsonQuote(src.contentRe);
        if (src.builtin) j += L",\n      \"builtin\": true";
        j += L"\n    }";
        written++;
        if (written < count) j += L",";
        j += L"\n";
    }
    j += L"  ]\n";
    j += L"}\n";
    return j;
}

bool SourceImportJson(const std::wstring& json, std::vector<BookSource>& out, std::wstring& err)
{
    err.clear();
    Parser p(json);
    p.skipWs();
    if (p.eof()) {
        err = L"文件是空的。";
        return false;
    }

    std::vector<BookSource> got;
    bool ok = false;
    if (p.peek() == L'[') {
        ok = ParseSourceArray(p, got);
    } else if (p.peek() == L'{') {
        ok = ParseSourceObject(p, got);
    } else {
        err = L"文件开头既不是 { 也不是 [，不像是 JSON。";
        return false;
    }
    if (!ok) {
        err = p.err.empty() ? L"解析失败。" : p.err;
        return false;
    }
    p.skipWs();
    if (!p.eof()) {
        /* 尾部还有别的东西：多半是拼了两个文件。不当作失败，但要说清楚。 */
        err = L"解析到第 " + std::to_wstring((unsigned long long)(p.i + 1)) +
              L" 个字符后面还有多余内容（只看第一个 JSON 值）。";
    }

    /* 进函数就先清空 out：这个接口的语义是"把文件里解析出的书源给你"，
     * 不是"追加"。以前这里是从 out.size() 开始 push_back，连续导两个文件时
     * 上一次的结果会残留在前面（单元测试里表现为"单个对象条数 got 3 want 1"）。 */
    out.clear();

    /* 过滤空名字（解析器已经过滤过一次，这里再兜一次，防止以后改坏了） */
    for (size_t n = 0; n < got.size(); n++) {
        if (got[n].name.empty()) continue;
        got[n].builtin = false;
        out.push_back(got[n]);
    }
    /* 空数组（[]）是合法的书源文件，只是没有书源：算成功、0 条，让调用方
     * 自己去提示"这个文件里没有书源"。这里报失败的话，导出空列表再导入
     * 就永远失败，很别扭。 */
    return true;
}

bool SourceExportJsonFile(const std::wstring& path, const std::vector<BookSource>& list,
                          bool includeBuiltin, std::wstring& err)
{
    err.clear();
    std::wstring text = SourceExportJson(list, includeBuiltin);
    if (!WriteAllBytes(path, WToUtf8(text), err)) return false;
    return true;
}

bool SourceImportJsonFile(const std::wstring& path, std::vector<BookSource>& out, std::wstring& err)
{
    err.clear();
    std::string raw;
    if (!ReadAllBytes(path, raw, err)) return false;
    std::wstring text = BytesToWide(raw);
    if (!SourceImportJson(text, out, err)) {
        /* 文件级的错误提示带上路径，用户才知道自己选错了文件 */
        err = L"解析「" + path + L"」失败：" + err;
        return false;
    }
    return true;
}
