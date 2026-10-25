/* pdfrender.h - 用系统 Windows.Data.Pdf 忠实渲染 PDF 页面（1.2.0）
 *
 * 为什么要有这个模块：
 *   pdf.cpp 只把 PDF 的文字抽出来重新排版，好处是能换字体、能夜间模式、能和 TXT 共用
 *   分章与进度，但代价是丢失原始版式（图片、表格、精确位置）。这个模块走系统自带的
 *   PDF 引擎（Windows.Data.Pdf，Win10 起内置），把整页渲染成位图，用于「PDF 原始版式」
 *   显示模式：图、表、双栏、公式全部原样呈现。
 *
 * 两种模式的关系（由 ui.cpp 的 F9 切换）：
 *   - 文字重排模式（默认，pdf.cpp）：可换字体、可搜索高亮、进度按字符偏移记
 *   - 原始版式模式（本模块）：一页一图，页码导航，搜索命中后跳到该页
 *
 * 实现要点：
 *   - WinRT 用 C 风格 ABI 直接调（MinGW 没有 C++/WinRT），接口虚表顺序与 IID 都按
 *     Windows 元数据核过：IPdfDocument / IPdfPage / IPdfDocumentStatics /
 *     IStorageFileStatics / IPdfPageRenderOptions
 *   - 异步操作不注册回调，直接轮询 IAsyncInfo::get_Status（Started=0/Completed=1/
 *     Canceled=2/Error=3），避免写一堆委托对象
 *   - 输出流用 CreateRandomAccessStreamOnFile 落到临时 PNG 文件，再用 GDI+ 解码成
 *     HBITMAP；比手搓 IBuffer/IBufferByteAccess 简单得多
 *   - 页面位图按「页码 + 目标宽度」缓存最近 3 张，翻页时不重复渲染
 */
#ifndef NR_PDFRENDER_H
#define NR_PDFRENDER_H

#include <windows.h>
#include <string>

/* 打开一份 PDF 用于渲染：成功返回 true，页数写入 g 内部的计数 */
bool PdfRenderOpen(const std::wstring& path, std::wstring& err);

/* 关闭当前文档并释放所有缓存位图 */
void PdfRenderClose();

/* 当前是否已打开一份可渲染的 PDF */
bool PdfRenderOk();

/* 总页数（0 表示没打开） */
int PdfRenderPageCount();

/* 某页的原始尺寸（单位：PDF 点，1 点 = 1/72 英寸）；失败返回 false */
bool PdfRenderPageSizePt(int index, double* wPt, double* hPt);

/* 把第 index 页渲染成位图，宽度尽量接近 targetW（内部按 2 倍超采样再缩小，
 * 文字边缘更干净）。返回的 HBITMAP 归调用方（用完 DeleteObject），
 * 但同一页在缓存命中时会返回缓存里那张，调用方**不要**删除缓存里的位图。
 * 用法：ViewSetPdfBitmap() 只借用，由本模块负责最终释放。 */
HBITMAP PdfRenderPageBitmap(int index, int targetW);

/* 清空缓存（窗口宽度变化过大时调用，避免占内存） */
void PdfRenderDropCache();

/* 最近一次渲染失败的内部原因（含 HRESULT），便于自检工具与调试输出定位问题 */
std::wstring PdfRenderLastError();

#endif /* NR_PDFRENDER_H */
