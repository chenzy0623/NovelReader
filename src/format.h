/* format.h - 多格式支持：把 EPUB / DOCX / HTML / Markdown 抽取成纯文本
 * ============================================================================
 * 设计思路（为什么是"抽取成纯文本"而不是"每种格式一套渲染"）：
 *   本程序的排版、分章、搜索、位置记忆全都建立在「一段一段的宽字符文本」之上
 *   （见 book.cpp 的 Book 结构与 Normalize 流程）。与其为每种格式再造一套渲染，
 *   不如在入口处把各种格式统一"拍平"成纯文本，后面所有功能零成本复用 ——
 *   也正因为如此，epub 里的章节标题、docx 里的段落，照样能被"第 N 章"规则识别。
 *
 * 三种处理路径：
 *   1) zip 容器（.epub / .docx）：必须拿原始字节，用 zip.cpp 解压出里面的
 *      xhtml / xml，再抽文字 → FormatFromZip；
 *   2) 标记型文本（.html / .htm / .xhtml / .md）：先按普通文本解码（编码自动识别），
 *      再剥掉标签 / Markdown 记号 → FormatStripMarkup；
 *   3) 纯文本（.txt 或任何其它扩展名）：什么都不做，直接走原来的管线。
 * ============================================================================ */
#ifndef NR_FORMAT_H
#define NR_FORMAT_H

#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

/* 这个路径是不是 zip 容器格式（.epub / .docx）——即"必须拿原始字节才能处理"。
 * 参数 path：文件路径（只看扩展名，大小写不敏感）。
 * 返回：是 zip 容器返回 true。调用方此时不要先解码成文本。 */
bool FormatIsZipContainer(const std::wstring& path);

/* 这个路径是不是标记型文本（.html / .htm / .xhtml / .md / .markdown）。
 * 参数 path：文件路径。返回 true 表示解码之后还要再剥一层标签。 */
bool FormatIsMarkup(const std::wstring& path);

/* 从 zip 容器（.epub / .docx）里抽取纯文本。
 * 参数 path    ：路径，只用来判断是 epub 还是 docx（以及出错时拼提示）。
 * 参数 bytes   ：整个文件的原始字节首地址（调用方已读进内存）。
 * 参数 len     ：字节数。
 * 参数 text    ：输出，抽出来的正文（宽字符，段落之间用 '\n' 分隔）。
 * 参数 err     ：输出，失败原因（中文，直接给用户看）。
 * 返回：成功 true。失败时 err 有内容、text 内容未定义。
 * 说明：EPUB 按 OPF 的 spine 顺序拼接各章；DOCX 取 word/document.xml 的段落。
 * 为什么传裸指针而不是 vector：book.cpp 里那份缓冲是 vector<char>，
 * 为了类型匹配再整份拷贝一次，打开 100MB 的 docx 就白吃一倍内存。 */
bool FormatFromZip(const std::wstring& path, const char* bytes, size_t len,
                   std::wstring& text, std::wstring& err);

/* 剥掉 HTML / Markdown 标记，返回纯文本。
 * 参数 path：路径（判断是 html 还是 md，两者规则不同）。
 * 参数 in  ：已经解码好的原始文本。
 * 返回：剥好的文本（段落之间用 '\n' 分隔）。失败也只是"什么都不认"，返回去标签后的结果。 */
std::wstring FormatStripMarkup(const std::wstring& path, const std::wstring& in);

#endif /* NR_FORMAT_H */
