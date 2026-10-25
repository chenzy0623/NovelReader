/* pdftest.cpp - PDF 抽取的调试自检（不参与正式构建）
 *
 * 直接把 pdf.cpp 的实现包含进来，于是匿名 namespace 里的 PdfDoc 也能访问，
 * 可以逐层打印：对象数 → 页数 → 每页字体数与 CMap 条目数 → 内容流解压长度
 * → 抽出的正文。用它定位"抽不出字"到底断在哪一步。
 *
 * 编译（在 reader 目录下）：
 *   g++ -std=gnu++17 -O1 -DUNICODE -D_UNICODE -municode -I src ^
 *       -o build\pdftest.exe tools\pdftest.cpp src\zip.cpp src\book.cpp ^
 *       src\format.cpp -static -luser32
 */
#include "common.h"
#include "pdf.h"

#include <cstdio>
#include <string>

/* 调试技巧：把 private 强改成 public，才能窥探 PdfDoc 的内部状态。
 * 这只在这个不参与正式构建的调试工具里用，正式代码不受影响。 */
#define private public
#include "../src/pdf.cpp"   // 故意包含实现，为了能直接用内部类
#undef private

static void Dump(bool ok, const std::wstring& msg) {
    std::wstring line = (ok ? L"[ok] " : L"[!!] ") + msg + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &s[0], n, nullptr, nullptr);
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { Dump(false, L"用法: pdftest.exe <file.pdf>"); return 2; }

    HANDLE h = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Dump(false, L"打不开文件"); return 2; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string buf((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    ReadFile(h, &buf[0], (DWORD)buf.size(), &got, nullptr);
    CloseHandle(h);
    buf.resize(got);

    xpdf:;
    PdfDoc doc;
    std::wstring err;
    if (!doc.Load((const uint8_t*)buf.data(), buf.size(), err)) { Dump(false, L"Load 失败: " + err); return 1; }

    wchar_t tmp[256];
    swprintf(tmp, 256, L"对象数=%d 页面数=%d", (int)doc.m_objs.size(), (int)doc.m_pages.size());
    Dump(true, tmp);

    int totalFonts = 0, totalMap = 0;
    std::wstring dump;
    for (size_t pi = 0; pi < doc.m_pages.size(); pi++) {
        const PdfObj* page = doc.Find(doc.m_pages[pi]);
        if (!page) continue;
        std::string res, fontDict;
        if (!PdfDictSub(page->dict, "Resources", res)) { Dump(false, L"页面没有 Resources"); continue; }
        if (!PdfDictSub(res, "Font", fontDict)) { Dump(false, L"Resources 里没有 Font"); continue; }

        int cnt = 0;
        size_t k = 0;
        while (k < fontDict.size()) {
            size_t slash = fontDict.find('/', k);
            if (slash == std::string::npos) break;
            size_t ne = slash + 1;
            while (ne < fontDict.size() && !PdfIsWhite((uint8_t)fontDict[ne]) && !PdfIsDelim((uint8_t)fontDict[ne])) ne++;
            std::string name = fontDict.substr(slash + 1, ne - slash - 1);
            size_t after = ne;
            while (after < fontDict.size() && PdfIsWhite((uint8_t)fontDict[after])) after++;
            size_t q = after;
            while (q < fontDict.size() && fontDict[q] >= '0' && fontDict[q] <= '9') q++;
            if (q > after) {
                int objNum = atoi(fontDict.substr(after, q - after).c_str());
                PdfFont f;
                bool ok = doc.LoadFont(objNum, f);
                cnt++;
                totalFonts++;
                totalMap += (int)f.map.size();
                std::string u8name(name);
                wchar_t line[256];
                swprintf(line, 256, L"  字体 %hs -> obj %d ok=%d codeBytes=%d map=%d",
                         u8name.c_str(), objNum, (int)ok, f.codeBytes, (int)f.map.size());
                Dump(ok, line);
                // 进一步：看 ToUnicode 引用与解出来的 CMap 头
                const PdfObj* fo = doc.Find(objNum);
                if (fo) {
                    std::wstring dd(fo->dict.begin(), fo->dict.end());
                    Dump(true, L"    字体字典: " + dd.substr(0, dd.size() < 200 ? dd.size() : 200));
                    int tu = 0;
                    bool hasRef = PdfDictRef(fo->dict, "ToUnicode", tu);
                    swprintf(line, 256, L"    ToUnicode ref=%d has=%d", tu, (int)hasRef);
                    Dump(hasRef, line);
                    if (hasRef) {
                        const PdfObj* to = doc.Find(tu);
                        std::string cm;
                        bool okd = to ? doc.DecodeStream(*to, cm) : false;
                        swprintf(line, 256, L"    CMap 解码=%d 长度=%d", (int)okd, (int)cm.size());
                        Dump(okd, line);
                        if (okd) {
                            std::wstring w(cm.begin(), cm.end());
                            Dump(true, L"    CMap 头: " + w.substr(0, w.size() < 160 ? w.size() : 160));
                        }
                    }
                }
            }
            k = (ne > k) ? ne : k + 1;
        }
        swprintf(tmp, 256, L"第 %d 页字体数=%d", (int)pi, cnt);
        Dump(cnt > 0, tmp);

        std::string cd;
        if (PdfDictSub(page->dict, "Contents", cd)) {
            std::wstring w(cd.begin(), cd.end());
            Dump(true, L"  Contents 段: " + w);
        } else {
            Dump(false, L"  没有 Contents");
        }
    }

    std::wstring text;
    if (!doc.Extract(text, err)) {
        Dump(false, L"Extract 失败: " + err);
        swprintf(tmp, 256, L"字体合计=%d CMap 条目合计=%d", totalFonts, totalMap);
        Dump(true, tmp);
        return 1;
    }
    swprintf(tmp, 256, L"抽出 %d 字符", (int)text.size());
    Dump(true, tmp);
    std::wstring head = text.substr(0, text.size() < 400 ? text.size() : 400);
    Dump(true, L"正文开头：\n" + head);
    return 0;
}
