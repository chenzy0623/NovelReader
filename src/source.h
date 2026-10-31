/* source.h - 书源：让用户自己填"某个小说站怎么搜、怎么找目录、怎么抽正文"
 * ============================================================================
 * 背景：原来书源是写死在代码里的，站点一改版、或者用户想去别的站，就得改源码。
 * 这里把书源的差异抽成四条"规则"，普通用户也能照猫画虎加一个站：
 *
 *   name     书源名字，界面上显示这个（随便起，例如"我的书源"）。
 *   search   搜索网址模板，用 {kw} 代表关键词，例如
 *            http://某个小说站/search?q={kw}
 *            留空表示这个源不支持按书名搜，只能在「打开网页小说」里贴网址。
 *   listRe   搜索结果页的正则，第 1 组=书籍页网址，第 2 组=书名，
 *            第 3 组（可选）=简介。留空则退化成"页面里所有链接都当书"。
 *   tocRe    书籍页的正则，第 1 组=章节网址，第 2 组=章节名。
 *            留空则用通用的"目录页识别"（页面里像'第N章'的链接）。
 *   contentRe 章节页的正则，第 1 组=正文容器的 HTML。
 *            留空则把整页剥成纯文本（广告、导航会一起进来）。
 *
 * 为什么用正则而不是 CSS 选择器：
 *   程序里没有 HTML DOM 解析器（自己写一个会让 exe 大不少），而 <regex> 是
 *   C++ 标准库自带的，体积零成本；对"抠链接 + 抠正文容器"这种固定形状的
 *   需求，一条正则就够，用户还能从别的阅读器的规则里抄来改。
 *
 * 规则存在 NovelReader.ini 的 [sources] 段里，形如：
 *   n=2
 *   s0_name=我的书源
 *   s0_search=http://某个小说站/search?q={kw}
 *   ...
 *   cur=0
 * 0 号恒为内置源：删不掉，改坏了可以"恢复内置"。
 * ============================================================================ */
#ifndef NR_SOURCE_H
#define NR_SOURCE_H

#include <string>
#include <vector>

/* 一个书源 = 名字 + 五条规则。字段含义见文件头的说明。 */
struct BookSource {
    std::wstring name;          /* 显示名 */
    std::wstring search;        /* 搜索网址模板，{kw} 占位 */
    std::wstring listRe;        /* 结果页正则 */
    std::wstring tocStart;      /* 目录容器起点（一个普通子串，可空） */
    std::wstring tocRe;         /* 目录正则 */
    std::wstring contentRe;     /* 正文正则 */
    bool builtin = false;       /* 内置源：不可删除，只能改 */
};

/* 全部书源（0 号一定是内置源）。返回引用是为了让界面直接改完再保存。 */
std::vector<BookSource>& SourceList();

/* 从 ini 读一次。程序启动时调用；读不到或数据坏了会自动补上内置源。 */
void SourceLoad();

/* 把当前的书源列表写回 ini（立即落盘，不等退出）。 */
void SourceSave();

/* 当前选中的书源下标。越界时自动夹回 0，所以调用方不用再判。 */
int SourceCurrent();

/* 切换当前书源（会落盘，下次启动还记得）。 */
void SourceSetCurrent(int idx);

/* 内置书源（无忧书城）的定义，改坏时可用来恢复。 */
const BookSource& SourceBuiltin();

/* 让 0 号位置一定是内置源：没有就插一个，有但不是内置源就覆盖。
 * SourceLoad 结束时和"恢复内置"按钮都会调用它。 */
void SourceEnsureBuiltin();

/* 追加一个空书源（名字默认"新书源"），返回它的下标。
 * 返回 -1 表示已经到上限（16 个）—— 书源列表只在 ini 里存几行文本，
 * 限个数是为了让界面上的下拉框不至于长得离谱。 */
int SourceAdd();

/* 删除第 idx 个书源。内置源（idx==0 且 builtin）删不掉，返回 false。 */
bool SourceDelete(int idx);

#endif /* NR_SOURCE_H */
