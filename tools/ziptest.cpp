// =============================================================================
// ziptest.cpp —— zip.cpp 的临时自测程序（只用于验证，不链进主程序）
//
// 用法：ziptest
// 依次验证三个样本：
//   test/fmt/stored.zip  —— stored + deflate 两种条目
//   test/fmt/sample.docx —— 真实 Word 文档，取 word/document.xml
//   test/fmt/sample.epub —— 手写 EPUB，取 container.xml 与 ch1.xhtml
// 每个检查都打印"期望 vs 实际"，最后汇总 PASS/FAIL 数并以非 0 退出码结束。
// =============================================================================

#include "zip.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        g_pass++;
        std::printf("  [ok]   %s\n", what);
    } else {
        g_fail++;
        std::printf("  [FAIL] %s\n", what);
    }
}

// 把整个文件读进内存（二进制安全，顺便拿到精确长度）
bool ReadAllBytes(const char* path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::printf("  [ERR] 打不开文件: %s\n", path);
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long n = std::ftell(f);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = (n > 0) ? std::fread(out.data(), 1, (size_t)n, f) : 0;
    std::fclose(f);
    if (got != (size_t)n) {
        std::printf("  [ERR] 读文件不完整: %s\n", path);
        return false;
    }
    return true;
}

// 打印前 n 个字节。控制字符转成可见的转义形式，其余（含 UTF-8 多字节）
// 原样输出，这样中文能直接在控制台里看到。
void PrintHead(const std::string& s, size_t n) {
    size_t lim = (s.size() < n) ? s.size() : n;
    for (size_t i = 0; i < lim; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') {
            std::printf("\\r");
        } else if (c == '\n') {
            std::printf("\\n");
        } else if (c == '\t') {
            std::printf("\\t");
        } else if (c < 0x20 || c == 0x7F) {
            std::printf(".");
        } else {
            std::putchar((int)c);
        }
    }
}

// 数一数 sub 在 s 里出现了几次（用于验证"重复"正好 200 次）
size_t CountSub(const std::string& s, const char* sub) {
    size_t subLen = std::strlen(sub);
    if (subLen == 0 || s.size() < subLen) return 0;
    size_t n = 0;
    for (size_t i = 0; i + subLen <= s.size(); i++) {
        if (std::memcmp(s.data() + i, sub, subLen) == 0) n++;
    }
    return n;
}

bool Contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

// 从本地文件头算出条目的数据区在文件里的位置和长度（测试里要直接喂给 InflateRaw）
bool DataRange(const std::vector<uint8_t>& bytes, const ZipEntry& e, size_t& off, size_t& len) {
    if (e.localOff + 30 > bytes.size()) return false;
    const uint8_t* p = bytes.data() + e.localOff;
    if (!(p[0] == 0x50 && p[1] == 0x4B && p[2] == 0x03 && p[3] == 0x04)) return false;
    uint32_t nl = (uint32_t)p[26] | ((uint32_t)p[27] << 8);
    uint32_t el = (uint32_t)p[28] | ((uint32_t)p[29] << 8);
    off = (size_t)e.localOff + 30 + nl + el;
    len = e.compSize;
    return off + len <= bytes.size();
}

// 打开一个 zip：读文件 + Load + 打印条目表。成功返回 true。
bool OpenAndDump(const char* path, std::vector<uint8_t>& bytes, ZipArchive& zip) {
    std::printf("\n=== %s ===\n", path);
    if (!ReadAllBytes(path, bytes)) {
        g_fail++;
        return false;
    }
    std::printf("  文件大小: %zu 字节\n", bytes.size());
    bool ok = zip.Load(bytes.data(), bytes.size());
    Check(ok, "ZipArchive::Load 返回 true");
    if (!ok) return false;

    std::printf("  条目数: %zu\n", zip.Count());
    std::printf("  %-34s %-6s %10s %10s %10s\n", "name", "method", "compSize", "uncompSize", "localOff");
    for (size_t i = 0; i < zip.Count(); i++) {
        const ZipEntry& e = zip.At(i);
        std::printf("  %-34s %-6u %10u %10u %10u\n",
                    e.name.c_str(), (unsigned)e.method,
                    (unsigned)e.compSize, (unsigned)e.uncompSize, (unsigned)e.localOff);
    }
    return true;
}

// 解压一个条目并打印长度 + 前 head 字节；内容通过 content 带回，返回是否成功
bool ReadAndDump(const ZipArchive& zip, const char* name, size_t head, std::string* content) {
    std::string out;
    bool ok = zip.ReadName(name, out);
    if (!ok) {
        std::printf("  [FAIL] 解压失败: %s\n", name);
        g_fail++;
        return false;
    }
    std::printf("  %s -> 长度 %zu，前 %zu 字节: ", name, out.size(),
                (out.size() < head) ? out.size() : head);
    PrintHead(out, head);
    std::printf("\n");
    if (content != nullptr) *content = out;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 控制台切到 UTF-8，中文/日文内容才不会被显示成乱码
    SetConsoleOutputCP(65001);
#endif
    // 辅助模式：ziptest --raw <zip 路径> <条目名> <输出文件>
    // 把单个条目解压后原样写成文件，方便和系统解压器逐字节比对（回归用）。
    if (argc >= 5 && std::strcmp(argv[1], "--raw") == 0) {
        std::vector<uint8_t> bytes;
        if (!ReadAllBytes(argv[2], bytes)) return 2;
        ZipArchive zip;
        if (!zip.Load(bytes.data(), bytes.size())) return 3;
        std::string content;
        if (!zip.ReadName(argv[3], content)) return 4;
        std::FILE* f = std::fopen(argv[4], "wb");
        if (f == nullptr) return 5;
        size_t wrote = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
        return (wrote == content.size()) ? 0 : 6;
    }

    // 辅助模式 2：ziptest --inflate <raw deflate> <期望输出文件>
    // 直接用 InflateRaw 解一段裸 DEFLATE 流并与期望文件逐字节比较，
    // 用来覆盖测试 zip 里不一定出现的块类型（stored / 固定 / 动态）。
    if (argc >= 4 && std::strcmp(argv[1], "--inflate") == 0) {
        std::vector<uint8_t> raw, want;
        if (!ReadAllBytes(argv[2], raw) || !ReadAllBytes(argv[3], want)) return 2;
        std::string out;
        size_t used = 0;
        if (!InflateRaw(raw.data(), raw.size(), out, want.size(), &used)) return 3;
        if (out.size() != want.size()) return 4;
        if (!want.empty() && std::memcmp(out.data(), want.data(), want.size()) != 0) return 5;
        if (used == 0 || used > raw.size()) return 6;
        std::printf("    ok  raw=%zu out=%zu consumed=%zu\n", raw.size(), out.size(), used);
        return 0;
    }

    const char* dir = "D:\\deepseek gongzuoqu\\reader\\test\\fmt\\";

    // ---------------------------------------------------------------- stored.zip
    {
        std::string path = std::string(dir) + "stored.zip";
        std::vector<uint8_t> bytes;
        ZipArchive zip;
        if (OpenAndDump(path.c_str(), bytes, zip)) {
            Check(zip.Count() == 2, "条目数 == 2");

            int iPlain = zip.Find("plain.txt");
            int iDefl  = zip.Find("DEFLATED.TXT");   // 故意用大写，验证忽略大小写
            Check(iPlain >= 0, "Find(\"plain.txt\") 命中");
            Check(iDefl >= 0, "Find(\"DEFLATED.TXT\") 忽略大小写命中");
            Check(zip.Find("no_such_entry") == -1, "Find 找不到时返回 -1");

            std::string plain;
            if (iPlain >= 0 && zip.ReadIndex((size_t)iPlain, plain)) {
                std::printf("  plain.txt -> 长度 %zu，内容: ", plain.size());
                PrintHead(plain, 40);
                std::printf("\n");
                Check(plain == "HelloStored", "plain.txt 内容 == \"HelloStored\"");
            } else {
                Check(false, "plain.txt 解压失败");
            }

            std::string defl;
            bool ok = ReadAndDump(zip, "deflated.txt", 40, &defl);
            Check(ok, "deflated.txt 解压成功");
            if (ok) {
                // 文件内容是 "重复重复重复"（18 字节）重复 200 次 = 3600 字节。
                // 注意 CountSub 是"重叠计数"：在完全均匀的串里，18 字节模式
                // 出现在每个 6 字节边界上，共 600-3+1 = 598 次，属正常现象。
                Check(defl.size() == 3600, "deflated.txt 长度严格等于 uncompSize(3600)");
                std::printf("  \"重复\" 出现 %zu 次，\"重复重复重复\" 出现 %zu 次（重叠计数）\n",
                            CountSub(defl, "重复"), CountSub(defl, "重复重复重复"));
                Check(CountSub(defl, "重复") == 600, "\"重复\" 恰好出现 600 次");
                // 整段内容必须严格等于 18 字节模式重复 200 遍（不容许多一个字节）
                std::string expect;
                for (int r = 0; r < 200; r++) expect += "重复重复重复";
                Check(defl == expect, "全文严格等于 18 字节模式 x 200，无任何多余字节");
                Check(zip.At((size_t)iDefl).uncompSize == 3600, "中央目录 uncompSize == 3600");
            }

            // ---- 直接压 InflateRaw：consumed 回填与截断健壮性 ----
            size_t dOff = 0, dLen = 0;
            if (iDefl >= 0 && DataRange(bytes, zip.At((size_t)iDefl), dOff, dLen)) {
                std::string raw;
                size_t used = 0;
                bool okRaw = InflateRaw(bytes.data() + dOff, dLen, raw, 3600, &used);
                Check(okRaw && raw.size() == 3600, "InflateRaw 直接解压得到 3600 字节");
                Check(okRaw && used > 0 && used <= dLen, "consumed 落在 (0, compSize] 内");
                std::printf("  deflate 数据 %zu 字节 -> 3600 字节，consumed=%zu\n", dLen, used);

                // 输出上限比声明小 1 字节：必须失败，不能悄悄截断
                std::string small;
                Check(!InflateRaw(bytes.data() + dOff, dLen, small, 3599, nullptr),
                      "expectedSize 偏小(3599) 时返回 false");

                // 把压缩流在每一个位置截断：都不许"误报成功"（也顺便压越界读）
                size_t badCut = 0;
                for (size_t cut = 0; cut < dLen; cut++) {
                    std::string t;
                    if (InflateRaw(bytes.data() + dOff, cut, t, 3600, nullptr)) badCut++;
                }
                Check(badCut == 0, "全部截断长度下 InflateRaw 都不误报成功");
            }

            // 用截断的整体文件 Load：要么失败，要么条目表不完整，不能崩
            ZipArchive cutZip;
            bool cutOk = cutZip.Load(bytes.data(), bytes.size() / 2);
            std::printf("  截断到 %zu 字节后 Load 返回 %s，条目数 %zu\n",
                        bytes.size() / 2, cutOk ? "true" : "false", cutZip.Count());
            Check(!cutOk || cutZip.Count() < 2, "截断文件不会被当成完整 zip");
        }
    }

    // ---------------------------------------------------------------- sample.docx
    {
        std::string path = std::string(dir) + "sample.docx";
        std::vector<uint8_t> bytes;
        ZipArchive zip;
        if (OpenAndDump(path.c_str(), bytes, zip)) {
            Check(zip.Count() > 5, "docx 条目数 > 5");

            int idx = zip.Find("word/document.xml");
            Check(idx >= 0, "找到 word/document.xml");
            if (idx >= 0) {
                std::string xml;
                bool ok = zip.ReadIndex((size_t)idx, xml);
                Check(ok, "解压 word/document.xml 成功");
                if (ok) {
                    std::printf("  word/document.xml -> 长度 %zu（声明 %u），前 200 字节: ",
                                xml.size(), (unsigned)zip.At((size_t)idx).uncompSize);
                    PrintHead(xml, 200);
                    std::printf("\n");
                    Check(xml.size() == (size_t)zip.At((size_t)idx).uncompSize,
                          "document.xml 长度 == uncompSize");
                    Check(Contains(xml, "<w:document"), "document.xml 含 \"<w:document\"");
                }
            }

            // 顺便压一个 349KB 的大条目，验证动态 Huffman + 大输出缓冲没问题
            std::string styles;
            bool okStyles = zip.ReadName("word/styles.xml", styles);
            Check(okStyles && !styles.empty(), "解压 word/styles.xml（349KB 动态块）成功");
            if (okStyles) {
                Check(styles.size() == 349458, "styles.xml 长度 == 349458");
                Check(Contains(styles, "<?xml"), "styles.xml 以 XML 声明开头");
            }

            // 最大的条目 438KB：跑一遍确认 64 位缓冲与长回溯都没问题
            std::string swe;
            bool okSwe = zip.ReadName("word/stylesWithEffects.xml", swe);
            Check(okSwe && swe.size() == 438131, "解压 stylesWithEffects.xml（438KB）长度 == 438131");
            if (okSwe) {
                Check(Contains(swe, "<?xml"), "stylesWithEffects.xml 以 XML 声明开头");
            }
        }
    }

    // ---------------------------------------------------------------- sample.epub
    {
        std::string path = std::string(dir) + "sample.epub";
        std::vector<uint8_t> bytes;
        ZipArchive zip;
        if (OpenAndDump(path.c_str(), bytes, zip)) {
            Check(zip.Find("mimetype") >= 0, "找到 mimetype（stored）");
            std::string mime;
            if (zip.ReadName("mimetype", mime)) {
                Check(mime == "application/epub+zip", "mimetype == application/epub+zip");
            } else {
                Check(false, "解压 mimetype 失败");
            }

            std::string container;
            bool okC = zip.ReadName("META-INF/container.xml", container);
            Check(okC, "解压 META-INF/container.xml 成功");
            if (okC) {
                std::printf("  META-INF/container.xml -> 长度 %zu，前 40 字节: ", container.size());
                PrintHead(container, 40);
                std::printf("\n");
                Check(Contains(container, "container"), "container.xml 含 \"container\"");
                Check(Contains(container, "OEBPS/content.opf"), "container.xml 指向 content.opf");
            }

            std::string ch1;
            bool ok1 = zip.ReadName("OEBPS/text/ch1.xhtml", ch1);
            Check(ok1, "解压 OEBPS/text/ch1.xhtml 成功");
            if (ok1) {
                std::printf("  OEBPS/text/ch1.xhtml -> 长度 %zu，前 40 字节: ", ch1.size());
                PrintHead(ch1, 40);
                std::printf("\n");
                Check(Contains(ch1, "初入宗门"), "ch1.xhtml 含 \"初入宗门\"");
                Check(ch1.size() == (size_t)zip.At((size_t)zip.Find("OEBPS/text/ch1.xhtml")).uncompSize,
                      "ch1.xhtml 长度 == uncompSize");
            }

            std::string ch2;
            Check(zip.ReadName("OEBPS/text/ch2.xhtml", ch2) && !ch2.empty(),
                  "解压 OEBPS/text/ch2.xhtml 成功");
        }
    }

    std::printf("\n===== 汇总: PASS %d / FAIL %d =====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
