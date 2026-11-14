/* sourcejson.h - 书源的 JSON 导入 / 导出
 * ============================================================================
 * 为什么要有这个文件：
 *   书源规则原来只能一条条手填（或者在 NovelReader.ini 的 [sources] 段里手改），
 *   想把自己配好的源发给别人、或者从别人那里拷一份，都没有"文件"这个载体。
 *   JSON 是最好抄的格式：一个源就是一段 {"name":…,"search":…}，谁都能看懂、
 *   能手改、能放进帖子/仓库里分享。
 *
 * 文件格式（UTF-8，自己写的时候不用带 BOM，带了解析器也认）：
 *   {
 *     "app": "NovelReader",
 *     "type": "book-sources",
 *     "version": 1,
 *     "sources": [
 *       { "name": "某站", "search": "https://…?q={kw}",
 *         "list": "…", "tocStart": "…", "toc": "…", "content": "…" }
 *     ]
 *   }
 *
 * 为了"别人发来的文件尽量能导入"，解析器故意做得很宽容：
 *   - 顶层也可以直接是数组（[…,…]），不一定非得包一层对象；
 *   - 键名认几种常见写法（list / listRe / searchRe 等），未知键直接跳过；
 *   - 多余的字段、注释外的空白、\uXXXX 转义、代理对都能吃；
 *   - 内置源导出时带 "builtin": true，导入时一律当普通源处理（不覆盖本机内置源）。
 * 不能吃的东西只有一种：真的不是一个合法的 JSON 对象/数组 —— 那时返回 false，
 * err 里会写明第几个字符出的问题，方便用户定位。
 *
 * 本文件不碰界面、不碰 ini，只做"字符串/文件 ⇄ std::vector<BookSource>"，
 * 所以在单元测试（tests\）里可以脱离窗口直接跑。
 * ============================================================================ */
#ifndef NR_SOURCEJSON_H
#define NR_SOURCEJSON_H

#include <string>
#include <vector>

#include "source.h"

/* 把书源列表编码成 JSON 文本（宽字符，落地时再转 UTF-8）。
 * includeBuiltin = false 时不导出内置源（0 号）：每个人的 0 号都是同一个内置例子，
 * 导出它只是让文件变长；分享自己的源时没必要带上。 */
std::wstring SourceExportJson(const std::vector<BookSource>& list, bool includeBuiltin);

/* 解析 JSON 文本。成功时把源按出现顺序追加到 out（不碰 out 里原有的内容），
 * 失败时返回 false 并在 err 里写明原因与位置。out 里的 builtin 一律是 false。 */
bool SourceImportJson(const std::wstring& json, std::vector<BookSource>& out, std::wstring& err);

/* 写文件：UTF-8（无 BOM）+ LF。父目录不存在会尝试建一次。
 * 返回 false 时 err 里是 Win32 错误的可读说明。 */
bool SourceExportJsonFile(const std::wstring& path, const std::vector<BookSource>& list,
                          bool includeBuiltin, std::wstring& err);

/* 读文件并解析。文件编码认 UTF-8（有/无 BOM）、UTF-16LE/BE（有 BOM）；
 * 认不出来按 UTF-8 强行解（宁可给出乱码，也不要"这文件不是 UTF-8 所以打不开"）。 */
bool SourceImportJsonFile(const std::wstring& path, std::vector<BookSource>& out, std::wstring& err);

/* JSON 字符串转义（导出用）：加引号、转义 " \ 和控制字符，中文保持原样。
 * 单独暴露是为了单元测试能直接对着它断言。 */
std::wstring SourceJsonQuote(const std::wstring& s);

#endif /* NR_SOURCEJSON_H */
