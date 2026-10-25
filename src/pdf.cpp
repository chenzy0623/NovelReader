/* pdf.cpp - PDF 文本抽取实现
 *
 * 整体流程（四步）：
 *   1. 扫对象：按 "N G obj ... endobj" 顺序把每个对象的字典文本和（可能的）流
 *      数据抓出来放进内存数组。不解析 xref / 交叉引用表 —— 小说 PDF 很少有
 *      增量更新，顺序扫描足够，而且对修补过（xref 偏移错乱）的文件更宽容。
 *   2. 找页面：把 /Type /Page 的对象按文件顺序排好，逐个拿 /Contents 流和
 *      /Resources /Font 里的字体表。
 *   3. 解字体：解析每个字体对象的 /ToUnicode CMap（bfchar/bfrange），得到
 *      "文本字节码 → Unicode" 的映射表。中文 PDF 一律是 Type0 + Identity-H，
 *      即双字节码，所以按 CMap 里码串的长度自动判断是 1 字节还是 2 字节。
 *   4. 解内容流：扫 BT/ET、Tf、Td/TD/Tm/T*、Tj/TJ/'/" 这些文本算子，把每段
 *      文字连同它的基线 y 坐标收集成"片段"；最后按页面顺序、y 从大到小合并：
 *      行距正常 → 同一段落内接上，行距明显变大 → 断段。这样 PDF 里被硬换行
 *      的每一行不会各自变成一段，而是还原成真正的段落，后面的分章/搜索才准。
 *
 * 明确不支持（宁可按错误报出，也不给用户一屏乱码）：
 *   - 加密 PDF、扫描件（没有文字层）、DCT/LZW 等非 Flate 过滤器
 *   - 对象流（/Type /ObjStm）里存放的字典：见到 /ObjStm 直接判失败
 *   - 竖排、旋转文字：按普通横排抽取，可能顺序不理想
 */
#include "common.h"
#include "pdf.h"
#include "zip.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <vector>

namespace {

// ============================================================================
// 一、字符分类与跳过空白
// ============================================================================

// PDF 语法里的空白字符（含 \0，PDF 规范把 NUL 也算空白）
inline bool PdfIsWhite(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

// 分隔符：出现它们就说明一个名字/数字结束了
inline bool PdfIsDelim(uint8_t c) {
    switch (c) {
    case '(': case ')': case '<': case '>': case '[': case ']':
    case '{': case '}': case '/': case '%':
        return true;
    default:
        return false;
    }
}

// 跳过空白和注释（% 到行尾）
void PdfSkipWs(const std::string& s, size_t& i) {
    while (i < s.size()) {
        uint8_t c = (uint8_t)s[i];
        if (PdfIsWhite(c)) { i++; continue; }
        if (c == '%') {
            while (i < s.size() && s[i] != '\n' && s[i] != '\r') i++;
            continue;
        }
        break;
    }
}

// ============================================================================
// 二、从字典文本里取值
//
// 这些函数都很"轻"：不做完整语法分析，只按 "/Key" 找到位置再读紧随的值。
// PDF 字典里同一个键不会重复出现，所以这种做法足够可靠，代码量却只有完整
// 解析器的十分之一。
// ============================================================================

// 找到 "/key" 之后的位置；找不到返回 std::string::npos。
// 末尾必须是空白或分隔符，避免 "/FontFF" 误匹配 "/Font"。
size_t PdfFindKey(const std::string& d, const char* key) {
    const std::string pat = std::string("/") + key;
    size_t p = 0;
    while ((p = d.find(pat, p)) != std::string::npos) {
        size_t after = p + pat.size();
        if (after >= d.size()) return std::string::npos;
        uint8_t c = (uint8_t)d[after];
        if (!PdfIsWhite(c) && !PdfIsDelim(c)) { p = after; continue; }
        return after;
    }
    return std::string::npos;
}

// 读引用：/Key 12 0 R（如果后面是数组，取第一个元素）
bool PdfDictRef(const std::string& d, const char* key, int& num) {
    size_t p = PdfFindKey(d, key);
    if (p == std::string::npos) return false;
    PdfSkipWs(d, p);
    if (p < d.size() && d[p] == '[') { p++; PdfSkipWs(d, p); }
    if (p >= d.size() || d[p] < '0' || d[p] > '9') return false;
    long n1 = 0;
    while (p < d.size() && d[p] >= '0' && d[p] <= '9') { n1 = n1 * 10 + (d[p] - '0'); p++; }
    PdfSkipWs(d, p);
    long n2 = 0;
    bool haveGen = false;
    while (p < d.size() && d[p] >= '0' && d[p] <= '9') { n2 = n2 * 10 + (d[p] - '0'); p++; haveGen = true; }
    (void)n2;
    if (!haveGen) return false;             // 不是引用而是直接整数
    PdfSkipWs(d, p);
    if (p >= d.size() || d[p] != 'R') return false;
    num = (int)n1;
    return true;
}

// 读名字：/Key /Name → out = "Name"
bool PdfDictName(const std::string& d, const char* key, std::string& out) {
    size_t p = PdfFindKey(d, key);
    if (p == std::string::npos) return false;
    PdfSkipWs(d, p);
    if (p >= d.size() || d[p] != '/') return false;
    p++;
    size_t st = p;
    while (p < d.size() && !PdfIsWhite((uint8_t)d[p]) && !PdfIsDelim((uint8_t)d[p])) p++;
    out.assign(d, st, p - st);
    return !out.empty();
}

// 读子字典：/Key << ... >> → out 含首尾的尖括号
bool PdfDictSub(const std::string& d, const char* key, std::string& out) {
    size_t p = PdfFindKey(d, key);
    if (p == std::string::npos) return false;
    PdfSkipWs(d, p);
    if (p + 1 >= d.size() || d[p] != '<' || d[p + 1] != '<') return false;
    size_t st = p;
    int depth = 0;
    while (p + 1 < d.size()) {
        if (d[p] == '<' && d[p + 1] == '<') { depth++; p += 2; continue; }
        if (d[p] == '>' && d[p + 1] == '>') {
            depth--;
            p += 2;
            if (depth == 0) { out.assign(d, st, p - st); return true; }
            continue;
        }
        p++;
    }
    return false;
}

// ============================================================================
// 三、UTF-16BE 字节串 → wstring
// ============================================================================

std::wstring PdfUtf16Be(const std::string& b) {
    std::wstring w;
    w.reserve(b.size() / 2);
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        w.push_back((wchar_t)(((uint8_t)b[i] << 8) | (uint8_t)b[i + 1]));
    }
    return w;
}

// ============================================================================
// 四、Flate 解压（复用 zip 里的原始 DEFLATE 解码器）
//
// PDF 的 FlateDecode 数据外面套着一层 zlib 头（2 字节 + 尾部 4 字节 adler32），
// 而 zip 里存的是"裸" DEFLATE，所以要跳过 2 字节头再解。
// 解压后的大小事先不知道（PDF 流字典里不写），所以给一个估计容量，失败就翻倍重试。
// ============================================================================

bool PdfInflateZlib(const std::string& in, std::string& out) {
    out.clear();
    if (in.size() < 3) return false;
    uint8_t cmf = (uint8_t)in[0], flg = (uint8_t)in[1];
    if ((cmf & 0x0F) != 8) return false;                  // 必须是 deflate
    if ((((uint32_t)cmf << 8) | flg) % 31 != 0) return false;  // zlib 头校验
    if (flg & 0x20) return false;                          // 有预设字典，不支持

    size_t cap = (in.size() - 2) * 6 + (1u << 18);
    for (int attempt = 0; attempt < 6; attempt++) {
        size_t consumed = 0;
        if (InflateRaw((const uint8_t*)in.data() + 2, in.size() - 2, out, cap, &consumed)) {
            return !out.empty();
        }
        if (cap > (size_t)256 << 20) break;   // 上限 256MB，防病态文件
        cap *= 4;
    }
    out.clear();
    return false;
}

// ============================================================================
// 五、对象与文档
// ============================================================================

struct PdfObj {
    int         num = 0;
    std::string dict;        // 对象体（到 stream/endobj 之前）
    std::string stream;      // 原始流数据（未解压）
    bool        hasStream = false;
};

// 字体的编码映射：码点 → Unicode（UTF-16 单元序列，通常是 1 个）
struct PdfFont {
    int codeBytes = 1;
    std::map<uint32_t, std::wstring> map;
};

class PdfDoc {
public:
    bool Load(const uint8_t* data, size_t len, std::wstring& err);
    bool Extract(std::wstring& out, std::wstring& err);

private:
    const PdfObj* Find(int num) const;
    bool DecodeStream(const PdfObj& o, std::string& out) const;   // 按 /Filter 解压
    bool LoadFont(int objNum, PdfFont& f);
    void ParseCMap(const std::string& cm, PdfFont& f) const;
    void RunContent(const std::string& cs, int page, const std::map<std::string, const PdfFont*>& fonts,
                    std::vector<std::pair<double, std::wstring> >* lines,
                    std::vector<double>* sizes) const;

    std::vector<PdfObj> m_objs;
    std::vector<int>    m_pages;   // /Type /Page 的对象号，按文件顺序
    std::map<int, PdfFont> m_fontCache;
};

const PdfObj* PdfDoc::Find(int num) const {
    for (size_t i = 0; i < m_objs.size(); i++) {
        if (m_objs[i].num == num) return &m_objs[i];
    }
    return nullptr;
}

bool PdfDoc::DecodeStream(const PdfObj& o, std::string& out) const {
    out.clear();
    if (!o.hasStream) return false;
    std::string filter;
    if (PdfDictName(o.dict, "Filter", filter)) {
        if (filter == "FlateDecode") return PdfInflateZlib(o.stream, out);
        return false;                       // 其它过滤器不支持
    }
    out = o.stream;                          // 没有过滤器就是明文
    return true;
}

// 扫 "N G obj"。返回 false 表示不是 PDF（连一个对象都没有）。
bool PdfDoc::Load(const uint8_t* data, size_t len, std::wstring& err) {
    if (data == nullptr || len < 16) { err = L"文件太小，不是有效的 PDF"; return false; }
    if (std::memcmp(data, "%PDF", 4) != 0) { err = L"文件头不是 %PDF，不是有效的 PDF"; return false; }

    const std::string body((const char*)data, len);

    // 对象流（把多个对象压缩进一个流里）本项目不支持：Adobe 系导出常见，
    // 浏览器/Word 导出的正常 PDF 不用它。见到就明确报错。
    if (body.find("/ObjStm") != std::string::npos) {
        err = L"这份 PDF 使用了对象流压缩（ObjStm），暂不支持，请用浏览器重新导出为 PDF";
        return false;
    }
    if (body.find("/Encrypt") != std::string::npos) {
        err = L"这份 PDF 是加密的，请先解密再打开";
        return false;
    }

    size_t p = 0;
    while ((p = body.find("obj", p)) != std::string::npos) {
        size_t after = p + 3;
        // "obj" 之后必须是空白或分隔符，排除 "objxyz"
        if (after < body.size() && !PdfIsWhite((uint8_t)body[after]) && !PdfIsDelim((uint8_t)body[after])) {
            p = after;
            continue;
        }
        // 往回读：<空格> gen <空格> num
        size_t q = p;
        while (q > 0 && PdfIsWhite((uint8_t)body[q - 1])) q--;
        size_t genEnd = q;
        while (q > 0 && body[q - 1] >= '0' && body[q - 1] <= '9') q--;
        if (q == genEnd) { p = after; continue; }          // gen 部分是空 → 不是对象头
        size_t q2 = q;
        while (q2 > 0 && PdfIsWhite((uint8_t)body[q2 - 1])) q2--;
        size_t numEnd = q2;
        while (q2 > 0 && body[q2 - 1] >= '0' && body[q2 - 1] <= '9') q2--;
        if (q2 == numEnd) { p = after; continue; }
        int num = 0;
        for (size_t k = q2; k < numEnd; k++) num = num * 10 + (body[k] - '0');

        // 对象体从 obj 之后开始，到 "stream" 或 "endobj" 结束
        size_t st = after;
        size_t streamPos = body.find("stream", st);
        size_t endObjPos  = body.find("endobj", st);
        if (endObjPos == std::string::npos) break;         // 文件尾残缺

        PdfObj o;
        o.num = num;
        if (streamPos != std::string::npos && streamPos < endObjPos) {
            o.dict.assign(body, st, streamPos - st);
            size_t d = streamPos + 6;                      // 跳过 "stream"
            if (d < body.size() && body[d] == '\r') d++;
            if (d < body.size() && body[d] == '\n') d++;
            size_t e = body.find("endstream", d);
            if (e == std::string::npos || e > endObjPos) e = endObjPos;
            o.stream.assign(body, d, e - d);
            o.hasStream = true;
            p = endObjPos + 6;
        } else {
            o.dict.assign(body, st, endObjPos - st);
            p = endObjPos + 6;
        }
        m_objs.push_back(o);
    }

    if (m_objs.empty()) { err = L"没找到任何 PDF 对象，文件可能已损坏"; return false; }

    // 页面：/Type /Page（注意不要误收 /Pages）
    for (size_t i = 0; i < m_objs.size(); i++) {
        const std::string& d = m_objs[i].dict;
        std::string type;
        if (!PdfDictName(d, "Type", type)) continue;
        if (type == "Page") m_pages.push_back(m_objs[i].num);
    }
    if (m_pages.empty()) { err = L"PDF 里没有页面对象"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 字体：解析 /ToUnicode CMap
//
// CMap 的结构（只列出本程序关心的部分）：
//   beginbfchar
//     <0001> <4E00>            ← 单映射：码 0x0001 → U+4E00
//   endbfchar
//   beginbfrange
//     <0020> <007E> <0020>     ← 区间映射：码 n → 起始值 + (n - lo)
//     <0100> <0102> [<4E01> <4E02> <4E03>]   ← 区间逐个映射
//   endbfrange
// ---------------------------------------------------------------------------
void PdfDoc::ParseCMap(const std::string& cm, PdfFont& f) const {
    size_t i = 0;
    // tokens 依次是 <hex>、[、]、名字；按需读取
    auto readHex = [&](std::string& hex) -> bool {
        PdfSkipWs(cm, i);
        if (i >= cm.size() || cm[i] != '<') return false;
        if (i + 1 < cm.size() && cm[i + 1] == '<') { i += 2; return false; }  // << 字典，跳过
        size_t st = ++i;
        while (i < cm.size() && cm[i] != '>') i++;
        if (i >= cm.size()) return false;
        hex.assign(cm, st, i - st);
        i++;
        return true;
    };
    auto hexToCode = [](const std::string& h, uint32_t& code) -> bool {
        code = 0;
        if (h.empty() || h.size() > 8) return false;
        for (size_t k = 0; k < h.size(); k++) {
            int v;
            char c = (char)tolower((unsigned char)h[k]);
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else return false;
            code = (code << 4) | (uint32_t)v;
        }
        return true;
    };

    bool inChar = false, inRange = false;
    // 关键坑：std::string::compare(pos, count, lit) 会同时比较长度，
    // count 与 lit 的 strlen 不等就直接判"不相等"（哪怕前缀一模一样），
    // 所以长度必须用 strlen 算，不能手写数字。
    auto matchTok = [&](const char* lit) -> bool {
        size_t n = std::strlen(lit);
        return i + n <= cm.size() && cm.compare(i, n, lit) == 0;
    };
    while (i < cm.size()) {
        PdfSkipWs(cm, i);
        if (i >= cm.size()) break;
        if (matchTok("beginbfchar"))  { inChar = true;  i += std::strlen("beginbfchar");  continue; }
        if (matchTok("endbfchar"))    { inChar = false; i += std::strlen("endbfchar");    continue; }
        if (matchTok("beginbfrange")) { inRange = true; i += std::strlen("beginbfrange"); continue; }
        if (matchTok("endbfrange"))   { inRange = false; i += std::strlen("endbfrange");  continue; }

        if (inChar) {
            std::string hs, hd;
            if (!readHex(hs) || !readHex(hd)) { i++; continue; }
            uint32_t code = 0;
            if (!hexToCode(hs, code)) continue;
            f.codeBytes = (int)(hs.size() / 2);
            std::string bytes;
            for (size_t k = 0; k + 1 < hd.size(); k += 2) {
                bytes.push_back((char)strtol(hd.substr(k, 2).c_str(), nullptr, 16));
            }
            f.map[code] = PdfUtf16Be(bytes);
            continue;
        }
        if (inRange) {
            std::string hs, he;
            if (!readHex(hs) || !readHex(he)) { i++; continue; }
            uint32_t lo = 0, hi = 0;
            if (!hexToCode(hs, lo) || !hexToCode(he, hi) || hi < lo || hi - lo > 65535) continue;
            f.codeBytes = (int)(hs.size() / 2);
            PdfSkipWs(cm, i);
            if (i < cm.size() && cm[i] == '[') {         // 逐个映射
                i++;
                for (uint32_t c = lo; c <= hi; c++) {
                    std::string hd;
                    if (!readHex(hd)) break;
                    std::string bytes;
                    for (size_t k = 0; k + 1 < hd.size(); k += 2) {
                        bytes.push_back((char)strtol(hd.substr(k, 2).c_str(), nullptr, 16));
                    }
                    f.map[c] = PdfUtf16Be(bytes);
                }
                if (i < cm.size() && cm[i] == ']') i++;
            } else {                                      // 起始值连续递增
                std::string hd;
                if (!readHex(hd)) continue;
                uint32_t base = 0;
                if (!hexToCode(hd, base)) continue;
                for (uint32_t c = lo; c <= hi; c++) {
                    uint32_t v = base + (c - lo);
                    wchar_t wc = (wchar_t)(v & 0xFFFF);
                    f.map[c] = std::wstring(1, wc);
                }
            }
            continue;
        }
        i++;
    }
}

bool PdfDoc::LoadFont(int objNum, PdfFont& f) {
    std::map<int, PdfFont>::iterator it = m_fontCache.find(objNum);
    if (it != m_fontCache.end()) { f = it->second; return !f.map.empty(); }

    const PdfObj* o = Find(objNum);
    if (o == nullptr) return false;

    // 默认单字节（简单字体）；Type0/Identity-H 是双字节，等 CMap 解析时再纠正
    f.codeBytes = 1;
    int tu = 0;
    if (PdfDictRef(o->dict, "ToUnicode", tu)) {
        const PdfObj* t = Find(tu);
        std::string cm;
        if (t != nullptr && DecodeStream(*t, cm)) ParseCMap(cm, f);
    }
    m_fontCache[objNum] = f;
    return !f.map.empty();
}

// ---------------------------------------------------------------------------
// 内容流解析：真正的"把文字挑出来"
// ---------------------------------------------------------------------------

// 当前文本状态里本程序真正用到的部分：文本矩阵、位置、字号、行距、字体
//
// 为什么必须跟踪矩阵：PDF 里的文字位置是"文本矩阵 Tm × 当前变换矩阵 CTM"的结果。
// 浏览器/Word 导出的 PDF 常常画一条 .24 0 0 -.24 0 792 cm 把整个坐标系上下翻转，
// 于是 Tm 里的 y 越大反而在页面上越靠下。如果只看 Tm 的 y 去排序，整本书的行
// 顺序会完全颠倒（实测就是"从最后一行读到标题"）。所以这里老老实实做矩阵乘法，
// 用换算到设备空间的 y 排序。
struct PdfMat {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// 先应用 l（在里层），再应用 r（在外层）
static PdfMat PdfMatMul(const PdfMat& l, const PdfMat& r) {
    PdfMat m;
    m.a = l.a * r.a + l.b * r.c;
    m.b = l.a * r.b + l.b * r.d;
    m.c = l.c * r.a + l.d * r.c;
    m.d = l.c * r.b + l.d * r.d;
    m.e = l.e * r.a + l.f * r.c + r.e;
    m.f = l.e * r.b + l.f * r.d + r.f;
    return m;
}

struct PdfTextState {
    PdfMat tm;             // 文本矩阵（Tm / Td / T* 改它）
    double leading = 0;    // 行距（TL）
    double size = 12;      // 字号（Tf 的第二个参数）
    const PdfFont* font = nullptr;
};

// 判断一行是不是"页眉页脚"之类该丢掉的东西：
//   纯页码（12 / - 12 - / 12 / 345 / 第 12 页）、浏览器打印时加的 file:// 路径、
//   "标题 + 2024/1/2 12:34" 这种带时间戳的页眉。
// 这些都是"打印成 PDF"时自动加的，对读小说没用，留着还会干扰分章。
static bool PdfLooksLikeFurniture(const std::wstring& s) {
    if (s.empty() || s.size() > 120) return false;

    // 纯数字（页码）
    bool allDigit = true;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] < L'0' || s[i] > L'9') { allDigit = false; break; }
    }
    if (allDigit && s.size() <= 4) return true;

    // "12 / 345"、"12/345"
    {
        size_t i = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') i++;
        if (i > 0 && i < s.size() && s[i] == L'/') {
            size_t j = i + 1;
            bool dig = false;
            while (j < s.size() && s[j] >= L'0' && s[j] <= L'9') { j++; dig = true; }
            if (dig && j == s.size()) return true;
        }
    }

    // "第 12 页"
    if (s.size() >= 4 && s.compare(0, 1, L"第") == 0) {
        size_t i = 1;
        while (i < s.size() && s[i] == L' ') i++;
        size_t st = i;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') i++;
        if (i > st) {
            while (i < s.size() && s[i] == L' ') i++;
            if (i + 1 == s.size() && s[i] == L'页') return true;
        }
    }

    // 浏览器打印时加的本地文件路径
    if (s.compare(0, 7, L"file://") == 0) return true;

    // "标题 2024/1/2 12:34"：末尾有日期时间就当作页眉
    size_t slash = s.rfind(L'/');
    if (slash != std::wstring::npos && slash + 1 < s.size()) {
        size_t i = slash + 1;
        size_t d2 = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') { i++; d2++; }
        if (d2 >= 1) {
            // 前面还得有别的内容（标题），并且出现过 xxxx/数字
            bool hasColon = s.find(L':') != std::wstring::npos;
            if (hasColon && slash > 3) return true;
        }
    }
    return false;
}

void PdfDoc::RunContent(const std::string& cs, int page,
                        const std::map<std::string, const PdfFont*>& fonts,
                        std::vector<std::pair<double, std::wstring> >* lines,
                        std::vector<double>* sizes) const {
    (void)page;
    PdfTextState ts;
    std::vector<PdfMat> ctmStack;      // q / Q 保存与恢复
    PdfMat ctm;                        // 当前变换矩阵
    std::wstring cur;                  // 当前片段文字
    double curY = 0;                   // 当前片段在设备空间的基线 y
    bool   haveSeg = false;

    // 收尾：把一个片段（同一基线上的一串字）压进结果
    auto flush = [&]() {
        // 收拾空白：连续空格压成一个，首尾去掉
        std::wstring s;
        s.reserve(cur.size());
        bool lastSpace = true;
        for (size_t k = 0; k < cur.size(); k++) {
            wchar_t c = cur[k];
            bool sp = (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n');
            if (sp) {
                if (lastSpace) continue;
                s.push_back(L' ');
                lastSpace = true;
            } else {
                s.push_back(c);
                lastSpace = false;
            }
        }
        while (!s.empty() && s[s.size() - 1] == L' ') s.erase(s.size() - 1);
        if (!s.empty() && !PdfLooksLikeFurniture(s)) {
            lines->push_back(std::make_pair(curY, s));
            sizes->push_back(ts.size > 1 ? ts.size : 12.0);
        }
        cur.clear();
        haveSeg = false;
    };

    // 把一串字节按当前字体的 CMap 翻译成 Unicode 追加到 cur
    auto show = [&](const std::string& raw) {
        if (raw.empty()) return;
        if (!haveSeg) {
            // 设备空间里的基线 y：y' = Tm.e * ctm.b + Tm.f * ctm.d + ctm.f
            curY = ts.tm.e * ctm.b + ts.tm.f * ctm.d + ctm.f;
            haveSeg = true;
        }
        const PdfFont* f = ts.font;
        if (f == nullptr || f->map.empty()) {
            // 没有 ToUnicode：只能当单字节 ASCII 猜（英文 PDF 常见）
            for (size_t k = 0; k < raw.size(); k++) {
                uint8_t c = (uint8_t)raw[k];
                if (c >= 32 && c < 127) cur.push_back((wchar_t)c);
            }
            return;
        }
        int cb = f->codeBytes > 0 ? f->codeBytes : 1;
        size_t k = 0;
        while (k < raw.size()) {
            uint32_t code = 0;
            if (cb >= 2) {
                if (k + 1 >= raw.size()) break;
                code = ((uint32_t)(uint8_t)raw[k] << 8) | (uint8_t)raw[k + 1];
                k += 2;
            } else {
                code = (uint32_t)(uint8_t)raw[k];
                k += 1;
            }
            std::map<uint32_t, std::wstring>::const_iterator it = f->map.find(code);
            if (it != f->map.end()) {
                cur.append(it->second);
            } else if (cb == 1 && code >= 32 && code < 127) {
                cur.push_back((wchar_t)code);
            }
        }
    };

    size_t i = 0;
    struct Operand { int type; double num; std::string str; std::vector<Operand> arr; };
    std::vector<Operand> ops;

    // 读字面量字符串 ( ... )，处理转义与括号嵌套
    auto readLit = [&](std::string& out) -> bool {
        if (i >= cs.size() || cs[i] != '(') return false;
        out.clear();
        int depth = 1;
        i++;
        while (i < cs.size()) {
            char c = cs[i];
            if (c == '\\') {
                i++;
                if (i >= cs.size()) break;
                char e = cs[i++];
                switch (e) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '(': out.push_back('('); break;
                case ')': out.push_back(')'); break;
                case '\\': out.push_back('\\'); break;
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && i < cs.size() && cs[i] >= '0' && cs[i] <= '7'; k++) {
                            v = v * 8 + (cs[i] - '0');
                            i++;
                        }
                        out.push_back((char)v);
                    } else {
                        out.push_back(e);
                    }
                    break;
                }
                continue;
            }
            if (c == '(') { depth++; out.push_back(c); i++; continue; }
            if (c == ')') {
                depth--;
                i++;
                if (depth == 0) return true;
                out.push_back(c);
                continue;
            }
            out.push_back(c);
            i++;
        }
        return true;
    };

    // 读十六进制串 < ... >
    auto readHexStr = [&](std::string& out) -> bool {
        if (i >= cs.size() || cs[i] != '<') return false;
        i++;
        out.clear();
        int hi = -1;
        while (i < cs.size() && cs[i] != '>') {
            char c = (char)tolower((unsigned char)cs[i]);
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else { i++; continue; }
            if (hi < 0) hi = v;
            else { out.push_back((char)((hi << 4) | v)); hi = -1; }
            i++;
        }
        if (hi >= 0) out.push_back((char)(hi << 4));
        if (i < cs.size() && cs[i] == '>') i++;
        return true;
    };

    while (i < cs.size()) {
        PdfSkipWs(cs, i);
        if (i >= cs.size()) break;
        char c = cs[i];
        if (c == '(') {
            Operand o; o.type = 3; o.num = 0;
            std::string s;
            readLit(s);
            o.str.swap(s);
            ops.push_back(o);
            continue;
        }
        if (c == '<') {
            if (i + 1 < cs.size() && cs[i + 1] == '<') {   // 内联字典：整段跳过
                int depth = 0;
                while (i + 1 < cs.size()) {
                    if (cs[i] == '<' && cs[i + 1] == '<') { depth++; i += 2; continue; }
                    if (cs[i] == '>' && cs[i + 1] == '>') { depth--; i += 2; if (depth == 0) break; continue; }
                    i++;
                }
                continue;
            }
            Operand o; o.type = 3; o.num = 0;
            std::string s;
            readHexStr(s);
            o.str.swap(s);
            ops.push_back(o);
            continue;
        }
        if (c == '/') {
            i++;
            size_t st = i;
            while (i < cs.size() && !PdfIsWhite((uint8_t)cs[i]) && !PdfIsDelim((uint8_t)cs[i])) i++;
            Operand o; o.type = 2; o.num = 0;
            o.str.assign(cs, st, i - st);
            ops.push_back(o);
            continue;
        }
        if (c == '[') {                                   // 数组（TJ 用）
            i++;
            Operand arr; arr.type = 4; arr.num = 0;
            while (i < cs.size()) {
                PdfSkipWs(cs, i);
                if (i >= cs.size()) break;
                if (cs[i] == ']') { i++; break; }
                if (cs[i] == '(') {
                    Operand o; o.type = 3; std::string s; readLit(s); o.str.swap(s); arr.arr.push_back(o); continue;
                }
                if (cs[i] == '<') {
                    Operand o; o.type = 3; std::string s; readHexStr(s); o.str.swap(s); arr.arr.push_back(o); continue;
                }
                if (cs[i] == '/') {
                    i++;
                    size_t st = i;
                    while (i < cs.size() && !PdfIsWhite((uint8_t)cs[i]) && !PdfIsDelim((uint8_t)cs[i])) i++;
                    Operand o; o.type = 2; o.str.assign(cs, st, i - st); arr.arr.push_back(o);
                    continue;
                }
                size_t st = i;
                if (cs[i] == '-' || cs[i] == '+') i++;
                while (i < cs.size() && ((cs[i] >= '0' && cs[i] <= '9') || cs[i] == '.')) i++;
                if (i == st) { i++; continue; }
                Operand o; o.type = 1;
                o.num = atof(cs.substr(st, i - st).c_str());
                arr.arr.push_back(o);
            }
            ops.push_back(arr);
            continue;
        }
        size_t st = i;
        if (c == '-' || c == '+' || c == '.') i++;
        while (i < cs.size() && !PdfIsWhite((uint8_t)cs[i]) && !PdfIsDelim((uint8_t)cs[i])) i++;
        if (i == st) { i++; continue; }
        std::string tok(cs, st, i - st);
        bool numeric = true;
        for (size_t k = 0; k < tok.size(); k++) {
            char t = tok[k];
            if (!((t >= '0' && t <= '9') || t == '-' || t == '+' || t == '.')) { numeric = false; break; }
        }
        if (numeric) {
            Operand o; o.type = 1; o.num = atof(tok.c_str());
            ops.push_back(o);
            continue;
        }

        // ---- 算子 ----
        if (tok == "q") {
            ctmStack.push_back(ctm);
        } else if (tok == "Q") {
            if (!ctmStack.empty()) { ctm = ctmStack.back(); ctmStack.pop_back(); }
        } else if (tok == "cm") {
            if (ops.size() >= 6) {
                PdfMat m;
                m.a = ops[ops.size() - 6].num; m.b = ops[ops.size() - 5].num;
                m.c = ops[ops.size() - 4].num; m.d = ops[ops.size() - 3].num;
                m.e = ops[ops.size() - 2].num; m.f = ops[ops.size() - 1].num;
                ctm = PdfMatMul(m, ctm);
            }
        } else if (tok == "BT") {
            ts.tm = PdfMat();
        } else if (tok == "ET") {
            flush();
        } else if (tok == "Tf") {
            if (ops.size() >= 2) {
                const Operand& nm = ops[ops.size() - 2];
                const Operand& sz = ops[ops.size() - 1];
                if (nm.type == 2) {
                    std::map<std::string, const PdfFont*>::const_iterator it = fonts.find(nm.str);
                    ts.font = (it != fonts.end()) ? it->second : nullptr;
                }
                if (sz.type == 1 && sz.num > 0) ts.size = sz.num;
            }
        } else if (tok == "TL") {
            if (!ops.empty() && ops.back().type == 1) ts.leading = ops.back().num;
        } else if (tok == "Td" || tok == "TD") {
            if (ops.size() >= 2 && ops[ops.size() - 1].type == 1 && ops[ops.size() - 2].type == 1) {
                double tx = ops[ops.size() - 2].num, ty = ops[ops.size() - 1].num;
                PdfMat t; t.e = tx; t.f = ty;
                ts.tm = PdfMatMul(t, ts.tm);
                if (tok == "TD") ts.leading = -ty;
            }
        } else if (tok == "Tm") {
            if (ops.size() >= 6) {
                ts.tm.a = ops[ops.size() - 6].num; ts.tm.b = ops[ops.size() - 5].num;
                ts.tm.c = ops[ops.size() - 4].num; ts.tm.d = ops[ops.size() - 3].num;
                ts.tm.e = ops[ops.size() - 2].num; ts.tm.f = ops[ops.size() - 1].num;
            }
        } else if (tok == "T*") {
            PdfMat t; t.f = -ts.leading;
            ts.tm = PdfMatMul(t, ts.tm);
        } else if (tok == "Tj") {
            if (!ops.empty() && ops.back().type == 3) show(ops.back().str);
        } else if (tok == "'") {
            PdfMat t; t.f = -ts.leading;
            ts.tm = PdfMatMul(t, ts.tm);
            if (!ops.empty() && ops.back().type == 3) show(ops.back().str);
        } else if (tok == "\"") {
            PdfMat t; t.f = -ts.leading;
            ts.tm = PdfMatMul(t, ts.tm);
            if (!ops.empty() && ops.back().type == 3) show(ops.back().str);
        } else if (tok == "TJ") {
            if (!ops.empty() && ops.back().type == 4) {
                const std::vector<Operand>& a = ops.back().arr;
                for (size_t k = 0; k < a.size(); k++) {
                    if (a[k].type == 3) {
                        show(a[k].str);
                    } else if (a[k].type == 1 && a[k].num < -200) {
                        cur.push_back(L' ');      // 字距被拉得很开，多半是词间空格
                    }
                }
            }
        } else if (tok == "BI") {
            size_t e = cs.find("EI", i);
            if (e != std::string::npos) i = e + 2;
        }
        ops.clear();
    }
    flush();
}

/* 最近一次 PdfExtractText 得到的"每页正文起点偏移"（元素个数 = 页数）。
 * 视图的"原始版式"模式用它把字符偏移换算成页码，搜索命中也靠它跳页。 */
static std::vector<uint32_t> s_lastPageOffsets;

bool PdfDoc::Extract(std::wstring& out, std::wstring& err) {
    out.clear();

    // 收集所有页面上的文字片段：{页号, y, 文字}
    struct Piece { int page; double y; double size; std::wstring text; };
    std::vector<Piece> pieces;

    for (size_t pi = 0; pi < m_pages.size(); pi++) {
        const PdfObj* page = Find(m_pages[pi]);
        if (page == nullptr) continue;

        // 页面字体表：/Resources /Font << /F1 5 0 R ... >>
        std::map<std::string, const PdfFont*> fonts;
        std::string res;
        if (PdfDictSub(page->dict, "Resources", res)) {
            std::string fontDict;
            if (PdfDictSub(res, "Font", fontDict)) {
                size_t k = 0;
                while (k < fontDict.size()) {
                    size_t slash = fontDict.find('/', k);
                    if (slash == std::string::npos) break;
                    size_t ne = slash + 1;
                    while (ne < fontDict.size() && !PdfIsWhite((uint8_t)fontDict[ne]) && !PdfIsDelim((uint8_t)fontDict[ne])) ne++;
                    std::string name = fontDict.substr(slash + 1, ne - slash - 1);
                    size_t after = ne;
                    while (after < fontDict.size() && PdfIsWhite((uint8_t)fontDict[after])) after++;
                    // 后面应该是 ref（数字 空格 数字 空格 R）
                    size_t q = after;
                    while (q < fontDict.size() && fontDict[q] >= '0' && fontDict[q] <= '9') q++;
                    if (q > after) {
                        int objNum = atoi(fontDict.substr(after, q - after).c_str());
                        PdfFont f;
                        if (LoadFont(objNum, f)) fonts[name] = &m_fontCache[objNum];
                    }
                    k = (ne > k) ? ne : k + 1;
                }
            }
        }

        // 内容流：/Contents 可能是单个引用（/Contents 10 0 R），
        // 也可能是引用数组（/Contents [8 0 R 9 0 R]），所以先把键后面的
        // 一小段原文取出来，再从中挑出所有 "N G R" 形式的对象号。
        std::vector<int> contents;
        size_t cp = PdfFindKey(page->dict, "Contents");
        if (cp != std::string::npos) {
            size_t lim = cp + 400 < page->dict.size() ? cp + 400 : page->dict.size();
            size_t k = cp;
            while (k < lim) {
                char ch = page->dict[k];
                if (ch >= '0' && ch <= '9') {
                    size_t st = k;
                    while (k < lim && page->dict[k] >= '0' && page->dict[k] <= '9') k++;
                    int v = atoi(page->dict.substr(st, k - st).c_str());
                    // 只有 "N G R" 才算引用：跳过空白与 gen 之后必须见到 R
                    size_t g = k;
                    while (g < lim && PdfIsWhite((uint8_t)page->dict[g])) g++;
                    while (g < lim && page->dict[g] >= '0' && page->dict[g] <= '9') g++;
                    while (g < lim && PdfIsWhite((uint8_t)page->dict[g])) g++;
                    if (g < lim && page->dict[g] == 'R') {
                        contents.push_back(v);
                        k = g + 1;
                        continue;
                    }
                    k = st + 1;          // 不是引用（比如 /Length 1202），跳过
                    continue;
                }
                if (ch == ']' || ch == '/' || ch == '>') break;   // 到数组尾或下一个键
                k++;
            }
        }

        std::vector<std::pair<double, std::wstring> > lines;
        std::vector<double> sizes;
        for (size_t ci = 0; ci < contents.size(); ci++) {
            const PdfObj* co = Find(contents[ci]);
            std::string cs;
            if (co == nullptr || !DecodeStream(*co, cs)) continue;
            RunContent(cs, (int)pi, fonts, &lines, &sizes);
        }

        for (size_t k = 0; k < lines.size(); k++) {
            Piece s;
            s.page = (int)pi;
            s.y = lines[k].first;
            s.size = (k < sizes.size()) ? sizes[k] : 12.0;
            s.text = lines[k].second;
            if (!s.text.empty()) pieces.push_back(s);
        }
    }

    if (pieces.empty()) {
        err = L"这份 PDF 里没有文字层（可能是扫描件或图片转的 PDF），无法抽出正文";
        return false;
    }

    // 按 页面 → y 从大到小（PDF 坐标原点在左下，y 越大越靠上）排序。
    // stable_sort 保证同一基线上的多段文字保持原始先后（即从左到右）。
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        if (a.page != b.page) return a.page < b.page;
        return a.y > b.y;
    });

    // 合并成段落：行距正常 → 同一段；行距明显变大 / 换页 → 断段
    std::wstring res2;
    int prevPage = -1;
    double prevY = 0, prevSize = 12;
    bool first = true;
    // 记录每一页的正文起点（页号 → 字符偏移）。视图切到"原始版式"后，
    // 要靠它把"当前文字偏移"换算成"第几页"；搜索命中也用它跳页。
    std::map<int, uint32_t> pageStart;
    for (size_t k = 0; k < pieces.size(); k++) {
        const Piece& p = pieces[k];
        if (first) {
            pageStart[p.page] = (uint32_t)res2.size();
            res2 += p.text;
            first = false;
        } else if (p.page != prevPage) {
            res2 += L'\n';
            pageStart[p.page] = (uint32_t)res2.size();
            res2 += p.text;
        } else {
            double gap = prevY - p.y;
            double sz = p.size > 1 ? p.size : prevSize;
            if (gap > sz * 1.75) {
                res2 += L'\n';                      // 段落间距
            } else if (gap < -sz * 0.6) {
                res2 += L'\n';                      // y 反而变大（异常），也断行
            } else {
                // 同一段落：中文直接接上；两边都是拉丁字母/数字时补一个空格
                if (!res2.empty() && !p.text.empty()) {
                    wchar_t a = res2[res2.size() - 1];
                    wchar_t b = p.text[0];
                    bool latinA = (a < 128 && (iswalnum(a) != 0));
                    bool latinB = (b < 128 && (iswalnum(b) != 0));
                    if (latinA && latinB && a != L' ' && b != L' ') res2.push_back(L' ');
                }
            }
            if (p.page == prevPage) res2 += p.text;
        }
        prevPage = p.page;
        prevY = p.y;
        prevSize = p.size;
    }

    out.swap(res2);

    /* 把"页号 → 起点偏移"整理成按页索引的数组（没有文字的页沿用下一页的起点，
     * 最后一页之后的空页统一用全文长度），供视图换算"偏移 ↔ 页码"。 */
    s_lastPageOffsets.assign(m_pages.size(), (uint32_t)out.size());
    {
        std::map<int, uint32_t>::const_iterator it = pageStart.begin();
        for (size_t i = 0; i < m_pages.size(); i++) {
            while (it != pageStart.end() && (size_t)it->first < i) ++it;
            if (it != pageStart.end() && (size_t)it->first == i) s_lastPageOffsets[i] = it->second;
            else if (it != pageStart.end()) s_lastPageOffsets[i] = it->second;
            else s_lastPageOffsets[i] = (uint32_t)out.size();
        }
    }

    return !out.empty();
}

} // namespace

// ============================================================================
// 对外接口
// ============================================================================

bool PdfIsPdf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    std::wstring low;
    for (size_t i = 0; i < ext.size(); i++) low.push_back((wchar_t)towlower(ext[i]));
    return low == L".pdf";
}

bool PdfExtractText(const uint8_t* data, size_t len, std::wstring& text, std::wstring& err) {
    text.clear();
    err.clear();
    s_lastPageOffsets.clear();
    PdfDoc doc;
    if (!doc.Load(data, len, err)) return false;
    return doc.Extract(text, err);
}

const std::vector<uint32_t>& PdfLastPageOffsets() {
    return s_lastPageOffsets;
}
