/* pdf.h - PDF 文本抽取：把 PDF 里的文字按排版顺序抽出来，再走通用的分章 / 搜索管线
 *
 * 设计取舍（为什么是"抽文字"而不是"画页面"）：
 *   系统里有 Windows.Data.Pdf，可以把每页渲染成位图，视觉上 100% 还原（含图片、
 *   字体、分栏）。但那样得到的是图片：没法全文搜索、没法选字复制、没法换字体
 *   和夜间配色，字号也不能调 —— 而这些正是本程序的核心功能。
 *   所以这里选择"抽出文字 + 按版面重建段落"：标题、段落、换行都保留，正文
 *   重新排版到当前字号下，于是搜索、书签、记忆位置、主题配色全部照旧可用。
 *   代价是 PDF 里嵌入的图片和特殊排版（多栏、表格）不还原 —— 这一点在使用
 *   说明里写明了。
 *
 * 支持的 PDF 子集（覆盖了浏览器 / Word / WPS 导出的绝大多数小说 PDF）：
 *   - 交叉引用表与 xref 流都不解析，直接按 "N G obj" 顺序扫对象（够用且不挑版本）
 *   - 流过滤器只支持 FlateDecode（zlib）和不压缩；其它过滤器（LZW/DCT 等）跳过
 *   - 字体只取 ToUnicode CMap 做编码映射（Type0/Identity-H 双字节码、简单字体单字节码）
 *   - 文本算子：BT/ET、Tf、Td/TD/Tm/T*、Tj/TJ/'/"（其余算子安全跳过）
 *   - 不支持加密 PDF、不支持对象流（ObjStm）里的字典 —— 遇到就报错而不是给出乱码
 */
#ifndef NR_PDF_H
#define NR_PDF_H

#include <cstdint>
#include <string>
#include <vector>

// 判断扩展名是不是 .pdf（大小写不敏感）
bool PdfIsPdf(const std::wstring& path);

// 从内存里的 PDF 字节抽出正文。
//   data/len ：整个 PDF 文件的内容
//   text     ：输出，段落之间用 '\n' 分隔（与 txt 小说的段落格式一致），
//              后续 BookLoad 会照常清洗、识别章节
//   err      ：失败时的中文原因（给界面直接显示）
// 返回 false 表示这份 PDF 读不出文字（加密、扫描件、结构损坏等）。
bool PdfExtractText(const uint8_t* data, size_t len, std::wstring& text, std::wstring& err);

// 最近一次 PdfExtractText 得到的"每页正文起始字符偏移"（元素个数 = 页数）。
// 视图的"原始版式"模式要靠它把字符偏移换算成页码，搜索命中也用它跳到对应页。
const std::vector<uint32_t>& PdfLastPageOffsets();

#endif // NR_PDF_H
