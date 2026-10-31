/* common.h - 小说阅读器的公共声明（数据结构 + 全局 + 各模块接口）
 *
 * 这个头文件是所有 .cpp 的"合同"：谁提供什么、谁调用什么，都写在这里。
 * 分成四块：数据结构、全局变量、调试日志宏、四个模块（book / ini / view / ui）的接口。
 *
 * 模块划分：
 *   book.cpp  打开 txt、识别编码、整理正文、建段落与章节索引、全文搜索
 *   ini.cpp   读写 ini（窗口位置 / 字号 / 主题 / 每本书的阅读进度 / 最近打开）
 *   view.cpp  自绘阅读视图：排版、绘制、滚动、选中
 *   ui.cpp    主窗口：菜单、目录、搜索条、状态栏、命令分发、自动下滑
 *
 * 文字编码的约定：程序内部**一律用 UTF-16**（std::wstring，Windows 原生），
 * 文件字节在 book.cpp 里解码后就不再是 char 了，避免各处再做编码转换。
 */
#ifndef NR_COMMON_H
#define NR_COMMON_H

/* 只引入需要的 Windows 头，缩短编译时间；NOMINMAX 防止 windows.h 的
 * min/max 宏和 <algorithm> / std::min 打架 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#include "ids.h"

/* 自定义消息：阅读视图滚动 / 换书 / 状态变化时，向主窗口投递，让它刷新
 * 状态栏、同步目录高亮、重置"保存进度"的防抖定时器。
 * 用 WM_APP+1 而不是 WM_USER，避免和控件类内部的消息号冲突。 */
#define WM_APP_STATUS     (WM_APP + 1)

/* =====================================================================
 * 数据结构
 * ===================================================================== */

/* 一个章节。章节不是单独存文本，只记标题在全文里的位置，
 * 正文靠段落索引去取，这样一整本书只有一份文本，内存省一半。 */
struct Chapter {
    int para;   /* 章标题所在的段落序号（从 0 开始） */
    int off;    /* 标题文本在 Book::text 中的字符偏移 */
    int len;    /* 标题文本的字符数（不含换行） */
};

/* 一本已打开的书。
 * 关键设计：全文只保存**一份** text，段落和章节都只存偏移量/下标。
 *   段落 i 的文本 = text[paraOff[i] .. paraOff[i+1])
 * 因为段落数可能上万，用 uint32_t 存偏移（最大支持 4G 字符，够用）。 */
struct Book {
    std::wstring path;                 /* 完整路径（用于 ini 里定位"这本书"） */
    std::wstring title;                /* 书名：取文件名去掉扩展名，用于标题栏/状态栏 */
    std::wstring text;                 /* 规范化后的全文：段落之间用单个 '\n' 分隔，
                                        * 内部不会出现 '\r'，也不会有连续空行 */
    std::vector<uint32_t> paraOff;     /* 段落起点表，大小 = 段数 + 1。
                                        * 末尾多存一个"全文长度"作哨兵，
                                        * 这样最后一段的长度也能用 paraOff[i+1]-paraOff[i] 算出来，
                                        * 不用在循环里到处写特判 */
    std::vector<Chapter> chapters;     /* 识别出的章节，按出现顺序排列，供二分查找 */

    /* 段落数（= 起点表大小 - 1，因为末尾是哨兵） */
    int ParaCount() const { return paraOff.empty() ? 0 : (int)paraOff.size() - 1; }

    /* 这本书是否可用：要有段落、有正文。界面各处都先用它判断，
     * 避免"空书"状态下到处空指针/越界 */
    bool Ok() const { return ParaCount() > 0 && !text.empty(); }
};

/* 用户设置（存在 ini 的 [settings] 段，每本书的位置单独存）。
 * 都是小整数 + 窗口矩形，写得直接，读的时候缺省值就放在成员初始值里。 */
struct Settings {
    int  fontPt  = 17;      /* 阅读字号（磅），界面里限制 10~48 */
    int  theme   = 0;       /* 配色：0 日间（米色纸） 1 夜间（深色） 2 自定义（用下面的 c* 颜色） */
    int  sidebar = 1;       /* 是否显示左侧目录面板 */
    int  spacing = 1;       /* 行距档位：0 紧凑 1 标准 2 宽松（对应 view.cpp 的 kSpacing[]） */
    int  autoSpeed = 4;     /* 自动下滑速度，1（最慢，约 3 秒一行）~ 10（最快，约 0.3 秒一行） */
    std::wstring lastFile;  /* 上次打开的书，启动时无参数就自动打开它 */
    RECT winRect{};         /* 上次的窗口位置和大小 */
    bool hasRect = false;   /* winRect 是否有效（第一次运行没有记录，就用默认尺寸） */

    /* ---- 外观：字体 ---- */
    /* 正文字体名（如 "Microsoft YaHei" / "思源宋体 CN"）。空串表示用内置默认字体。
     * 名字来自系统字体对话框，或用户导入的字体文件里真实的 family 名 —— 名字写错
     * 时 CreateFontIndirectW 会自动回退到系统默认字体，不会崩，只是显示不如预期。 */
    std::wstring fontFace = L"Microsoft YaHei";
    /* 用户导入的字体文件列表（.ttf/.otf 绝对路径），多个之间用 '|' 分隔。
     * 存路径而不是把字体嵌进 exe：字体动辄几 MB，写进 ini 才能"导入一次，以后都在"。
     * 启动时由 view.cpp 的 FontsLoadImported() 逐个 AddFontResourceExW 私有加载，
     * 私有加载只对本进程可见，不会污染系统的"已安装字体"列表。 */
    std::wstring fontFiles;

    /* ---- 外观：自定义配色（只有 theme == 2 时生效） ---- */
    /* 注意 ini 里存的是 COLORREF 的整数（0x00BBGGRR），读写时转十进制，不存 "#RRGGBB"
     * 字符串 —— 整数解析更短、更不容易因为手改 ini 写错颜色而解析失败。 */
    COLORREF cBg    = RGB(0xF7, 0xF3, 0xE9);  /* 正文背景 */
    COLORREF cFg    = RGB(0x31, 0x2D, 0x27);  /* 正文文字 */
    COLORREF cTitle = RGB(0x8A, 0x5A, 0x2B);  /* 章节标题文字 */
    COLORREF cSel   = RGB(0xBF, 0xDC, 0xF4);  /* 选中文字的背景（正文色会按对比度自动反转） */
    COLORREF cHov   = RGB(0xEC, 0xE6, 0xD8);  /* 悬停/次级底色：首页书架悬停行、折叠条 */
};

/* 一套配色。日间/夜间各一份，见 view.cpp 的 kDay / kNight。 */
struct Theme {
    COLORREF bg;      /* 正文背景 */
    COLORREF fg;      /* 正文文字 */
    COLORREF title;   /* 章节标题文字（日间是褐色，夜间是暖黄） */
    COLORREF selBg;   /* 选中文字的背景 */
    COLORREF selFg;   /* 选中文字的前景色 */
    COLORREF dim;     /* 次要文字：欢迎页提示、状态栏补充信息 */
    COLORREF hov;     /* 悬停 / 次级面板底色：首页书架的悬停行、侧栏折叠条都用它，
                       * 比 selBg 淡，不抢正文的注意力 */
};

/* 一处搜索命中。搜索结果是"偏移 + 长度"，不复制文本，几千条也不占内存。 */
struct Hit {
    int off;        /* 命中在全文中的字符偏移 */
    int len;        /* 关键词长度（用于跳转后高亮那一段） */
    int chapter;    /* 命中所属章节索引，-1 表示这段不在任何章节内（开篇文字） */
};

/* 书架上的一本书（首页"我读过哪些书"的条目）。
 *
 * 为什么这些字段要存进 ini 而不是临时算：书架在**没打开任何书**的时候显示，
 * 那时内存里没有全文，想算"读到第几章、百分之多少"就必须把每本书都加载一遍——
 * 十本书几十 MB，首页会卡住好几秒。所以保存进度时顺手把章节号/章数/百分比也
 * 写进 ini，首页只读 ini 就能画出来（见 ini.cpp 的 PosSaveMeta / ShelfRefresh）。 */
struct ShelfItem {
    std::wstring path;     /* 完整路径（点条目时用它打开） */
    std::wstring title;    /* 书名（保存时存的文件名去扩展名；旧记录没有就用文件名兜底） */
    std::wstring chapter;  /* 上次读到的章名，例如 "第3章 三年之约"；可能为空 */
    int  ch  = 0;          /* 那是第几章（1 开始；0 表示还没进章节） */
    int  chn = 0;          /* 全书共几章（0 表示这本书没有章节） */
    int  pct = 0;          /* 阅读百分比 ×10，即 0~1000 表示 0.0%~100.0% */
    bool missing = false;  /* 文件已经不在了（被删/改名/挪走），首页上灰掉不给点 */
};

/* =====================================================================
 * 全局变量（都定义在 ui.cpp / view.cpp 里，这里只作声明）
 * ===================================================================== */
extern Book     g_book;    /* 当前打开的书（整个程序只有一本） */
extern Settings g_set;     /* 用户设置（ini.cpp 里定义） */
extern int      g_dpi;     /* 系统 DPI，96 为 100% 缩放；所有像素都要过 S() 换算 */
extern HWND     g_hMain;   /* 主窗口句柄，给弹对话框/取父窗口用 */

/* DPI 缩放：设计尺寸 v（按 96 DPI 写死）→ 实际像素。
 * 界面上所有边距、宽度、高度都要套这一层，否则高分屏上会挤成一团。 */
inline int S(int v) { return MulDiv(v, g_dpi, 96); }

/* 调试日志：只对定义 NR_TRACE 的调试版（build_dbg.bat）生效，
 * 正式版里宏展开成空语句，连字符串都不会进二进制。用法：
 *     NrTrace(L"打开文件 %s，用时 %d ms", path.c_str(), ms); */
#ifdef NR_TRACE
void NrTrace(const wchar_t* fmt, ...);
#else
#define NrTrace(...) ((void)0)
#endif

/* 当前配色（日间/夜间），实现见 view.cpp */
const Theme& CurTheme();

/* =====================================================================
 * 书籍处理 (book.cpp)
 * ===================================================================== */

/* 读文件 + 识别编码 + 规范化 + 建段落与章节索引。
 * path 是绝对路径；失败时把中文原因写进 *err（可为 nullptr），返回 false。
 * 成功时 b 被清空重填，界面其他状态需要调用方自己重置（见 UiOpenFile）。 */
bool BookLoad(Book& b, const std::wstring& path, std::wstring* err);

/* 字符偏移 → 段落序号（二分）。返回最后一段的序号，越界偏移会被夹住，不会返回 -1。 */
int  BookCharToPara(const Book& b, int off);

/* 段落序号 → 它属于哪一章。返回章节下标，段落不在任何章节内时返回 -1。 */
int  BookChapterAtPara(const Book& b, int para);

/* 这一段是不是章节标题行（章节索引就是靠它建起来的，正文排版时也用它决定是否加粗） */
bool BookIsChapterPara(const Book& b, int para);

/* 取章节标题文本（已去掉首尾空白的原样标题），chIdx 越界返回空串 */
std::wstring BookChapterTitle(const Book& b, int chIdx);

/* 取第 para 段的正文（不含换行符）；越界返回空串 */
std::wstring BookParaText(const Book& b, int para);

/* 网页抓取用的两个桥函数（实现都在 book.cpp，见那里的注释）。
 * BytesToText：任意字节流按与打开文件完全相同的规则解码成宽字符；
 * BookLooksLikeChapterTitle：这一行是否算章节标题（与分章逻辑同一判定）。 */
std::wstring BytesToText(const std::string& s);
bool BookLooksLikeChapterTitle(const std::wstring& line);

/* 从 from 处向后找第一处 needle，返回命中的字符偏移，找不到返回 -1；
 * *hitLen 返回命中长度（调用方用它做高亮）。ASCII 字母大小写不敏感。
 * 自动下滑/查找下一个都是靠它逐个跳。 */
int  BookFindNext(const Book& b, const std::wstring& needle, int from, int* hitLen);

/* 全文搜索，命中追加到 out（不清空，调用方可先自行 clear）；
 * cap 是最多返回多少条（界面用 4000，防止关键词太常见时列表卡死）。 */
void BookSearch(const Book& b, const std::wstring& needle, std::vector<Hit>& out, int cap);

/* =====================================================================
 * 配置 (ini.cpp)
 * ===================================================================== */

/* 程序启动时调用一次：决定 ini 放哪儿（exe 同目录优先，不可写则退回
 * %APPDATA%\NovelReader\），文件不存在时创建并写入 UTF-16 BOM。
 * 必须在任何 Ini* 调用之前执行。 */
void IniInit();

/* 当前 ini 的完整路径（"关于"对话框里展示给用户看） */
std::wstring IniFilePath();

std::wstring IniGetStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def);
void IniSetStr(const wchar_t* sec, const wchar_t* key, const std::wstring& val);
int  IniGetInt(const wchar_t* sec, const wchar_t* key, int def);
void IniSetInt(const wchar_t* sec, const wchar_t* key, int v);

/* 读 / 写 [settings] 段。Read 在启动时调一次，Save 在关窗口时调一次。 */
void SettingsLoad();
void SettingsSave();

/* 一本书 → ini 段名："f" + 路径小写后的 FNV-1a 哈希（8 位十六进制），
 * 例如 [fd9ade76a]。用哈希而不是路径本身，是为了不把中文长路径塞进段名。 */
std::wstring PosSectionFor(const std::wstring& path);

/* 读取这本书上次读到的字符偏移；没记录时返回 0 */
int  PosLoad(const std::wstring& path);

/* 保存阅读进度：字符偏移 + 章节号/章数/百分比/章名/书名。
 *
 * 比只存 off 多存那几项，纯粹是为了首页书架（见 ShelfItem 的注释）：
 * 首页不必加载全文就能显示"第 3/120 章 3.2%"。这些键是**附加信息**，
 * 老的 ini 里没有它们也能正常读（缺省值 0）。
 * 滚动后 2.5 秒防抖调一次，关窗口/切书时再调一次。 */
void PosSaveMeta(const std::wstring& path, int off, const std::wstring& chname,
                 int ch, int chn, int pct);

/* 重新扫一遍 ini，重建首页书架列表（最近打开的书 + 各自进度）。
 * 调用时机：启动、打开一本书之后、关掉当前书回到首页之前。
 * 结果用 ShelfItems() 取，返回的是内部常引用，别再拷贝。 */
void ShelfRefresh();

/* 首页书架的只读视图（ini.cpp 内部维护，ShelfRefresh 之后有效） */
const std::vector<ShelfItem>& ShelfItems();

/* =====================================================================
 * 阅读视图 (view.cpp)
 * ===================================================================== */

/* 创建自绘视图子窗口（类名 NRView），返回句柄 */
HWND ViewCreate(HWND parent);

/* 换书后调用：清空排版缓存、把锚点归零、滚回开头 */
void ViewSetBook();

/* 跳到某个字符偏移，并把接下来 selLen 个字符高亮显示（搜索跳转用）。
 * 自动下滑会被停掉。 */
void ViewGotoOffset(int off, int selLen);

/* 跳到某一段的段首 */
void ViewGotoPara(int para);

/* 按章跳转：dir = -1 上一章，+1 下一章；没有章节时不做任何事 */
void ViewGotoChapter(int dir);

/* 相对当前锚点滚动 n 个视觉行（正数向下），并修正越界 */
void ViewScrollLines(int n);

/* 翻页：n = +1 下一页，-1 上一页（一页 = 视图内可见的行数） */
void ViewScrollPages(int n);

/* 当前顶行的字符偏移（阅读进度、状态栏百分比都用它） */
int  ViewAnchorOffset();

/* 当前顶行的段落序号（用来查"现在是第几章"） */
int  ViewCurrentPara();

/* 字号 / 行距 / 窗口宽度 / 主题变化后调用。
 * layoutDirty = true 表示换行可能变了（字号、宽度、行距），要作废所有段落行数缓存；
 * 只是换主题的话传 false，避免几万段白算一遍。 */
void ViewMetricsChanged(bool layoutDirty);

/* 请求重绘（异步，安全） */
void ViewInvalidate();

/* 复制当前选中的文字到剪贴板（没选中时什么都不做） */
void ViewCopySelection();

/* 全选正文 */
void ViewSelectAll();

/* 把焦点放到正文视图上（打开书、搜索跳转之后调，保证键盘能直接翻页） */
void ViewFocus();

/* 当前字号对应的像素高度（标题行高由它推算） */
int  ViewFontPixel();

/* ============ PDF「原始版式」模式（view.cpp 实现，仅对 PDF 有效） ============
 * 常规模式是把 PDF 抽出的文字按当前字号重排（能搜索、能换字体、能配色）；
 * 「原始版式」模式则用系统 Windows.Data.Pdf 把整页渲染成位图直接显示，
 * 版式与原 PDF 完全一致（图片、表格、字体效果都在），代价是这一屏搜不了字。
 * 两种模式共用同一份文字，所以阅读位置、章节、书签都连续。 */

/* 切换原始版式。on = true 切过去，返回 false 表示切不过去（不是 PDF 或渲染失败）。 */
bool ViewSetPdfMode(bool on);

/* 当前是否处于原始版式模式 */
bool ViewIsPdfMode();

/* 当前书的 PDF 总页数（不在原始版式模式时为 0） */
int  ViewPdfPageCount();

/* 当前显示的是第几页（0 基） */
int  ViewPdfPage();

/* 字符偏移换算成页号（搜索命中后要跳到对应页时用） */
int  ViewPdfPageFromOffset(int off);

/* 翻页：delta = +1 下一页，-1 上一页；会把阅读锚点同步到该页开头 */
void ViewPdfFlip(int delta);

/* 页内纵向滚动（整页比窗口高时用），dy 为像素，正数向下 */
void ViewPdfScroll(int dy);

/* ============ 主界面里与 PDF 版式相关的入口 ============ */
/* 菜单 / F9：在「PDF 原始版式」与「文字重排」之间切换，附带提示与状态栏刷新 */
void UiTogglePdfMode();

/* =====================================================================
 * 主界面 (ui.cpp)
 * ===================================================================== */

/* 打开一本书：加载 → 恢复上次位置 → 刷新标题/目录/状态栏 → 记入最近打开 */
void UiOpenFile(const std::wstring& path);

/* 关掉当前书，回到首页书架（"文件 → 关闭当前书"）。
 * 会先把进度落盘、重建书架列表，再把标题栏/目录/状态栏清干净。 */
void UiCloseFile();

/* 打开网页小说：弹网址输入框 → 抓整本 → 存成缓存 txt → 打开（实现见 ui.cpp）。 */
void UiOpenWebNovel();

/* 书源搜索：内置无忧书城（51shucheng）。弹窗里输入书名 → 选结果 → 下载整本
 * → 存成缓存 txt → 打开。比"打开网页小说"省掉"自己找目录页网址"这一步。 */
void UiSourceSearch();

/* 手机阅读：启动局域网服务并弹出地址提示窗（已经启动就只弹地址）*/
void UiShowPhone();

/* 跳到手机端最近保存的阅读位置（控制键 Ctrl+G）*/
void UiJumpPhoneProgress();

/* 把当前的阅读进度写进 ini（偏移 + 章节号 + 章数 + 百分比 + 章名 + 书名）。
 * 界面里所有"该存盘了"的地方都调它，省得每个调用点自己拼一遍参数。 */
void UiSaveProgress();

/* 刷新底部三段状态栏：书名(+补充) ｜ 当前章名 ｜ 百分比 · 第几/共几章 */
void UiUpdateStatus();

/* 让目录列表的选中项跟随当前阅读位置（不触发跳转） */
void UiSyncToc();

/* 显示 / 隐藏搜索条（隐藏时连搜索结果面板一起关掉） */
void UiShowSearchBar(bool show);

/* 执行全文搜索：从当前阅读位置往后找第一处命中并跳过去，结果填进左侧列表 */
void UiRunSearch(const std::wstring& kw);

/* 跳到下一个搜索结果（到末尾就绕回第一条） */
void UiFindNext();

void UiToggleSidebar();
void UiSetFontDelta(int d);      /* 字号增减 d 磅（会夹到 10~48 并重排版） */
void UiCycleSpacing();           /* 行距在 紧凑/标准/宽松 之间循环 */
void UiToggleTheme();            /* 日间 / 夜间切换 */
void UiRebuildRecentMenu();      /* 按 ini 里的记录重建"最近打开"子菜单 */

std::wstring RecentGet(int idx);                /* ini.cpp：取第 idx 条最近打开（0 最新） */
void RecentAdd(const std::wstring& path);       /* ini.cpp：把一本书插到最近打开列表头部 */

/* 跳转到第 idx 条搜索结果并高亮（列表点击 / F3 都走这里） */
void UiJumpHit(int idx);

/* 自动下滑（ui.cpp）：开启/关闭、加速、减速、遇到用户手动滚动时停止。
 * 节奏由 AutoIntervalMs() = 3000 / autoSpeed 决定（1 级 3 秒一行，10 级 0.3 秒一行）。 */
void UiAutoToggle();
void UiAutoFaster();
void UiAutoSlower();
void UiAutoStop();
void UiAutoApplyTimer();   /* 速度变了：按新的间隔重设定时器 */
bool UiAutoRunning();

/* =====================================================================
 * 书签 / 外观（ui.cpp + ini.cpp）
 * ===================================================================== */

/* 书签：每本书一份，元素是 "字符偏移|标题" 的字符串。
 * 为什么存在 ini 的独立段（段名 = "bm" + 该书的 f 哈希）而不是塞进进度段：
 * 进度段每 2.5 秒就可能被重写一次，而书签是低频、用户珍视的数据，
 * 混在一起重写时更容易因为某次意外而整段丢掉。分开存，互不影响。 */
std::vector<std::wstring> BookmarkLoad(const std::wstring& path);
void BookmarkSave(const std::wstring& path, const std::vector<std::wstring>& items);

/* 带书签的段落序号（升序），供 view.cpp 在正文左侧画一个小旗标记。
 * 由 ui.cpp 在打开书、增删书签时重建。 */
extern std::vector<int> g_bmParas;

void UiAddBookmark();      /* 给当前阅读位置加一条书签（已在同一段则更新标题） */
void UiShowBookmarks();    /* 打开书签面板（复用侧栏列表） */
void UiDelBookmark();      /* 删掉当前这一段上的书签 */

/* 外观：选字体 / 导入字体文件 / 自定义配色 / 套用预设方案。
 * 四者改完都会立刻重排重绘并把设置写回 ini。 */
void UiPickFont();
void UiImportFont();
void UiPickColors();
void UiApplyPreset(int idx);

/* 载入用户导入的字体文件（启动时调一次，私有加载，不装进系统） */
void FontsLoadImported();

#endif /* NR_COMMON_H */
