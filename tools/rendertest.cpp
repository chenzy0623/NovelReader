/* rendertest.cpp - 开发期自检工具（不参与正式构建）
 *
 * 用法：rendertest.exe <pdf 路径> [输出 bmp 路径] [目标像素宽度]
 * 作用：走 pdfrender.cpp 打开 PDF，打印页数与页面尺寸，把第 1 页渲染成 BMP 存盘，
 *       用来肉眼确认「Windows.Data.Pdf 渲染链路」是否通、字号清不清晰。
 *
 * 输出一律转成 UTF-8 再用 WriteFile 写 stdout：控制台代码页是 GBK 时 wprintf 会把
 * 中文按 ANSI 丢弃，看不到真正的错误原因。
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/pdfrender.h"

/* 把宽字符串按 UTF-8 写到 stdout */
static void Say(const std::wstring& s)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string utf8((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &utf8[0], need, nullptr, nullptr);
    utf8 += "\n";
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD wrote = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
}

/* 把 HBITMAP 存成 24 位 BMP（开发工具，写最简单的格式） */
static bool SaveBmp(HBITMAP hbm, const wchar_t* path)
{
    BITMAP bm = { 0 };
    if (!GetObjectW(hbm, sizeof(bm), &bm)) return false;

    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    const int stride = ((w * 3 + 3) / 4) * 4;
    const DWORD dataSize = (DWORD)(stride * h);

    BITMAPINFOHEADER bih = { 0 };
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;                  /* 正数 = 自下而上，BMP 默认如此 */
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = dataSize;

    std::vector<BYTE> buf(dataSize);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hbm, 0, (UINT)h, buf.data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines == 0) return false;

    BITMAPFILEHEADER fh = { 0 };
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bih);
    fh.bfSize = fh.bfOffBits + dataSize;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") != 0 || !fp) return false;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&bih, sizeof(bih), 1, fp);
    fwrite(buf.data(), dataSize, 1, fp);
    fclose(fp);
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { Say(L"用法: rendertest.exe <pdf> [out.bmp] [width] [pageIndex]"); return 1; }

    std::wstring pdf = argv[1];
    std::wstring out = argc >= 3 ? argv[2] : L"render_page1.bmp";
    int wantW = argc >= 4 ? _wtoi(argv[3]) : 800;
    int pageIdx = argc >= 5 ? _wtoi(argv[4]) : 0;

    Say(L"文件: " + pdf);

    std::wstring err;
    if (!PdfRenderOpen(pdf, err)) {
        Say(L"PdfRenderOpen 失败: " + err);
        return 2;
    }
    Say(L"页数 = " + std::to_wstring(PdfRenderPageCount()));

    double wpt = 0, hpt = 0;
    if (PdfRenderPageSizePt(pageIdx, &wpt, &hpt)) {
        wchar_t tmp[128];
        /* 用 swprintf（stdio 版）而不是 wsprintfW：后者不支持 %f */
        swprintf(tmp, 128, L"第%d页尺寸 = %.1f x %.1f 点", pageIdx + 1, wpt, hpt);
        Say(tmp);
    } else {
        Say(L"取页面尺寸失败");
    }

    HBITMAP hbm = PdfRenderPageBitmap(pageIdx, wantW);
    if (!hbm) {
        Say(L"渲染失败: " + PdfRenderLastError());
        PdfRenderClose();
        return 3;
    }

    BITMAP bm = { 0 };
    GetObjectW(hbm, sizeof(bm), &bm);
    Say(L"位图 = " + std::to_wstring((int)bm.bmWidth) + L" x " + std::to_wstring((int)bm.bmHeight));

    if (SaveBmp(hbm, out.c_str())) Say(L"已保存: " + out);
    else Say(L"保存 BMP 失败");

    PdfRenderClose();
    return 0;
}
