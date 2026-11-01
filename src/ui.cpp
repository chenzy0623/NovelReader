/* ui.cpp - 主窗口、菜单、目录/搜索面板、状态栏 */
/* =====================================================================
 *  ui.cpp —— 主窗口、菜单、整体交互与全局装配
 *
 *  【模块职责】
 *  本文件只做「界面骨架 + 用户操作」，不解析文本、不负责排版绘制：
 *    book.cpp  解码 / 规范化 / 建段落与章节索引 / 全文搜索（UiRunSearch 调它）
 *    view.cpp  自绘阅读视图：排版、绘制、滚动、选中（所有跳转都调它的接口）
 *    ini.cpp   ini 读写：窗口矩形、字号、主题、每本书的阅读位置
 *    ui.cpp    ← 本文件：建窗口、摆控件、分发命令、维护状态栏与定时器
 *  可以把它理解成「决策层」：决定什么时候该做什么，脏活全部转手给别的模块。
 *
 *  【窗口与控件】
 *  一个主窗口（窗口类名 NRMain，窗口过程 MainProc）＋ 一批子窗口。没有用
 *  对话框模板，也没有用布局器：子窗口全由 UiCreateChildren 创建、由 UiLayout 摆放。
 *    顶部  搜索条：[输入框 s_hFind] [搜索] [下一个] [关闭]  ← 仅 s_searchShown 时可见
 *    左侧  目录列表 s_hToc 或 搜索结果列表 s_hHits  ← 同一块区域，由 s_panel 二选一
 *    中间  阅读视图 s_hView（类名 NRView，属于 view.cpp，不是本文件创建的）
 *    底部  状态栏 s_hStatus 三段：书名(+补充) ｜ 当前章名 ｜ x.y% · n/N 章
 *  侧栏宽度：目录 232、搜索结果 320，都是 96 DPI 下的设计值，使用时经 S() 缩放。
 *
 *  【消息流】看懂这四条线，整个文件就没有秘密了
 *  1) 用户操作 → 命令 → 干活：菜单 / 加速键 / 按钮 / 列表点击全部汇成 WM_COMMAND，
 *     由 OnCommand() 按 id 分派到 UiOpenFile、UiJumpHit、UiAutoToggle 等函数。
 *  2) 视图变化 → 界面刷新（反向通知）：view.cpp 滚动、换书、跳转之后向主窗口
 *     PostMessage(WM_APP_STATUS)，MainProc 收到后刷新状态栏、同步目录高亮、
 *     重置「保存进度」的防抖定时器。用消息而不是函数直调，一是让绘制路径里
 *     不出现落盘这类慢操作，二是避免 ui 与 view 互相 #include 对方的内部状态。
 *  3) 两个定时器：TIMER_POS 保存阅读进度、TIMER_AUTO 驱动自动下滑。
 *  4) 退出：WM_CLOSE → SaveAll()（位置 + 窗口矩形 + ini）→ DestroyWindow →
 *     WM_DESTROY → PostQuitMessage → 消息循环退出，wWinMain 返回。
 *
 *  【注意事项】
 *  - 所有像素尺寸都要过 S() 换算（定义见 common.h）：232 / 320 / 38 / 26 这些
 *    数字都是 96 DPI 下的设计值，高分屏上不缩放就会挤成一团。
 *  - 任何可能「还没打开书」的入口都先判 g_book.Ok()，否则会越界或空指针。
 *  - 两个列表的选中项被我们程序化修改时会回发 LBN_SELCHANGE，必须用 s_tocSync /
 *    s_hitSync 挡住，否则会形成「同步 → 跳转 → 再同步」的循环。
 *  - 子窗口句柄都是文件级 static，用前一律判空（启动早期的 UiLayout 就会撞上）。
 *  - 下文所说的「设计值」均指 96 DPI 数值；「偏移」均指 g_book.text 里的字符下标。
 * =====================================================================
 */
#include "common.h"
#include "web.h"
#include "server.h"
/* 1.7.0：局域网 P2P 阅读进度同步（电脑和手机各开监听、互相广播发现） */
#include "sync.h"
#include "pdf.h"
#include "source.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cwchar>
#include <algorithm>

/* ---- 全局量（common.h 里 extern 声明，此处定义）----
 * g_dpi ：启动时由 wWinMain 从屏幕 DC 取一次，之后只读；S() 换算全靠它。
 * g_hMain：主窗口句柄。弹 MessageBox、取菜单、给 view/ini 当父窗口都要用；
 *          WM_DESTROY 之后就不该再用它了。
 */
int g_dpi = 96;
HWND g_hMain = nullptr;

/* ---- 子窗口句柄 ----
 * 全部初始为 nullptr，由 UiCreateChildren 创建。四组：
 *   阅读视图 s_hView（来自 ViewCreate，本文件只负责摆位置）
 *   状态栏 s_hStatus、目录列表 s_hToc、搜索结果列表 s_hHits
 *   搜索条：输入框 s_hFind ＋ 三个按钮 s_bFind / s_bNext / s_bClose
 */
static HWND s_hView = nullptr, s_hStatus = nullptr, s_hToc = nullptr, s_hHits = nullptr;
static HWND s_hFind = nullptr, s_bFind = nullptr, s_bNext = nullptr, s_bClose = nullptr;
/* 侧栏折叠条：自绘的细长按钮（BS_OWNERDRAW），贴在侧栏底部；收起后变成左边缘的细竖条。
 * 它和菜单里的「显示/隐藏目录」是同一个功能的两个入口，都调 UiToggleSidebar。 */
static HWND s_hFold = nullptr;
/* 顶部工具栏（自绘窗口 NRToolbar）与它的悬停状态。放在这里而不是工具栏代码块里，
 * 是因为 UiCreateChildren 要用它，而 UiCreateChildren 定义在前面。
 * s_barHover = -1 表示鼠标不在任何按钮上；s_barTrack 表示是否已经 TrackMouseEvent。 */
static HWND s_hBar = nullptr;
static int  s_barHover = -1;
static bool s_barTrack = false;
/* 标题区最后一次绘制时的宽度：BarHitTest 要按同一宽度算按钮位置（鼠标消息里没有客户区宽度），
 * 用最近一次 WM_PAINT 的量代替再取一次 GetClientRect，省一次系统调用也避免两处不一致。 */
static int  s_lastBarW = 0;
/* 「最近打开」子菜单句柄：BuildMenu 建好后存下来，UiRebuildRecentMenu 只动它，不动整个菜单栏 */
static HMENU s_hRecentMenu = nullptr;
/* 整个应用菜单：**不挂在窗口上**（SetMenu 会带出一条系统灰菜单栏，和纸感界面不搭），
 * 靠左上角三条横线按钮 / 右键 / Ctrl+F10 弹出，命令分发仍然走 WM_COMMAND。 */
static HMENU s_hAppMenu = nullptr;
static HFONT s_uiFont = nullptr;
/* ---- 界面瞬时状态（都不进 ini，退出即丢）----
 * s_searchShown：顶部搜索条是否展开，影响 UiLayout 里正文顶部要让出多少高度。
 * s_panel：侧栏当前显示哪一页，0 = 目录、1 = 搜索结果。搜索结果条目更长，所以更宽。
 */
static bool s_searchShown = false;
static int  s_panel = 0;                 /* 0 目录, 1 搜索结果 */
/* 上一次用过的高亮颜色（0~3）。Ctrl+H 快速标记时沿用它，
 * 免得每标一句都要重新挑颜色。 */
static int  s_lastHlColor = 0;
/* 防递归用的「正在程序化同步」标志：我们调 LB_SETCURSEL 时列表会回发
 * LBN_SELCHANGE，若不挡住就会变成「同步目录 → 触发跳章 → 再同步」的死循环。
 * 用法固定为：置位 → 发消息 → 立刻复位，中间不调任何会抛消息的函数。
 */
static bool s_tocSync = false, s_hitSync = false;
/* ---- 搜索会话状态 ----
 * s_hits ：当前关键词的全部命中（最多 4000 条；Hit 只存偏移/长度/章号，不复制文本）。
 * s_kw   ：当前关键词，用来判断「同一个词再搜一次可以直接复用结果」而不重扫全书。
 * s_curHit：当前停在第几条命中，-1 表示还没跳过；UiFindNext 在它基础上 +1。
 * 换书时由 ClearSearchState 整体清空。
 */
static std::vector<Hit> s_hits;
static std::wstring s_kw;
static int  s_curHit = -1;
/* 状态栏真实高度：由 UiLayout 量出来缓存，因为状态栏带 SBARS_SIZEGRIP，不同 DPI 下高度不同 */
static int  s_sbH = 24;
/* 状态栏第一格后缀（如搜索后的「共 N 处」）；单独存一份，避免污染属于书本身的 g_book.title */
static std::wstring s_statusExtra;
/* 最近一次算出的阅读百分比（千分数）。状态栏右侧数字与底部进度条都用它；
 * 单独存一份是因为 UiSetStatus 也可能被别处（网页抓取进度）调用，
 * 那时不该把"书读到哪儿"的进度条冲掉。 */
static int  s_lastPct = 0;

/* ---- 定时器 id（SetTimer 的第二个参数，同一窗口内必须唯一）----
 * TIMER_POS ：阅读进度防抖保存（见 WM_APP_STATUS 与 WM_TIMER）。
 * TIMER_AUTO：自动下滑。
 */
#define TIMER_POS  1
#define TIMER_AUTO 2
/* 打开书后"等对面出现"的轮询定时器。刚开机、刚开同步那几秒手机上还没答话，
 * 一次性检查必然扑空，所以给一个短轮询窗口（最多约 12 秒）。 */
#define TIMER_SYNC 3

/* ---- 自动下滑状态 ---- */
/* s_autoOn / s_lastSave：自动下滑的权威状态与落盘时间戳
 * s_autoOn ：是否正在自动下滑，它是唯一权威状态，定时器只是它的执行者。
 * s_lastSave：上次把位置写进 ini 的时刻（GetTickCount，毫秒），用于实现
 *            「最长 15 秒必落盘」的兜底，防止长时间滚动把防抖一直重置导致一次都没存上。
 */
static bool  s_autoOn = false;      /* 是否正在自动下滑 */
static DWORD s_lastSave = 0;        /* 上次落盘位置的时间（自动下滑时保证也能存盘） */

/* 前置声明：这几个函数互相引用且定义在文件后半段，先把原型摊在这里，免得来回调整定义顺序 */
static void UiLayout();
static void FillToc();
static void FillHits();
static void AutoSyncMenu();
/* 侧栏切页：书签面板（UiShowBookmarks）的代码放在 ShowPanel 定义之前，
 * 所以这里先声明一下；s_panel 的取值 0 目录 / 1 搜索命中 / 2 书签。 */
static void ShowPanel(int which);
static void SyncCheckOnOpen();          /* 打开书后对账进度 */
static void SyncTryPending();           /* 等对面出现后再对账（见 TIMER_SYNC） */
static bool s_syncPending = false;      /* 有"待对账"没做完 */
static int  s_syncTries = 0;            /* 已经轮询了几次 */

static void SyncPushCurrent();           /* 把本机进度推给对端 */
static void UiShowSyncStatus();          /* 同步状态对话框 */
/* 1.6.0：下面三个都在文件后半段定义，但导出笔记、快捷键对话框在前面就要用，
 * 所以原型提前（C++ 不允许调用未声明的函数）。 */
static std::wstring ClipToWidth(HDC dc, const std::wstring& s, int maxW);
static bool WriteTextFileUtf8Bom(const std::wstring& path, const std::wstring& text);
static HACCEL BuildAccel();

/* ---------------- 字体 ---------------- */

/* 造一个界面字体。参数 pt 是字号（磅），bold 决定是否加粗；返回 HFONT。
 * 为什么 lfHeight 写成 -MulDiv(pt, g_dpi, 72)：LOGFONT 要的是像素高，
 * 负值表示「字符高度」而非「行高」，这样 96 / 120 / 144 DPI 下字号才一致。
 * 字体优先「微软雅黑 UI」，老系统没有这个字体族时退到 Tahoma —— 宁可字形差
 * 一点，也不要出现方框豆腐字。失败返回 nullptr，调用方需判空。
 * 本文件只用这一种字体给所有子窗口；正文的字体由 view.cpp 按字号另建，互不影响。
 */
static HFONT MakeUiFont(int pt, bool bold)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) {
        lstrcpynW(lf.lfFaceName, L"Tahoma", LF_FACESIZE);
        f = CreateFontIndirectW(&lf);
    }
    return f;
}

/* ---------------- 子窗口 ---------------- */

/* 统一创建一个子窗口，并顺手把界面字体设上去。
 * cls / style / exStyle / id / text 直接对应 CreateWindowExW 的各个参数；
 * 函数内部会自动补 WS_CHILD，text 传 nullptr 当空串处理。
 * id 同时充当 WM_COMMAND 的分派依据和 GetDlgItem 的查找键，必须唯一。
 * 位置尺寸先给 10×10 占位，真正的摆放全部交给 UiLayout —— 这里绝不写布局，
 * 否则 DPI 或窗口尺寸一变就对不上。返回句柄，失败返回 nullptr。
 * 注意 (HMENU)(INT_PTR)id 是 Win32 的老约定：子窗口「菜单句柄」位置放的是控件 id。
 */
static HWND MakeChild(const wchar_t* cls, DWORD style, DWORD exStyle, int id, const wchar_t* text)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE);
    HWND h = CreateWindowExW(exStyle, cls, text ? text : L"", WS_CHILD | style,
                             0, 0, 10, 10, g_hMain, (HMENU)(INT_PTR)id, inst, nullptr);
    if (h && s_uiFont) SendMessageW(h, WM_SETFONT, (WPARAM)s_uiFont, TRUE);
    return h;
}

/* 搜索输入框的子类化过程（用 SetWindowSubclass 挂在 s_hFind 上）。
 * 目的是让输入框自己接管三个键，省得主窗口去猜「焦点现在在谁身上」：
 *   Enter  → 取框里的文本开始搜索（空文本什么都不做）
 *   Esc    → 收起搜索条，焦点回正文
 *   F3     → 下一个结果（与主窗口的 F3 加速键等价，只是这里路径更短）
 * 返回 0 表示已处理；其余一律交给 DefSubclassProc 走编辑框默认逻辑。
 * 注意必须用 DefSubclassProc 而不是 DefWindowProcW，否则会漏掉子类链上
 * 其他处理者，输入法和右键菜单都会出问题。
 */
static LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN) {
            wchar_t buf[512];
            buf[0] = 0;
            SendMessageW(h, WM_GETTEXT, 512, (LPARAM)buf);
            if (buf[0]) UiRunSearch(buf);
            return 0;
        }
        if (w == VK_ESCAPE) { UiShowSearchBar(false); return 0; }
        if (w == VK_F3) { UiFindNext(); return 0; }
    }
    return DefSubclassProc(h, m, w, l);
}

/* 前置声明：这两个窗口过程定义在"顶部工具栏 / 自绘状态栏"那两段里（位于
 * UiCreateChildren 之后），而注册窗口类的函数在 UiCreateChildren 之前就要用到它们的地址。 */
static LRESULT CALLBACK ToolbarProc(HWND h, UINT m, WPARAM w, LPARAM l);
static LRESULT CALLBACK StatusProc(HWND h, UINT m, WPARAM w, LPARAM l);

/* 注册"自绘框架"窗口类：工具栏（NRToolbar）与状态栏（NRStatus）。
 *
 * 之所以要自己注册窗口类，是因为这两个区域需要完全自绘（配色跟着主题走、
 * 圆角、进度条），而系统 STATUSCLASSNAME 之类的控件只能吃系统配色。
 * 两个类共用同一个窗口过程 ToolbarProc / StatusProc，样式都是 CS_HREDRAW|CS_VREDRAW
 * （改尺寸时整块重画，避免右/下边留残影），光标用箭头（不注册 IDC_ARROW 时
 * 系统会给一个很丑的默认光标）。
 * 注册失败（比如类已存在）不当成错误：主窗口还没建，影响不到别的逻辑。 */
static void UiRegisterChromeClasses(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;          /* 全部自绘，不要系统刷底 */
    wc.lpfnWndProc = ToolbarProc;
    wc.lpszClassName = L"NRToolbar";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = StatusProc;
    wc.lpszClassName = L"NRStatus";
    RegisterClassExW(&wc);
}

/* 一次性创建所有子窗口（在 CreateWindowEx 建好主窗口之后、UiLayout 之前调用）。
 * 参数 inst 其实用不上（MakeChild 自己从 g_hMain 取实例句柄），保留是为了
 * 与其他初始化函数签名一致，故用 (void)inst 消掉未使用警告。
 * 顺序有讲究：① 先造字体，MakeChild 才能立刻 WM_SETFONT；② 再建 view
 * （它不用界面字体，自带一套）；③ 然后状态栏、两个列表、搜索条的编辑框与按钮。
 * 可见性：状态栏和目录一开始就 WS_VISIBLE；s_hHits 与搜索条故意不带
 * WS_VISIBLE，等真正要用时由 UiLayout / UiShowSearchBar 显出来，避免启动时闪一下。
 * 最后两行：给输入框挂子类（接管 Enter/Esc/F3）；让主窗口接受拖进来的文件。
 */
static void UiCreateChildren(HINSTANCE inst)
{
    (void)inst;
    s_uiFont = MakeUiFont(9, false);
    UiRegisterChromeClasses((HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE));

    /* 正文视图由 view.cpp 创建（它自己注册窗口类 NRView、自绘、处理滚轮键盘选中） */
    s_hView = ViewCreate(g_hMain);

    /* 顶部工具栏：自绘窗口（窗口类 NRToolbar，由 UiRegisterChromeClasses 注册）。
     * 它不属于"系统控件"，所以没有边框、没有主题配色问题，画成什么样完全由 DrawToolbar 决定。 */
    s_hBar = MakeChild(L"NRToolbar", WS_VISIBLE | WS_CLIPSIBLINGS, 0, IDC_BAR, nullptr);

    /* 底部状态栏：同样是自绘（窗口类 NRStatus），三段文本 + 通条进度见 DrawStatusBar。
     * 不用系统 STATUSCLASSNAMEW 的原因：它只接受系统配色，夜间模式下会露出一条亮灰，
     * 而且没法在末尾画进度条。 */
    s_hStatus = MakeChild(L"NRStatus", WS_VISIBLE | WS_CLIPSIBLINGS, 0, IDC_STATUS, nullptr);

    /* 目录列表：LBS_NOTIFY 让选中变化能通知父窗口（WM_COMMAND + LBN_SELCHANGE）；
     * LBS_NOINTEGRALHEIGHT 允许高度不是行高的整数倍，避免最后一行被裁掉；
     * LBS_OWNERDRAWFIXED + LBS_HASSTRINGS 表示"条目文字还是由列表自己存，
     * 但每行的样子由我们在 WM_DRAWITEM 里画"（见 DrawListRow），
     * 这样才能做出圆角选中条、当前章左侧强调竖条这些效果。
     * 去掉 WS_EX_CLIENTEDGE：自绘边框已经在 WM_DRAWITEM/主窗口背景里处理了，
     * 再套一层系统凹陷边框会把圆角切开。
     */
    s_hToc = MakeChild(L"LISTBOX", WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                       LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                       0, IDC_TOC, nullptr);
    /* 搜索结果列表：与目录同一块区域、同样样式，初始不带 WS_VISIBLE，切换靠 ShowWindow */
    s_hHits = MakeChild(L"LISTBOX", WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                        0, IDC_HITS, nullptr);
    /* 搜索条：ES_AUTOHSCROLL 让超长关键词横向滚动，不加 ES_MULTILINE 是因为
     * 搜索词不会有换行；三个按钮都带 WS_TABSTOP，保证 Tab 键能在它们之间走。
     */
    /* 搜索框不要 WS_EX_CLIENTEDGE（系统立体边框跟纸面不搭），改由 WM_CTLCOLOREDIT 给底色 */
    s_hFind = MakeChild(L"EDIT", ES_AUTOHSCROLL | WS_TABSTOP, 0, IDC_FINDEDIT, L"");
    /* 三个按钮都自绘（BS_OWNERDRAW），样子在 DrawMiniButton 里定义 */
    s_bFind = MakeChild(L"BUTTON", BS_OWNERDRAW | WS_TABSTOP, 0, IDC_BFIND, L"搜索");
    s_bNext = MakeChild(L"BUTTON", BS_OWNERDRAW | WS_TABSTOP, 0, IDC_BNEXT, L"下一个");
    s_bClose = MakeChild(L"BUTTON", BS_OWNERDRAW | WS_TABSTOP, 0, IDC_BCLOSE, L"关闭");

    /* 侧栏折叠条：BS_OWNERDRAW 表示"长什么样由我们自己在 WM_DRAWITEM 里画"（见 DrawFoldBar）。
     * 不用普通按钮是因为它要贴成一条细手柄，普通按钮的 3D 边框在这里很笨重。
     * 文字留空 —— 自绘按钮的文本只有辅助工具能读到，靠 DrawFoldBar 现画箭头。 */
    s_hFold = MakeChild(L"BUTTON", BS_OWNERDRAW | WS_TABSTOP, 0, IDC_FOLD, L"");

    SetWindowSubclass(s_hFind, EditSub, 1, 0);
    /* 注册接受拖放文件：必须在主窗口上调用，WM_DROPFILES 才会送过来（见 MainProc） */
    DragAcceptFiles(g_hMain, TRUE);
}

/* 画侧栏折叠条 —— 自绘按钮 IDC_FOLD 的全部外观（由 MainProc 的 WM_DRAWITEM 调用）。
 * 参数 dc：按钮的绘制 DC（WM_DRAWITEM 给的 hDC）；r：按钮矩形（rcItem）；
 * 参数 pressed：是否处于按下状态（ODS_SELECTED），按下时文字压暗一点作为反馈。
 * 两种形态，靠矩形形状区分而不是靠标志位 —— 宽度小于阈值就当竖条：
 *   展开态：侧栏底部的横条，文字「‹ 收起目录」，告诉用户点它会收起来；
 *   收起态：左边缘的细竖条，只画一个「›」，点它重新展开。
 * 配色跟着主题走（卡片色底 + 分隔线 + 次要文字色）：新版界面里侧栏本身已经是
 * 主题色的自绘列表，折叠条再刷系统灰就会露出一条突兀的浅色带。
 * 边界：r 宽或高 <= 0 时直接返回（缩到极小的窗口里会出现），否则 FillRect 无意义、
 * 文字也没有地方放。
 */
static void DrawFoldBar(HDC dc, const RECT& r, bool pressed)
{
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return;

    /* 1) 底色 + 分隔线：横条在顶边画线，竖条在右边画线，与相邻的列表分开 */
    const Theme& th = CurTheme();
    HBRUSH bg = CreateSolidBrush(th.card);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, th.line);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, r.left, r.top, nullptr);
    LineTo(dc, r.right, r.top);
    if (r.right - r.left < S(30)) {
        MoveToEx(dc, r.right - 1, r.top, nullptr);
        LineTo(dc, r.right - 1, r.bottom);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    /* 2) 图标与文字：横条居中显示"<  收起目录"，竖条画一个矢量箭头 */
    HFONT oldFont = (HFONT)SelectObject(dc, s_uiFont);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    /* 文字色：正常用正文色偏淡一档的感觉没有现成字段，这里直接用 dim（次要信息色）；
     * 按下时更暗一点 —— 和列表里"灰显一行"的观感一致，不引入新颜色。 */
    COLORREF oldColor = SetTextColor(dc, pressed ? th.track : th.dim);

    if (r.right - r.left >= S(30)) {
        /* 横条：居中，文字左右各留 6，放不下就截断（用 DT_END_ELLIPSIS 让它自己加省略号） */
        RECT tr = r;
        tr.left += S(6);
        tr.right -= S(6);
        /* 箭头用半角 "<" / ">" 而不是 U+2039 之类的印刷引号：字体里不一定有那个字形，
         * 缺字形时会画成方框或怪字符（踩过）。 */
        DrawTextW(dc, L"<  收起目录", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        /* 竖条：用两根线画一个 ">" 箭头，不用文字。
         * 系统界面字体在中英混排下的小字号箭头又细又飘（实测 S(18) 宽的竖条里
         * 那个 '>' 几乎看不见），矢量画出来的粗细、位置都可控。 */
        int cx = (r.left + r.right) / 2;
        int cy = r.top + S(14) + S(7);
        int arm = S(4);                 /* 箭头两臂的半径 */
        int thick = S(2) > 1 ? S(2) : 1;
        HPEN arrowPen = CreatePen(PS_SOLID, thick, pressed ? th.track : th.dim);
        HGDIOBJ oldArrowPen = SelectObject(dc, arrowPen);
        MoveToEx(dc, cx - arm / 2, cy - arm, nullptr);
        LineTo(dc, cx + arm / 2, cy);
        LineTo(dc, cx - arm / 2, cy + arm);
        SelectObject(dc, oldArrowPen);
        DeleteObject(arrowPen);
    }

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

/* ---------------- 顶部工具栏 ---------------- */

/* 工具栏上的按钮表。顺序就是从左到右的顺序，宽是 96 DPI 设计值（用时过 S()）。
 * 为什么不直接用 BS_PUSHBUTTON 子窗口：系统按钮的 3D 边框、灰底、按下动画
 * 和这套"纸感"界面完全不合，而且换成自绘一个窗口就够，不必为每个按钮建子类。
 * 每个按钮点下去都只发 WM_COMMAND，走的还是原来那套命令号，功能零改动。 */
struct UiBarBtn {
    int id;                 /* 命令号（WM_COMMAND 的 LOWORD） */
    const wchar_t* label;   /* 按钮文字（静态默认值；自动/主题两枚会按当前状态换字，见 BarBtnText） */
};

static const UiBarBtn kBarBtns[] = {
    { IDM_TOC,         L"目录" },
    { IDM_OPEN,        L"打开" },
    { IDM_FIND,        L"搜索" },
    { IDM_AUTO_TOGGLE, L"自动" },
    { IDM_SOURCE,      L"书源" },
    { IDM_THEME,       L"主题" },
};
static const int kBarBtnCount = (int)(sizeof(kBarBtns) / sizeof(kBarBtns[0]));

/* 按钮文字（少数按钮按当前状态换字，比"自动"开关在开着时显示"暂停"更好懂：
 * 按钮上写的是"点下去会发生什么"。同样，主题在夜间模式显示"日间"。 */
static const wchar_t* BarBtnText(int index)
{
    if (index < 0 || index >= kBarBtnCount) return L"";
    if (kBarBtns[index].id == IDM_AUTO_TOGGLE) return UiAutoRunning() ? L"暂停" : L"自动";
    if (kBarBtns[index].id == IDM_THEME)       return (g_set.theme == 1) ? L"日间" : L"夜间";
    return kBarBtns[index].label;
}

/* 按钮是否处于"激活"状态（画成强调色）：目录＝侧栏开着、自动＝正在滚、
 * 搜索＝搜索条正开着（这时正文被搜索条压住一截，按钮亮着能提示"面板从哪儿关"）。 */
static bool BarBtnActive(int index)
{
    if (index < 0 || index >= kBarBtnCount) return false;
    int id = kBarBtns[index].id;
    if (id == IDM_TOC)         return g_set.sidebar && g_book.Ok();
    if (id == IDM_AUTO_TOGGLE) return UiAutoRunning();
    if (id == IDM_FIND)        return s_searchShown != 0;
    return false;
}

/* 顶部标题区的高度（96 DPI 设计值）。54 是"放得下一行 15px 粗体书名 + 呼吸感"的高度，
 * 再高就抢正文的地方了。 */
static int BarH()      { return S(54); }
static int BarBtnH()   { return S(30); }   /* 胶囊组里单个按钮的高度 */
static int BarBtnW()   { return S(56); }   /* 每个按钮的宽度（都是两个汉字，统一宽度最整齐） */
static int BarPadX()   { return S(16); }   /* 标题区左右留白 */
static int BarGroupPad() { return S(4); }  /* 胶囊组内侧留白（让圆角不啃字） */

/* 状态栏（自绘）：三段文本 + 一条通条进度。文本由 UiSetStatus 写，绘制时读。 */
static std::wstring s_stL, s_stC, s_stR;
static int s_stPct = -1;          /* 0~1000；-1 = 不画进度条（没开书） */

/* 把空闲的工作集页还给系统（声明与理由见 common.h）。
 * 只在"内存需要回收"的时机调用：打开/关闭一本书、换 PDF 页、网页抓完。
 * 千万不能放进 PaintAll / 滚动里——那会让每一帧都触发一次缺页中断风暴。 */
void MemTrim()
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

/* 颜色混合（本文件局部版本）。view.cpp 里也有一份，但那是 static 的，
 * 而 ui.cpp 需要它来算"选中行的淡色底"，所以这里再来一份。
 * 参数 pa 是颜色 a 占的百分比（0~100）。 */
static COLORREF UiMix(COLORREF a, COLORREF b, int pa)
{
    if (pa < 0) pa = 0;
    if (pa > 100) pa = 100;
    return RGB((GetRValue(a) * pa + GetRValue(b) * (100 - pa)) / 100,
               (GetGValue(a) * pa + GetGValue(b) * (100 - pa)) / 100,
               (GetBValue(a) * pa + GetBValue(b) * (100 - pa)) / 100);
}

/* 画一个圆角填充矩形。用 GDI 的 RoundRect：它同时用当前画笔（描边）和画刷（填充），
 * 所以这里先选空画笔/目标色画刷，画完恢复。
 * 参数 r 是目标矩形，rad 是圆角半径（像素，已过 S()），fill 是填充色，border 是描边色
 * （border 传 CLR_INVALID 表示不描边）。 */
static void UiRoundRect(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pen = (border == CLR_INVALID)
                 ? CreatePen(PS_NULL, 0, RGB(0, 0, 0))
                 : CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

/* =====================================================================
 * 主题化消息框（1.7.0）
 *
 * 为什么不用系统的 MessageBoxW：
 *   1. 它是灰底 + 系统字体，在米色纸感和夜间深色界面里非常突兀，
 *      而且字号跟正文毫无关系（正文 17 磅，弹窗却是 9 磅系统字）；
 *   2. 它只会出现在自己创建的那个线程里，一旦被别的东西遮住，
 *      用户根本不知道该往哪看（这次联调"同步问句看不见"就是这个观感问题）；
 *   3. 按钮文案是系统的"是/否/确定"，跟界面里其它按钮（我自绘的圆角按钮）
 *      完全不是一个风格。
 *
 * 自己画的好处：纸色卡片、强调色按钮、Enter/Esc 直接生效、
 * 标题用章节标题那种褐色加粗体，跟正文是一个家族。
 *
 * 用法上保持和 MessageBoxW 一样的调用形状（见文件末尾的 NrMessageBoxW），
 * 这样原来 50 多处调用点不用逐个改，也不会漏掉某处。
 * ===================================================================== */

#define NRMB_OK     0
#define NRMB_YESNO  1

struct NrMsgState
{
    std::wstring title;
    std::wstring text;
    int  kind;                  /* NRMB_OK / NRMB_YESNO */
    int  result;                /* 对话框返回值 */
    bool done;
    int  hover;                 /* 悬停的按钮：0 主按钮、1 次按钮、-1 无 */
    bool tracking;              /* 是否已经 TrackMouseEvent(TME_LEAVE) */
    RECT rcMain;                /* 主按钮（确定 / 同步） */
    RECT rcSub;                 /* 次按钮（取消），NRMB_OK 时为空 */
    HFONT fTitle, fBody, fBtn;
};

static LRESULT CALLBACK NrMsgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NrMsgState* st = (NrMsgState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_CREATE:
        return 0;

    case WM_ERASEBKGND:
        return 1;               /* 全部在 WM_PAINT 里双缓冲画，别闪 */

    case WM_PAINT:
    {
        if (!st) break;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int W = rc.right, H = rc.bottom;

        /* 双缓冲：先画到内存位图再一次贴过去，避免按钮悬停时闪烁 */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HGDIOBJ ob = SelectObject(mem, bmp);
        const Theme& th = CurTheme();

        /* 整块卡片 */
        HBRUSH bg = CreateSolidBrush(th.bg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        /* 顶部一条强调色细线，跟工具栏/状态栏呼应 */
        RECT acc = { 0, 0, W, S(3) };
        HBRUSH ab = CreateSolidBrush(th.accent);
        FillRect(mem, &acc, ab);
        DeleteObject(ab);

        /* 标题 */
        int padX = S(24);
        int y = S(20);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, th.title);
        HGDIOBJ of = SelectObject(mem, st->fTitle);
        RECT rt = { padX, y, W - padX, y + S(28) };
        DrawTextW(mem, st->title.c_str(), -1, &rt, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(32);

        /* 标题下的分隔线 */
        RECT ln = { padX, y, W - padX, y + 1 };
        HBRUSH lb = CreateSolidBrush(th.line);
        FillRect(mem, &ln, lb);
        DeleteObject(lb);
        y += S(14);

        /* 正文（自动换行，多段用 \n） */
        SetTextColor(mem, th.fg);
        SelectObject(mem, st->fBody);
        RECT rbody = { padX, y, W - padX, H - S(64) };
        DrawTextW(mem, st->text.c_str(), -1, &rbody, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

        /* 按钮：主按钮用强调色底 + 白字，次按钮用卡片色 + 描边 */
        int bh = S(30);
        int by = H - S(16) - bh;
        int bw = S(96);
        int bx = W - padX - bw;

        RECT rSub = { 0, 0, 0, 0 };
        if (st->kind == NRMB_YESNO) {
            rSub.left = bx - S(10) - bw;
            rSub.top = by;
            rSub.right = rSub.left + bw;
            rSub.bottom = by + bh;
            UiRoundRect(mem, rSub, S(6), (st->hover == 1) ? th.hov : th.card, th.line);
        }

        RECT rMain = { bx, by, bx + bw, by + bh };
        UiRoundRect(mem, rMain, S(6),
                    (st->hover == 0) ? th.accent : UiMix(th.accent, th.bg, 14), th.accent);

        SetBkMode(mem, TRANSPARENT);
        SelectObject(mem, st->fBtn);
        const wchar_t* mainTxt = (st->kind == NRMB_YESNO) ? L"同步" : L"知道了";
        const wchar_t* subTxt  = L"不用";

        SetTextColor(mem, RGB(255, 255, 255));
        DrawTextW(mem, mainTxt, -1, &rMain, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (st->kind == NRMB_YESNO) {
            SetTextColor(mem, th.fg);
            DrawTextW(mem, subTxt, -1, &rSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        /* 把算好的矩形存回状态，鼠标消息直接用（避免两处算法不一致） */
        st->rcMain = rMain;
        st->rcSub = rSub;

        SelectObject(mem, of);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int hv = -1;
        if (PtInRect(&st->rcMain, POINT{ x, y })) hv = 0;
        else if (st->kind == NRMB_YESNO && PtInRect(&st->rcSub, POINT{ x, y })) hv = 1;
        if (hv != st->hover) {
            st->hover = hv;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (!st->tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            st->tracking = true;
        }
        /* 手型光标：让"能点"这件事一眼看出来 */
        SetCursor(LoadCursorW(nullptr, (hv >= 0) ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE:
        if (st) { st->hover = -1; st->tracking = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;

    case WM_SETCURSOR:
        return 0;               /* 光标已在 WM_MOUSEMOVE 里按命中设好 */

    case WM_LBUTTONDOWN:
    {
        if (!st) break;
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        POINT pt = { x, y };
        if (PtInRect(&st->rcMain, pt)) {
            st->result = (st->kind == NRMB_YESNO) ? IDYES : IDOK;
            st->done = true;
        } else if (st->kind == NRMB_YESNO && PtInRect(&st->rcSub, pt)) {
            st->result = IDNO;
            st->done = true;
        }
        if (st->done) DestroyWindow(h);
        return 0;
    }

    case WM_KEYDOWN:
        if (!st) break;
        if (w == VK_RETURN) {
            st->result = (st->kind == NRMB_YESNO) ? IDYES : IDOK;
            st->done = true;
            DestroyWindow(h);
            return 0;
        }
        if (w == VK_ESCAPE) {
            st->result = (st->kind == NRMB_YESNO) ? IDNO : IDOK;
            st->done = true;
            DestroyWindow(h);
            return 0;
        }
        if (st->kind == NRMB_YESNO && (w == 'Y' || w == 'y')) {
            st->result = IDYES; st->done = true; DestroyWindow(h); return 0;
        }
        if (st->kind == NRMB_YESNO && (w == 'N' || w == 'n')) {
            st->result = IDNO; st->done = true; DestroyWindow(h); return 0;
        }
        break;

    case WM_CLOSE:
        if (st) { st->result = (st->kind == NRMB_YESNO) ? IDNO : IDOK; st->done = true; }
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 真正干活的消息框。owner 为空时用主窗口，title 为空时用"提示"。
 * 返回 IDOK / IDYES / IDNO，与 MessageBoxW 的返回值保持兼容。 */
static int NrMsgBox(HWND owner, const wchar_t* title, const std::wstring& text, int kind)
{
    static bool s_clsReady = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE);
    if (!s_clsReady) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = NrMsgProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"NRMsgBox";
        RegisterClassExW(&wc);
        s_clsReady = true;
    }

    /* 先量正文需要多宽多高：宽度固定，高度按字算 */
    NrMsgState st;
    st.title = (title && *title) ? title : L"提示";
    st.text = text;
    st.kind = kind;
    st.result = (kind == NRMB_YESNO) ? IDNO : IDOK;
    st.done = false;
    st.hover = -1;
    st.tracking = false;
    SetRectEmpty(&st.rcMain);
    SetRectEmpty(&st.rcSub);

    st.fTitle = MakeUiFont(12, true);
    st.fBody  = MakeUiFont(10, false);
    st.fBtn   = MakeUiFont(10, false);

    int W = S(460);
    HDC sdc = GetDC(g_hMain);
    HGDIOBJ of = SelectObject(sdc, st.fBody);
    RECT rcT = { 0, 0, W - S(48), 0 };
    DrawTextW(sdc, st.text.c_str(), -1, &rcT,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(sdc, of);
    ReleaseDC(g_hMain, sdc);

    int textH = rcT.bottom - rcT.top;
    int H = S(20) + S(32) + S(14) + textH + S(20) + S(30) + S(16);
    if (H < S(150)) H = S(150);
    if (H > S(520)) H = S(520);

    /* 居中到 owner（owner 不在就用主窗口），再夹到工作区里 */
    RECT ro;
    HWND ref = owner ? owner : g_hMain;
    GetWindowRect(ref, &ro);
    int x = ro.left + ((ro.right - ro.left) - W) / 2;
    int y = ro.top + ((ro.bottom - ro.top) - H) / 2;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x < wa.left + S(8)) x = wa.left + S(8);
    if (y < wa.top + S(8)) y = wa.top + S(8);
    if (x + W > wa.right - S(8)) x = wa.right - S(8) - W;
    if (y + H > wa.bottom - S(8)) y = wa.bottom - S(8) - H;

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                             L"NRMsgBox", L"", WS_POPUP,
                             x, y, W, H, ref, nullptr, inst, nullptr);
    if (!h) {
        /* 自绘窗口创建失败时退回系统消息框，至少别让提示丢掉 */
        DeleteObject(st.fTitle); DeleteObject(st.fBody); DeleteObject(st.fBtn);
        return MessageBoxW(owner ? owner : g_hMain, text.c_str(),
                          st.title.c_str(), (kind == NRMB_YESNO) ? MB_YESNO : MB_OK);
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    SetForegroundWindow(h);
    SetFocus(h);

    EnableWindow(ref, FALSE);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == h && msg.message == WM_KEYDOWN) {
            SendMessageW(h, WM_KEYDOWN, msg.wParam, msg.lParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(h)) DestroyWindow(h);
    EnableWindow(ref, TRUE);
    DeleteObject(st.fTitle);
    DeleteObject(st.fBody);
    DeleteObject(st.fBtn);
    return st.result;
}

/* 和 MessageBoxW 一模一样形状的薄壳：这样原来 50 多处调用点不用改，
 * 也不会漏掉某处。MB_YESNO 映射成两个按钮，其它样式（图标）忽略。 */
static int NrMessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
{
    int kind = ((type & MB_TYPEMASK) == MB_YESNO) ? NRMB_YESNO : NRMB_OK;
    return NrMsgBox(owner, caption, text ? text : L"", kind);
}

/* 从这里开始，文件里所有 MessageBoxW(...) 自动走主题化消息框。
 * 注意：宏必须放在 NrMessageBoxW 定义之后，否则会把自己的函数体也替换掉。 */
#define MessageBoxW NrMessageBoxW

/* 「菜单」按钮（三条横线，像常见的汉堡按钮）的矩形：贴左上角、垂直居中。
 * 为什么不把菜单项塞进右边那组胶囊：菜单是"整个应用"的入口，和"当前这本书"的操作
 * （目录/搜索/自动…）不是一类东西，放一起会让胶囊组变得又长又乱。 */
static RECT BarMenuRect()
{
    RECT r = { 0, 0, 0, 0 };
    int s = S(30);
    int y = (BarH() - s) / 2;
    r.left = BarPadX() - S(2);
    r.top = y;
    r.right = r.left + s;
    r.bottom = y + s;
    return r;
}

/* 鼠标是否落在菜单按钮上 */
static bool BarMenuHit(int x, int y)
{
    RECT r = BarMenuRect();
    POINT p = { x, y };
    return PtInRect(&r, p) != FALSE;
}

/* 弹出整个应用菜单（屏幕坐标）。菜单没有挂在窗口上，所以只能这样弹；
 * 用 TPM_RETURNCMD 拿到命令号后 Post 成 WM_COMMAND，走和菜单栏时期完全一样的分发路径。
 * 右键、工具栏的「菜单」按钮、以及 Ctrl+F10（见 accelerators）都走这里。 */
void UiShowAppMenuAt(int sx, int sy)
{
    if (!s_hAppMenu) return;
    /* 弹出前把"最近打开"重刷一遍：用户可能刚打开过新文件，菜单里的列表要是旧的会点错 */
    UiRebuildRecentMenu();
    int cmd = (int)TrackPopupMenu(s_hAppMenu,
                                  TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                                  sx, sy, 0, g_hMain, nullptr);
    if (cmd) PostMessageW(g_hMain, WM_COMMAND, (WPARAM)cmd, 0);
}

/* 整组按钮（胶囊容器）的矩形：贴右边缘，垂直居中。绘制与命中共用。 */
static RECT BarGroupRect(int W)
{
    RECT r = { 0, 0, 0, 0 };
    int gw = kBarBtnCount * BarBtnW() + BarGroupPad() * 2;
    int gh = BarBtnH() + BarGroupPad() * 2;
    int right = W - BarPadX();
    if (right - gw < BarPadX()) {          /* 窗口太窄：让出左边至少 BarPadX */
        right = BarPadX() + gw;
    }
    int y = (BarH() - gh) / 2;
    r.right = right;
    r.left = right - gw;
    r.top = y;
    r.bottom = y + gh;
    return r;
}

/* 第 index 个按钮的矩形（已经算进胶囊组的内侧留白）。 */
static RECT BarBtnRect(int index, int W)
{
    RECT r = { 0, 0, 0, 0 };
    if (index < 0 || index >= kBarBtnCount) return r;
    RECT g = BarGroupRect(W);
    int x = g.left + BarGroupPad() + index * BarBtnW();
    r.left = x;
    r.right = x + BarBtnW();
    r.top = g.top + BarGroupPad();
    r.bottom = r.top + BarBtnH();
    return r;
}

/* 鼠标位置 → 按钮序号，不落在任何按钮上返回 -1 */
static int BarHitTest(int x, int y)
{
    POINT p = { x, y };
    for (int i = 0; i < kBarBtnCount; i++) {
        RECT r = BarBtnRect(i, s_lastBarW);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

/* 顶部标题区绘制。设计要点（这版是重写过的，上一版"一条横杠 + 一排小字按钮"太像工具栏）：
 *   - 底色用纸张色，和正文连成一片，只在最底下一条 1px 细线收边 —— 不再是一块突兀的色带；
 *   - 左边是书名（粗体）+ 章名（次要色），像书的页眉，而不是"标题栏"；
 *   - 右边是一整组胶囊按钮（segmented control）：圆角容器 + 内部分隔线，
 *     悬停/激活只给单格上淡色底，视觉上是一个整体，不是六个散落的文字；
 *   - 进度不再放这里（底部状态栏已经有一条通条进度 + 百分比），少一处重复信息。 */
static void DrawToolbar(HDC dc, const RECT& rcClient)
{
    const Theme& th = CurTheme();
    int W = rcClient.right, H = rcClient.bottom;
    s_lastBarW = W;

    /* 1) 底：纸张色 + 底部 1px 收边线 */
    HBRUSH bb = CreateSolidBrush(th.bg);
    FillRect(dc, &rcClient, bb);
    DeleteObject(bb);
    HBRUSH lb = CreateSolidBrush(th.line);
    RECT line = { 0, H - 1, W, H };
    FillRect(dc, &line, lb);
    DeleteObject(lb);

    /* 2) 左侧「菜单」按钮：悬停时给一块淡色底，三条横线用正文色；
     * 三条线的粗细跟着 DPI 走（S(1) 在 125% 下是 1px、150% 下是 2px，别用硬编码 1）。 */
    {
        RECT mr = BarMenuRect();
        if (s_barHover == -2) {          /* -2 是"悬停在菜单按钮上"的约定值 */
            UiRoundRect(dc, mr, S(8), th.hov, CLR_INVALID);
        }
        int lh = S(2), lw = (mr.right - mr.left) / 2 + S(2);
        int lx = mr.left + ((mr.right - mr.left) - lw) / 2;
        int gap = S(4);
        int total = lh * 3 + gap * 2;
        int ly = mr.top + ((mr.bottom - mr.top) - total) / 2;
        HBRUSH bb2 = CreateSolidBrush(th.fg);
        for (int i = 0; i < 3; i++) {
            RECT bar = { lx, ly + i * (lh + gap), lx + lw, ly + i * (lh + gap) + lh };
            FillRect(dc, &bar, bb2);
        }
        DeleteObject(bb2);
    }

    /* 3) 右侧胶囊按钮组：先画容器（卡片色 + 细边 + 全圆角），再画分隔线与每格 */
    RECT g = BarGroupRect(W);
    if (g.right > g.left) {
        int rad = (g.bottom - g.top) / 2;
        UiRoundRect(dc, g, rad, th.card, th.line);

        HFONT btnFont = MakeUiFont(9, false);
        HFONT oldFont = (HFONT)SelectObject(dc, btnFont ? btnFont : s_uiFont);
        int oldMode = SetBkMode(dc, TRANSPARENT);

        for (int i = 0; i < kBarBtnCount; i++) {
            RECT r = BarBtnRect(i, W);
            bool hot = (i == s_barHover);
            bool active = BarBtnActive(i);

            /* 悬停/激活：单格淡色底（比容器圆角小一点，看起来像"胶囊里被按亮的一格"） */
            if (hot || active) {
                RECT hr = r;
                InflateRect(&hr, -S(1), -S(1));
                UiRoundRect(dc, hr, (hr.bottom - hr.top) / 2,
                            active ? UiMix(th.accent, th.card, 14) : th.hov, CLR_INVALID);
            }
            /* 分隔线：画在相邻两格中间，最后一格不用 */
            if (i > 0) {
                HPEN p = CreatePen(PS_SOLID, 1, th.line);
                HGDIOBJ op = SelectObject(dc, p);
                MoveToEx(dc, r.left, r.top + S(8), nullptr);
                LineTo(dc, r.left, r.bottom - S(8));
                SelectObject(dc, op);
                DeleteObject(p);
            }

            SetTextColor(dc, active ? th.accent : (hot ? th.fg : th.dim));
            DrawTextW(dc, BarBtnText(i), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        SetBkMode(dc, oldMode);
        SelectObject(dc, oldFont);
        if (btnFont) DeleteObject(btnFont);
    }

    /* 3) 左侧：书名（粗体）+ 章名（次要色）。宽度让出胶囊组，放不下就省略。
     * 书名用 12 号粗体（比正文大一点，像页眉的书名），章名用 9 号次要色接在后面。 */
    int leftLimit = BarMenuRect().right + S(14);
    int rightLimit = (g.right > g.left) ? g.left - S(16) : W - BarPadX();
    if (rightLimit <= leftLimit + S(60)) return;   /* 窄窗口：只留按钮，不硬挤书名 */

    HFONT titleFont = MakeUiFont(12, true);
    HFONT oldF = (HFONT)SelectObject(dc, titleFont ? titleFont : s_uiFont);
    int oldM = SetBkMode(dc, TRANSPARENT);

    /* 1.6.0：书名走 UiDisplayTitle()，隐私模式开着时这里显示"文档"而不是真书名。 */
    std::wstring title = g_book.Ok() ? UiDisplayTitle() : L"书架";
    SIZE tsz; tsz.cx = tsz.cy = 0;
    GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &tsz);

    RECT tr = { leftLimit, 0, rightLimit, H };
    if (tsz.cx > rightLimit - leftLimit) {
        /* 太长：整个书名区自己截断，章名就不画了 */
        SetTextColor(dc, th.fg);
        DrawTextW(dc, title.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        SetTextColor(dc, th.fg);
        ExtTextOutW(dc, leftLimit, (H - tsz.cy) / 2, 0, nullptr, title.c_str(), (int)title.size(), nullptr);

        /* 章名：书名右边 S(14)，用次要色；空间不够就算了 */
        if (g_book.Ok() && !s_stC.empty()) {
            HFONT nf = MakeUiFont(9, false);
            HFONT of2 = nf ? (HFONT)SelectObject(dc, nf) : nullptr;
            int x2 = leftLimit + tsz.cx + S(14);
            RECT cr = { x2, 0, rightLimit, H };
            SetTextColor(dc, th.dim);
            DrawTextW(dc, s_stC.c_str(), -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (of2) SelectObject(dc, of2);
            if (nf) DeleteObject(nf);
        }
    }

    SetBkMode(dc, oldM);
    SelectObject(dc, oldF);
    if (titleFont) DeleteObject(titleFont);
}

/* 工具栏窗口过程：只做三件事 —— 画自己、跟踪悬停、把点击翻译成 WM_COMMAND。
 * 为什么用 PostMessage 而不是 SendMessage：按钮命令里可能开模态框（打开文件、
 * 搜索、主题），在鼠标消息的调用栈里开模态框会被重入，Post 出去更安全。 */
static LRESULT CALLBACK ToolbarProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        /* 双缓冲：先画到内存位图再一次性贴，避免按钮区闪烁 */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        if (mem && bmp) {
            HGDIOBJ old = SelectObject(mem, bmp);
            DrawToolbar(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        /* -2 表示"悬停在菜单按钮上"（胶囊里的按钮用 0..n-1，-1 表示都不在） */
        int idx = BarMenuHit(x, y) ? -2 : BarHitTest(x, y);
        if (idx != s_barHover) {
            s_barHover = idx;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (!s_barTrack) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            s_barTrack = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        s_barTrack = false;
        if (s_barHover != -1) { s_barHover = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (BarMenuHit(x, y)) {
            /* 菜单按钮：在选择条正下方弹出整个菜单 */
            RECT mr = BarMenuRect();
            POINT p = { mr.left, mr.bottom + S(2) };
            ClientToScreen(h, &p);
            UiShowAppMenuAt(p.x, p.y);
            return 0;
        }
        int idx = BarHitTest(x, y);
        if (idx >= 0) PostMessageW(g_hMain, WM_COMMAND, (WPARAM)kBarBtns[idx].id, 0);
        return 0;
    }

    case WM_RBUTTONUP: {
        /* 标题区右键也能出菜单，和正文区右键一致 */
        POINT p;
        GetCursorPos(&p);
        UiShowAppMenuAt(p.x, p.y);
        return 0;
    }

    case WM_SETCURSOR:
        /* 鼠标移到按钮上给个手型，明确"这里能点" */
        int mx = GET_X_LPARAM(GetMessagePos()), my = GET_Y_LPARAM(GetMessagePos());
        if (LOWORD(l) == HTCLIENT && (BarMenuHit(mx, my) || BarHitTest(mx, my) >= 0)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------- 自绘状态栏 ---------------- */

/* 状态栏绘制：一行三段 + 底部通条进度。
 * 三段的分工与原来系统状态栏一致（书名 / 章名 / 进度），但排版上做了两处改进：
 *   1) 章名居中、左侧书名与右侧进度分别贴边，视线扫过时更有层次；
 *   2) 整个窗口底部压一条 S(3) 的进度条，读书时余光就能看到读了多远。
 * 进度条只在有书且知道百分比时画（s_stPct >= 0），否则只留底槽。 */
static void DrawStatusBar(HDC dc, const RECT& rcClient)
{
    const Theme& th = CurTheme();
    int W = rcClient.right, H = rcClient.bottom;

    HBRUSH bb = CreateSolidBrush(th.card);
    FillRect(dc, &rcClient, bb);
    DeleteObject(bb);
    HBRUSH lb = CreateSolidBrush(th.line);
    RECT line = { 0, 0, W, 1 };
    FillRect(dc, &line, lb);
    DeleteObject(lb);

    HFONT oldFont = (HFONT)SelectObject(dc, s_uiFont);
    int oldMode = SetBkMode(dc, TRANSPARENT);

    int pad = S(12);
    RECT rl = { pad, 0, W / 3, H };
    RECT rc = { W / 3, 0, W * 2 / 3, H };
    RECT rr = { W * 2 / 3, 0, W - pad, H };

    SetTextColor(dc, th.fg);
    DrawTextW(dc, s_stL.c_str(), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, th.dim);
    DrawTextW(dc, s_stC.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextW(dc, s_stR.c_str(), -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);

    /* 底部通条进度：底槽 + accent 填充 */
    int barH = S(3);
    RECT tr = { 0, H - barH, W, H };
    HBRUSH tb = CreateSolidBrush(th.track);
    FillRect(dc, &tr, tb);
    DeleteObject(tb);
    if (s_stPct >= 0) {
        int p = s_stPct;
        if (p < 0) p = 0;
        if (p > 1000) p = 1000;
        RECT fr = { 0, H - barH, W * p / 1000, H };
        HBRUSH fb = CreateSolidBrush(th.accent);
        FillRect(dc, &fr, fb);
        DeleteObject(fb);
    }
}

static LRESULT CALLBACK StatusProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        if (mem && bmp) {
            HGDIOBJ old = SelectObject(mem, bmp);
            DrawStatusBar(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   /* 全部在 WM_PAINT 里双缓冲画完，禁止系统先刷一遍底色（会闪） */
    }
    return DefWindowProcW(h, m, w, l);
}

/* 写状态栏三段文本 + 进度。所有想改状态栏的地方都走这里（原来那些 SB_SETTEXTW
 * 的调用点已全部替换），这样自绘状态窗口不需要知道任何业务逻辑。
 * 参数 pct 是千分数（0~1000），传 -1 表示不画进度（例如没开书）。 */
static void UiSetStatus(const std::wstring& l, const std::wstring& c,
                        const std::wstring& r, int pct)
{
    s_stL = l;
    s_stC = c;
    s_stR = r;
    s_stPct = pct;
    if (s_hStatus) InvalidateRect(s_hStatus, nullptr, FALSE);
    /* 工具栏右侧也显示百分比，状态变了要一起刷新 */
    if (s_hBar) InvalidateRect(s_hBar, nullptr, FALSE);
}



/* ---------------- 自绘列表行（目录 / 搜索结果 / 书签共用） ---------------- */

/* 列表行高（96 DPI 设计值 S(30)）。目录每行只是一行章名，30 足够留出上下呼吸；
 * 搜索命中行文字更长，但列表本身不会换行（超长已被 ClipToWidth 截断），同样用 30。 */
static int ListRowH() { return S(30); }

/* 画列表里的一行。
 * 参数 dc / r：控件 DC 与行矩形（来自 DRAWITEMSTRUCT）；text：行文字；
 * selected：是否被选中（ODS_SELECTED）；dim：是否要画成次要色（搜索结果用，标题与片段同色，
 * 这里保留参数以区分书签里"章名"这类弱信息）。
 *
 * 视觉设计：选中不再是系统蓝，而是"在纸面上压一道淡淡的高亮 + 左侧一根强调竖条"，
 * 这样即使换成夜间或自定义配色也不会突兀；竖条用 accent 色，和进度条、当前章形成呼应。
 */
static void DrawListRow(HDC dc, const RECT& r, const std::wstring& text,
                        bool selected, bool dim)
{
    const Theme& th = CurTheme();
    RECT row = r;

    /* 1) 行底：选中先铺一层 accent 淡色；不选中就保持面板底色（由列表自己的背景画） */
    if (selected) {
        COLORREF fill = UiMix(th.accent, th.card, 18);   /* accent 占 18%，淡到不抢字 */
        UiRoundRect(dc, row, S(6), fill, CLR_INVALID);
    }

    /* 2) 文字：左侧留出竖条的位置 */
    RECT tr = row;
    tr.left += S(14);
    tr.right -= S(8);
    HFONT oldFont = (HFONT)SelectObject(dc, s_uiFont);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? th.fg : (dim ? th.dim : th.fg));
    DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);

    /* 3) 左侧强调竖条：只在选中时出现，高出行高的 60%，垂直居中 */
    if (selected) {
        int bh = (row.bottom - row.top) * 3 / 5;
        int by = row.top + ((row.bottom - row.top) - bh) / 2;
        RECT bar = { row.left + S(5), by, row.left + S(9), by + bh };
        HBRUSH bb = CreateSolidBrush(th.accent);
        FillRect(dc, &bar, bb);
        DeleteObject(bb);
    }
}

/* 搜索条上的三个小按钮（搜索 / 下一个 / 关闭）自绘。
 * 用 BS_OWNERDRAW 而不是系统 BS_PUSHBUTTON：系统的灰色立体按钮在米色纸面上很扎眼，
 * 夜间配色下更是亮得发光。这里画成和顶栏胶囊一致的样子：圆角、卡片底、细描边，
 * 按下时底色压深一档、文字用强调色。文字直接取控件自己的标题（GetWindowTextW），
 * 将来改文案不用动绘制代码。 */
static void DrawMiniButton(HDC dc, const RECT& r, const std::wstring& label, UINT state)
{
    const Theme& th = CurTheme();
    bool pressed = (state & ODS_SELECTED) != 0;
    bool disabled = (state & ODS_DISABLED) != 0;

    /* 常态用 hov（比纸面稍深一点）而不是 card，否则按钮和背景几乎同色、看不出可点 */
    COLORREF fill = pressed ? UiMix(th.accent, th.card, 16) : th.hov;
    UiRoundRect(dc, r, S(7), fill, th.line);

    HFONT oldFont = (HFONT)SelectObject(dc, s_uiFont);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dc, disabled ? th.dim : (pressed ? th.accent : th.fg));
    RECT tr = r;
    DrawTextW(dc, label.c_str(), (int)label.size(), &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

/* 取列表第 i 条的文字。OWNERDRAWFIXED + HASSTRINGS 时文字仍由列表控件保存，
 * 这里只是把它读回来给自绘用；越界或空条目返回空串。 */
static std::wstring ListText(HWND list, int i)
{
    int n = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    if (i < 0 || i >= n) return L"";
    int len = (int)SendMessageW(list, LB_GETTEXTLEN, (WPARAM)i, 0);
    if (len <= 0) return L"";
    std::vector<wchar_t> buf((size_t)len + 2, 0);
    SendMessageW(list, LB_GETTEXT, (WPARAM)i, (LPARAM)&buf[0]);
    return std::wstring(&buf[0]);
}

/* ---------------- 布局 ---------------- */

/* 重新摆放全部子窗口 —— 本文件唯一写布局的地方。
 * 无参数，全部依据当前客户区尺寸 + DPI + g_set.sidebar + s_panel + s_searchShown 推算。
 * 调用时机：启动、WM_SIZE、开关搜索条、开关侧栏、切换面板、打开新书。
 * 计算顺序（下面的行内注释跟着代码走）：
 *   1) 状态栏：让它自己 WM_SIZE 归位 → 量出真实高度存进 s_sbH → 设三段宽度；
 *   2) 侧栏宽：目录 232、搜索结果 320（过 S()），并夹到不超过窗口一半；
 *   3) 搜索条高：展开 38，收起 0，正文顺势上移；
 *   4) 正文高度 = 客户区高 - 状态栏高 - 搜索条高，并夹一个最小高度；
 *   5) 两个列表铺满正文那块区域，再按 s_panel 只显示一个；
 *   6) 搜索条内部从右往左排，因为按钮宽度固定、输入框吃掉剩余空间。
 * 边界：g_hMain 为空（启动早期）直接返回；状态栏高度量不到时沿用上次的 s_sbH。
 */
static void UiLayout()
{
    if (!g_hMain) return;
    RECT rc;
    GetClientRect(g_hMain, &rc);
    int W = rc.right, H = rc.bottom;

    /* 顶部工具栏：一条通栏，高度固定（BarH()），里面的按钮位置全由 BarBtnRect 现算，
     * 所以这里只摆工具栏本身。它不参与"谁让出多少高度"之外的任何计算。 */
    if (s_hBar) MoveWindow(s_hBar, 0, 0, W, BarH(), TRUE);

    /* 底部状态栏：自绘窗口，高度固定 S(26)。原来要靠系统 WM_SIZE 量高度，
     * 现在自己说了算 —— 顺便让 s_sbH 这个缓存继续有效（正文高度要用）。 */
    s_sbH = S(26);
    if (s_hStatus) MoveWindow(s_hStatus, 0, H - s_sbH, W, s_sbH, TRUE);

    /* 两个侧栏页共用一份宽度策略：目录 232、搜索结果 320，都是 96 DPI 设计值，用时过 S()。
     * 232 是按「第 N 章 标题」十来个汉字估的；320 是按「章名 · 前文【关键词】后文」估的，
     * 不更宽的话几乎每条命中都要被 ClipToWidth 截掉后半段。
     * 为什么不直接用客户区宽度算：下面那行夹取就是理由 —— 窗口窄时要主动收窄。
     */
    /* 搜索结果列表里的条目比较长，此时侧栏放宽一些 */
    int sideW = g_set.sidebar ? (s_panel != 0 ? S(320) : S(232)) : 0;
    /* 首页书架（还没打开书）时不留侧栏：书都没开，目录和搜索结果都是空的，
     * 白占一块地方还把书架的可用宽度压窄了。折叠条也一并藏起来，
     * 因为它此时没有"可折叠的对象"，留着只会让人以为坏了。 */
    bool shelfMode = !g_book.Ok();
    if (shelfMode) sideW = 0;
    /* 上限夹到一半：窗口被拖得很窄时若侧栏还按 320 缩放，正文只剩几十像素，排版会退化成一行一个字 */
    if (sideW > W / 2) sideW = W / 2;
    /* 38 是设计值：上下各 5px 留白 + 26px 控件高 + 一点余量。
     * 收起时给 0，正文自然上移到窗口顶端，不留空条。
     */
    int searchH = s_searchShown ? S(38) : 0;
    /* 正文区域的顶边 = 工具栏 + （展开时的）搜索条。工具栏永远在场，所以这一项不能省，
     * 否则正文会被压在工具栏底下，第一行看不见。 */
    int top = BarH() + searchH;
    int viewH = H - s_sbH - top;
    /* 正文高度夹一个下限，否则 view 会算出 0 行可用 */
    if (viewH < S(40)) viewH = S(40);

    /* ---- 侧栏折叠条（IDC_FOLD）----
     * 两种形态都在这里定位置，绘制在 DrawFoldBar：
     *   侧栏展开（sideW > 0）：横条贴在侧栏最底部，高 S(22)；两个列表高度让出这 22px。
     *   侧栏收起（sideW == 0）：变成左边缘宽 S(18) 的细竖条，正文从它右边开始 ——
     *   这样「重新展开目录」的入口永远在鼠标够得到的地方，不必记 Ctrl+T。
     * textX 就是"正文真正的左边界"，下面 view / 搜索条都改用它，别再直接用 sideW。
     */
    int foldH = S(22);
    int textX = sideW;
    if (s_hFold) {
        if (shelfMode) {
            /* 首页书架：没有可折叠的东西，整条藏起来 */
            ShowWindow(s_hFold, SW_HIDE);
        } else {
            if (sideW > 0) {
                MoveWindow(s_hFold, 0, top + viewH - foldH, sideW, foldH, TRUE);
            } else {
                textX = S(18);
                MoveWindow(s_hFold, 0, top, textX, viewH, TRUE);
            }
            ShowWindow(s_hFold, SW_SHOW);
            InvalidateRect(s_hFold, nullptr, TRUE);   /* 形态切换后文字要从"< 收起目录"换成">"，重画 */
        }
    }

    if (s_hView) MoveWindow(s_hView, textX, top, W - textX, viewH, TRUE);

    if (sideW > 0) {
        /* 两个列表都铺满「正文那块区域」（同样的 top 与高度），这样从目录切到搜索结果时
         * 视觉上不会跳一下；侧栏宽度为 0（隐藏侧栏）时干脆不摆，省掉无效的 MoveWindow。
         * 高度让出底部折叠条的 22px，否则列表最后一行会被折叠条压住。
         */
        int sh = viewH - foldH;
        if (sh < S(40)) sh = S(40);
        /* 侧栏做成"浮在纸上的一块面板"：四周留 S(8) 的内边距，面板本身用主题卡片色
         * （列表底色来自 WM_CTLCOLORLISTBOX），和纸色背景拉开层次，看起来不是一堵字墙。
         * 内嵌后宽度要重新夹一次下限，否则窗口窄时可能算出负数宽度。 */
        int px = S(8), py = S(6);
        int pw = sideW - px - S(4);
        if (pw < S(60)) pw = S(60);
        int ph = sh - py - S(4);
        if (ph < S(40)) ph = S(40);
        if (s_hToc)  MoveWindow(s_hToc, px, top + py, pw, ph, TRUE);
        if (s_hHits) MoveWindow(s_hHits, px, top + py, pw, ph, TRUE);
    }
    if (s_hToc)  ShowWindow(s_hToc,  (sideW > 0 && s_panel == 0) ? SW_SHOW : SW_HIDE);
    /* s_panel==2（书签面板）也复用 s_hHits 这个列表，所以要一起显示，
     * 否则切到书签页会看到一块空白（列表被隐藏、目录也不该显示）。 */
    if (s_hHits) ShowWindow(s_hHits, (sideW > 0 && s_panel != 0) ? SW_SHOW : SW_HIDE);

    /* 搜索条内部：以右边界为基准逐个往左挤。按钮之间留 6，按钮与输入框之间留 12，
     * 输入框拿走剩余宽度。这样无论窗口多宽，按钮组始终贴着右边、不会乱飘。
     */
    if (s_searchShown) {
        int y = BarH() + S(6), hh = S(26);
        int bw = S(58);
        int right = W - S(8);
        int x = textX + S(8);       /* 左边从"正文左边界"起算：侧栏收起时也不会压在折叠条上 */
        if (s_bClose) { MoveWindow(s_bClose, right - bw, y, bw, hh, TRUE); ShowWindow(s_bClose, SW_SHOW); }
        right -= bw + S(6);
        if (s_bNext) { MoveWindow(s_bNext, right - bw - S(6), y, bw + S(6), hh, TRUE); ShowWindow(s_bNext, SW_SHOW); }
        right -= bw + S(12);
        if (s_bFind) { MoveWindow(s_bFind, right - bw, y, bw, hh, TRUE); ShowWindow(s_bFind, SW_SHOW); }
        right -= bw + S(6);
        /* 输入框宽度 = 从侧栏右边 8px 到最后一个按钮左边，并夹一个下限；
         * 窗口极窄时它可能与按钮略重叠，但总好过算出负宽度让 MoveWindow 失败。
         */
        int ew = right - x;
        if (ew < S(80)) ew = S(80);
        if (s_hFind) { MoveWindow(s_hFind, x, y, ew, hh, TRUE); ShowWindow(s_hFind, SW_SHOW); }
    } else {
        if (s_hFind)  ShowWindow(s_hFind, SW_HIDE);
        if (s_bFind)  ShowWindow(s_bFind, SW_HIDE);
        if (s_bNext)  ShowWindow(s_bNext, SW_HIDE);
        if (s_bClose) ShowWindow(s_bClose, SW_HIDE);
    }
    /* 子窗口已经各自重绘，这里刷的是主窗口自己露出来的背景（侧栏与状态栏之间的缝、
     * 搜索条那块）。夜间模式下不刷会留出一片浅色。
     */
    InvalidateRect(g_hMain, nullptr, TRUE);
}

/* ---------------- 状态栏 ---------------- */

/* 刷新底部状态栏三格的文字 —— 界面上所有阅读状态的唯一出口。
 * 滚动、翻章、搜索跳转、开关自动下滑、开关书，改完状态后都调它；无参数无返回值。
 * 三格的取值来源（改代码时务必保持这个对应关系）：
 *   第 0 格 p0 = 书名 g_book.title + 可选的 s_statusExtra（如搜索结果计数）；
 *                没开书时显示「未打开文件」并给出 Ctrl+O 的引导。
 *   第 1 格 p1 = 当前章名。用 ViewCurrentPara() 取屏幕顶行所属段落，再问
 *                BookChapterAtPara() 它属于哪一章；顶行还在第一章之前（书名、作者
 *                那几行）时显示「（开篇）」；全书没识别出章节则显示「未识别章节」。
 *                章名超过 34 字截断加省略号 —— 三格里它最容易撑破。
 *   第 2 格 p2 = 「x.y% · n/N 章」。百分比按「当前顶行字符偏移 / 全文长度」算，
 *                先乘 1000 再拆成整数位与一位小数（pct/10 与 pct%10）。
 * 注意三格宽度在 UiLayout 里按 34%/66%/剩余固定，文字长了会被状态栏直接裁掉、
 * 不会自动加省略号，所以上面才主动做了 34 / 40 字的截断。
 */
void UiUpdateStatus()
{
    if (!s_hStatus) return;
    std::wstring p0, p1, p2;
    if (!g_book.Ok()) {
        p0 = L"未打开文件";
        /* 有书架记录时提示改成"单击书名" —— 首页第一屏就是那几本书，
         * 用户不需要再去记 Ctrl+O。 */
        p1 = ShelfItems().empty() ? L"Ctrl+O 打开小说文件" : L"单击书名继续阅读";
        p2 = L"";
    } else {
        /* 隐私模式：状态栏第一格也不显示真书名 */
        p0 = UiDisplayTitle();
        if (!s_statusExtra.empty()) p0 += L"  ·  " + s_statusExtra;
        /* 章节号按「屏幕顶行」算，而不是按选中位置或鼠标位置：
         * 用户感知的「我在哪一章」永远是眼睛看到的第一行。
         */
        int ch = BookChapterAtPara(g_book, ViewCurrentPara());
        if (!g_book.chapters.empty()) {
            if (ch >= 0) {
                p1 = BookChapterTitle(g_book, ch);
                if (p1.size() > 34) p1 = p1.substr(0, 34) + L"…";
            } else {
                p1 = L"（开篇）";
            }
        } else {
            p1 = L"未识别章节";
        }
        /* 进度用字符偏移比，不用段落比：段落长短差异巨大（一章可能 3 段也可能 300 段），
         * 按段算进度会忽快忽慢。先乘 1000 再除，是为了留下一位小数的精度。
         */
        int total = (int)g_book.text.size();
        int pos = ViewAnchorOffset();
        int pct = total > 0 ? (int)((LONGLONG)pos * 1000 / total) : 0;
        s_lastPct = pct;
        wchar_t buf[128];
        /* PDF 原始版式模式：这里要报"第几页"，因为用户看的就是一页页的图，
         * 报"第几章"对不上他眼前的内容（章和页不是一一对应）。 */
        if (ViewIsPdfMode()) {
            int pg = ViewPdfPage() + 1, tot = ViewPdfPageCount();
            if (!g_book.chapters.empty() && ch >= 0) {
                wsprintfW(buf, L"%d.%d%%  ·  第 %d/%d 页  ·  %d/%d 章", pct / 10, pct % 10,
                          pg, tot, ch + 1, (int)g_book.chapters.size());
            } else {
                wsprintfW(buf, L"%d.%d%%  ·  第 %d/%d 页", pct / 10, pct % 10, pg, tot);
            }
        } else if (!g_book.chapters.empty()) {
            // 有章节时一律显示"第几个/共几个章"；还没进入第一章（书名、作者等开篇文字）时显示总章数
            if (ch >= 0) {
                wsprintfW(buf, L"%d.%d%%  ·  %d/%d 章", pct / 10, pct % 10,
                          ch + 1, (int)g_book.chapters.size());
            } else {
                wsprintfW(buf, L"%d.%d%%  ·  共 %d 章", pct / 10, pct % 10,
                          (int)g_book.chapters.size());
            }
        } else {
            wsprintfW(buf, L"%d.%d%%  ·  共 %d 段", pct / 10, pct % 10, g_book.ParaCount());
        }
        p2 = buf;
        /* 自动下滑时在进度后面挂一个旗帜标记 + 速度档，让用户一眼确认「是开着的」而不是程序抽风 */
        if (s_autoOn) {
            wchar_t ab[64];
            wsprintfW(ab, L"  ⏵自动 %d", g_set.autoSpeed);
            p2 += ab;
        }
    }
    /* 状态栏：左侧书名 + 补充信息，中间章名，右侧进度。三段都是给 UiSetStatus 的纯文本，
     * 排版（哪段靠哪边、字号、进度条）全部由 DrawStatusBar 负责。 */
    UiSetStatus(p0, p1, p2, g_book.Ok() ? s_lastPct : -1);
}

/* 让目录列表的选中项跟随当前阅读位置（只移动高亮，不触发跳转）。
 * 数据来源与状态栏第 1 格一致：ViewCurrentPara → BookChapterAtPara。
 * 四种情况直接早退：目录没建 / 没开书 / 当前显示的是搜索结果面板（目录正隐藏着）/
 * 用户把侧栏关了；另外顶行在第一章之前（ch < 0）时没有对应条目可选。
 * 关键点：LB_SETCURSEL 会回一个 LBN_SELCHANGE，所以前后用 s_tocSync 把它挡掉。
 */
void UiSyncToc()
{
    if (!s_hToc || !g_book.Ok() || s_panel != 0 || !g_set.sidebar) return;
    int ch = BookChapterAtPara(g_book, ViewCurrentPara());
    if (ch < 0) return;
    s_tocSync = true;
    SendMessageW(s_hToc, LB_SETCURSEL, ch, 0);
    s_tocSync = false;
}

/* ---------------- 目录 / 搜索结果 ---------------- */

/* 把章节索引灌进目录列表（整表重建，不做增量）。
 * 为什么整表重建：章节表只在换书时变，几百上千条 LB_ADDSTRING 也就毫秒级，
 * 比维护「哪一条变了」的增量逻辑省心得多。
 * 边界：没识别到章节（n == 0）时放两行提示，让用户知道书是打开的、只是没有
 * 章节结构，而不是列表坏了。
 * 性能：WM_SETREDRAW FALSE/TRUE 夹住避免每条都重绘；LB_INITSTORAGE 预分配
 * （每条约 80 字节的估算）让大目录插入更快。
 */
static void FillToc()
{
    if (!s_hToc) return;
    SendMessageW(s_hToc, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s_hToc, LB_RESETCONTENT, 0, 0);
    int n = (int)g_book.chapters.size();
    if (n == 0) {
        SendMessageW(s_hToc, LB_ADDSTRING, 0, (LPARAM)L"（未识别到章节）");
        wchar_t b[128];
        wsprintfW(b, L"共 %d 段", g_book.ParaCount());
        SendMessageW(s_hToc, LB_ADDSTRING, 0, (LPARAM)b);
    } else {
        SendMessageW(s_hToc, LB_INITSTORAGE, n, (LPARAM)(n * 80));
        for (int i = 0; i < n; i++) {
            std::wstring t = BookChapterTitle(g_book, i);
            /* 超过 40 字就截断：列表框不做自动省略，太长的条目会被硬切出半个汉字，
             * 看着比主动截断更糟。
             */
            if (t.size() > 40) t = t.substr(0, 40) + L"…";
            SendMessageW(s_hToc, LB_ADDSTRING, 0, (LPARAM)t.c_str());
        }
    }
    SendMessageW(s_hToc, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_hToc, nullptr, TRUE);
}

/* 把一条命中渲染成列表里看得懂的一行文字。参数 h 是命中记录，返回单行字符串。
 * 输出形如：第十二章 初入宗门 · …前文【关键词】后文…
 * 上下文窗口取「前 12、后 24」个字符：眼睛的落点在关键词上，后面多露一点更能
 * 判断「这是不是我要找的那处」。两端都要夹到 [0, text.size()]，否则 substr 抛异常。
 * 关键词用【】包起来而不是加颜色/加粗：LISTBOX 不支持富文本，全角方括号在中文里
 * 视觉重量足够，也不用额外算宽度。
 * 边界：换行一律换成空格（列表条目不能带换行，否则行高被撑破）；h.chapter < 0
 * （开篇文字不属于任何章）时前缀显示「开篇」；章名超过 12 字截断，否则上下文没地方。
 */
static std::wstring HitLabel(const Hit& h)
{
    const std::wstring& t = g_book.text;
    int from = h.off - 12;
    if (from < 0) from = 0;
    int to = h.off + h.len + 24;
    if (to > (int)t.size()) to = (int)t.size();
    std::wstring mid = t.substr((size_t)from, (size_t)(h.off - from));
    mid += L"【";
    mid += t.substr((size_t)h.off, (size_t)h.len);
    mid += L"】";
    mid += t.substr((size_t)(h.off + h.len), (size_t)(to - h.off - h.len));
    for (size_t i = 0; i < mid.size(); i++) {
        if (mid[i] == L'\n' || mid[i] == L'\r') mid[i] = L' ';
    }
    std::wstring ch = h.chapter >= 0 ? BookChapterTitle(g_book, h.chapter) : L"";
    if (ch.size() > 12) ch = ch.substr(0, 12) + L"…";
    if (ch.empty()) ch = L"开篇";
    return ch + L" · " + mid;
}

/* 参数与返回值约定（函数本身很短，重点全在这几条约定上）：
 * dc  ：已选好界面字体的设备上下文 —— 宽度必须用「将要显示它的那个字体」来量，
 *       换了字体就得重新裁，这是必须传 HDC 的原因。
 * s   ：原始字符串；maxW：允许的最大像素宽度（不是字符数！）。
 * 返回：放得进 maxW 的最长前缀 + 「…」；本来就放得下则原样返回。
 * 为什么按像素而不是按字数：中英文数字混排时同样字数量出来的宽度能差一倍，
 * 按字数裁照样会溢出。
 * 边界：空串或 maxW <= 0 直接原样返回；不把省略号本身的宽度计入 maxW，
 * 允许轻微超出，这点误差远小于重算一次的成本，视觉上无感。
 */
/* 按像素宽度裁剪字符串（列表条目太长会溢出侧栏），末尾补省略号 */
static std::wstring ClipToWidth(HDC dc, const std::wstring& s, int maxW)
{
    if (s.empty() || maxW <= 0) return s;
    SIZE sz;
    sz.cx = sz.cy = 0;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    if (sz.cx <= maxW) return s;
    /* 二分：找「最长的、量出来不超过 maxW 的前缀长度」——宽度对长度单调不减，可以二分 */
    int lo = 0, hi = (int)s.size();
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        GetTextExtentPoint32W(dc, s.c_str(), mid, &sz);
        if (sz.cx <= maxW) lo = mid; else hi = mid - 1;
    }
    std::wstring t = s.substr(0, (size_t)lo);
    t += L"…";
    return t;
}

/* 把当前命中集渲染进搜索结果列表（整表重建，与 FillToc 同构）。
 * 两个容易踩的点：
 * 1) 宽度用「将要变成的面板宽度」而不是 GetClientRect：调用顺序上 FillHits 可能
 *    早于 ShowPanel / UiLayout，此刻列表还按旧的 232 宽摆着；若按当时的客户区裁，
 *    等面板展开到 320，条目就会显得莫名其妙地短。按「即将生效的」宽度算一次到位。
 * 2) 量宽度必须用界面字体：GetDC 拿到的默认字体比界面字体小一号，量出来对不上，
 *    会裁得过宽或过窄，所以先 SelectObject(s_uiFont) 再量，用完换回去。
 * 其余：WM_SETREDRAW 夹住避免闪；LB_INITSTORAGE 按每条约 120 字节预分配；
 * 没有命中时放一行「（没有找到：关键词）」，让「搜过但没结果」与「还没搜」可区分。
 */
static void FillHits()
{
    if (!s_hHits) return;
    NrTrace(L"FillHits n=%d", (int)s_hits.size());
    /* 搜索结果列表按侧栏宽度裁剪，避免长条目被硬切；
       面板宽度用“将要变成”的值（搜索结果面板更宽），因为 FillHits 可能先于 UiLayout 调用 */
    int panelW = g_set.sidebar ? (s_panel != 0 ? S(320) : S(232)) : S(232);
    /* 再留 S(30) 给列表项左右内边距、WS_EX_CLIENTEDGE 边框和可能出现的竖向滚动条 */
    int maxW = panelW - S(30);
    /* 借列表自己的 DC 来量文字：同一窗口类，字体选择行为一致 */
    HDC dc = GetDC(s_hHits);
    HFONT old = s_uiFont ? (HFONT)SelectObject(dc, s_uiFont) : nullptr;

    SendMessageW(s_hHits, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s_hHits, LB_RESETCONTENT, 0, 0);
    if (s_hits.empty()) {
        std::wstring m = L"（没有找到：";
        m += s_kw;
        m += L"）";
        SendMessageW(s_hHits, LB_ADDSTRING, 0, (LPARAM)m.c_str());
    } else {
        int n = (int)s_hits.size();
        SendMessageW(s_hHits, LB_INITSTORAGE, n, (LPARAM)(n * 120));
        for (int i = 0; i < n; i++) {
            std::wstring s = ClipToWidth(dc, HitLabel(s_hits[(size_t)i]), maxW);
            SendMessageW(s_hHits, LB_ADDSTRING, 0, (LPARAM)s.c_str());
        }
    }

    if (old) SelectObject(dc, old);
    ReleaseDC(s_hHits, dc);
    SendMessageW(s_hHits, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_hHits, nullptr, TRUE);
}

/* ---------------- 书签 ---------------- */

/* 当前书的书签，元素是 "字符偏移|标题" 的原始串，顺序 = 用户添加的顺序。
 * 为什么直接存这种串而不定义结构体：它既是 ini 里的存储格式，也是内存里的格式，
 * 中间少一层来回转换；解析只在"显示 / 跳转"时做一次，成本可忽略。 */
static std::vector<std::wstring> s_bms;

/* 带书签的段落序号（升序、去重）。view.cpp 画正文时靠它在段落左边画标记，
 * 因为是二分查找，所以必须保持有序。 */
std::vector<int> g_bmParas;

/* 从 "偏移|标题" 里解析出偏移。格式坏了（没有 '|'、含非数字、数值离谱）返回 -1，
 * 调用方跳过这条 —— 一条坏数据不该让整个面板或跳转崩掉。 */
static int BmParseOffset(const std::wstring& item)
{
    size_t bar = item.find(L'|');
    if (bar == std::wstring::npos || bar == 0) return -1;
    int off = 0;
    for (size_t i = 0; i < bar; i++) {
        wchar_t c = item[i];
        if (c < L'0' || c > L'9') return -1;
        off = off * 10 + (int)(c - L'0');
        if (off > 100000000) return -1;   /* 1 亿字符以上当作坏数据 */
    }
    return off;
}

/* 取出 "偏移|标题" 里的标题部分 */
static std::wstring BmParseTitle(const std::wstring& item)
{
    size_t bar = item.find(L'|');
    if (bar == std::wstring::npos || bar + 1 >= item.size()) return std::wstring();
    return item.substr(bar + 1);
}

/* 重建 g_bmParas：把每条书签的偏移换算成段落号，排序去重。 */
static void BmRebuildParas()
{
    g_bmParas.clear();
    if (!g_book.Ok()) return;
    int len = (int)g_book.text.size();
    for (size_t i = 0; i < s_bms.size(); i++) {
        int off = BmParseOffset(s_bms[i]);
        if (off < 0 || off > len) continue;
        g_bmParas.push_back(BookCharToPara(g_book, off));
    }
    std::sort(g_bmParas.begin(), g_bmParas.end());
    g_bmParas.erase(std::unique(g_bmParas.begin(), g_bmParas.end()), g_bmParas.end());
}

/* 打开一本书之后调：把这本书的书签从 ini 读进内存，并重建段落标记。 */
static void BmReload()
{
    s_bms = BookmarkLoad(g_book.path);
    BmRebuildParas();
    NrTrace(L"BmReload n=%d", (int)s_bms.size());
}

/* 把书签填进侧栏列表（复用搜索结果那个 LISTBOX：s_panel == 2 时它就是书签列表，
 * 这样不用再建一个控件、也不用心维护两套布局）。 */
static void FillBookmarks()
{
    if (!s_hHits) return;
    int panelW = g_set.sidebar ? S(320) : S(232);
    int maxW = panelW - S(30);
    HDC dc = GetDC(s_hHits);
    HFONT old = s_uiFont ? (HFONT)SelectObject(dc, s_uiFont) : nullptr;

    SendMessageW(s_hHits, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s_hHits, LB_RESETCONTENT, 0, 0);
    if (s_bms.empty()) {
        SendMessageW(s_hHits, LB_ADDSTRING, 0, (LPARAM)L"（还没有书签：Ctrl+D 给当前位置加一条）");
    } else {
        for (size_t i = 0; i < s_bms.size(); i++) {
            std::wstring t = BmParseTitle(s_bms[i]);
            if (t.empty()) t = L"（未命名位置）";
            std::wstring s = ClipToWidth(dc, t, maxW);
            SendMessageW(s_hHits, LB_ADDSTRING, 0, (LPARAM)s.c_str());
        }
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(s_hHits, dc);
    SendMessageW(s_hHits, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_hHits, nullptr, TRUE);
}

/* 给当前阅读位置加一条书签（Ctrl+D / 菜单）。
 * 落点规则：取"当前视口顶行所在的段落"的段首偏移 —— 比顶行偏移稳定（滚到段中间时，
 * 同一个段落再加书签仍然指向段首，不会出现两条书签挤在同一段里）。
 * 标题 = 章名 + 该段前 24 个字，这样列表里能看出"这是哪一章的哪一段"。
 * 同一段已有书签时替换标题（相当于"重新标记"），不重复添加。 */
void UiAddBookmark()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一本书。", L"提示", MB_ICONINFORMATION);
        return;
    }
    int off = ViewAnchorOffset();
    if (off < 0) off = 0;
    if (off > (int)g_book.text.size()) off = (int)g_book.text.size();
    int para = BookCharToPara(g_book, off);
    if (para >= 0 && para < g_book.ParaCount()) off = (int)g_book.paraOff[(size_t)para];

    std::wstring title;
    int ch = BookChapterAtPara(g_book, para);
    if (ch >= 0) { title = BookChapterTitle(g_book, ch); title += L" · "; }
    std::wstring txt = BookParaText(g_book, para);
    if (txt.size() > 24) { txt.resize(24); txt += L"…"; }
    title += txt;
    if (title.empty()) title = L"（未命名位置）";

    wchar_t o[24];
    wsprintfW(o, L"%d|", off);
    std::wstring item = o + title;

    bool replaced = false;
    for (size_t i = 0; i < s_bms.size(); i++) {
        int o2 = BmParseOffset(s_bms[i]);
        if (o2 >= 0 && BookCharToPara(g_book, o2) == para) { s_bms[i] = item; replaced = true; break; }
    }
    if (!replaced) {
        if (s_bms.size() >= 200) {
            MessageBoxW(g_hMain, L"书签最多 200 条，先删几条吧。", L"提示", MB_ICONINFORMATION);
            return;
        }
        s_bms.push_back(item);
    }
    BookmarkSave(g_book.path, s_bms);
    BmRebuildParas();
    if (s_panel == 2) FillBookmarks();
    ViewInvalidate();
    s_statusExtra = replaced ? L"书签已更新" : L"已添加书签";
    UiUpdateStatus();
}

/* 删掉"当前这一段"上的书签（菜单项，没有快捷键 —— 删数据类操作不适合给顺手键）。 */
void UiDelBookmark()
{
    if (!g_book.Ok()) return;
    int off = ViewAnchorOffset();
    if (off < 0) off = 0;
    if (off > (int)g_book.text.size()) off = (int)g_book.text.size();
    int para = BookCharToPara(g_book, off);

    bool removed = false;
    for (size_t i = 0; i < s_bms.size(); i++) {
        int o2 = BmParseOffset(s_bms[i]);
        if (o2 >= 0 && BookCharToPara(g_book, o2) == para) {
            s_bms.erase(s_bms.begin() + (std::ptrdiff_t)i);
            removed = true;
            break;
        }
    }
    if (!removed) {
        MessageBoxW(g_hMain, L"这一段上没有书签。", L"提示", MB_ICONINFORMATION);
        return;
    }
    BookmarkSave(g_book.path, s_bms);
    BmRebuildParas();
    if (s_panel == 2) FillBookmarks();
    ViewInvalidate();
    s_statusExtra = L"书签已删除";
    UiUpdateStatus();
}

/* 打开书签面板（没书签也打开，里面有一行提示告诉用户怎么加）。 */
void UiShowBookmarks()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一本书。", L"提示", MB_ICONINFORMATION);
        return;
    }
    FillBookmarks();
    ShowPanel(2);
}

/* ---------------- 高亮与笔记 ---------------- */

/* 高亮条目在侧栏列表里的编码，跟书签一样是 "偏移|显示文字"：
 * 列表只存字符串，偏移藏在文字前面的数字里，点击时反解出来即可，
 * 不必再维护一份与列表索引一一对应的数组（那种做法在插入/删除后会错位）。
 * 注意：高亮自己有 off/len 两份数据在 g_hls 里，这里只是"哪个高亮"的定位。 */

/* 取一条高亮的显示文字：章名 + 该处前后的原文摘要。
 * 摘要起点要往前退一点（S(0) 不够，读者需要上下文才认得出是哪一段），
 * 但退太多会把上一段的内容也带进来，所以定在 8 个字。 */
static std::wstring HlLabel(const Highlight& h)
{
    if (!g_book.Ok() || h.off < 0 || h.off >= (int)g_book.text.size()) return std::wstring();
    int ch = BookChapterAtPara(g_book, BookCharToPara(g_book, h.off));
    std::wstring name = BookChapterTitle(g_book, ch);
    int from = h.off - 8;
    if (from < 0) from = 0;
    /* 不跨段：从段首起，否则摘要会带着上一段的尾巴，看起来很怪 */
    int paraStart = (int)g_book.paraOff[(size_t)BookCharToPara(g_book, h.off)];
    if (from < paraStart) from = paraStart;
    int to = h.off + h.len + 24;
    if (to > (int)g_book.text.size()) to = (int)g_book.text.size();
    std::wstring mid = g_book.text.substr((size_t)from, (size_t)(to - from));
    /* 换行会让列表框里的一行断成两行，摘要里不能有换行 */
    for (size_t i = 0; i < mid.size(); i++) if (mid[i] == L'\n' || mid[i] == L'\r') mid[i] = L' ';
    if (!name.empty()) return name + L" · " + mid;
    return mid;
}

/* 把 g_hls 填进侧栏列表（s_panel == 3 时 s_hHits 就是高亮列表）。
 * 有笔记的高亮在末尾加一个 ✎ 记号，让读者一眼看出"这条我写过东西"。 */
static void FillHighlights()
{
    if (!s_hHits) return;
    int panelW = g_set.sidebar ? S(320) : S(232);
    int maxW = panelW - S(30);
    SendMessageW(s_hHits, LB_RESETCONTENT, 0, 0);
    if (g_hls.empty()) {
        SendMessageW(s_hHits, LB_ADDSTRING, 0, (LPARAM)L"（还没有高亮：选中正文后按 Ctrl+H）");
        return;
    }
    HDC dc = GetDC(s_hHits);
    for (size_t i = 0; i < g_hls.size(); i++) {
        std::wstring label = HlLabel(g_hls[i]);
        if (!g_hls[i].note.empty()) label += L"  ✎";
        /* 列表里只放"给人看"的文字，偏移不进字符串 —— g_hls 是按 off 排好序的，
         * 列表下标和它对得上（FillHighlights 每次都整表重建），不需要像书签那样
         * 把偏移编进条目里。之前带上 "偏移|" 前缀，结果列表里真的显示了 "0|"。 */
        SendMessageW(s_hHits, LB_ADDSTRING, 0, (LPARAM)ClipToWidth(dc, label, maxW).c_str());
    }
    ReleaseDC(s_hHits, dc);
}

/* 高亮按起点排序的比较器。
 * 用非捕获的普通函数而不是 lambda：std::sort 每次都传同一份，避免每调用一次就生成
 * 一个新的 lambda 类型（虽然模板会合并，但写成函数更直观，也方便别处复用）。
 * 起点相同就按长度短的在前 —— 这样套叠在一起的高亮画出来时，短的在上面，看不漏。 */
static bool HlLess(const Highlight& a, const Highlight& b)
{
    if (a.off != b.off) return a.off < b.off;
    return a.len < b.len;
}

/* 加高亮：优先用当前选中的范围，没有选中就取"当前阅读位置那一段"。
 * 参数 color 是 0..3 的配色序号（黄/绿/蓝/粉），越界夹住。
 * 为什么允许"没选中就高亮整段"：读网文时想标记的是"这一段写得好"，
 * 让用户先精确拖选一遍反而烦；要改精确范围可以删掉重来。 */
void UiAddHighlight(int color)
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一本书。", L"提示", MB_ICONINFORMATION);
        return;
    }
    if (color < 0) color = 0;
    if (color > 3) color = 3;
    int a = 0, b = 0;
    Highlight h;
    if (ViewSelection(&a, &b) && b > a) {
        h.off = a;
        h.len = b - a;
    } else {
        int off = ViewAnchorOffset();
        if (off < 0) off = 0;
        if (off > (int)g_book.text.size()) off = (int)g_book.text.size();
        int para = BookCharToPara(g_book, off);
        int s = (int)g_book.paraOff[(size_t)para];
        int e = (int)g_book.paraOff[(size_t)para + 1];
        while (e > s && (g_book.text[(size_t)e - 1] == L'\n' || g_book.text[(size_t)e - 1] == L'\r')) e--;
        if (e <= s) {
            MessageBoxW(g_hMain, L"这一行没有正文可标记。", L"提示", MB_ICONINFORMATION);
            return;
        }
        h.off = s;
        h.len = e - s;
    }
    h.color = color;
    if (g_hls.size() >= 2000) {
        MessageBoxW(g_hMain, L"这本书的高亮已经到 2000 条上限了，先删一些再标。", L"提示", MB_ICONINFORMATION);
        return;
    }
    g_hls.push_back(h);
    std::sort(g_hls.begin(), g_hls.end(), HlLess);
    HighlightSave(g_book.path);
    if (s_panel == 3) FillHighlights();
    ViewInvalidate();
    wchar_t msg[64];
    wsprintfW(msg, L"已高亮 %d 个字", h.len);
    s_statusExtra = msg;
    UiUpdateStatus();
}

/* 删掉"当前阅读位置"覆盖到的那条高亮。
 * 为什么按位置而不是按列表选中项：正文里点到哪就删哪，比"先切到列表、再找到条目、
 * 再删"顺手得多；列表里要删也行 —— 点条目会先跳过去，再按 Ctrl+Shift+H 就是删它。 */
void UiRemoveHighlight()
{
    if (!g_book.Ok()) return;
    if (g_hls.empty()) {
        MessageBoxW(g_hMain, L"这本书还没有高亮。", L"提示", MB_ICONINFORMATION);
        return;
    }
    int off = ViewAnchorOffset();
    int len = (int)g_book.text.size();
    bool removed = false;
    /* 从后往前扫：删掉最后一条，前面的下标不受影响 */
    for (int i = (int)g_hls.size() - 1; i >= 0; i--) {
        const Highlight& h = g_hls[(size_t)i];
        if (off >= h.off && off < h.off + h.len) {
            g_hls.erase(g_hls.begin() + i);
            removed = true;
            break;
        }
        /* 光标正好落在末尾（读者习惯把光标停在刚读过的地方）也算命中 */
        if (off >= len - 1 && h.off + h.len >= len - 1 && h.off <= off) {
            g_hls.erase(g_hls.begin() + i);
            removed = true;
            break;
        }
    }
    if (!removed) {
        MessageBoxW(g_hMain, L"当前位置没有高亮。把光标放到高亮的文字上再试。", L"提示", MB_ICONINFORMATION);
        return;
    }
    HighlightSave(g_book.path);
    if (s_panel == 3) FillHighlights();
    ViewInvalidate();
    s_statusExtra = L"高亮已删除";
    UiUpdateStatus();
}

/* 一个极简的文本输入框（写笔记用）。
 * 为什么不用现成的 DialogBox + 资源：本程序没有 .rc 里的对话框模板（资源里只有图标
 * 和 manifest），自己建一个窗口比维护模板文件更省事，而且能和主题配色保持一致。
 * 返回值：true = 用户点了确定（out 里是文本，可能是空串 —— 表示清空笔记）；
 * false = 取消或直接关窗，调用方不要改数据。 */
static HWND s_txtEdit = nullptr;
static bool s_txtDone = false;
static std::wstring s_txtText;

static LRESULT CALLBACK TxtBoxProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        /* 提示行 + 多行编辑框 + 两个按钮。坐标写死成 S() 缩放后的像素，
         * 因为这个窗口尺寸固定（不符合 DPI 会在这里崩），不做自由布局。 */
        CreateWindowExW(0, L"STATIC", L"笔记内容（可以直接留空表示清空这条笔记）",
                        WS_CHILD | WS_VISIBLE, S(14), S(12), S(420), S(20),
                        h, nullptr, nullptr, nullptr);
        s_txtEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                    L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                                    S(14), S(38), S(432), S(170), h, (HMENU)1, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(268), S(220), S(84), S(28), h, (HMENU)2, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(362), S(220), S(84), S(28), h, (HMENU)3, nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2) {
            /* 先把文本取出来再销毁：窗口没了就读不到了 */
            int n = GetWindowTextLengthW(s_txtEdit);
            std::wstring t;
            if (n > 0) {
                t.resize((size_t)n + 1);
                int got = GetWindowTextW(s_txtEdit, &t[0], n + 1);
                t.resize((size_t)(got > 0 ? got : 0));
            }
            s_txtText = t;
            s_txtDone = true;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == 3) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool AskText(const wchar_t* caption, const std::wstring& init, std::wstring& out)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE);
    const wchar_t* kCls = L"NRTxtBox";
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = TxtBoxProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kCls;
        if (!RegisterClassExW(&wc)) return false;
        s_reg = true;
    }
    RECT pr;
    GetWindowRect(g_hMain, &pr);
    int w = S(470), h = S(300);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    s_txtEdit = nullptr;
    s_txtDone = false;
    s_txtText.clear();
    HWND box = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls, caption,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, w, h, g_hMain, nullptr, inst, nullptr);
    if (!box) return false;
    if (s_txtEdit && !init.empty()) SetWindowTextW(s_txtEdit, init.c_str());
    if (s_txtEdit) SetFocus(s_txtEdit);
    EnableWindow(g_hMain, FALSE);
    MSG msg;
    while (IsWindow(box) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(box, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
    if (!s_txtDone) return false;
    out = s_txtText;
    return true;
}

/* 导出笔记：写成 Markdown，方便贴到别处或存进网盘。
 * 只导有笔记的高亮（纯高亮没有文字内容，导出去是一堆原文片段，没意义）。
 * 文件编码用 UTF-8 带 BOM，记事本打开才不会乱码（和 web 抓书的落盘一致）。 */
void UiExportNotes()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一本书。", L"提示", MB_ICONINFORMATION);
        return;
    }
    int n = 0;
    for (size_t i = 0; i < g_hls.size(); i++) if (!g_hls[i].note.empty()) n++;
    if (n == 0) {
        MessageBoxW(g_hMain, L"这本书还没有写过笔记。先用 Ctrl+Shift+N 给高亮写一条。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s-笔记.md", g_book.title.c_str());
    /* 文件名里的 \ / : * ? " < > | 会让保存失败，统一换成下划线 */
    for (wchar_t* p = path; *p; p++) {
        if (*p == L'\\' || *p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
            *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|') *p = L'_';
    }
    std::wstring text;
    text += L"# " + g_book.title + L" 的笔记\n\n";
    for (size_t i = 0; i < g_hls.size(); i++) {
        const Highlight& h = g_hls[(size_t)i];
        if (h.note.empty()) continue;
        wchar_t head[64];
        int ch = BookChapterAtPara(g_book, BookCharToPara(g_book, h.off));
        std::wstring name = BookChapterTitle(g_book, ch);
        if (name.empty()) name = L"（开篇）";
        wsprintfW(head, L"## %s\n\n", name.c_str());
        text += head;
        /* 原文摘录：把换行压成空格，方便在 Markdown 里当成引用块 */
        int to = h.off + h.len;
        if (to > (int)g_book.text.size()) to = (int)g_book.text.size();
        std::wstring quote = g_book.text.substr((size_t)h.off, (size_t)(to - h.off));
        for (size_t k = 0; k < quote.size(); k++) if (quote[k] == L'\n' || quote[k] == L'\r') quote[k] = L' ';
        text += L"> " + quote + L"\n\n";
        text += h.note + L"\n\n";
    }
    if (!WriteTextFileUtf8Bom(path, text)) {
        MessageBoxW(g_hMain, L"写文件失败（目录不可写？）", L"导出笔记", MB_ICONWARNING);
        return;
    }
    std::wstring msg = L"已导出到：\n" + std::wstring(path) + L"\n\n现在打开看看吗？";
    if (MessageBoxW(g_hMain, msg.c_str(), L"导出笔记", MB_ICONINFORMATION | MB_YESNO) == IDYES) {
        ShellExecuteW(g_hMain, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

/* 给一条高亮写/改笔记。
 * 目标高亮的找法：先看"当前阅读位置是否落在某条高亮里"（读者刚标记完、刚跳过来，
 * 光标就在那儿）；没有的话，如果高亮列表正开着，就用列表里选中的那条。
 * 两者都没有就提示用户先标一条 —— 笔记必须挂在某条高亮上，否则它没有位置可回顾。 */
void UiEditNote()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一本书。", L"提示", MB_ICONINFORMATION);
        return;
    }
    if (g_hls.empty()) {
        MessageBoxW(g_hMain, L"这本书还没有高亮。先选中文字按 Ctrl+H 标一条，再写笔记。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    int idx = -1;
    int off = ViewAnchorOffset();
    for (size_t i = 0; i < g_hls.size(); i++) {
        const Highlight& h = g_hls[i];
        if (off >= h.off && off < h.off + h.len) { idx = (int)i; break; }
    }
    if (idx < 0 && s_panel == 3 && s_hHits) {
        /* 列表里选中的那条：条目字符串是 "偏移|文字"，偏移能直接反解 */
        int sel = (int)SendMessageW(s_hHits, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)g_hls.size()) idx = sel;
    }
    if (idx < 0) {
        MessageBoxW(g_hMain, L"把光标放到高亮的文字上（或在高亮列表里点一条）再写笔记。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    std::wstring note = g_hls[(size_t)idx].note;
    std::wstring input;
    if (!AskText(L"写笔记", note, input)) return;
    g_hls[(size_t)idx].note = input;
    HighlightSave(g_book.path);
    if (s_panel == 3) FillHighlights();
    s_statusExtra = input.empty() ? L"笔记已清空" : L"笔记已保存";
    UiUpdateStatus();
}

/* 打开高亮列表（没高亮也打开，里面有一行提示）。 */
void UiShowHighlights()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一本书。", L"提示", MB_ICONINFORMATION);
        return;
    }
    FillHighlights();
    ShowPanel(3);
}

/* ---------------- 快捷键设置 ---------------- */

/* 当前的加速键表。1.6.0 起用户可以改键，改完必须换一张新表：
 * 加速键表是 CreateAcceleratorTableW 建的内核对象，内容不可变，只能销毁重建。
 * 消息循环每轮都问 UiAccelHandle()，所以换表后立刻生效，不必重启程序。 */
static HACCEL s_accel = nullptr;

HACCEL UiAccelHandle()
{
    return s_accel;
}

void UiRebuildAccel()
{
    if (s_accel) {
        DestroyAcceleratorTable(s_accel);
        s_accel = nullptr;
    }
    s_accel = BuildAccel();
}

/* 快捷键列表里的一行文字："功能名  ——  当前键（或"未设置"）" */
static std::wstring KeyRowText(const KeyBind& b)
{
    std::wstring s = b.action;
    s += L"    ";
    /* 用全角空格而不是制表符：LISTBOX 里制表符会被显示成一个空格，对不齐 */
    s += b.cur.empty() ? L"（未设置）" : b.cur;
    return s;
}

static HWND s_keysList = nullptr;
static HWND s_keysHint = nullptr;

static void KeysFillList()
{
    if (!s_keysList) return;
    int sel = (int)SendMessageW(s_keysList, LB_GETCURSEL, 0, 0);
    SendMessageW(s_keysList, LB_RESETCONTENT, 0, 0);
    std::vector<KeyBind>& binds = KeyBinds();
    for (size_t i = 0; i < binds.size(); i++) {
        std::wstring t = KeyRowText(binds[i]);
        SendMessageW(s_keysList, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    if (sel >= 0 && sel < (int)binds.size()) SendMessageW(s_keysList, LB_SETCURSEL, sel, 0);
}

/* 列表子类化：把"在列表上按键"解释成"给这一行设快捷键"。
 * 为什么在列表上按键而不是弹一个"请按键"的小窗：用户刚点了某一行，手就在键盘上，
 * 直接按最省事；而且不需要额外窗口来抢焦点。
 * 规则：
 *   - 只按修饰键（Ctrl/Shift/Alt/Win）不算，等真的按到主键；
 *   - Esc = 取消这条绑定（相当于"这个功能不要快捷键"）；
 *   - Delete/Backspace = 清空绑定；
 *   - 其余组合键写回 KeyBinds()[sel].cur，只改内存，点「确定」才落盘。 */
static LRESULT CALLBACK KeysListSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        int sel = (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
        std::vector<KeyBind>& binds = KeyBinds();
        if (sel < 0 || sel >= (int)binds.size()) return 0;
        int vk = (int)w;
        /* 单独按修饰键：不当作绑定，等主键 */
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN) {
            return 0;
        }
        if (vk == VK_ESCAPE || vk == VK_DELETE || vk == VK_BACK) {
            binds[(size_t)sel].cur.clear();
            KeysFillList();
            SetWindowTextW(s_keysHint, L"已清空这个功能的快捷键。");
            return 0;
        }
        BYTE mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
        /* 没按修饰键时，F1~F24 与功能键（PrintScreen 等）可以单独用；
         * 普通字母数字不加修饰会抢走正文里的输入，强制要求带 Ctrl/Alt。 */
        bool isFn = (vk >= VK_F1 && vk <= VK_F24);
        if (mods == 0 && !isFn) {
            SetWindowTextW(s_keysHint, L"普通按键请加上 Ctrl 或 Alt（否则打字时会误触发）。");
            return 0;
        }
        binds[(size_t)sel].cur = KeysFormat((BYTE)vk, mods);
        KeysFillList();
        SetWindowTextW(s_keysHint, L"按「确定」保存，按「恢复默认」全部还原。");
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK KeysBoxProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC",
                        L"点一行，然后直接按下想要的新组合键。Esc 或 Delete = 清空这条。",
                        WS_CHILD | WS_VISIBLE, S(14), S(10), S(520), S(20), h, nullptr, nullptr, nullptr);
        s_keysList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX",
                                     L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                     S(14), S(34), S(520), S(360), h, (HMENU)1, nullptr, nullptr);
        SetWindowSubclass(s_keysList, KeysListSub, 1, 0);
        s_keysHint = CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | WS_VISIBLE, S(14), S(400), S(520), S(20),
                                     h, (HMENU)5, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(268), S(426), S(84), S(28), h, (HMENU)2, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"恢复默认", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(360), S(426), S(100), S(28), h, (HMENU)3, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(468), S(426), S(66), S(28), h, (HMENU)4, nullptr, nullptr);
        KeysFillList();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2) {
            KeysSave();
            UiRebuildAccel();
            /* 老板键不在加速键表里（它是全局热键），改完得单独同步到
             * g_set.bossKey 再重新注册，否则改了等于没改。 */
            {
                std::vector<KeyBind>& binds = KeyBinds();
                for (size_t i = 0; i < binds.size(); i++) {
                    if (binds[i].id == IDM_BOSS) {
                        g_set.bossKey = binds[i].cur;
                        if (g_set.bossKey.empty()) g_set.bossKey = L"Ctrl+Alt+H";
                        SettingsSave();
                        UiApplyBossKey();
                        break;
                    }
                }
            }
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == 3) {
            /* 恢复默认：把每条 cur 填回 def，不清空表 */
            std::vector<KeyBind>& binds = KeyBinds();
            for (size_t i = 0; i < binds.size(); i++) binds[i].cur = binds[i].def;
            KeysFillList();
            SetWindowTextW(s_keysHint, L"已恢复默认，点「确定」保存。");
            return 0;
        }
        if (LOWORD(w) == 4) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 快捷键设置对话框（菜单 阅读 → 快捷键设置 / Ctrl+F12）。
 * 改完立刻存 ini 并换加速键表 —— 不做"确定/取消"两套状态备份，
 * 因为一旦用户看到"确定"，就会以为不点确定也能用；干脆就一个「确定」。 */
void UiKeysDialog()
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(g_hMain, GWLP_HINSTANCE);
    const wchar_t* kCls = L"NRKeysBox";
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = KeysBoxProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kCls;
        if (!RegisterClassExW(&wc)) return;
        s_reg = true;
    }
    RECT pr;
    GetWindowRect(g_hMain, &pr);
    int w = S(560), hgt = S(500);
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - hgt) / 2;
    HWND box = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls, L"快捷键设置",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, w, hgt, g_hMain, nullptr, inst, nullptr);
    if (!box) return;
    EnableWindow(g_hMain, FALSE);
    MSG msg;
    while (IsWindow(box) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(box, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
}

/* ---------------- 排版（每本书独立） ---------------- */

/* 排版设置改完的统一出口：存盘 → 让 view 重算度量 → 重画 → 状态栏提示。
 * 参数 relayout 表示"段落换行结果会变"（字号、字体、栏宽、首行缩进、页边距都算），
 * 这时必须让 view 把每段的行数缓存全部作废；只改页面模式（分页开关）时行数不变，
 * 传 false 可以省掉一次全书重排 —— 8000 章的书重排要几百毫秒，能省就省。 */
void UiLayoutChanged(bool relayout)
{
    if (!g_book.Ok()) return;
    LayoutSave(g_book.path);
    ViewMetricsChanged(relayout);
    ViewInvalidate();
    UiUpdateStatus();
}



/* 首行缩进开关（中文小说习惯缩两格，英文/技术书不缩） */
static void LayoutToggleIndent()
{
    g_lay.indent = g_lay.indent ? 0 : 1;
    s_statusExtra = g_lay.indent ? L"首行缩进：开" : L"首行缩进：关";
    UiLayoutChanged(true);
}

/* 分页模式开关。
 * 关掉分页时必须顺带把双栏收成单栏：双栏靠"一屏两栏"来排，连续滚动时两栏的
 * 位置没法同步（左栏滚到一半，右栏该显示哪一段？），所以双栏本来就强制分页。 */
static void LayoutTogglePaged()
{
    g_lay.paged = g_lay.paged ? 0 : 1;
    if (!g_lay.paged && g_lay.cols >= 2) g_lay.cols = 1;
    s_statusExtra = g_lay.paged ? L"分页模式：开" : L"分页模式：关";
    UiLayoutChanged(true);
}

/* 双栏开关：开双栏自动打开分页（理由同上） */
static void LayoutToggleCols()
{
    if (g_lay.cols >= 2) {
        g_lay.cols = 1;
        s_statusExtra = L"单栏";
    } else {
        g_lay.cols = 2;
        g_lay.paged = 1;
        s_statusExtra = L"双栏（自动开启分页）";
    }
    UiLayoutChanged(true);
}

/* "强制字体"：忽略每本书自己的字体，统一用全局字体。
 * 用途：有些人给某本书设了仿宋，后来机器上卸了这个字体，每次打开都退回雅黑；
 * 打开强制字体就不用逐本去改。 */
static void LayoutToggleForceFont()
{
    g_lay.forceFont = g_lay.forceFont ? 0 : 1;
    s_statusExtra = g_lay.forceFont ? L"强制使用全局字体" : L"使用本书字体";
    UiLayoutChanged(true);
}

/* 恢复本书排版默认值：字号/行距/字体回到全局默认，其余回出厂值 */
static void LayoutReset()
{
    LayoutFromSettings();
    s_statusExtra = L"本书排版已恢复默认";
    UiLayoutChanged(true);
}

/* ---------------- 隐私与老板键 ---------------- */

/* 标题栏 / 顶栏 / 状态栏显示的"书名"要不要真的显示。
 * 隐私模式下统一显示这个占位词：别人从任务栏、截图、甚至 Alt+Tab 预览里
 * 都看不出你在读什么书。 */
std::wstring UiDisplayTitle()
{
    if (!g_book.Ok()) return L"小说阅读器";
    if (g_set.privacy) return L"文档";
    return g_book.title;
}

/* 按当前状态刷标题栏。开书、关书、切隐私模式都调它，避免三处各写一份拼串逻辑。 */
static void ApplyWindowTitle()
{
    std::wstring cap;
    if (!g_book.Ok()) cap = L"小说阅读器";
    else cap = L"小说阅读器 - " + UiDisplayTitle();
    SetWindowTextW(g_hMain, cap.c_str());
}

/* 隐私模式开关：只影响"显示"，不影响任何读写 —— 书还是那本书，进度照存。 */
void UiTogglePrivacy()
{
    g_set.privacy = g_set.privacy ? 0 : 1;
    SettingsSave();
    ApplyWindowTitle();
    UiLayout();          /* 顶栏要重画（书名变了） */
    UiUpdateStatus();
    s_statusExtra = g_set.privacy ? L"隐私模式：开（标题已隐藏）" : L"隐私模式：关";
    UiUpdateStatus();
}

/* 注册/重新注册老板键。
 * 老板键只能用 RegisterHotKey 实现：程序被隐藏、没有焦点时也要能把窗口叫回来，
 * 而普通键盘消息只在有焦点时到窗口。
 * 注册失败（被别的程序占了同一个组合）不算错误流程，只记下来在对话框里提示。 */
static bool s_bossOk = false;
void UiApplyBossKey()
{
    UnregisterHotKey(g_hMain, 1);
    s_bossOk = false;
    BYTE vk = 0, mods = 0;
    if (!KeysParse(g_set.bossKey, &vk, &mods)) return;
    /* RegisterHotKey 用的是 MOD_* 那一套，和 KeysParse 输出的正好一致 */
    s_bossOk = RegisterHotKey(g_hMain, 1, (UINT)mods, (UINT)vk) != FALSE;
    /* 注册失败不弹框（启动阶段用户只想看书），只在状态栏挂一句后缀；
     * 真正要看这个结果的时机是用户刚改完老板键，那时状态栏正好就在眼前。 */
    if (!s_bossOk && g_hMain && g_book.Ok()) {
        s_statusExtra = L"老板键注册失败（可能被别的程序占用）";
        UiUpdateStatus();
    }
}

/* 老板键：把窗口藏起来 / 叫回来。
 * 只 ShowWindow 不 DestroyWindow：藏起来时程序还在后台跑，阅读位置照常记忆，
 * 再按一次（或点任务栏图标）就回来。 */
void UiToggleBoss()
{
    if (!g_hMain) return;
    if (IsWindowVisible(g_hMain)) {
        ShowWindow(g_hMain, SW_HIDE);
    } else {
        ShowWindow(g_hMain, SW_SHOW);
        if (IsIconic(g_hMain)) ShowWindow(g_hMain, SW_RESTORE);
        SetForegroundWindow(g_hMain);
    }
}

/* ---------------- 外观：字体 / 配色 ---------------- */

/* 选择正文字体：直接借系统字体对话框（ChooseFontW）。
 * 自带列表要自己枚举几百个字体名、还要处理"中文名/英文名同族"，用系统对话框
 * 一次解决，而且用户熟悉。只取回字体名，字号仍归我们自己的 Ctrl+= / Ctrl+- 管 ——
 * 两处都能改字号容易互相打架，所以这里明确不采纳对话框里的字号选择。
 */
void UiPickFont()
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(g_lay.fontPt, g_dpi, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, g_lay.fontFace.empty() ? L"Microsoft YaHei" : g_lay.fontFace.c_str(), LF_FACESIZE);

    CHOOSEFONTW cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hMain;
    cf.lpLogFont = &lf;
    /* CF_SCREENFONTS：只列屏幕字体（打印字体选了也用不上）；
     * CF_INITTOLOGFONTSTRUCT：打开时就定位到当前字体；
     * CF_NOSTYLESEL / CF_NOSCRIPTSEL：不要粗斜体和脚本下拉 —— 正文只有常规体，
     * 标题的加粗是我们自己按 FW_BOLD 建的，让用户在这里选粗体反而会打乱排版。 */
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSTYLESEL | CF_NOSCRIPTSEL;
    if (!ChooseFontW(&cf)) return;

    g_lay.fontFace = lf.lfFaceName;
    /* 1.6.0：字体是"每本书"的设置。同时更新会话默认值，下一本新书能用上；
     * 若这本书开了"强制字体"，那么改字体同时把强制关掉 —— 否则用户选了字体却
     * 看不出变化，会以为功能坏了。 */
    g_lay.forceFont = 0;
    g_set.fontFace = lf.lfFaceName;
    SettingsSave();          /* 立刻落盘：换字体是"设置"，不该等到关窗口才存 */
    if (g_book.Ok()) LayoutSave(g_book.path);
    ViewMetricsChanged(true);   /* 字体变了：行高、换行位置全部重算（传 true 让段落行数缓存作废） */
    ViewInvalidate();
    s_statusExtra = std::wstring(L"字体：") + g_lay.fontFace;
    UiUpdateStatus();
}

/* 导入字体文件（.ttf/.otf/.ttc）：私有加载 + 记住路径 + 顺手弹出字体对话框。
 * 不装进系统字体库（FR_PRIVATE），所以用户"导入一个字体"不会污染系统；
 * 路径写进 ini 的 fontFiles，下次启动由 FontsLoadImported() 重新加载。 */
void UiImportFont()
{
    wchar_t path[MAX_PATH];
    path[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = L"字体文件 (*.ttf;*.otf;*.ttc)\0*.ttf;*.otf;*.ttc\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"导入字体文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    int cnt = AddFontResourceExW(path, FR_PRIVATE, nullptr);
    if (cnt <= 0) {
        MessageBoxW(g_hMain, L"这个文件里没有可用的字体，可能不是 ttf/otf/ttc，或者文件已损坏。",
                    L"导入失败", MB_ICONWARNING);
        return;
    }
    /* 记进字体文件列表（'|' 分隔，重复导入同一路径就跳过，避免列表越来越长） */
    if (g_set.fontFiles.find(path) == std::wstring::npos) {
        if (!g_set.fontFiles.empty()) g_set.fontFiles += L'|';
        g_set.fontFiles += path;
    }
    SettingsSave();
    MessageBoxW(g_hMain,
                L"字体已导入，只对本程序生效，不会装进系统。\n\n"
                L"接下来在弹出的字体列表里挑它就行（名字一般就是文件里的字体名，"
                L"如果列表很长，可以在「字体」框里敲几个字过滤）。",
                L"导入成功", MB_ICONINFORMATION);
    UiPickFont();
}

/* 自定义配色：背景 / 正文 / 标题 / 选中色 / 悬停色 依次弹系统取色器。
 * 任何一步点「取消」就整体放弃 —— 不做"改了一半"的配色，否则很容易出现
 * "背景换了、文字还是夜间那种浅灰"的看不清组合。
 * 取消之外的情形都会切到 theme = 2（自定义），并立刻落盘。 */
void UiPickColors()
{
    COLORREF tmp[5] = { g_set.cBg, g_set.cFg, g_set.cTitle, g_set.cSel, g_set.cHov };
    /* 取色器里的"自定义颜色"格子（系统要求在调用之间保持内容，所以必须 static） */
    static COLORREF cust[16] = { 0 };

    for (int i = 0; i < 5; i++) {
        CHOOSECOLORW cc;
        ZeroMemory(&cc, sizeof(cc));
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = g_hMain;
        cc.rgbResult = tmp[i];
        cc.lpCustColors = cust;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc)) return;   /* 取消 = 整体放弃 */
        tmp[i] = cc.rgbResult;
    }
    g_set.cBg    = tmp[0];
    g_set.cFg    = tmp[1];
    g_set.cTitle = tmp[2];
    g_set.cSel   = tmp[3];
    g_set.cHov   = tmp[4];
    g_set.theme  = 2;
    SettingsSave();
    ViewInvalidate();
    s_statusExtra = L"已切换到自定义配色";
    UiUpdateStatus();
}

/* 套用一套预设配色。索引与菜单顺序一致：
 * 0 日间 / 1 夜间 / 2 护眼绿 / 3 羊皮纸 / 4 高对比暗。
 * 前两个复用内置的 kDay / kNight（theme 0/1），后三个写进自定义色的槽位（theme 2）。 */
void UiApplyPreset(int idx)
{
    switch (idx) {
    case 0:
        g_set.theme = 0;
        break;
    case 1:
        g_set.theme = 1;
        break;
    case 2:   /* 护眼绿：淡豆绿底 + 深墨绿字，长时间看最不刺眼 */
        g_set.theme  = 2;
        g_set.cBg    = RGB(0xC7, 0xED, 0xCC);
        g_set.cFg    = RGB(0x1E, 0x2B, 0x1F);
        g_set.cTitle = RGB(0x2C, 0x5C, 0x2E);
        g_set.cSel   = RGB(0x9E, 0xD8, 0xA8);
        g_set.cHov   = RGB(0xB9, 0xE3, 0xC1);
        break;
    case 3:   /* 羊皮纸：偏黄的纸感，适合暖光下阅读 */
        g_set.theme  = 2;
        g_set.cBg    = RGB(0xF3, 0xE7, 0xC8);
        g_set.cFg    = RGB(0x40, 0x32, 0x22);
        g_set.cTitle = RGB(0x8B, 0x5A, 0x1E);
        g_set.cSel   = RGB(0xD8, 0xC4, 0x92);
        g_set.cHov   = RGB(0xE8, 0xDB, 0xB8);
        break;
    case 4:   /* 高对比暗：纯黑底 + 亮白字，夜里关灯看最清楚 */
        g_set.theme  = 2;
        g_set.cBg    = RGB(0x0A, 0x0A, 0x0B);
        g_set.cFg    = RGB(0xEC, 0xEC, 0xEE);
        g_set.cTitle = RGB(0xFF, 0xD0, 0x6A);
        g_set.cSel   = RGB(0x30, 0x5A, 0x8C);
        g_set.cHov   = RGB(0x18, 0x18, 0x1C);
        break;
    default:
        return;
    }
    SettingsSave();
    ViewInvalidate();
    UiUpdateStatus();
}
/* 切换侧栏显示哪一页（0 目录 / 1 搜索结果 / 2 书签），并保证侧栏是打开的。
 * 为什么自己动手开侧栏：用户可能在「关侧栏」状态下按了 Ctrl+F，若搜到结果却看不到
 * 列表，会以为搜索坏了；所以这里顺手把 g_set.sidebar 置 1。
 * 注意只改内存里的设置、不写 ini —— 只有用户在菜单里主动开关才持久化，
 * 免得「搜一次就把我的侧栏偏好改了」。
 */
static void ShowPanel(int which)
{
    s_panel = which;
    NrTrace(L"ShowPanel %d sidebar=%d", which, g_set.sidebar);
    if (!g_set.sidebar) {
        g_set.sidebar = 1;
    }
    UiLayout();
}

/* 跳到第 idx 条命中：正文滚到该处并按 len 高亮，同时让列表选中项跟过去。
 * 参数 idx 越界（<0 或 >= size）时直接返回，调用方不必先判；列表点击与 F3 都走这里。
 * 三步：① ViewGotoOffset 让正文定位并高亮；② 列表选中该条并用 LB_SETTOPINDEX
 * 让它出现在可视区（往上留 3 行上下文，而不是顶死在第一行）；③ 刷新状态栏。
 * s_hitSync 的作用：② 会引发 LBN_SELCHANGE，不挡就会「跳转 → 选中 → 又跳转」。
 * 注意 s_curHit 在跳转前就设好 —— UiFindNext 依赖它算下一条。
 */
void UiJumpHit(int idx)
{
    NrTrace(L"UiJumpHit idx=%d total=%d", idx, (int)s_hits.size());
    if (idx < 0 || idx >= (int)s_hits.size()) return;
    s_curHit = idx;
    const Hit& h = s_hits[(size_t)idx];
    ViewGotoOffset(h.off, h.len);
    s_hitSync = true;
    SendMessageW(s_hHits, LB_SETCURSEL, idx, 0);
    SendMessageW(s_hHits, LB_SETTOPINDEX, idx > 3 ? idx - 3 : 0, 0);
    s_hitSync = false;
    UiUpdateStatus();
}

/* 全文搜索，并跳到「离当前阅读位置最近的那一处」，同时把结果填进左侧列表。
 * 参数 kw 为空直接返回；与上次相同的词且结果还在时不重扫，直接复用（连按两次
 * 回车不会白扫一遍全书）。
 * 关于 4000 条上限：搜「的」这种字，百万字的书能有几万处命中，逐条量宽度再灌进
 * LISTBOX 会让界面卡死好几秒；4000 条已远超人工翻找的耐心，超出部分用状态栏提示。
 * 关于「从当前锚点往后找」：读者按 Ctrl+F 通常是想在正读到的地方继续找下去，
 * 而不是被拽回书的开头。所以以 ViewAnchorOffset() 为锚点做一次线性扫 —— 命中已按
 * 位置升序排列，第一个 off >= anchor 的就是目标；扫不到（锚点已在最后一条之后）
 * 就从头开始（start 保持 0），行为上是绕回第一条。
 * 边界：没开书、关键词为空、搜不到，都安全返回；填内容在切面板之前，避免切过去时列表空着。
 */
void UiRunSearch(const std::wstring& kw)
{
    NrTrace(L"UiRunSearch in kw='%s' bookOk=%d", kw.c_str(), (int)g_book.Ok());
    if (!g_book.Ok()) return;
    if (kw.empty()) return;
    if (kw != s_kw || s_hits.empty()) {
        s_kw = kw;
        BookSearch(g_book, kw, s_hits, 4000);
        NrTrace(L"  BookSearch done hits=%d", (int)s_hits.size());
        FillHits();
        NrTrace(L"  FillHits done");
        ShowPanel(1);
        NrTrace(L"  ShowPanel done");
        wchar_t b[128];
        if (s_hits.size() >= 4000) wsprintfW(b, L"“%s” 命中超过 4000 处（仅显示前 4000）", kw.c_str());
        else wsprintfW(b, L"“%s” 共 %d 处", kw.c_str(), (int)s_hits.size());
        s_statusExtra = b;
        UiUpdateStatus();
    }
    if (s_hits.empty()) return;
    int anchor = ViewAnchorOffset();
    int start = 0;
    for (size_t i = 0; i < s_hits.size(); i++) {
        if (s_hits[i].off >= anchor) { start = (int)i; break; }
    }
    UiJumpHit(start);
}

/* 跳到下一条命中（F3 / 「下一个」按钮 / 菜单都走这里）。
 * 用 s_curHit + 1 往后走，走到末尾绕回第 0 条（循环查找）；只有一条命中时
 * 等价于原地重跳一次，无害。
 * 三种边界：① 没开书直接返回；② 还没有命中集（用户没搜过就按 F3）——搜索框里
 * 有字就拿它去搜，否则展开搜索条让用户输入，这样 F3 在任何状态下都有合理反应；
 * ③ s_curHit 为 -1 时 +1 得 0，正好是第一条。
 * 注意真正的跳转与高亮全在 UiJumpHit，这里只负责「算下一条是谁」。
 */
void UiFindNext()
{
    if (!g_book.Ok()) return;
    if (s_hits.empty()) {
        wchar_t buf[512];
        if (s_hFind && GetWindowTextW(s_hFind, buf, 512) > 0) UiRunSearch(buf);
        else UiShowSearchBar(true);
        return;
    }
    int idx = s_curHit + 1;
    if (idx >= (int)s_hits.size()) idx = 0;
    UiJumpHit(idx);
}

/* 展开 / 收起顶部搜索条。
 * show = true ：展开，并把焦点放进输入框且全选原有文字（方便直接覆写关键词）；
 * show = false：收起，焦点还给正文。
 * 边界：没打开书时不允许「展开」—— 没有正文可搜，弹一句提示后原样返回；
 * 但收起永远允许，否则会卡在「打不开又收不起」的死状态。
 * 实现上只改 s_searchShown 再调 UiLayout()：控件的位置、显隐、正文上移全由布局
 * 函数统一处理，这里不重复写一遍摆放逻辑，免得两处规则渐渐不一致。
 */
void UiShowSearchBar(bool show)
{
    if (!g_book.Ok() && show) {
        MessageBoxW(g_hMain, L"请先打开一个 TXT 文件。", L"提示", MB_ICONINFORMATION);
        return;
    }
    s_searchShown = show;
    UiLayout();
    if (show) {
        SetFocus(s_hFind);
        SendMessageW(s_hFind, EM_SETSEL, 0, -1);
    } else {
        ViewFocus();
    }
}

/* ---------------- 打开文件 ---------------- */

/* 把搜索相关的状态清干净（换书时必调）。
 * 清：命中集、关键词、当前下标、状态栏后缀、结果列表内容。
 * 为什么 s_panel 要设成 g_set.sidebar ? 0 : 1：侧栏开着就回目录页（新书当然先看
 * 目录）；侧栏本来关着就指向 1，这样之后一旦展开侧栏直接显示结果页，不会先闪
 * 一下空目录。
 * 注意不重置 s_searchShown —— 搜索条是否展开属于界面偏好，换书后应保留。
 */
static void ClearSearchState()
{
    s_hits.clear();
    s_kw.clear();
    s_curHit = -1;
    s_statusExtra.clear();
    s_panel = g_set.sidebar ? 0 : 1;
    if (s_hHits) SendMessageW(s_hHits, LB_RESETCONTENT, 0, 0);
}

/* 打开一本书并把它变成「当前书」：加载 → 恢复位置 → 刷新全部界面 → 记入最近打开。
 * 参数 path 是绝对路径（来自对话框 / 拖放 / 命令行 / 最近打开菜单）；无返回值。
 * 失败时弹提示，并且**保持原来的书不变**。步骤顺序都有原因：
 * 1) 先 UiAutoStop()：换书前停下自动下滑，否则定时器可能对着新书/空书滚一下；
 * 2) 旧书有进度就先存盘，用户可能马上又切回来，少一次丢进度的机会；
 * 3) 用等待光标 + 先加载到临时 Book nb：失败就整个丢弃，g_book 一点没动 ——
 *    这是「打开失败不该毁掉当前阅读」的关键；
 * 4) 成功后用 swap 搬四块数据：常数时间，避免拷贝几十 MB 的全文；
 * 5) ClearSearchState + ViewSetBook + FillToc：清搜索、让视图重新排版、重建目录；
 * 6) PosLoad 恢复上次位置，且必须 0 < off < 全文长度才用，否则回开头
 *    （文件被改短或换过内容时旧偏移会越界）；
 * 7) 标题栏、最近打开、g_set.lastFile、状态栏、目录高亮、布局、焦点，一条龙刷完；
 * 8) 成功和失败两条路都要把等待光标还回去，所以失败分支里单独还了一次。
 */
void UiOpenFile(const std::wstring& path)
{
    if (path.empty()) return;
    UiAutoStop();                                  /* 换书时先停下自动下滑 */
    UiSaveProgress();                              /* 旧书有进度就先存盘，用户可能马上又切回来 */

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring err;
    Book nb;
    bool ok = BookLoad(nb, path, &err);

    if (!ok) {
        std::wstring m = L"打开失败：";
        m += err;
        m += L"\n\n";
        m += path;
        MessageBoxW(g_hMain, m.c_str(), L"打开失败", MB_ICONWARNING);
        SetCursor(old);
        return;
    }

    g_book.path = nb.path;
    g_book.title = nb.title;
    g_book.text.swap(nb.text);
    g_book.paraOff.swap(nb.paraOff);
    g_book.chapters.swap(nb.chapters);

    /* 1.6.0：先把这本书自己的排版和高亮读进来，再让视图建书。
     * 顺序不能反 —— ViewSetBook 之后会算度量、排首屏，那时 g_lay 必须已经是
     * "这本书的设置"，否则第一屏会用上一本书的字号/栏数画出来再跳一次。 */
    LayoutLoad(path);
    HighlightLoad(path);

    ClearSearchState();
    ViewSetBook();
    FillToc();
    /* 书签是按书存的：换书后立刻读新书的书签，否则会在新书里看到上一本的书签标记 */
    BmReload();

    int off = PosLoad(path);
    if (off > 0 && off < (int)g_book.text.size()) ViewGotoOffset(off, 0);
    else ViewGotoOffset(0, 0);

    /* 1.7.0：手机同步 —— 如果手机上读得比这台电脑更晚，就跳到手机上那个位置。
     * 判断依据是两个时间戳：本机的 ts（每次本机保存进度时写）、手机的 phts
     * （手机上报进度时写）。同一份 ini，两边各用各的键，谁晚听谁的，
     * 这样"电脑读一段、手机接着读、回头再用电脑"也不会把进度倒回老地方。 */
    /* 1.7.0：和手机对账（谁读得靠后听谁的，改位置前先问用户）。
     * 放在本地恢复之后：先站到自己上次的位置，再决定要不要挪。 */
    /* 打开书就立刻对账是扑空的：手机那边的广播可能还没来。
     * 所以先记一个"待对账"，由 TIMER_SYNC 每 1.2 秒探一次，看到对端再真正比。 */
    if (g_set.phoneSrv) {
        s_syncPending = true;
        s_syncTries = 0;
        SetTimer(g_hMain, TIMER_SYNC, 1200, nullptr);
    }

    std::wstring cap = L"小说阅读器 - ";
    cap += UiDisplayTitle();
    SetWindowTextW(g_hMain, cap.c_str());

    RecentAdd(path);
    g_set.lastFile = path;
    UiRebuildRecentMenu();
    /* 书架元数据重算一遍：这本书刚被写进最近打开，首页要立刻能看到它、
     * 以及它刚才保存下来的进度（章名 + 百分比）。 */
    ShelfRefresh();
    UiUpdateStatus();
    UiSyncToc();
    UiLayout();
    ViewFocus();
    SetCursor(old);
    /* 加载过程中临时用过的大缓冲区（原始字节、解码中间结果）此刻都已经 free 了，
     * 但它们占的页还挂在本进程的工作集里。这里顺手压一次，打开 15 MB 的书之后
     * 任务管理器里的数字才不会一直停在峰值上。 */
    MemTrim();
}

/* 关掉当前这本书，回到首页书架（Ctrl+W、菜单「关闭当前书」都走这里）。无参数。
 * 为什么不叫"清空"：书只是从窗口里拿掉了，书架记录必须留着 —— 下次启动首页还要
 * 列出它、还要从上次的位置接着读。所以这里只清内存里的 Book，绝不动 ini 里的进度。
 * 顺序与原因：
 *   1) UiAutoStop：定时器不能对着空书继续滚；
 *   2) UiSaveProgress：把"关的时候读到哪"先落盘，首页立刻要显示这个最新进度；
 *   3) 五块数据用 swap 干净地交还内存（clear() 只清长度、不还容量，十几 MB 的正文
 *      会一直占着），顺序无所谓但要全清，漏一块就会留下"半本书"的状态；
 *   4) ClearSearchState + ViewSetBook + FillToc：搜索、视图、目录各自复位；
 *   5) ShelfRefresh 让首页读到最新进度；标题栏退回程序名；状态栏回"未打开文件"。
 */
void UiCloseFile()
{
    if (!g_book.Ok()) return;
    UiAutoStop();
    UiSaveProgress();
    /* 1.6.0：把这本书的排版与高亮落盘（进度已经在上面 UiSaveProgress 存过）。
     * 必须在清空 g_book.path 之前 —— 段名是用路径算出来的。 */
    LayoutSave(g_book.path);
    HighlightSave(g_book.path);

    /* swap 空对象 = 真正释放内存（clear 不保证还容量） */
    std::wstring().swap(g_book.text);
    std::wstring().swap(g_book.path);
    std::wstring().swap(g_book.title);
    std::vector<uint32_t>().swap(g_book.paraOff);
    std::vector<Chapter>().swap(g_book.chapters);
    /* 书签也跟着清掉：段号/偏移都是针对刚才那本书算的，留着就是脏数据
     * （ini 里的书签本身不动，下次打开这本书会重新读回来） */
    std::vector<std::wstring>().swap(s_bms);
    g_bmParas.clear();
    /* 高亮同理：偏移是那本书的坐标，留着会在下一本书里画到毫不相干的位置 */
    std::vector<Highlight>().swap(g_hls);
    /* 排版回默认值：回书架后顶栏/标题不该还挂着上一本书的字号设置 */
    LayoutFromSettings();

    ClearSearchState();
    ViewSetBook();
    FillToc();
    ShelfRefresh();

    /* 搜索条收起来：新书还没打开，留着输入框没有意义，而且它的按钮都是禁用语义 */
    s_searchShown = false;

    SetWindowTextW(g_hMain, L"小说阅读器");
    UiUpdateStatus();
    UiLayout();
    ViewFocus();
    InvalidateRect(g_hMain, nullptr, TRUE);
    /* 十几 MB 的正文刚交还给堆，把空闲页也一并还给系统：关掉一本书之后
     * 任务管理器里的内存应该立刻掉回空窗口的水平，而不是等下本书来挤掉。 */
    MemTrim();
}

/* ============================================================================
 * 网页小说（Ctrl+U / 文件菜单「打开网页小说」）
 * ============================================================================
 * 做三件事：①问用户要网址（剪贴板里是网址就替他填好）；②带进度地抓完整本书；
 * ③把抓下来的正文写成 UTF-8 文件塞进缓存目录，然后走正常 UiOpenFile。
 * 为什么不直接把文本塞进 g_book：落成文件之后，书架、进度、书签、重新打开、
 * 拖拽、最近打开列表统统不用改代码就能用 —— 这是"多格式先抽成文本"思路的延续。
 */

/* 去首尾空白（网页/剪贴板里的网址常带换行和空格）。和 web.cpp 里那个 TrimW 同名同义，
 * 但那个是 web.cpp 的 static，这里再写一份，避免为了一个 3 行函数去导出一个公共符号。 */
static std::wstring TrimW(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b) {
        wchar_t c = s[a];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000 || c == 0xA0) a++;
        else break;
    }
    while (b > a) {
        wchar_t c = s[b - 1];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000 || c == 0xA0) b--;
        else break;
    }
    return s.substr(a, b - a);
}
/* 网址输入框的返回值（跨 WndProc 传递，所以放文件级静态） */
static std::wstring s_urlBoxText;
static bool         s_urlBoxDone   = false;
static HWND         s_urlBoxEdit   = nullptr;
static bool         s_urlBoxReady  = false;   /* 窗口类是否已经注册过 */

/* 网址输入框的子控件 ID（只在这一次对话框里用，不放进 ids.h 免得和主窗口撞号） */
#define URLBOX_EDIT  1
#define URLBOX_OK    2
#define URLBOX_CANCEL 3

/* 网址输入框的窗口过程：只有一个 EDIT 和两个按钮，回车=确定、ESC=取消。
 * 自己画一个而不是用 InputBox：Win32 没有原生 InputBox，引 dialog 资源又会
 * 让 exe 多一份 .res 依赖。 */
static LRESULT CALLBACK UrlBoxProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        /* 提示文字 + 输入框 + 两个按钮，坐标全部按 DPI 缩放 */
        s_urlBoxEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       S(16), S(46), S(488), S(26), h, (HMENU)URLBOX_EDIT, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"打开(&O)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(352), S(84), S(74), S(28), h, (HMENU)URLBOX_OK, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(432), S(84), S(72), S(28), h, (HMENU)URLBOX_CANCEL, nullptr, nullptr);
        /* 预先填好剪贴板里的网址（用户从浏览器复制过来直接按回车就行） */
        if (s_urlBoxEdit) {
            SetWindowTextW(s_urlBoxEdit, s_urlBoxText.c_str());
            SendMessageW(s_urlBoxEdit, EM_SETSEL, 0, -1);
            SetFocus(s_urlBoxEdit);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == URLBOX_OK) {
            wchar_t buf[2048];
            buf[0] = 0;
            if (s_urlBoxEdit) GetWindowTextW(s_urlBoxEdit, buf, 2048);
            s_urlBoxText = buf;
            s_urlBoxDone = true;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == URLBOX_CANCEL) {
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        s_urlBoxEdit = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* 从剪贴板里取一段文本（取不到返回空串）。用来给网址输入框做预填。 */
static std::wstring ClipboardText()
{
    std::wstring out;
    if (!OpenClipboard(g_hMain)) return out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) { out = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return TrimW(out);
}

/* 弹网址输入框。确定返回 true 并把网址写进 out，取消返回 false。
 * 用 EnableWindow(g_hMain, FALSE) 做成模态：主窗口在输入期间不接受输入，
 * 但消息循环仍然在跑（所以界面不会假死）。 */
static bool AskUrlBox(std::wstring& out)
{
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!s_urlBoxReady) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = UrlBoxProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"NRUrlBox";
        RegisterClassExW(&wc);
        s_urlBoxReady = true;
    }

    /* 剪贴板里已经是网址就预填（这是最常见的用法：浏览器里复制地址栏再切过来） */
    s_urlBoxText = ClipboardText();
    if (!WebIsUrl(s_urlBoxText)) s_urlBoxText.clear();
    s_urlBoxDone = false;

    RECT wr;
    GetWindowRect(g_hMain, &wr);
    int wpx = S(520), hpx = S(160);
    int x = (wr.left + wr.right - wpx) / 2;
    int y = (wr.top + wr.bottom - hpx) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NRUrlBox", L"打开网页小说 — 粘贴目录页网址",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, wpx, hpx, g_hMain, nullptr, hi, nullptr);
    if (!dlg) return false;

    /* 标题下面那句提示单独画：CreateWindow 时没有静态控件的空间，用 WM_PAINT 不方便，
     * 所以这里塞一个 STATIC 子窗口（比自绘省事，也会自动跟 DPI 走）。 */
    CreateWindowExW(0, L"STATIC",
                    L"支持 http / https。目录页会自动找出所有章节并逐章抓取；"
                    L"如果只有单页，就把这一页当全书。",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    S(16), S(16), S(480), S(24), dlg, nullptr, hi, nullptr);

    EnableWindow(g_hMain, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    /* 局部消息循环：直到这个对话框销毁才返回 */
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {     /* 让 TAB / 回车在对话框内部生效 */
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);

    if (!s_urlBoxDone) return false;
    out = TrimW(s_urlBoxText);
    return !out.empty();
}


/* ---- 手机阅读提示窗 ----------------------------------------------------
 * 为什么用自建窗口而不是 MessageBoxW：
 *   MessageBox 里的文字不能选中、不能复制，而网址必须能复制到手机浏览器。
 * 窗口里放一个只读的多行 EDIT 装地址，一个"复制地址"按钮就够用了。 */
static LRESULT CALLBACK PhoneBoxProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_CREATE:
    {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        CreateWindowExW(0, L"STATIC", L"手机与电脑连同一个 WiFi，用浏览器打开下面的地址：",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(16), S(14), S(528), S(22), h, nullptr, hi, nullptr);
        /* 地址框：只读、多行、自动换行不必要（一行一个地址） */
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                  ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                  S(16), S(40), S(528), S(112), h,
                                  (HMENU)(INT_PTR)5, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"复制地址",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        S(16), S(166), S(130), S(34), h, (HMENU)(INT_PTR)2, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"关闭",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(414), S(166), S(130), S(34), h, (HMENU)(INT_PTR)3, hi, nullptr);
        CreateWindowExW(0, L"STATIC",
                        L"连不上时：确认手机和电脑在同一个 WiFi，并允许本程序通过 Windows 防火墙。",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(16), S(208), S(528), S(40), h, nullptr, hi, nullptr);
        (void)ed;
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 2)
        {
            /* 复制：把地址框里的文字放进剪贴板 */
            HWND ed = GetDlgItem(h, 5);
            int n = ed ? GetWindowTextLengthW(ed) : 0;
            if (n > 0 && OpenClipboard(h))
            {
                EmptyClipboard();
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
                if (g)
                {
                    wchar_t* p = (wchar_t*)GlobalLock(g);
                    if (p) { GetWindowTextW(ed, p, n + 1); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                }
                CloseClipboard();
            }
            return 0;
        }
        if (LOWORD(w) == 3) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ======================= 1.7.0 阅读同步（P2P） ==========================
 *
 * 三个小工具，配合 sync.cpp 用：
 *   SyncPushCurrent  —— 把本机进度推给对端（后台线程，失败静默）
 *   SyncCheckOnOpen  —— 打开书后对账，谁读得靠后听谁的，改位置前先问用户
 *   UiShowSyncStatus —— 给用户看"找到谁了、两边各读到哪"
 */

/* 推本机进度给对端。
 *
 * 失败**不弹窗**：同步是锦上添花，手机没开、Wi-Fi 断了、对面锁屏了，
 * 都不该在用户读得正入神时跳个对话框出来。
 * 用后台线程发，是因为 TCP 连接可能卡到 4 秒超时，不能让界面线程等。 */
static void SyncPushCurrent()
{
    if (!SyncRunning()) return;
    std::wstring url = SyncPeerUrl();
    if (url.empty()) return;

    SyncRec rec;
    if (!SyncLocalRecord(rec)) return;

    /* 线程只认一个裸指针，参数打包在堆上，线程自己负责 delete */
    struct Job { std::wstring url; SyncRec rec; };
    Job* job = new Job();
    job->url = url;
    job->rec = rec;

    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        Job* j = (Job*)p;
        std::wstring err;
        SyncPush(j->url, j->rec, err);
        delete j;
        return 0;
    }, job, 0, nullptr);
    if (th) CloseHandle(th);
    else delete job;
}

/* 打开一本书之后对账。
 *
 * 规则（按用户要求）：两边比进度，**读得更靠后的那一方赢**；如果对面更靠后
 * 就直接改用户的阅读位置是不礼貌的 —— 用户可能就是想翻回前面重读一段 ——
 * 所以这里先弹问句，点"是"才跳。
 *
 * 只有差距超过 200 个字符（大约半屏）才打扰用户：差几个字就弹窗是噪音。 */
/* 每 1.2 秒看一眼：对面出现了没有？出现就立刻对账并收工。
 * 最多等 10 轮（约 12 秒），等不到就把待办撤掉——不无限期地空转定时器。 */
static void SyncTryPending()
{
    if (!s_syncPending) { KillTimer(g_hMain, TIMER_SYNC); return; }
    NrTrace(L"SYNCTRY tries=%d running=%d peer='%s'", s_syncTries, SyncRunning() ? 1 : 0, SyncPeerUrl().c_str());
    NrTrace(L"SYNCTRY tries=%d running=%d peer='%s'", s_syncTries, SyncRunning() ? 1 : 0, SyncPeerUrl().c_str());
    if (!SyncRunning() || !g_book.Ok()) {
        s_syncPending = false;
        KillTimer(g_hMain, TIMER_SYNC);
        return;
    }
    if (SyncPeerUrl().empty()) {
        if (++s_syncTries > 10) {
            s_syncPending = false;
            KillTimer(g_hMain, TIMER_SYNC);
        }
        return;
    }
    s_syncPending = false;
    KillTimer(g_hMain, TIMER_SYNC);
    SyncCheckOnOpen();
}

static void SyncCheckOnOpen()
{
    NrTrace(L"SYNCCHECK enter peer='%s'", SyncPeerUrl().c_str());
    if (!SyncRunning()) return;

    SyncRec mine;
    if (!SyncLocalRecord(mine)) return;         /* 没开书，或者书太短不参与同步 */
    NrTrace(L"SYNC local key=%s off=%d pct=%d", mine.key.c_str(), mine.off, mine.pct);

    std::wstring url = SyncPeerUrl();
    if (url.empty())
    {
        s_statusExtra = L"阅读同步：还没找到手机（手机端也要打开同步）";
        UiUpdateStatus();
        return;
    }

    std::vector<SyncRec> theirs;
    std::wstring err;
    NrTrace(L"SYNC peer url=%s", url.c_str());
    if (!SyncFetch(url, theirs, err))
    {
        NrTrace(L"SYNC fetch failed: %s", err.c_str());
        s_statusExtra = L"阅读同步：" + err;
        UiUpdateStatus();
        return;
    }

    NrTrace(L"SYNC fetch ok count=%u", (unsigned)theirs.size());
    for (size_t z = 0; z < theirs.size(); z++)
        NrTrace(L"SYNC   peer rec key=%s off=%d pct=%d", theirs[z].key.c_str(), theirs[z].off, theirs[z].pct);

    /* 按指纹找同一本书 */
    const SyncRec* hit = nullptr;
    for (size_t i = 0; i < theirs.size(); i++)
    {
        if (theirs[i].key == mine.key) { hit = &theirs[i]; break; }
    }

    if (!hit)
    {
        /* 对面没有这本书：把本机进度推过去，手机下次打开就有位置可跳 */
        SyncPushCurrent();
        return;
    }

    NrTrace(L"SYNC hit off=%d mine=%d diff=%d", hit->off, mine.off, hit->off - mine.off);
    if (hit->off - mine.off <= 200)
    {
        /* 本机不比手机落后（甚至更靠后）：把本机进度推过去让手机跟上 */
        SyncPushCurrent();
        wchar_t buf[192];
        wsprintfW(buf, L"阅读同步：本机 %d.%d%% · 手机 %d.%d%%",
                  mine.pct / 10, mine.pct % 10, hit->pct / 10, hit->pct % 10);
        s_statusExtra = buf;
        UiUpdateStatus();
        return;
    }

    /* 手机读得更靠后 —— 问用户 */
    std::wstring q = L"这本书两边的进度不一样：\n\n  本机：";
    wchar_t buf[256];
    wsprintfW(buf, L"%d.%d%%", mine.pct / 10, mine.pct % 10);
    q += buf;
    if (!mine.chapter.empty()) { q += L"  "; q += mine.chapter; }
    q += L"\n  手机：";
    wsprintfW(buf, L"%d.%d%%", hit->pct / 10, hit->pct % 10);
    q += buf;
    if (!hit->chapter.empty()) { q += L"  "; q += hit->chapter; }
    q += L"\n\n手机读得更靠后。要跳到手机上那个位置继续读吗？\n";
    q += L"（选“否”就留在本机，并把本机进度推给手机）";

    if (MessageBoxW(g_hMain, q.c_str(), L"阅读同步", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        SyncPushCurrent();
        return;
    }

    ViewGotoOffset(hit->off, 0);
    /* 立刻按新位置落盘：否则关窗口时会把旧位置写回去，等于白跳 */
    UiSaveProgress();

    /* 不写手机自报的百分比：两边文本长度可能微差（对端可能是不同版本、或做过清洗），
     * 用本机真实位置算出来的百分比才不会前后矛盾。 */
    int tlen = (int)g_book.text.size();
    int minePct = (tlen > 0) ? (int)((__int64)hit->off * 1000 / tlen) : 0;
    wsprintfW(buf, L"已按手机的位置同步到 %d.%d%%", minePct / 10, minePct % 10);
    s_statusExtra = buf;
    UiUpdateStatus();
}

/* 给用户看的同步状态：找到谁、两边各读到哪、协议端口是多少。
 * 第一次开启同步时自动弹一次 —— 用户需要知道"接下来该做什么"（去手机上也打开）。 */
static void UiShowSyncStatus()
{
    std::wstring s;

    if (!SyncRunning())
    {
        s = L"阅读同步：未开启。\n\n"
            L"开启后，电脑和手机会在同一个局域网里互相发现，\n"
            L"读到哪里都会互相对账（两边都不需要联网服务器）。";
        MessageBoxW(g_hMain, s.c_str(), L"阅读同步", MB_OK | MB_ICONINFORMATION);
        return;
    }

    s = L"阅读同步：已开启（端口 8233，局域网内互相发现）\n\n";

    std::vector<std::wstring> peers = SyncPeers();
    if (peers.empty())
    {
        s += L"暂时没发现别的设备。\n"
             L"· 手机要和电脑连同一个 Wi-Fi（不能是手机热点、也不能是访客网络）；\n"
             L"· 手机上的应用也要打开「阅读同步」；\n"
             L"· 两台机器都开着广播，一般 2~3 秒内就能看到对方。";
    }
    else
    {
        s += L"发现的设备：\n";
        for (size_t i = 0; i < peers.size(); i++)
        {
            s += L"  ";
            s += peers[i];
            s += L"\n";
        }
        s += L"\n";
    }

    SyncRec mine;
    if (SyncLocalRecord(mine))
    {
        s += L"\n当前这本书：";
        s += mine.title;
        s += L"\n  本机：";
        wchar_t num[64];
        wsprintfW(num, L"%d.%d%%", mine.pct / 10, mine.pct % 10);
        s += num;
        if (!mine.chapter.empty()) { s += L"  "; s += mine.chapter; }

        /* 顺带把对面的记录也拉一次，让用户当场看到差距 */
        std::wstring url = SyncPeerUrl();
        std::vector<SyncRec> theirs;
        std::wstring err;
        if (!url.empty() && SyncFetch(url, theirs, err))
        {
            for (size_t i = 0; i < theirs.size(); i++)
            {
                if (theirs[i].key != mine.key) continue;
                s += L"\n  手机：";
                wsprintfW(num, L"%d.%d%%", theirs[i].pct / 10, theirs[i].pct % 10);
                s += num;
                if (!theirs[i].chapter.empty()) { s += L"  "; s += theirs[i].chapter; }
                break;
            }
        }
    }

    MessageBoxW(g_hMain, s.c_str(), L"阅读同步", MB_OK | MB_ICONINFORMATION);
}
/* 手机同步的总开关（用户可以关掉）。默认关：没开时连端口都不监听，
 * 局域网里别的设备扫不到这台机器 —— 这就是"由用户自己开启"的实际含义。
 *
 * 1.7.0 起同步改成 P2P：电脑和手机各开一个监听、各往局域网广播，
 * 互相发现后直接对话，谁都不是"服务端"（用户明确不要电脑当服务器）。 */
void UiTogglePhoneSync()
{
    if (SyncRunning())
    {
        SyncStop();
        g_set.phoneSrv = 0;
        SettingsSave();
        s_statusExtra = L"阅读同步已关闭";
        UiUpdateStatus();
        return;
    }

    std::wstring err;
    if (!SyncStart(err))
    {
        MessageBoxW(g_hMain, (L"开启阅读同步失败：" + err).c_str(),
                    L"阅读同步", MB_OK | MB_ICONWARNING);
        return;
    }
    g_set.phoneSrv = 1;
    SettingsSave();

    s_statusExtra = L"阅读同步已开启，正在局域网里找手机…";
    UiUpdateStatus();

    /* 刚开同步时书往往早就打开了（Opening 那一刻还没同步在跑，那时排不到检查），
     * 所以这里补排一次"等对端出现再对账"。 */
    if (g_book.Ok()) {
        s_syncPending = true;
        s_syncTries = 0;
        SetTimer(g_hMain, TIMER_SYNC, 1200, nullptr);
    }
    /* 开着书时不弹"地址提示"：紧接着还会弹"要不要同步"的问句，
     * 两个模态框叠在一起既挡视线又难关闭。地址信息在「帮助 → 手机阅读」里随时能看。 */
    if (!g_book.Ok()) UiShowSyncStatus();
}

/* 弹出手机阅读提示窗；同时确保服务已经启动 */
void UiShowPhone()
{
    std::wstring err;
    if (!ServerStart(0, err))
    {
        MessageBoxW(g_hMain, (L"启动手机阅读服务失败：" + err).c_str(),
                    L"手机阅读", MB_OK | MB_ICONWARNING);
        return;
    }

    /* 拼地址清单 */
    std::vector<std::wstring> urls = ServerUrls();
    std::wstring text;
    if (urls.empty())
    {
        text = L"（没找到局域网地址，请先连上 WiFi 或有线网络）";
    }
    else
    {
        for (size_t i = 0; i < urls.size(); i++)
        {
            if (i) text += L"\r\n";
            text += urls[i];
        }
    }

    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool s_reg = false;
    if (!s_reg)
    {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PhoneBoxProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"NRPhoneBox";
        RegisterClassExW(&wc);
        s_reg = true;
    }

    RECT wr;
    GetWindowRect(g_hMain, &wr);
    int wpx = S(576), hpx = S(268);
    int x = (wr.left + wr.right - wpx) / 2;
    int y = (wr.top + wr.bottom - hpx) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NRPhoneBox", L"手机阅读 — 用手机浏览器打开",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, wpx, hpx, g_hMain, nullptr, hi, nullptr);
    if (!dlg)
    {
        MessageBoxW(g_hMain, text.c_str(), L"手机阅读", MB_OK | MB_ICONINFORMATION);
        return;
    }

    SetDlgItemTextW(dlg, 5, text.c_str());
    EnableWindow(g_hMain, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(dlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!IsWindow(dlg)) EnableWindow(g_hMain, TRUE);
    else EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);
}

/* 跳到手机端最近保存的阅读位置 */
void UiJumpPhoneProgress()
{
    std::wstring path, chapter;
    int off = 0, pct = 0;
    if (!ServerLastPhoneProgress(path, off, chapter, pct))
    {
        MessageBoxW(g_hMain,
                    L"还没收到手机端的阅读进度。\n\n"
                    L"先在手机上打开菜单「帮助 → 手机阅读」，用浏览器读几页，再按 Ctrl+G 跳过来。",
                    L"手机进度", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!g_book.Ok() || g_book.path != path)
    {
        std::wstring hint = L"手机正在读另一本书：\n" + path + L"\n\n要打开它吗？";
        if (MessageBoxW(g_hMain, hint.c_str(), L"手机进度",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        UiOpenFile(path);
    }
    ViewGotoOffset(off, 0);
    wchar_t b[128];
    swprintf(b, 128, L"已跳到手机端位置：%.1f%%", (double)pct / 10.0);
    s_statusExtra = b;
    UiUpdateStatus();
}

/* 抓书进度回调：把进度写进状态栏第 0 段，并抽空处理消息（否则界面在几十章
 * 的抓取过程中会一直白屏、用户以为卡死了）。按住 ESC 可以取消。 */
static bool WebProgressCb(void* ctx, int done, int total, const std::wstring& msg)
{
    (void)ctx;
    std::wstring s;
    if (!msg.empty()) {
        s = msg;
    } else if (total > 0) {
        wchar_t b[128];
        swprintf(b, 128, L"已抓取 %d/%d 章…", done, total);
        s = b;
    } else {
        s = L"正在抓取…";
    }
    /* 进度消息放到状态栏中间那段（"当前章名"平时占的位置）：抓书时章名没有意义，
     * 中间一格最显眼。抓完由 UiUpdateStatus 立刻恢复正常三段。 */
    if (s_hStatus) UiSetStatus(g_book.Ok() ? g_book.title : L"抓取网页小说", s, L"", -1);

    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); return false; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    /* ESC 取消：用 GetAsyncKeyState 而不是收键盘消息 —— 抓取期间焦点可能不在本窗口 */
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return false;
    return true;
}

/* 把抓到的正文写成 UTF-8（带 BOM）文件。带 BOM 是为了让 BookLoad 一眼认出编码，
 * 也方便用户用记事本打开看。 */
static bool WriteTextFileUtf8Bom(const std::wstring& path, const std::wstring& text)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (n < 0) return false;
    std::string u8((size_t)n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &u8[0], n, nullptr, nullptr);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD wrote = 0;
    bool ok = WriteFile(h, bom, 3, &wrote, nullptr) && wrote == 3;
    if (ok && !u8.empty()) {
        ok = WriteFile(h, u8.data(), (DWORD)u8.size(), &wrote, nullptr) && wrote == (DWORD)u8.size();
    }
    CloseHandle(h);
    return ok;
}

/* 菜单/Ctrl+U 的入口：问网址 → 抓 → 存文件 → 打开。 */
void UiOpenWebNovel()
{
    std::wstring url;
    if (!AskUrlBox(url)) return;
    if (!WebIsUrl(url)) {
        MessageBoxW(g_hMain, L"网址要以 http:// 或 https:// 开头。\n\n例如：https://www.example.com/book/123/",
                    L"网址不对", MB_ICONINFORMATION);
        return;
    }

    /* 抓取期间先把自动下滑停掉，避免定时器在"没有书"的状态下乱滚 */
    UiAutoStop();

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring text, title, err;
    bool ok = WebFetchBook(url, text, title, err, WebProgressCb, nullptr);
    SetCursor(old);

    if (!ok) {
        std::wstring m = L"抓取失败：\n\n";
        m += err.empty() ? std::wstring(L"未知原因。") : err;
        m += L"\n\n";
        m += url;
        MessageBoxW(g_hMain, m.c_str(), L"抓取失败", MB_ICONWARNING);
        UiUpdateStatus();
        return;
    }

    /* 落到缓存目录：文件名就是书名，所以打开后标题栏/书架都会显示书名 */
    std::wstring path = WebCachePath(title);
    if (!WriteTextFileUtf8Bom(path, text)) {
        std::wstring m = L"抓到了内容，但写文件失败：\n\n";
        m += path;
        MessageBoxW(g_hMain, m.c_str(), L"保存失败", MB_ICONWARNING);
        UiUpdateStatus();
        return;
    }

    /* 记下来源网址：以后想"重新抓取"或者想找回原站时用得上 */
    IniSetStr(PosSectionFor(path).c_str(), L"src", url.c_str());

    std::wstring chars = std::to_wstring((long long)text.size());
    std::wstring m = L"抓取完成：";
    m += title;
    m += L"\n\n共 ";
    m += chars;
    m += L" 字\n已保存到：";
    m += path;
    m += L"\n\n现在打开阅读吗？";
    if (MessageBoxW(g_hMain, m.c_str(), L"抓取完成", MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        UiUpdateStatus();
        return;
    }
    UiOpenFile(path);
}

/* ---------------- 书源搜索与书源管理 ----------------
 * 为什么单独做一个对话框：通用的「打开网页小说」要让用户自己找目录页网址，
 * 门槛太高。书源把"输入书名 → 搜索结果里挑一本 → 抓整本"串成一条龙。
 *
 * 界面就是几样东西：书源下拉框（切换用哪个源）、关键词输入框、结果列表、
 * 下载按钮、关闭按钮，以及一个「管理…」按钮打开书源管理对话框。
 * 列表里一条 = 一本书，显示"书名 · 简介前 60 字"，方便从同名书里挑。
 * 下载期间用 WebProgressCb 把进度写进主窗口状态栏，并按 ESC 可取消。
 *
 * 书源本身不写死在代码里（见 source.h）：规则存在 ini 的 [sources] 段，
 * 用户可以在「书源管理」里改规则、加新源、删源，所以这个对话框里的文字
 * 一律不提具体站点名。
 */
#define SRC_EDIT   101      /* 关键词输入框 */
#define SRC_LIST   102      /* 搜索结果列表 */
#define SRC_SEARCH 103      /* 搜索按钮 */
#define SRC_DOWN   104      /* 下载并打开 */
#define SRC_CLOSE  105      /* 关闭 */
#define SRC_COMBO  106      /* 书源下拉框 */
#define SRC_MGR    107      /* 「管理…」按钮 */

static HWND s_srcEdit = nullptr;
static HWND s_srcList = nullptr;
static HWND s_srcCombo = nullptr;            /* 书源下拉框 */
static std::vector<WebBookHit> s_srcHits;    /* 与列表项一一对应 */
static std::wstring s_srcOpenPath;           /* 关窗后要打开的文件 */
static bool s_srcBusy = false;               /* 正在联网，挡住重复点击 */
static bool s_srcReady = false;              /* 窗口类注册过没有 */

static void ShowSourceMgr(HWND parent);      /* 实现在下面，先声明 */

/* 用当前的 SourceList() 重填下拉框，并选中 SourceCurrent()。
 * 书源管理里改过规则或者加删过源之后都要调一次。 */
static void SrcRefillCombo()
{
    if (!s_srcCombo) return;
    SendMessageW(s_srcCombo, CB_RESETCONTENT, 0, 0);
    std::vector<BookSource>& list = SourceList();
    for (size_t i = 0; i < list.size(); i++) {
        SendMessageW(s_srcCombo, CB_ADDSTRING, 0, (LPARAM)list[i].name.c_str());
    }
    int cur = SourceCurrent();
    if (cur < 0 || cur >= (int)list.size()) cur = 0;
    SendMessageW(s_srcCombo, CB_SETCURSEL, (WPARAM)cur, 0);
}

/* 点「搜索」：问一次站点，把结果灌进列表。 */
static void SrcDoSearch(HWND dlg)
{
    if (s_srcBusy) return;
    wchar_t buf[256];
    buf[0] = 0;
    if (s_srcEdit) GetWindowTextW(s_srcEdit, buf, 256);
    std::wstring kw = TrimW(buf);
    NrTrace(L"SRC search enter kw='%s' edit=%p", kw.c_str(), (void*)s_srcEdit);
    if (kw.empty()) {
        MessageBoxW(dlg, L"先输入书名或作者。", L"书源搜索", MB_ICONINFORMATION);
        return;
    }

    if (s_srcList) SendMessageW(s_srcList, LB_RESETCONTENT, 0, 0);
    s_srcHits.clear();

    s_srcBusy = true;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::vector<WebBookHit> hits;
    std::wstring err;
    bool ok = WebSourceSearch(kw, hits, err);
    SetCursor(old);
    s_srcBusy = false;
    NrTrace(L"SRC search done ok=%d hits=%d err='%s'", (int)ok, (int)hits.size(), err.c_str());

    if (!ok) {
        MessageBoxW(dlg, err.c_str(), L"没搜到", MB_ICONINFORMATION);
        return;
    }
    s_srcHits = hits;
    for (size_t i = 0; i < hits.size(); i++) {
        std::wstring line = hits[i].title;
        if (!hits[i].desc.empty()) {
            line += L"  ·  ";
            line += hits[i].desc;
        }
        SendMessageW(s_srcList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    SendMessageW(s_srcList, LB_SETCURSEL, 0, 0);
}

/* 点「下载并打开」：抓整本 → 存 txt → 问要不要马上看。 */
static void SrcDoDownload(HWND dlg)
{
    int idx0 = s_srcList ? (int)SendMessageW(s_srcList, LB_GETCURSEL, 0, 0) : -1;
    NrTrace(L"SRC dl enter busy=%d idx=%d hits=%d list=%p", (int)s_srcBusy, idx0,
            (int)s_srcHits.size(), (void*)s_srcList);
    (void)idx0;                 /* release 版里 NrTrace 是空宏，避免"未使用变量"警告 */
    if (s_srcBusy) return;
    int idx = s_srcList ? (int)SendMessageW(s_srcList, LB_GETCURSEL, 0, 0) : -1;
    if (idx < 0 || idx >= (int)s_srcHits.size()) {
        MessageBoxW(dlg, L"先在列表里选一本书。", L"书源搜索", MB_ICONINFORMATION);
        return;
    }
    const WebBookHit hit = s_srcHits[idx];

    s_srcBusy = true;
    if (s_srcEdit) EnableWindow(s_srcEdit, FALSE);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring text, title, err;
    bool ok = WebSourceFetchBook(hit.url, text, title, err, WebProgressCb, nullptr, hit.title);
    SetCursor(old);
    if (s_srcEdit) EnableWindow(s_srcEdit, TRUE);
    s_srcBusy = false;
    UiUpdateStatus();           /* 把状态栏从"正在抓…"恢复成正常内容 */

    if (!ok) {
        std::wstring m = L"下载失败：\n\n";
        m += err.empty() ? std::wstring(L"未知原因。") : err;
        m += L"\n\n";
        m += hit.url;
        MessageBoxW(dlg, m.c_str(), L"下载失败", MB_ICONWARNING);
        return;
    }

    /* 抓取被 ESC 打断时 err 非空但 text 有内容：让用户决定要不要留半本。
     * 不问就直接存，用户会以为"整本抓完了"，其实后面几十章都没有。 */
    if (!err.empty()) {
        std::wstring m = err + L"\n\n已抓到的部分要保存成文件吗？";
        if (MessageBoxW(dlg, m.c_str(), L"抓取中断", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }

    std::wstring path = WebCachePath(title);
    if (!WriteTextFileUtf8Bom(path, text)) {
        std::wstring m = L"抓到了内容，但写文件失败：\n\n";
        m += path;
        MessageBoxW(dlg, m.c_str(), L"保存失败", MB_ICONWARNING);
        return;
    }
    IniSetStr(PosSectionFor(path).c_str(), L"src", hit.url.c_str());

    wchar_t msg[1400];
    swprintf(msg, 1400, L"抓完了：《%s》\n共 %d 字\n已保存到：\n%s\n\n现在打开阅读吗？",
             title.c_str(), (int)text.size(), path.c_str());
    if (MessageBoxW(dlg, msg, L"下载完成", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        s_srcOpenPath = path;
        DestroyWindow(dlg);
    }
}

static LRESULT CALLBACK SourceBoxProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        /* 第一行：选书源 + 管理按钮。书源可以随时换，所以放在最上面 */
        CreateWindowExW(0, L"STATIC", L"书源：", WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(16), S(20), S(50), S(20), h, nullptr, nullptr, nullptr);
        s_srcCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     CBS_DROPDOWNLIST,
                                     S(70), S(16), S(300), S(220), h, (HMENU)SRC_COMBO, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"管理(&M)...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(378), S(16), S(90), S(26), h, (HMENU)SRC_MGR, nullptr, nullptr);
        SrcRefillCombo();

        /* 第二行：关键词 + 搜索 */
        CreateWindowExW(0, L"STATIC", L"书名或作者：", WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(16), S(56), S(200), S(20), h, nullptr, nullptr, nullptr);
        s_srcEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    S(16), S(80), S(400), S(26), h, (HMENU)SRC_EDIT, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"搜索(&S)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(424), S(80), S(86), S(26), h, (HMENU)SRC_SEARCH, nullptr, nullptr);
        s_srcList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                    LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                    S(16), S(116), S(494), S(220), h, (HMENU)SRC_LIST, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"下载并打开(&D)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(16), S(348), S(140), S(30), h, (HMENU)SRC_DOWN, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(438), S(348), S(72), S(30), h, (HMENU)SRC_CLOSE, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC",
                        L"书源可以换（「管理…」里能改规则、加新站）。整本少则几十章、"
                        L"多则一两千章，下载时进度显示在主窗口状态栏，按 ESC 可取消。",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(16), S(386), S(494), S(44), h, nullptr, nullptr, nullptr);
        if (s_srcEdit) SetFocus(s_srcEdit);
        return 0;
    }
    case WM_COMMAND:
        NrTrace(L"SRC dlg cmd id=%d code=%d", (int)LOWORD(w), (int)HIWORD(w));
        switch (LOWORD(w)) {
        case SRC_SEARCH: SrcDoSearch(h); return 0;
        case SRC_DOWN:   SrcDoDownload(h); return 0;
        case SRC_CLOSE:  DestroyWindow(h); return 0;
        case SRC_MGR:
            /* 打开书源管理；关掉之后规则可能变了，重填下拉框 */
            ShowSourceMgr(h);
            SrcRefillCombo();
            return 0;
        case SRC_COMBO:
            /* 下拉框换选中项 = 换书源，立刻落盘，下次启动还用这个 */
            if (HIWORD(w) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(s_srcCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0) SourceSetCurrent(sel);
            }
            return 0;
        case SRC_LIST:
            /* 双击列表项 = 直接下载这一本，省一次点按钮 */
            if (HIWORD(w) == LBN_DBLCLK) { SrcDoDownload(h); return 0; }
            return 0;
        }
        return 0;
    case WM_CLOSE:
        NrTrace(L"SRC dlg WM_CLOSE");
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        NrTrace(L"SRC dlg WM_DESTROY");
        s_srcEdit = nullptr;
        s_srcList = nullptr;
        s_srcCombo = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ---------------- 书源管理对话框 ----------------
 * 一个书源 = 名字 + 五条规则（见 source.h）。这个对话框让用户直接改这几样东西：
 * 左边是书源列表，右边是当前书源的名字与五条规则。
 *
 * 交互约定（都是为了"别把用户改了一半的规则弄丢"）：
 *   - 切换列表项之前，先把右边编辑框的内容写回内存（否则改一半切走就没了）；
 *   - 「保存」写回内存并落盘 ini，下次启动还在；
 *   - 「新增」最多 SourceList 上限（16 个）；「删除」不允许删 0 号内置源；
 *   - 「恢复内置」把内置源的规则重新塞回 0 号，规则被改坏了能一键还原。
 */
#define MGR_LIST     201    /* 左侧书源列表 */
#define MGR_NAME     202    /* 名称 */
#define MGR_SEARCH   203    /* 搜索网址模板 */
#define MGR_LISTRE   204    /* 结果页正则 */
#define MGR_TOCSTART 205    /* 目录容器起点（可空） */
#define MGR_TOCRE    206    /* 目录正则 */
#define MGR_CONTENT  207    /* 正文正则 */
#define MGR_ADD      210    /* 新增 */
#define MGR_SAVE     211    /* 保存 */
#define MGR_DEL      212    /* 删除 */
#define MGR_RESET    213    /* 恢复内置规则 */
#define MGR_CLOSE    214    /* 关闭 */

static HWND s_mgrList = nullptr;
static HWND s_mgrName = nullptr;
static HWND s_mgrSearch = nullptr;
static HWND s_mgrListRe = nullptr;
static HWND s_mgrTocStart = nullptr;
static HWND s_mgrTocRe = nullptr;
static HWND s_mgrContent = nullptr;
static bool s_mgrReady = false;      /* 窗口类注册过没有 */
static int  s_mgrCur = -1;           /* 右边编辑框里显示的是第几个书源 */

/* 把内存里的书源填进右边编辑框 */
static void MgrLoad(int idx)
{
    std::vector<BookSource>& list = SourceList();
    if (idx < 0 || idx >= (int)list.size()) { s_mgrCur = -1; return; }
    const BookSource& s = list[idx];
    if (s_mgrName)     SetWindowTextW(s_mgrName, s.name.c_str());
    if (s_mgrSearch)   SetWindowTextW(s_mgrSearch, s.search.c_str());
    if (s_mgrListRe)   SetWindowTextW(s_mgrListRe, s.listRe.c_str());
    if (s_mgrTocStart) SetWindowTextW(s_mgrTocStart, s.tocStart.c_str());
    if (s_mgrTocRe)    SetWindowTextW(s_mgrTocRe, s.tocRe.c_str());
    if (s_mgrContent)  SetWindowTextW(s_mgrContent, s.contentRe.c_str());
    s_mgrCur = idx;
    if (s_mgrList) SendMessageW(s_mgrList, LB_SETCURSEL, (WPARAM)idx, 0);
}

/* 把右边编辑框的内容写回内存（不落盘，落盘由调用方决定） */
static void MgrCommit()
{
    std::vector<BookSource>& list = SourceList();
    if (s_mgrCur < 0 || s_mgrCur >= (int)list.size()) return;
    BookSource& s = list[s_mgrCur];
    wchar_t buf[2048];
    if (s_mgrName) {
        buf[0] = 0; GetWindowTextW(s_mgrName, buf, 2048);
        std::wstring t = TrimW(buf);
        if (!t.empty()) s.name = t;      /* 名字留空就保留原名，避免列表里出现空白行 */
    }
    if (s_mgrSearch)   { buf[0] = 0; GetWindowTextW(s_mgrSearch, buf, 2048);   s.search = buf; }
    if (s_mgrListRe)   { buf[0] = 0; GetWindowTextW(s_mgrListRe, buf, 2048);   s.listRe = buf; }
    if (s_mgrTocStart) { buf[0] = 0; GetWindowTextW(s_mgrTocStart, buf, 2048); s.tocStart = buf; }
    if (s_mgrTocRe)    { buf[0] = 0; GetWindowTextW(s_mgrTocRe, buf, 2048);    s.tocRe = buf; }
    if (s_mgrContent)  { buf[0] = 0; GetWindowTextW(s_mgrContent, buf, 2048);  s.contentRe = buf; }
}

/* 重填左侧列表（名字变了、增删过源之后都要调） */
static void MgrFillList()
{
    if (!s_mgrList) return;
    SendMessageW(s_mgrList, LB_RESETCONTENT, 0, 0);
    std::vector<BookSource>& list = SourceList();
    for (size_t i = 0; i < list.size(); i++) {
        std::wstring line = list[i].name;
        if (i == 0) line += L"（内置）";
        SendMessageW(s_mgrList, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (s_mgrCur >= 0) SendMessageW(s_mgrList, LB_SETCURSEL, (WPARAM)s_mgrCur, 0);
}

static LRESULT CALLBACK SourceMgrProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        /* 左侧：书源列表 + 新增/删除 */
        s_mgrList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                    LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                    S(16), S(16), S(180), S(300), h, (HMENU)MGR_LIST, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"新增(&N)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(16), S(324), S(86), S(28), h, (HMENU)MGR_ADD, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"删除(&X)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(108), S(324), S(86), S(28), h, (HMENU)MGR_DEL, nullptr, nullptr);

        /* 右侧：六项规则。标签在上、输入框在下，一行一行排下来 */
        struct { const wchar_t* label; int labelY; int editY; int id; } rows[6] = {
            { L"名称（随便起，方便自己认）",            16,  34, MGR_NAME },
            { L"搜索网址（关键词处写 {kw}，留空则不能搜）", 64,  82, MGR_SEARCH },
            { L"结果页正则（第1组网址 / 第2组书名 / 第3组简介可省）", 112, 130, MGR_LISTRE },
            { L"目录容器起点（普通文字，可留空）",       160, 178, MGR_TOCSTART },
            { L"目录正则（第1组网址 / 第2组章节名）",     208, 226, MGR_TOCRE },
            { L"正文正则（第1组正文，可留空）",          256, 274, MGR_CONTENT },
        };
        for (int i = 0; i < 6; i++) {
            CreateWindowExW(0, L"STATIC", rows[i].label, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            S(212), S(rows[i].labelY), S(430), S(18), h, nullptr, nullptr, nullptr);
            HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      S(212), S(rows[i].editY), S(430), S(26), h,
                                      (HMENU)(INT_PTR)rows[i].id, nullptr, nullptr);
            switch (rows[i].id) {
            case MGR_NAME:     s_mgrName = ed; break;
            case MGR_SEARCH:   s_mgrSearch = ed; break;
            case MGR_LISTRE:   s_mgrListRe = ed; break;
            case MGR_TOCSTART: s_mgrTocStart = ed; break;
            case MGR_TOCRE:    s_mgrTocRe = ed; break;
            case MGR_CONTENT:  s_mgrContent = ed; break;
            }
        }

        CreateWindowExW(0, L"BUTTON", L"保存(&S)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        S(212), S(312), S(96), S(30), h, (HMENU)MGR_SAVE, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"恢复内置规则(&R)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(316), S(312), S(140), S(30), h, (HMENU)MGR_RESET, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        S(560), S(312), S(82), S(30), h, (HMENU)MGR_CLOSE, nullptr, nullptr);
        CreateWindowExW(0, L"STATIC",
                        L"规则说明：搜索网址里的 {kw} 会被换成书名。三条正则各只用前两组括号 —— "
                        L"第 1 组是网址、第 2 组是名字。留空的规则会自动退化成"
                        L"「页面上所有链接」/「整页文字」，所以最省事可以只填搜索网址和目录正则。",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(212), S(352), S(430), S(80), h, nullptr, nullptr, nullptr);

        MgrFillList();
        MgrLoad(0);
        if (s_mgrList) SetFocus(s_mgrList);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case MGR_LIST:
            if (HIWORD(w) == LBN_SELCHANGE && s_mgrList) {
                int sel = (int)SendMessageW(s_mgrList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel != s_mgrCur) {
                    MgrCommit();          /* 先把改了一半的内容收下 */
                    MgrLoad(sel);
                }
            }
            return 0;
        case MGR_ADD: {
            MgrCommit();
            int i = SourceAdd();
            if (i < 0) {
                MessageBoxW(h, L"书源已经满了（最多 16 个）。先删掉不用的再加。",
                            L"书源管理", MB_ICONINFORMATION);
                return 0;
            }
            SourceSave();
            MgrFillList();
            MgrLoad(i);
            return 0;
        }
        case MGR_SAVE:
            MgrCommit();
            SourceSave();
            MgrFillList();
            MessageBoxW(h, L"已保存，下次启动还用这些规则。", L"书源管理", MB_ICONINFORMATION);
            return 0;
        case MGR_DEL: {
            if (s_mgrCur <= 0) {
                MessageBoxW(h, L"第 1 个是内置示例源，删不了；想换成自己的规则可以改它。",
                            L"书源管理", MB_ICONINFORMATION);
                return 0;
            }
            if (MessageBoxW(h, L"确定删掉这个书源吗？", L"书源管理",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            int del = s_mgrCur;
            s_mgrCur = -1;              /* 先断开，免得删完还往已失效的下标里写 */
            SourceDelete(del);
            SourceSave();
            MgrFillList();
            MgrLoad(0);
            return 0;
        }
        case MGR_RESET:
            if (MessageBoxW(h, L"把第 1 个书源（内置示例源）的规则恢复成出厂设置吗？",
                            L"书源管理", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            SourceList()[0] = SourceBuiltin();
            SourceList()[0].builtin = true;
            SourceSave();
            MgrFillList();
            MgrLoad(0);
            MessageBoxW(h, L"内置规则已恢复。", L"书源管理", MB_ICONINFORMATION);
            return 0;
        case MGR_CLOSE:
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        s_mgrList = nullptr;
        s_mgrName = nullptr;
        s_mgrSearch = nullptr;
        s_mgrListRe = nullptr;
        s_mgrTocStart = nullptr;
        s_mgrTocRe = nullptr;
        s_mgrContent = nullptr;
        s_mgrCur = -1;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* 以模态方式打开书源管理。parent 一般是书源搜索对话框。 */
static void ShowSourceMgr(HWND parent)
{
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!s_mgrReady) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SourceMgrProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"NRSourceMgr";
        RegisterClassExW(&wc);
        s_mgrReady = true;
    }
    if (SourceList().empty()) SourceLoad();

    RECT wr;
    GetWindowRect(parent ? parent : g_hMain, &wr);
    int wpx = S(664), hpx = S(460);
    int x = (wr.left + wr.right - wpx) / 2;
    int y = (wr.top + wr.bottom - hpx) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NRSourceMgr",
                               L"书源管理（改规则 / 加新站）",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, wpx, hpx, parent ? parent : g_hMain, nullptr, hi, nullptr);
    if (!dlg) return;

    if (parent) EnableWindow(parent, FALSE);
    else EnableWindow(g_hMain, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (parent) EnableWindow(parent, TRUE);
    else EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(parent ? parent : g_hMain);
}

/* 菜单「文件 → 书源搜索」的入口。对话框关掉之后再去打开文件 ——
 * 下载过程中主窗口一直是禁用的，边下边开书容易把状态搞乱。 */
void UiSourceSearch()
{
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!s_srcReady) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SourceBoxProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"NRSourceBox";
        RegisterClassExW(&wc);
        s_srcReady = true;
    }
    if (SourceList().empty()) SourceLoad();   /* 第一次用：把内置源读/写进 ini */

    s_srcHits.clear();
    s_srcOpenPath.clear();
    s_srcBusy = false;

    RECT wr;
    GetWindowRect(g_hMain, &wr);
    int wpx = S(526), hpx = S(466);
    int x = (wr.left + wr.right - wpx) / 2;
    int y = (wr.top + wr.bottom - hpx) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NRSourceBox",
                               L"书源搜索（输入书名，下载整本）",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, wpx, hpx, g_hMain, nullptr, hi, nullptr);
    if (!dlg) return;

    EnableWindow(g_hMain, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hMain, TRUE);
    SetForegroundWindow(g_hMain);

    if (!s_srcOpenPath.empty()) UiOpenFile(s_srcOpenPath);
}

/* 弹「打开文件」对话框，返回选中路径；用户取消返回空串。
 * 细节：过滤器给两组（txt 默认 + 所有文件），末尾的 "\0\0" 是必需的收尾，
 * 漏了会把相邻内存当成字符串读出一串乱码；OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
 * 从对话框层面就挡掉不存在的路径；OFN_HIDEREADONLY 不显示「只读打开」复选框，
 * 因为我们从不写回原文件。
 * 初始目录取 g_set.lastFile 的父目录：连续打开同一目录下的多本书时不用每次点进去。
 * initDir 声明成 static 而不是局部变量，是因为 OPENFILENAME 只存指针，函数返回后
 * 那块内存按约定仍须有效；截目录的办法就是把最后一个反斜杠写成 0。
 */
static std::wstring OpenDlg()
{
    wchar_t buf[MAX_PATH * 2];
    ZeroMemory(buf, sizeof(buf));
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter =
        L"所有支持的格式 (*.txt;*.epub;*.pdf;*.docx;*.html;*.md)\0"
        L"*.txt;*.epub;*.pdf;*.docx;*.html;*.htm;*.md\0"
        L"纯文本 (*.txt;*.md;*.html;*.htm)\0*.txt;*.md;*.html;*.htm\0"
        L"电子书 (*.epub)\0*.epub\0"
        L"PDF 文档 (*.pdf)\0*.pdf\0"
        L"Word 文档 (*.docx)\0*.docx\0"
        L"所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"选择小说文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    static wchar_t initDir[MAX_PATH * 2];
    if (!g_set.lastFile.empty()) {
        lstrcpynW(initDir, g_set.lastFile.c_str(), MAX_PATH * 2);
        wchar_t* p = wcsrchr(initDir, L'\\');
        if (p) { *p = 0; ofn.lpstrInitialDir = initDir; }
    }
    if (!GetOpenFileNameW(&ofn)) return L"";
    return std::wstring(buf);
}

/* ---------------- 菜单 ---------------- */

/* 按 ini 里的记录重建「最近打开」子菜单（无参数，读 RecentGet(i)，0 是最新一条）。
 * 先 DeleteMenu 清空到 count == 0，再按顺序重新 AppendMenu；命令 id 用
 * IDM_RECENT_BASE + i —— id 与「第几条记录」一一对应，OnCommand 靠这个减法反查
 * 路径，所以顺序绝不能乱。
 * 细节：只显示文件名不留完整路径（路径太长会把菜单撑到屏幕外）；超过 52 字截断；
 * 文件名里的 & 要写成 &&，否则会被当成助记符下划线，带 & 的文件名显示起来缺字；
 * 一条记录都没有时放一条灰色的「（暂无记录）」占位，id 用 BASE+99 故意不可点。
 * 最后 DrawMenuBar：菜单项增删后必须重画菜单栏，否则要等下次打开菜单才更新。
 */
void UiRebuildRecentMenu()
{
    if (!s_hRecentMenu) return;
    while (GetMenuItemCount(s_hRecentMenu) > 0) DeleteMenu(s_hRecentMenu, 0, MF_BYPOSITION);

    int added = 0;
    for (int i = 0; i < IDM_RECENT_MAX; i++) {
        std::wstring p = RecentGet(i);
        if (p.empty()) break;
        std::wstring name = p;
        size_t k = name.find_last_of(L"\\/");
        if (k != std::wstring::npos) name = name.substr(k + 1);
        std::wstring label;
        wchar_t num[8];
        wsprintfW(num, L"%d  ", i + 1);
        label = num;
        label += name;
        if (label.size() > 52) label = label.substr(0, 52) + L"…";
        std::wstring esc;
        for (size_t c = 0; c < label.size(); c++) {
            if (label[c] == L'&') esc += L"&&";
            else esc += label[c];
        }
        AppendMenuW(s_hRecentMenu, MF_STRING, IDM_RECENT_BASE + i, esc.c_str());
        added++;
    }
    if (added == 0) AppendMenuW(s_hRecentMenu, MF_STRING | MF_GRAYED, IDM_RECENT_BASE + 99, L"（暂无记录）");
    if (g_hMain) DrawMenuBar(g_hMain);
}

/* 建整个菜单栏并返回句柄（在 wWinMain 里通过 SetMenu 挂到主窗口）。
 * 结构：文件 / 阅读 / 搜索 / 帮助 四个下拉，id 全部来自 ids.h 的 IDM_*，
 * 由 OnCommand 统一分派。
 * 约定：菜单文字里的 "\tCtrl+O" 只是显示用的文字，真正让快捷键生效的是
 * BuildAccel 建的加速键表 —— 两处必须一起改，否则会出现「菜单上写着却按不出来」。
 * 「最近打开」是挂在「文件」下的二级菜单，句柄存进 s_hRecentMenu，内容由
 * UiRebuildRecentMenu 单独维护（它随时会变，不该与静态菜单混在一起建）。
 * 注意本函数只调一次；之后改菜单项状态要用 AutoSyncMenu 里的 CheckMenuItem /
 * EnableMenuItem，别重建菜单栏，否则会丢掉勾选与置灰状态。
 */
static HMENU BuildMenu()
{
    HMENU h = CreateMenu();
    HMENU f = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, IDM_OPEN, L"打开(&O)...\tCtrl+O");
    s_hRecentMenu = CreatePopupMenu();
    AppendMenuW(f, MF_POPUP, (UINT_PTR)s_hRecentMenu, L"最近打开(&R)");
    AppendMenuW(f, MF_STRING, IDM_RELOAD, L"重新载入(&L)\tF5");
    /* 关闭当前书：回到首页书架。和「退出」分开是有意的 —— 想换一本书时不该把程序关掉 */
    AppendMenuW(f, MF_STRING, IDM_CLOSEFILE, L"关闭当前书(&C)\tCtrl+W");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_OPEN_URL, L"打开网页小说(&U)...\tCtrl+U");
    /* 书源搜索：按书源规则输入书名就能搜到并下载整本，不用自己找目录页网址。
     * 书源可以在「书源管理」里换，所以菜单里不提具体站点名 */
    AppendMenuW(f, MF_STRING, IDM_SOURCE, L"书源搜索(&S)...\tCtrl+S");
    AppendMenuW(f, MF_STRING, IDM_SOURCE_MGR, L"书源管理(&B)...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_EXIT, L"退出(&X)");
    AppendMenuW(h, MF_POPUP, (UINT_PTR)f, L"文件(&F)");

    HMENU r = CreatePopupMenu();
    AppendMenuW(r, MF_STRING, IDM_PREV_CH, L"上一章\t←");
    AppendMenuW(r, MF_STRING, IDM_NEXT_CH, L"下一章\t→");
    AppendMenuW(r, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(r, MF_STRING, IDM_FONT_UP, L"字号增大\tCtrl+=");
    AppendMenuW(r, MF_STRING, IDM_FONT_DOWN, L"字号减小\tCtrl+-");
    AppendMenuW(r, MF_STRING, IDM_LINE_SPACE, L"切换行距");
    AppendMenuW(r, MF_STRING, IDM_THEME, L"日间 / 夜间切换\tF2");
    AppendMenuW(r, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(r, MF_STRING, IDM_AUTO_TOGGLE, L"自动下滑\tF6");
    AppendMenuW(r, MF_STRING, IDM_AUTO_FAST, L"下滑加速\tF8");
    AppendMenuW(r, MF_STRING, IDM_AUTO_SLOW, L"下滑减速\tF7");
    AppendMenuW(r, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(r, MF_STRING, IDM_TOC, L"显示/隐藏目录\tCtrl+T");
    /* PDF 专用：原始版式（整页渲染，版式 100% 还原）与文字重排（可搜索、可换字体）之间切换。
       不是 PDF 时这条会被 UiSyncMenuState 置灰，避免用户按了没反应。 */
    AppendMenuW(r, MF_STRING, IDM_PDF_MODE, L"PDF 原始版式（F9 切换）\tF9");

    /* 书签子菜单：加 / 看 / 删。删除不给快捷键 —— 删数据不该有"顺手就按到"的键。 */
    HMENU bm = CreatePopupMenu();
    AppendMenuW(bm, MF_STRING, IDM_BM_ADD,  L"添加书签\tCtrl+D");
    AppendMenuW(bm, MF_STRING, IDM_BM_LIST, L"书签列表\tCtrl+B");
    AppendMenuW(bm, MF_STRING, IDM_BM_DEL,  L"删除此段书签");
    AppendMenuW(r, MF_POPUP, (UINT_PTR)bm, L"书签(&M)");

    /* 高亮与笔记子菜单（1.6.0）：标 4 种颜色、写笔记、看列表、导出。
     * 颜色不做成"先选颜色再标"的两步，而是一键一色 —— 读到好句子时手边动作越少越好。 */
    HMENU hl = CreatePopupMenu();
    AppendMenuW(hl, MF_STRING, IDM_HL_COLOR_BASE + 0, L"黄色高亮\tCtrl+H");
    AppendMenuW(hl, MF_STRING, IDM_HL_COLOR_BASE + 1, L"绿色高亮");
    AppendMenuW(hl, MF_STRING, IDM_HL_COLOR_BASE + 2, L"蓝色高亮");
    AppendMenuW(hl, MF_STRING, IDM_HL_COLOR_BASE + 3, L"粉色高亮");
    AppendMenuW(hl, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hl, MF_STRING, IDM_HL_NOTE, L"写笔记...\tCtrl+Shift+N");
    AppendMenuW(hl, MF_STRING, IDM_HL_LIST, L"高亮与笔记列表\tCtrl+L");
    AppendMenuW(hl, MF_STRING, IDM_HL_DEL,  L"删除此处高亮\tCtrl+Shift+X");
    AppendMenuW(hl, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hl, MF_STRING, IDM_HL_EXPORT, L"导出笔记为 Markdown...");
    AppendMenuW(r, MF_POPUP, (UINT_PTR)hl, L"高亮与笔记(&H)");

    /* 排版子菜单（1.6.0）：每本书一份，所以菜单里不写"全局"字样 */
    HMENU lay = CreatePopupMenu();
    HMENU mg = CreatePopupMenu();
    AppendMenuW(mg, MF_STRING, IDM_LAY_MARGIN_BASE + 0, L"窄");
    AppendMenuW(mg, MF_STRING, IDM_LAY_MARGIN_BASE + 1, L"标准");
    AppendMenuW(mg, MF_STRING, IDM_LAY_MARGIN_BASE + 2, L"宽");
    AppendMenuW(lay, MF_POPUP, (UINT_PTR)mg, L"页边距");
    AppendMenuW(lay, MF_STRING, IDM_LAY_INDENT, L"首行缩进两字\tCtrl+Shift+I");
    AppendMenuW(lay, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(lay, MF_STRING, IDM_LAY_PAGED, L"分页模式\tCtrl+Shift+P");
    AppendMenuW(lay, MF_STRING, IDM_LAY_COLS,  L"双栏显示\tCtrl+Shift+C");
    AppendMenuW(lay, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(lay, MF_STRING, IDM_LAY_FORCE_FONT, L"强制使用全局字体");
    AppendMenuW(lay, MF_STRING, IDM_LAY_RESET, L"恢复本书排版默认值");
    AppendMenuW(r, MF_POPUP, (UINT_PTR)lay, L"排版(&L)");

    /* 外观子菜单：字体 / 导入字体 / 自定义配色 / 预设方案。
     * 预设放在二级子菜单里，是因为一共五套，平铺会把「阅读」菜单撑得太长。 */
    HMENU look = CreatePopupMenu();
    AppendMenuW(look, MF_STRING, IDM_PICK_FONT,   L"选择字体...");
    AppendMenuW(look, MF_STRING, IDM_IMPORT_FONT, L"导入字体文件...");
    AppendMenuW(look, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(look, MF_STRING, IDM_PICK_COLORS, L"自定义配色...");
    HMENU pre = CreatePopupMenu();
    AppendMenuW(pre, MF_STRING, IDM_PRESET_BASE + 0, L"日间（米色纸）");
    AppendMenuW(pre, MF_STRING, IDM_PRESET_BASE + 1, L"夜间（深色）");
    AppendMenuW(pre, MF_STRING, IDM_PRESET_BASE + 2, L"护眼绿");
    AppendMenuW(pre, MF_STRING, IDM_PRESET_BASE + 3, L"羊皮纸");
    AppendMenuW(pre, MF_STRING, IDM_PRESET_BASE + 4, L"高对比暗");
    AppendMenuW(look, MF_POPUP, (UINT_PTR)pre, L"配色方案");
    AppendMenuW(r, MF_POPUP, (UINT_PTR)look, L"外观(&A)");

    AppendMenuW(h, MF_POPUP, (UINT_PTR)r, L"阅读(&R)");

    HMENU s = CreatePopupMenu();
    AppendMenuW(s, MF_STRING, IDM_FIND, L"全文搜索(&F)...\tCtrl+F");
    AppendMenuW(s, MF_STRING, IDM_FIND_NEXT, L"查找下一个\tF3");
    AppendMenuW(s, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(s, MF_STRING, IDM_COPY, L"复制选中(&C)\tCtrl+C");
    AppendMenuW(s, MF_STRING, IDM_SELALL, L"全选当前书(&A)\tCtrl+A");
    AppendMenuW(h, MF_POPUP, (UINT_PTR)s, L"搜索(&S)");

    HMENU hp = CreatePopupMenu();
    AppendMenuW(hp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hp, MF_STRING, IDM_PHONE, L"手机阅读（局域网）(&P)...\tCtrl+G");
    AppendMenuW(hp, MF_STRING, IDM_PHONE_JUMP, L"跳到手机读到的位置\tCtrl+Shift+G");
    /* 1.7.0：总开关。默认关，开了才监听端口；勾选状态由 AutoSyncMenu 维护 */
    AppendMenuW(hp, MF_STRING, IDM_PHONE_SYNC, L"开启手机同步（局域网）");
    AppendMenuW(hp, MF_SEPARATOR, 0, nullptr);
    /* 1.6.0：隐私与老板键。放在帮助菜单里是因为它们属于"环境设置"，
     * 和阅读/排版不是一类；但都是随时要能按到的东西，所以放最显眼的位置。 */
    AppendMenuW(hp, MF_STRING, IDM_KEYS_EDIT, L"快捷键设置(&K)...\tCtrl+F12");
    AppendMenuW(hp, MF_STRING, IDM_PRIVACY, L"隐私模式（隐藏书名）\tCtrl+Shift+H");
    /* 老板键的组合是可以改的（Ctrl+F12），所以菜单后面的提示要现读当前值，
     * 不能像别的项那样写死字符串。 */
    {
        std::wstring bossItem = L"立即隐藏 / 唤回窗口\t" + g_set.bossKey;
        AppendMenuW(hp, MF_STRING, IDM_BOSS, bossItem.c_str());
    }
    AppendMenuW(hp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hp, MF_STRING, IDM_KEYS, L"快捷键说明");
    AppendMenuW(hp, MF_STRING, IDM_ABOUT, L"关于");
    AppendMenuW(h, MF_POPUP, (UINT_PTR)hp, L"帮助(&H)");
    return h;
}

/* ---------------- 命令处理 ---------------- */

/* 弹「快捷键说明」对话框：一大段静态文本，按「阅读 / 文件与搜索 / 自动下滑」分组，
 * 分组顺序和用户找东西的习惯一致（先看怎么翻，再看怎么找）。
 * 注意这里列的是「用户能按的键」，其中一部分由 view.cpp 的键盘处理实现
 * （方向键、空格、Home/End、Ctrl+滚轮），另一部分由加速键表实现 ——
 * 所以任何一处增删快捷键，这段说明和 BuildAccel 都要同步更新。
 */
static void ShowKeys()
{
    const wchar_t* t =
        L"阅读\n"
        L"  ↑ ↓          上下滚动一行\n"
        L"  PgUp / PgDn  上一页 / 下一页（空格、Shift+空格 同）\n"
        L"  ← →          上一章 / 下一章\n"
        L"  Home / End   全书开头 / 结尾\n"
        L"  Ctrl+滚轮    调整字号\n"
        L"  鼠标拖动     选中文字，双击选中整段\n"
        L"\n"
        L"文件与搜索\n"
        L"  Ctrl+O       打开文件（txt / epub / pdf / html / md / docx）\n"
        L"  Ctrl+W       关闭当前书，回到首页书架\n"
        L"  Ctrl+F       全文搜索（回车开始搜索）\n"
        L"  F3           下一个结果\n"
        L"  Ctrl+C       复制选中文字\n"
        L"  Ctrl+T       显示/隐藏章节目录（也可点侧栏底部那条「收起目录」）\n"
        L"  F2           日间 / 夜间切换\n"
        L"  F5           重新载入当前文件\n"
        L"  F9           PDF：文字重排 / 原始版式（整页渲染）切换\n"
        L"  Ctrl+U       打开网页小说（输入目录页网址，自动抓全书）\n"
        L"  Ctrl+S       书源搜索（输入书名 → 选书 → 下载整本）\n"
        L"\n"
        L"书签与外观\n"
        L"  Ctrl+D       给当前位置加书签（同一段再加会更新标题）\n"
        L"  Ctrl+B       打开书签列表，点条目直接跳过去\n"
        L"  阅读 → 书签 → 删除此段书签   删掉当前这一段的书签\n"
        L"  阅读 → 外观 → 选择字体       从系统字体里挑正文字体\n"
        L"  阅读 → 外观 → 导入字体文件   导入 .ttf/.otf/.ttc（只对本程序生效）\n"
        L"  阅读 → 外观 → 自定义配色     背景 / 正文 / 标题 / 选中色逐个选\n"
        L"  阅读 → 外观 → 配色方案       日间 / 夜间 / 护眼绿 / 羊皮纸 / 高对比暗\n"
        L"\n"
        L"自动下滑\n"
        L"  F6           开启 / 关闭自动下滑\n"
        L"  F7 / F8      下滑减速 / 加速（1~10 级）\n"
        L"  （手动滚动、翻章、搜索跳转会自己停下）\n"
        L"\n"
        L"也可以直接把 txt 文件拖进窗口。";    MessageBoxW(g_hMain, t, L"快捷键", MB_ICONINFORMATION);
}

/* 弹「关于」对话框：程序简介 + ini 的实际路径。
 * 把 ini 路径显示出来是有意为之：用户要备份或清理阅读进度时找得到文件，
 * 而路径会在「exe 同级」与 %APPDATA%\NovelReader 之间因可写性而变，写死没用。
 */
static void ShowAbout()
{
    std::wstring m = L"轻量小说阅读器  NovelReader\n\n";
    m += L"第一作者：chenzy0623\n";
    m += L"第二作者：xuanmo 的大肥鱼\n\n";
    m += L"支持 TXT / EPUB / PDF / HTML / Markdown / DOCX 等格式；\n";
    m += L"文本编码自动识别：UTF-8 / GBK(GB18030) / UTF-16；\n";
    m += L"自动识别“第 N 章”等章节标题；\n";
    m += L"首页书架显示每本书读到第几章、共几章、百分之多少；\n";
    m += L"关闭后自动记住每本书的阅读位置；\n";
    m += L"支持全文搜索（章节名、任意关键词）。\n\n";
    m += L"配置与进度保存在：\n";
    m += IniFilePath();
    MessageBoxW(g_hMain, m.c_str(), L"关于", MB_ICONINFORMATION);
}

/* 把「读到哪」写进 ini 的书架信息里（关闭这本书、程序退出、定时防抖落盘都走这里）。
 * 无参数、无返回值。
 * 为什么落盘时要连带章名与百分比：首页书架是在**没有打开这本书**的时候画的，那时
 * 内存里根本没有 Book 可算，只能靠这里事先存下来的元数据（见 ini.cpp 的 PosSaveMeta）。
 * 百分比口径必须和状态栏第三格一字不差（顶行偏移 / 全文长度 × 1000 的千分数），
 * 否则用户会发现首页写 12.3%、阅读时状态栏写 12.4%，像是算错了。
 * 边界：没开书直接返回；偏移夹到 [0, 长度-1]，防止文件被改短后越界；
 * 章节号 -1 表示"在第一章之前"（书名、作者那几行），书架就不显示章名。
 */
void UiSaveProgress()
{
    if (!g_book.Ok()) return;
    int off = ViewAnchorOffset();
    if (off < 0) off = 0;
    if (off > (int)g_book.text.size()) off = (int)g_book.text.size();

    /* 千分数：先乘 1000 再除，避免整数除法把 0.4% 直接舍成 0 */
    int pct = 0;
    if (!g_book.text.empty())
        pct = (int)((long long)off * 1000 / (long long)g_book.text.size());

    int ch = -1;
    int chn = (int)g_book.chapters.size();
    std::wstring name;
    if (!g_book.chapters.empty()) {
        ch = BookChapterAtPara(g_book, ViewCurrentPara());
        if (ch >= 0) name = BookChapterTitle(g_book, ch);
    }
    PosSaveMeta(g_book.path, off, name, ch, chn, pct);

    /* 1.7.0：把这本书的"身份指纹"也写进 ini。
     * 指纹要把正文前 4096 字算进去，只有此刻书在内存里才能顺手算；
     * 手机那边每次拉 /peer/books 都要用，写到 ini 里就只算这一次。 */
    {
        std::wstring sk = SyncBookKey(g_book.title, g_book.text);
        if (!sk.empty())
        {
            std::wstring sec = PosSectionFor(g_book.path);
            IniSetStr(sec.c_str(), L"skey", sk.c_str());
        }
    }

    /* 1.7.0：顺手把新进度推给手机。
     * 节流到 5 秒一次 —— 滚动时 UiSaveProgress 由 2.5 秒防抖定时器调用，
     * 不节流的话每滚一下就开一个后台线程去连手机，太浪费。
     * 手机没开/掉线时 SyncPush 会失败，但这里是静默的，不打扰阅读。 */
    static DWORD s_lastPush = 0;
    if (SyncRunning())
    {
        DWORD now = GetTickCount();
        if (now - s_lastPush > 5000)
        {
            s_lastPush = now;
            SyncPushCurrent();
        }
    }
}

/* 关窗口前的「保存一切」：阅读位置 + 窗口矩形 + 所有设置。无参数、无返回值。
 * 顺序上的取舍：
 * 1) 先 PosSave（当前阅读位置）—— 最容易丢、也最让用户心疼的数据放最前面，
 *    万一同进程后面出错或进程被强杀，至少进度已经落盘；
 * 2) 再存窗口矩形。用 GetWindowPlacement 而不是 GetWindowRect：窗口最大化时
 *    GetWindowRect 拿到的是「最大化后」的矩形（可能带负坐标），存下来下次复原不对；
 *    rcNormalPosition 是「还原状态」下的矩形，才是想要的。另外要求宽 > 300、
 *    高 > 200（设计值）才采纳，防止最小化或异常尺寸被写进 ini；
 * 3) 最后 SettingsSave 一次性写 ini（字号、主题、侧栏、速度、矩形都在里面）。
 * 注意只有 WM_CLOSE 调它，WM_DESTROY 里不再重复保存，免得退出路径上有两处写文件。
 */
static void SaveAll()
{
    /* 阅读位置（连同章名、百分比一起写进书架元数据）—— 最容易丢的数据放最前面 */
    UiSaveProgress();
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (GetWindowPlacement(g_hMain, &wp)) {
        RECT r = wp.rcNormalPosition;
        if (r.right - r.left > S(300) && r.bottom - r.top > S(200)) {
            g_set.winRect = r;
            g_set.hasRect = true;
        }
    }
    SettingsSave();
}

/* 字号增减 d 磅并立即重排版、写入 ini。d = +1 变大、-1 变小。
 * 夹取到 9~42 磅：再小没法读，再大一行放不下几个字，排版会退化。
 * 夹取后若与原值相同就直接返回 —— 避免到顶了还每次重排一遍全书，大书重排
 * 是毫秒到几十毫秒级的开销，按键连击时很浪费。
 *
 * 1.6.0：字号是"每本书"的（g_lay.fontPt），改完落进这本书的 ini 段，同时更新
 * 会话默认值（g_set.fontPt），这样下一本新书继承的是你最近用过的字号，而不是
 * 出厂值。没有打开书时就直接改默认值 —— 在书架上按 Ctrl+= 也要有反应。
 */
void UiSetFontDelta(int d)
{
    int f = g_lay.fontPt + d;
    if (f < 9) f = 9;
    if (f > 42) f = 42;
    if (f == g_lay.fontPt) return;
    g_lay.fontPt = f;
    g_set.fontPt = f;
    IniSetInt(L"settings", L"fontPt", f);
    if (g_book.Ok()) {
        LayoutSave(g_book.path);
    }
    wchar_t msg[48];
    wsprintfW(msg, L"字号 %d 磅", f);
    s_statusExtra = msg;
    ViewMetricsChanged(true);
    UiUpdateStatus();
}

/* 行距在 紧凑 → 标准 → 宽松 之间循环（0/1/2，对应 view.cpp 的 kSpacing[]）。
 * 取模 3 保证永远落在 0..2，不用写 if 分支；行距一变换行位置也变，
 * 所以 ViewMetricsChanged(true)。1.6.0 起同样是"每本书"的设置。 */
void UiCycleSpacing()
{
    g_lay.spacing = (g_lay.spacing + 1) % 3;
    g_set.spacing = g_lay.spacing;
    IniSetInt(L"settings", L"spacing", g_lay.spacing);
    if (g_book.Ok()) LayoutSave(g_book.path);
    /* 状态栏顺带告诉用户现在是哪一档，省得靠肉眼比行距 */
    static const wchar_t* kName[] = { L"行距：紧凑", L"行距：标准", L"行距：宽松" };
    s_statusExtra = kName[g_lay.spacing];
    ViewMetricsChanged(true);
    UiUpdateStatus();
}

/* 日间 / 夜间配色切换（F2）。
 * 现在的 theme 有三种取值：0 日间 / 1 夜间 / 2 自定义。F2 只在「日间 ↔ 夜间」之间跳，
 * 从自定义状态按 F2 会回到日间（否则用户会困在自己配的颜色里找不到一键变亮的路）。
 * ViewMetricsChanged(false)：换主题只影响颜色，不影响换行与行高；传 true 会让
 * 几万段的行数缓存白算一遍，滚动立刻卡一下。
 * 具体颜色由 view.cpp 的 CurTheme() 给：theme==2 时用 g_set 里的 c* 颜色现拼。 */
void UiToggleTheme()
{
    g_set.theme = (g_set.theme == 1) ? 0 : 1;
    ViewMetricsChanged(false);
    IniSetInt(L"settings", L"theme", g_set.theme);
}

/* 显示 / 隐藏左侧目录面板（菜单 Ctrl+T）。无参数。
 * 关掉时 sideW 变 0，正文占满整个宽度（见 UiLayout）；重新打开时把 s_panel 拉回 0，
 * 因为用户按「显示目录」就是想看目录，而不是想回到上次的搜索结果页。
 * 这里改的是持久设置，所以立刻 IniSetInt 写盘。
 */
void UiToggleSidebar()
{
    g_set.sidebar = g_set.sidebar ? 0 : 1;
    if (g_set.sidebar) s_panel = 0;
    UiLayout();
    IniSetInt(L"settings", L"sidebar", g_set.sidebar);
}

/* 在「PDF 原始版式」与「文字重排」之间切换（菜单 / F9）。无参数。
 * 为什么需要这个开关：文字重排能搜索、能换字体、能夜间配色，但会把 PDF 的
 * 分栏、表格、图片丢掉；原始版式则 100% 还原版式，代价是那一屏的字搜不了。
 * 两种模式给不同场景用，所以让用户自己随时切，而不是替他选一种。
 * 边界：没开书 / 不是 PDF 时给出提示而不是静默失败（静默会让人以为是程序坏了）；
 * 渲染器打不开（加密 PDF、系统组件缺失）时保持文字模式并说明原因。 */
void UiTogglePdfMode()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一个 PDF 文件。", L"提示", MB_ICONINFORMATION);
        return;
    }
    if (!PdfIsPdf(g_book.path)) {
        MessageBoxW(g_hMain, L"「原始版式」只对 PDF 文件有效。\n当前这本是重排后的文本，已经是最好的显示方式了。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    bool want = !ViewIsPdfMode();
    if (!ViewSetPdfMode(want)) {
        MessageBoxW(g_hMain,
                    L"这个 PDF 无法按原始版式渲染。\n"
                    L"可能是加密文件、扫描件，或者系统的 PDF 组件不可用。\n"
                    L"继续用文字重排模式阅读不影响搜索与书签。",
                    L"提示", MB_ICONWARNING);
        return;
    }
    /* 状态栏第三格要从"第几章"变成"第几页"（反之亦然），所以立刻刷一次 */
    UiUpdateStatus();
    ViewFocus();
}

/* ---------------- 自动下滑 ---------------- */

/* 自动下滑的定时器周期（毫秒）。公式：3000 / autoSpeed。
 * 极端值：速度 1 → 3000ms（3 秒一行），速度 10 → 300ms；中间单调、无跳变。
 * 为什么是除法而不是线性递减：用户按 F7/F8 时期待「快一档就是明显快一截」，
 * 除法给的是等比节奏，比「每次减 200ms」更符合直觉。
 * 边界：autoSpeed 先夹到 1..10 再做除数，防止 ini 被手改成 0 时除零崩溃。
 * 注意时间不随 DPI 缩放，别给它套 S()。
 */
static int AutoIntervalMs()
{
    int sp = g_set.autoSpeed;
    if (sp < 1) sp = 1;
    if (sp > 10) sp = 10;
    /* 速度 1 最慢：每 3 秒滚一行；速度 10 最快：每 0.3 秒滚一行 */
    return 3000 / sp;
}

/* 按当前状态重设定时器（速度变了、开关变了都调它）。无参数。
 * 先 KillTimer 再按需 SetTimer：对同一个 id 再 SetTimer 虽然也会替换旧定时器，
 * 但显式 Kill 能保证「关闭自动下滑」这条路径一定停干净，不依赖系统行为。
 * 只在 s_autoOn 且真的开了书时才设，这样 WM_TIMER 里几乎不用防御性判断。
 * SetTimer 第四个参数传 nullptr：走窗口的 WM_TIMER 消息，而不是回调函数，
 * 省掉一个额外的静态函数和它的上下文问题。
 */
void UiAutoApplyTimer()
{
    if (!g_hMain) return;
    KillTimer(g_hMain, TIMER_AUTO);
    if (s_autoOn && g_book.Ok()) SetTimer(g_hMain, TIMER_AUTO, (UINT)AutoIntervalMs(), nullptr);
}

/* 查询自动下滑是否在运行：给 view.cpp 判断「这次滚动是不是定时器造成的」用 */
bool UiAutoRunning() { return s_autoOn; }

/* 停止自动下滑（幂等：本来就没开就什么都不做，省掉多余的重绘与状态刷新）。
 * 谁调它：用户手动滚动（滚轮/键盘/拖滚动条，见 view.cpp）、换书、翻章、搜索跳转、
 * 滚到全书末尾。
 * 「任何手动操作都停下自动下滑」是刻意策略：自动下滑的语义是「我不用动手了」，
 * 一旦用户自己动了手，就该把手交还给他，否则页面一边被拖一边自己往下跑，像失控。
 * 三件收尾事：重设定时器（等于删掉）、刷状态栏（去掉「⏵自动 N」）、同步菜单勾选。
 */
void UiAutoStop()
{
    if (!s_autoOn) return;
    s_autoOn = false;
    NrTrace(L"AUTO stop speed=%d", g_set.autoSpeed);
    UiAutoApplyTimer();
    UiUpdateStatus();
    AutoSyncMenu();
}

/* 开 / 关自动下滑（F6 与菜单项）。无参数。
 * 边界：没开书时不许开 —— 没有正文可滚，弹一句提示就返回。
 * 为什么最后要把焦点交回正文（ViewFocus）：用户常常是「在搜索框里按了半天回车，
 * 再按 F6」，此时焦点还留在 EDIT 上；自动下滑开着而键盘焦点在输入框，用户按 ↑↓
 * 会变成在改搜索词，非常反直觉。
 * 顺序：改状态 → 重设定时器 → 刷状态栏 → 同步菜单 → 抢焦点，保证任何一步看到的
 * 状态都是一致的。
 */
void UiAutoToggle()
{
    if (!g_book.Ok()) {
        MessageBoxW(g_hMain, L"请先打开一个 TXT 文件。", L"自动下滑", MB_ICONINFORMATION);
        return;
    }
    s_autoOn = !s_autoOn;
    NrTrace(L"AUTO toggle -> %d speed=%d ivl=%d", (int)s_autoOn, g_set.autoSpeed, AutoIntervalMs());
    UiAutoApplyTimer();
    UiUpdateStatus();
    AutoSyncMenu();
    if (s_autoOn) ViewFocus();   /* 焦点交回正文，避免键盘还留在搜索框 */
}

/* 自动下滑加速一档（F8）。上限 10，到顶了就只重设一次定时器，无副作用。
 * 立即 IniSetInt 落盘：速度是用户手感，退出时丢了会很明显。
 * 即使当前没在自动下滑也允许调速度，下次按 F6 就用新速度。
 */
void UiAutoFaster()
{
    if (g_set.autoSpeed < 10) g_set.autoSpeed++;
    IniSetInt(L"settings", L"autoSpeed", g_set.autoSpeed);
    UiAutoApplyTimer();
    UiUpdateStatus();
}

/* 自动下滑减速一档（F7）。下限 1（3 秒一行），语义与 UiAutoFaster 对称 */
void UiAutoSlower()
{
    if (g_set.autoSpeed > 1) g_set.autoSpeed--;
    IniSetInt(L"settings", L"autoSpeed", g_set.autoSpeed);
    UiAutoApplyTimer();
    UiUpdateStatus();
}

/* 调用时机：UiAutoStop / UiAutoToggle 改完 s_autoOn 之后，以及 wWinMain 里建完菜单
 * 立刻同步一次 —— 菜单勾选必须永远等于 s_autoOn，不能让用户看到「勾着但没在滚」。
 * IDM_AUTO_TOGGLE 按 s_autoOn 打勾/去勾；IDM_AUTO_FAST / IDM_AUTO_SLOW 只在
 * 自动下滑进行中才可用，否则灰掉 —— 没在跑还去调速度没意义，灰掉比「按了没反应」好。
 * MF_BYCOMMAND 表示按命令 id 定位（菜单项会增删，按位置不方便）。
 * 边界：主窗口或菜单还没建好时直接返回（启动早期会调一次）。
 */
/* 菜单里给"自动下滑"打勾 */
static void AutoSyncMenu()
{
    if (!g_hMain) return;
    HMENU hm = GetMenu(g_hMain);
    if (!hm) return;
    CheckMenuItem(hm, IDM_AUTO_TOGGLE, MF_BYCOMMAND | (s_autoOn ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(hm, IDM_AUTO_FAST, MF_BYCOMMAND | (s_autoOn ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hm, IDM_AUTO_SLOW, MF_BYCOMMAND | (s_autoOn ? MF_ENABLED : MF_GRAYED));
}

/* WM_COMMAND 的中央分派：菜单项、加速键、按钮、列表通知全在这里落地。
 * 参数 id  ：命令 id（菜单的 IDM_* 或控件的 IDC_*）；
 * 参数 code：通知码。菜单与加速键是 0，按钮是 BN_CLICKED，列表是 LBN_SELCHANGE 等；
 * 参数 ctl ：发出通知的控件句柄。这里统一 (void) 掉 —— 本程序的控件 id 都是唯一的，
 *            按 id 分派已经足够，不必再看句柄。
 * 结构分三段，顺序不能换：
 * ① 先处理「最近打开」这一族动态 id（连续区间 IDM_RECENT_BASE + [0, MAX)）。
 *    它必须是 if 而不是 case，因为这些 id 不是固定常量；命中区间后自行 return，
 *    绝不往下掉进 switch。这里还顺手用 GetFileAttributesW 验一下文件是否还在，
 *    因为记录可能指向已删除或已移动的书，提前给明白提示比「打开失败」友好。
 * ② 菜单命令：一对一调用对应的 Ui* 函数，每个 case 一行，分派本身不做业务处理。
 * ③ 控件通知：两个列表要先检查 s_tocSync / s_hitSync，否则自己设选中项会绕回来
 *    再跳一次；列表里可能有「（未识别到章节）」这类占位行，所以下标要判 < size。
 * 边界：未知 id 走 default 什么都不做 —— 新增控件忘了加 case 也只是「按了没反应」，
 * 不会崩。
 */
static void OnCommand(int id, int code, HWND ctl)
{
    NrTrace(L"OnCommand id=%d code=%d", id, code);
    (void)ctl;
    if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + IDM_RECENT_MAX) {
        std::wstring p = RecentGet(id - IDM_RECENT_BASE);
        if (p.empty()) return;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(g_hMain, L"这个文件已经不存在了。", L"提示", MB_ICONINFORMATION);
            return;
        }
        UiOpenFile(p);
        return;
    }
    /* 预设配色：IDM_PRESET_BASE..+IDM_PRESET_MAX-1 连号，用区间判断省掉五个 case */
    if (id >= IDM_PRESET_BASE && id < IDM_PRESET_BASE + IDM_PRESET_MAX) {
        UiApplyPreset(id - IDM_PRESET_BASE);
        return;
    }
    /* 高亮四色：IDM_HL_COLOR_BASE..+3 连号，颜色序号 = id 偏移。
     * 单独成区间而不是四个 case，是为了以后加第 5、第 6 种颜色时只改 ids.h 和
     * view.cpp 的调色板，这里一个字都不用动。 */
    if (id >= IDM_HL_COLOR_BASE && id < IDM_HL_COLOR_BASE + IDM_HL_COLOR_MAX) {
        s_lastHlColor = id - IDM_HL_COLOR_BASE;
        UiAddHighlight(s_lastHlColor);
        return;
    }
    /* 页边距三档：同样连号 */
    if (id >= IDM_LAY_MARGIN_BASE && id < IDM_LAY_MARGIN_BASE + IDM_LAY_MARGIN_MAX) {
        g_lay.margin = id - IDM_LAY_MARGIN_BASE;
        static const wchar_t* kM[] = { L"页边距：窄", L"页边距：标准", L"页边距：宽" };
        s_statusExtra = kM[g_lay.margin];
        UiLayoutChanged(true);
        return;
    }
    switch (id) {
    case IDM_OPEN: {
        std::wstring p = OpenDlg();
        if (!p.empty()) UiOpenFile(p);
        break;
    }
    case IDM_RELOAD:
        if (g_book.Ok()) UiOpenFile(g_book.path);
        break;
    case IDM_CLOSEFILE:
        UiCloseFile();
        break;
    case IDM_EXIT:
        SendMessageW(g_hMain, WM_CLOSE, 0, 0);
        break;
    case IDM_PREV_CH:
        ViewGotoChapter(-1);
        break;
    case IDM_NEXT_CH:
        ViewGotoChapter(1);
        break;
    case IDM_FONT_UP:   UiSetFontDelta(1); break;
    case IDM_FONT_DOWN: UiSetFontDelta(-1); break;
    case IDM_LINE_SPACE: UiCycleSpacing(); break;
    case IDM_THEME:     UiToggleTheme(); break;
    case IDM_TOC:       UiToggleSidebar(); break;
    case IDM_FIND:      UiShowSearchBar(true); break;
    case IDM_FIND_NEXT: UiFindNext(); break;
    case IDM_COPY:      ViewCopySelection(); break;
    case IDM_SELALL:    ViewSelectAll(); break;
    case IDM_KEYS:      ShowKeys(); break;
    case IDM_ABOUT:     ShowAbout(); break;
    case IDM_MENU: {
        /* 键盘唤起主菜单（Ctrl+F10）：弹在左上角「菜单」按钮的正下方，
         * 位置和用鼠标点那个按钮完全一致。BarMenuRect 是自绘顶栏里的坐标，
         * 顶栏贴在主窗口客户区左上角，所以对它做 ClientToScreen 就够。 */
        RECT mr = BarMenuRect();
        POINT mp = { mr.left, mr.bottom + S(2) };
        if (s_hBar) ClientToScreen(s_hBar, &mp);
        UiShowAppMenuAt(mp.x, mp.y);
        break;
    }
    case IDM_PHONE:       UiShowPhone(); break;
    case IDM_PHONE_JUMP:  UiJumpPhoneProgress(); break;
    case IDM_PHONE_SYNC:  UiTogglePhoneSync(); AutoSyncMenu(); break;
    case IDM_AUTO_TOGGLE: UiAutoToggle(); break;
    case IDM_AUTO_FAST:   UiAutoFaster(); break;
    case IDM_AUTO_SLOW:   UiAutoSlower(); break;
    case IDM_BM_ADD:      UiAddBookmark(); break;
    case IDM_BM_LIST:     UiShowBookmarks(); break;
    case IDM_BM_DEL:      UiDelBookmark(); break;
    case IDM_PDF_MODE:    UiTogglePdfMode(); break;
    case IDM_OPEN_URL:    UiOpenWebNovel(); break;
    case IDM_SOURCE:      UiSourceSearch(); break;
    case IDM_SOURCE_MGR:  ShowSourceMgr(g_hMain); break;   /* 主窗口直接开管理 */
    case IDM_PICK_FONT:   UiPickFont(); break;
    case IDM_IMPORT_FONT: UiImportFont(); break;
    case IDM_PICK_COLORS: UiPickColors(); break;

    /* ---- 1.6.0 新增：高亮与笔记 ---- */
    /* IDM_HL_ADD 是"用上次用过的颜色快速标一条"（快捷键 Ctrl+H），
     * 连着标十句时不该每句都重新挑颜色。 */
    case IDM_HL_ADD:      UiAddHighlight(s_lastHlColor); break;
    case IDM_HL_NOTE:     UiEditNote(); break;
    case IDM_HL_LIST:     UiShowHighlights(); break;
    case IDM_HL_DEL:      UiRemoveHighlight(); break;
    case IDM_HL_EXPORT:   UiExportNotes(); break;

    /* ---- 1.6.0 新增：排版（每本书） ---- */
    case IDM_LAY_INDENT:     LayoutToggleIndent(); break;
    case IDM_LAY_PAGED:      LayoutTogglePaged(); break;
    case IDM_LAY_COLS:       LayoutToggleCols(); break;
    case IDM_LAY_FORCE_FONT: LayoutToggleForceFont(); break;
    case IDM_LAY_RESET:      LayoutReset(); break;

    /* ---- 1.6.0 新增：快捷键 / 隐私 / 老板键 ---- */
    case IDM_KEYS_EDIT:  UiKeysDialog(); break;
    case IDM_PRIVACY:    UiTogglePrivacy(); break;
    case IDM_BOSS:       UiToggleBoss(); break;

    case IDC_TOC:
        if (code == LBN_SELCHANGE && !s_tocSync && g_book.Ok()) {            int sel = (int)SendMessageW(s_hToc, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_book.chapters.size()) {
                ViewGotoPara(g_book.chapters[(size_t)sel].para);
                UiUpdateStatus();
            }
        }
        break;
    case IDC_HITS:
        if (code == LBN_SELCHANGE && !s_hitSync) {
            int sel = (int)SendMessageW(s_hHits, LB_GETCURSEL, 0, 0);
            /* 这个 LISTBOX 是三个面板共用的：s_panel==1 搜索结果、2 书签、3 高亮与笔记。
             * 所以点击后的动作要按当前面板分开处理，否则点书签会去查搜索结果数组。 */
            if (s_panel == 2) {
                if (sel >= 0 && sel < (int)s_bms.size()) {
                    int off = BmParseOffset(s_bms[(size_t)sel]);
                    if (off >= 0) {
                        ViewGotoOffset(off, 0);
                        UiUpdateStatus();
                    }
                }
            } else if (s_panel == 3) {
                /* 高亮条目同样是 "偏移|文字"，直接反解偏移跳过去；
                 * 顺便把这一条高亮选中（ViewSelectRange），读者一眼看到是哪一段。 */
                if (sel >= 0 && sel < (int)g_hls.size()) {
                    const Highlight& h = g_hls[(size_t)sel];
                    ViewSelectRange(h.off, h.off + h.len);
                    UiUpdateStatus();
                }
            } else if (sel >= 0 && sel < (int)s_hits.size()) {
                UiJumpHit(sel);
            }
        }
        break;
    case IDC_BFIND: {
        /* 用 WM_GETTEXT 取搜索框文本：该编辑框被子类化过，这样取最可靠 */
        wchar_t buf[512];
        buf[0] = 0;
        /* 为什么用 SendMessageW(WM_GETTEXT) 取文本而不是 GetWindowTextW：编辑框已被
         * EditSub 子类化，我们想拿的是「控件自身窗口过程认定」的内容，直接发消息语义
         * 最干净，不经过 GetWindowTextW 那层封装规则（它对跨线程/挂起窗口的行为不同）。
         * 512 是够用的关键词缓冲区上限，超长关键词本来也没有意义。
         */
        SendMessageW(s_hFind, WM_GETTEXT, 512, (LPARAM)buf);
        if (buf[0]) UiRunSearch(buf);
        break;
    }
    case IDC_BNEXT:
        UiFindNext();
        break;
    case IDC_BCLOSE:
        UiShowSearchBar(false);
        break;
    /* 侧栏折叠条被点：和菜单「显示/隐藏目录」走同一条路（UiToggleSidebar 内部会 UiLayout，
     * 因此按钮自己的形态也会跟着从横条变竖条、或反过来）。这里不看 code ——
     * BS_OWNERDRAW 按钮只发 BN_CLICKED，没有别的通知码。
     */
    case IDC_FOLD:
        UiToggleSidebar();
        break;
    default:
        break;
    }
}

/* ---------------- 主窗口过程 ---------------- */

/* 主窗口过程。只处理必须由主窗口处理的消息，其余全部交给 DefWindowProcW。
 * 处理清单：WM_SIZE → UiLayout；WM_GETMINMAXINFO → 最小尺寸 600×420（过 S()）；
 * WM_CLOSE → SaveAll 后 DestroyWindow；WM_DESTROY → PostQuitMessage；
 * WM_COMMAND → OnCommand；WM_APP_STATUS → 刷状态栏 + 同步目录 + 兜底落盘 + 重置防抖；
 * WM_TIMER → TIMER_POS 落盘 / TIMER_AUTO 滚一行；WM_DROPFILES → 打开拖进来的文件；
 * WM_SETFOCUS → 焦点主动放进正文，用户切回窗口就能直接翻页。
 * 注意本函数不做任何业务判断，判断都在被调用的 Ui* 函数里 —— 窗口过程保持成一个
 * 纯分派表，出了问题容易定位是哪一层。
 */
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        /* 尺寸一变就整体重摆：子窗口没有一个用自动布局，漏调这次就会错位 */
        UiLayout();
        return 0;

    case WM_ERASEBKGND: {
        /* 主窗口自己露出来的地方（侧栏与工具栏、状态栏之间的缝，卡片四周的留白）
         * 统一刷成主题纸色。窗口类里注册的是 COLOR_3DFACE 系统灰，如果不在
         * 这里盖掉，日间米色主题下会看到一圈系统灰边框，像没画完。
         * 用 th.bg 而不是 th.card：卡片是"浮在纸上的块"，底色要比卡片略深一点才有层次。 */
        HDC dc = (HDC)w;
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(CurTheme().bg);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    case WM_GETMINMAXINFO: {
        /* 最小尺寸 600×420 是「侧栏 232 + 正文 368、还能排十来行」的经验值，过 S() 适配 DPI */
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = S(600);
        mmi->ptMinTrackSize.y = S(420);
        return 0;
    }

    case WM_CLOSE:
        /* 关闭：先存盘、再销毁。SaveAll 依赖窗口还活着（要读窗口矩形和当前阅读位置），
         * 所以这两句的顺序不能颠倒；DestroyWindow 之后才会有 WM_DESTROY → 退出消息循环。
         */
        SaveAll();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(w), HIWORD(w), (HWND)l);
        return 0;

    case WM_MEASUREITEM: {
        /* 自绘列表要求父窗口给出每行高度（LBS_OWNERDRAWFIXED 只会问一次）。
         * 行高统一 ListRowH()，目录与搜索结果共用 —— 两栏切换时行距不会跳。 */
        MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)l;
        if (mis && (mis->CtlID == IDC_TOC || mis->CtlID == IDC_HITS)) {
            mis->itemHeight = (UINT)ListRowH();
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        /* 自绘控件共四类：侧栏折叠条（按钮）、目录列表、搜索结果/书签列表、
         * 以及搜索条上的三个小按钮。全部在这里分发，绘制细节各自成函数，
         * MainProc 只做"谁画的"判断。 */
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)l;
        if (!dis) break;

        if (dis->CtlID == IDC_BFIND || dis->CtlID == IDC_BNEXT || dis->CtlID == IDC_BCLOSE) {
            wchar_t label[32] = { 0 };
            GetWindowTextW(dis->hwndItem, label, 32);
            DrawMiniButton(dis->hDC, dis->rcItem, label, dis->itemState);
            return TRUE;
        }

        if (dis->CtlID == IDC_FOLD) {
            DrawFoldBar(dis->hDC, dis->rcItem, (dis->itemState & ODS_SELECTED) != 0);
            return TRUE;
        }

        if (dis->CtlID == IDC_TOC || dis->CtlID == IDC_HITS) {
            const Theme& th = CurTheme();
            /* 只刷"这一行"的底，绝不能刷整个控件！
             * WM_DRAWITEM 是逐行发过来的，如果每收到一行就把整个客户区涂一次底色，
             * 会把前面已经画好的行全盖掉 —— 表现出来就是"侧栏只剩最后一行"
             * （这个 bug 实际踩过：目录里只看得见最下面那条章名）。
             * 控件整体的底色由 WM_CTLCOLORLISTBOX 给画刷，见 MainProc 里那段。 */
            HBRUSH bg = CreateSolidBrush(th.card);
            FillRect(dis->hDC, &dis->rcItem, bg);
            DeleteObject(bg);

            if (dis->itemID != (UINT)-1) {
                std::wstring text = ListText(dis->hwndItem, (int)dis->itemID);
                /* 搜索结果条目较长、属于"辅助信息"，用次要色；
                 * 书签与高亮是用户自己存下来的东西，用正文色更清楚。 */
                DrawListRow(dis->hDC, dis->rcItem, text,
                            (dis->itemState & ODS_SELECTED) != 0,
                            dis->CtlID == IDC_HITS && s_panel == 1);
            }
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLOREDIT: {
        /* 搜索框的底色/字色：默认是白底黑字，在米色纸面和夜间深底上都很突兀。
         * 这里跟列表一样按主题现建画刷并缓存（每次先删旧的，防止切主题时泄漏）。 */
        static HBRUSH s_editBrush = nullptr;
        const Theme& teh = CurTheme();
        if (s_editBrush) { DeleteObject(s_editBrush); s_editBrush = nullptr; }
        s_editBrush = CreateSolidBrush(teh.card);
        SetTextColor((HDC)w, teh.fg);
        SetBkColor((HDC)w, teh.card);
        return (LRESULT)s_editBrush;
    }

    case WM_CTLCOLORLISTBOX: {
        /* 列表控件的"空白处"底色与文字色。返回一个画刷系统拿去刷底，
         * 这里按当前主题现建、并缓存上一只以便释放（主题切换时会重建）。 */
        static HBRUSH s_listBrush = nullptr;
        const Theme& th = CurTheme();
        if (s_listBrush) { DeleteObject(s_listBrush); s_listBrush = nullptr; }
        s_listBrush = CreateSolidBrush(th.card);
        SetTextColor((HDC)w, th.fg);
        SetBkColor((HDC)w, th.card);
        return (LRESULT)s_listBrush;
    }

    case WM_APP_STATUS: {
        /* 视图状态变化（滚动 / 换书 / 跳转）的统一回调。view.cpp 用 PostMessage 发，
         * 于是这条消息排到当前绘制之后，绘制路径里就不会插进布局、落盘这类慢操作。
         * 这里的 now 供下面的兜底判断用：GetTickCount 精度约 15.6ms，够用且无系统调用开销。
         */
        DWORD now = GetTickCount();
        UiUpdateStatus();
        UiSyncToc();
        /* 自动下滑时会不停收到状态通知，定时器会被反复重置，
           所以这里额外做一次"最长 15 秒必落盘"的兜底，避免长时间滚动丢进度 */
        /* 兜底与防抖的分工：这半段负责「一直在动时也别丢太多进度」，下面那半段负责
         * 「停手后及时保存」并避免逐帧写盘，两个机制缺一个都不行。
         * 注意两条存盘路径都要更新 s_lastSave（这里和 TIMER_POS 分支各写一次），
         * 否则兜底会以为「还没存过」，在下一次状态通知里立刻又存一遍。
         */
        if (g_book.Ok() && now - s_lastSave > 15000) {
            UiSaveProgress();
            s_lastSave = now;
        }
        /* 防抖：每收到一次状态变化就把 2.5 秒定时器重置，只有真正安静 2.5 秒才写一次 ini；
         * 也就是说一整轮滚动只落盘一次，而不是每次滚轮都去写文件。
         */
        SetTimer(h, TIMER_POS, 2500, nullptr);
        return 0;
    }

    case WM_TIMER:
        /* 安静期到了：写一次位置，并记下时间给上面 15 秒兜底用。定时器是单发的 ——
         * 先 KillTimer，下次状态变化时再重新 SetTimer。
         */
        if (w == TIMER_POS) {
            KillTimer(h, TIMER_POS);
            if (g_book.Ok()) {
                UiSaveProgress();
                s_lastSave = GetTickCount();
            }
        /* 自动下滑的一拍：往下滚一行。若状态已被别处关掉（s_autoOn 为假）或书没了，
         * 立刻停掉，防止定时器空转。
         */
        /* 等对面出现：只在对账待办存在时会真的做事，做完自己 KillTimer。 */
        } else if (w == TIMER_SYNC) {
            SyncTryPending();
            return 0;
        } else if (w == TIMER_AUTO) {
            if (!s_autoOn || !g_book.Ok()) { UiAutoStop(); return 0; }
            /* 记下滚动前后的锚点偏移：如果滚不动了（到全书末尾），两者相等 —— 这是判断
             * 「已经到底」最省事、也最不依赖 view 内部实现的办法。
             */
            int before = ViewAnchorOffset();
            ViewScrollLines(1);
            int after = ViewAnchorOffset();
            NrTrace(L"AUTO tick speed=%d ivl=%d off %d->%d", g_set.autoSpeed, AutoIntervalMs(), before, after);
            /* 已经滚到全书末尾时自动停下 */
            /* 为什么走 UiAutoStop 而不只是 KillTimer：它顺带会去掉状态栏的「⏵自动 N」、取消菜单勾选，
             * 只杀掉定时器的话界面会显示「开着但不动」，比干脆停掉更让人困惑。
             */
            if (after == before) UiAutoStop();
        }
        return 0;

    /* 拖放：一次可能拖进多个文件，这里只认第一个（0 号）；无论成功与否都要
     * DragFinish 释放系统分配的那块内存，否则会泄漏。
     */
    case WM_DROPFILES: {
        HDROP d = (HDROP)w;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        if (n > 0) {
            wchar_t path[MAX_PATH * 2];
            if (DragQueryFileW(d, 0, path, MAX_PATH * 2)) UiOpenFile(path);
        }
        DragFinish(d);
        return 0;
    }

    case WM_COPYDATA: {
        /* 第二个实例被启动时会走这条路：它把「用户双击的那个文件」递过来，自己退出。
         * 这样同一个 ini 永远只有一个进程在写，不会出现两个窗口交替覆盖阅读进度。
         * dwData 是自定标识 0x4E52（"NR"），用来忽略其它程序发来的 COPYDATA。
         * 注意：WM_COPYDATA 由系统跨进程编组，lpData 指向调用进程的临时缓冲，
         * 只能在处理期间读，不能留指针。 */
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)l;
        if (cds && cds->dwData == 0x4E52 && cds->lpData &&
            cds->cbData >= sizeof(wchar_t) * 2) {
            std::wstring path((const wchar_t*)cds->lpData);
            if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                UiOpenFile(path);
            }
        }
        return TRUE;
    }

    case WM_HOTKEY:
        /* 老板键（默认 Ctrl+Alt+H）：这是唯一能在程序没有焦点时收到消息的途径，
         * 所以窗口藏起来之后只有它能叫回来。wParam 是注册时给的 id（我们只用 1）。 */
        if (w == 1) {
            UiToggleBoss();
            return 0;
        }
        break;

    case WM_SETFOCUS:
        ViewFocus();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------- 入口 ---------------- */

/* 运行时构建加速键表（而不是把 ACCELERATORS 写进 .rc）。
 * 为什么放在代码里：① 本工程刻意不带 .rc 加速键表，快捷键与命令 id 的对应关系
 * 写在同一处，改的时候不必同时动资源和 C++；② 数组可以直接用 ids.h 的 IDM_*，
 * 不依赖资源编译器；③ 表很小，CreateAcceleratorTableW 的开销可忽略，而且失败会
 * 返回 nullptr —— 消息循环判一下就能降级成「没有快捷键」，程序照样能跑。
 * 表的内容分两类：带 FCONTROL 的组合键（Ctrl+O/F/T、Ctrl+= / Ctrl+-），
 * 以及裸功能键（F2/F3/F5/F6/F7/F8）。VK_OEM_PLUS / VK_ADD 与 VK_OEM_MINUS /
 * VK_SUBTRACT 各登记一次：前者是主键盘区的 + -，后者是小键盘的，用户两边都可能按。
 * 特别注意：Ctrl+C 与 Ctrl+A **故意不登记在这里**。TranslateAcceleratorW 在消息
 * 派发之前拦截按键，一旦登记，EDIT（搜索框）自己的 Ctrl+C 复制、Ctrl+A 全选就永远
 * 收不到，会变成「在搜索框里按 Ctrl+A 结果把整本书选中了」。所以这两个键交给当前
 * 有焦点的窗口自己处理：view.cpp 的键盘处理里收 Ctrl+C/Ctrl+A 操作正文，EDIT 里则
 * 操作输入框文本。同理 Ctrl+滚轮调字号做在 view 的 WM_MOUSEWHEEL 里，也不进这张表。
 * 返回值：加速键表句柄，失败为 nullptr（调用方需判空）；本程序退出时由系统回收。
 */
/* 从快捷键表（layout.cpp 的 KeyBinds）生成加速键表。
 *
 * 1.6.0 起快捷键可自定义：表内容来自 ini 的 [keys] 段，用户在「快捷键设置」里
 * 改完就调 UiRebuildAccel() 重新生成 —— 所以不能再像以前那样写一个静态数组。
 * 另外补几条**不允许自定义**的固定键（重载、上一个搜索命中），它们要么是系统
 * 惯例，要么改了会跟别的功能打架，干脆不进设置界面。
 *
 * 设计说明（沿用旧注释，仍然成立）：
 *  - Ctrl+C / Ctrl+A 故意不在这张表里。加速键是在消息派发之前拦截按键的，一旦登记，
 *    EDIT（搜索框）自己的 Ctrl+C 复制、Ctrl+A 全选就永远收不到，会变成「在搜索框里
 *    按 Ctrl+A 结果把整本书选中了」。这两个键交给当前有焦点的窗口自己处理。
 *  - 解析失败的条目直接跳过：ini 是文本文件，用户手改成 "Ctrl+随便" 时不至于
 *    整张表都建不出来，只是那一条失效。
 * 返回值：加速键表句柄，失败为 nullptr（调用方需判空）。 */
static HACCEL BuildAccel()
{
    std::vector<ACCEL> a;
    std::vector<KeyBind>& binds = KeyBinds();
    for (size_t i = 0; i < binds.size(); i++) {
        if (binds[i].cur.empty()) continue;      /* 用户取消了这个快捷键 */
        BYTE vk = 0, mods = 0;
        if (!KeysParse(binds[i].cur, &vk, &mods)) continue;
        ACCEL ac;
        ZeroMemory(&ac, sizeof(ac));
        /* 加速键表用的是 FCONTROL/FALT/FSHIFT 这套位，和 MOD_* 不是同一套值，
         * 必须逐位翻译一遍（直接或运算会因为数值撞上而出错）。 */
        ac.fVirt = FVIRTKEY;
        if (mods & MOD_CONTROL) ac.fVirt |= FCONTROL;
        if (mods & MOD_ALT)     ac.fVirt |= FALT;
        if (mods & MOD_SHIFT)   ac.fVirt |= FSHIFT;
        ac.key = vk;
        ac.cmd = (WORD)binds[i].id;
        a.push_back(ac);
    }
    /* 固定键：重载（F5）和"上一个命中"（Shift+F3），不进设置界面 */
    {
        ACCEL ac; ZeroMemory(&ac, sizeof(ac));
        ac.fVirt = FVIRTKEY; ac.key = VK_F5; ac.cmd = IDM_RELOAD;
        bool dup = false;
        for (size_t i = 0; i < a.size(); i++) if (a[i].cmd == IDM_RELOAD) dup = true;
        if (!dup) a.push_back(ac);
    }
    {
        ACCEL ac; ZeroMemory(&ac, sizeof(ac));
        ac.fVirt = FVIRTKEY | FSHIFT; ac.key = VK_F3; ac.cmd = IDM_FIND_PREV;
        bool dup = false;
        for (size_t i = 0; i < a.size(); i++) if (a[i].cmd == IDM_FIND_PREV) dup = true;
        if (!dup) a.push_back(ac);
    }
    if (a.empty()) return nullptr;
    return CreateAcceleratorTableW(&a[0], (int)a.size());
}

/* 返回值给 wWinMain 决定「启动时打开哪本书」：非空串就是命令行给出的文件，
 * 空串表示命令行没有可用文件，调用方退到 ini 里的 lastFile。
 * 覆盖的用法：「发送到」、把 txt 拖到 exe 图标上、命令行带文件启动。
 * 为什么用 CommandLineToArgvW 而不是自己按空格切：它按 Windows 的引号规则切分，
 * 路径带空格时（D:\我的 小说\x.txt）才不会切坏。从 argv[1] 开始（argv[0] 是 exe），
 * 跳过空项和以 '-' 开头的项（将来可能有开关，至少现在不会把它当文件），
 * 再用 GetFileAttributesW 确认存在 —— 拖错目录或文件已被删时不该崩。
 * 边界：LocalFree(argv) 必须调用，CommandLineToArgvW 分配的内存要自己释放；
 * 分配失败时返回空串，调用方会退到「上次打开的书」。
 */
/* 命令行里第一个真实存在的文件（支持“发送到”、拖到 exe 上等用法） */
static std::wstring FirstFileArg()
{
    std::wstring r;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return r;
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || !argv[i][0]) continue;
        if (argv[i][0] == L'-') continue;
        if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) { r = argv[i]; break; }
    }
    LocalFree(argv);
    return r;
}

/* 程序入口：定 DPI → 读设置 → 建窗口与控件 → 跑消息循环。
 * 顺序与原因（每一步都在为下一步准备前置条件）：
 * 1) SetProcessDPIAware()：必须在取 DPI、建窗口之前调用，否则系统会对我们做位图
 *    拉伸，高分屏上字是糊的；
 * 2) 从屏幕 DC 取 LOGPIXELSY 得到 g_dpi（96/120/144…），并夹到最小 96 —— 低于
 *    96 的老设置会把界面缩得比设计值还小，直接按 100% 处理；
 * 3) IniInit 再 SettingsLoad：前者决定 ini 放哪儿，必须先跑；
 * 4) InitCommonControlsEx：状态栏属于通用控件（ICC_BAR_CLASSES），不初始化就建不出来；
 * 5) 注册窗口类 NRMain（CS_DBLCLKS 让双击消息能被识别）并创建主窗口；
 * 6) 控件 → 菜单 → 最近打开 → 菜单勾选 → 布局，顺序依赖见下面行内注释；
 * 7) BuildAccel 放在消息循环之前建，别太早占着；8) 显示窗口、刷状态栏、开书、循环。
 * 关于初始尺寸：先按 1060×800 设计值，再限制在工作区 88%×90% 以内（工作区不含
 * 任务栏），保证小屏上窗口不会大过可用区域；若 ini 里有上次的矩形且尺寸合理，
 * 就连位置一起恢复。居中按工作区算而不是按屏幕，避免标题栏被任务栏盖住。
 * 关于打开哪本书：命令行文件参数 > ini 里的 lastFile。前者是用户的明确意图
 * （我双击了这个文件），后者只是「上次看到哪」。两者都不可用时不打开书，
 * 让 view 显示自己的欢迎页，并主动 ViewFocus()，用户按 Ctrl+O 就能开始。
 * 关于消息循环：GetMessageW 返回 > 0 才继续（-1 是错误，0 是 WM_QUIT）；
 * TranslateAcceleratorW 必须在 TranslateMessage 之前 —— 加速键是「命令」不是
 * 「字符」，先翻译成 WM_COMMAND；顺序反了某些按键会先被当成字符消息发给控件。
 * 返回值：消息循环收到的 wParam（正常退出为 0）；主窗口创建失败返回 1。
 */
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int)
{
    (void)cmdLine;
    /* 必须在任何窗口/DC 操作之前调用，否则后面取到的 g_dpi 是错的 */
    SetProcessDPIAware();
    /* 取屏幕 DC 只为问一个数：LOGPIXELSY = 每英寸多少像素，96 即 100% 缩放 */
    HDC sc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sc, LOGPIXELSY);
    ReleaseDC(nullptr, sc);
    if (g_dpi < 96) g_dpi = 96;

    /* ---- 单实例保护 ----
     * 为什么需要：两个实例同时开着会互相踩同一个 ini —— 谁后关窗，谁就用自己那份
     * 内存里的位置覆盖掉对方刚存的进度，用户会看到"进度莫名回退"。
     * 做法：命名互斥量探一下；已经有一个实例时，把命令行里的文件用 WM_COPYDATA
     * 交给它，再把它的窗口提到前台，然后自己直接退出（不建窗口、不碰 ini）。
     * 名字带 Local\ 前缀：同一台机器上多个登录会话各开一个，互不干扰。
     */
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\NovelReader_SingleInstance_v1");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"NRMain", nullptr);
        if (prev) {
            std::wstring arg = FirstFileArg();
            if (!arg.empty() && GetFileAttributesW(arg.c_str()) != INVALID_FILE_ATTRIBUTES) {
                COPYDATASTRUCT cds;
                cds.dwData = 0x4E52;   /* 'N''R'，与 MainProc 里的判断对应 */
                cds.cbData = (DWORD)((arg.size() + 1) * sizeof(wchar_t));
                cds.lpData = (void*)arg.c_str();
                SendMessageW(prev, WM_COPYDATA, 0, (LPARAM)&cds);
            }
            if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        CloseHandle(hMutex);
        return 0;
    }

    /* ini 的存放路径要先定下来，后面的 SettingsLoad / IniGet* 全靠它 */
    IniInit();
    SettingsLoad();
    /* 首页书架的数据源：必须在第一次绘制之前读好，否则第一帧是空的、
     * 要等下一次重绘才出现书（会看到一次闪烁）。 */
    ShelfRefresh();

    /* 用户导入过的字体文件要在这里私有加载一遍，必须早于第一次建字体
     * （否则第一帧会用回退字体画，等换页才"突然变成"用户选的字体）。 */
    FontsLoadImported();

    /* 书源也在这里读一遍：不等到用户第一次点「书源搜索」才读，
     * 这样 ini 里的 [sources] 段从第一次启动就是完整的（用户想直接改 ini
     * 也有东西可改），书源管理打开时列表也立刻是齐的。 */
    SourceLoad();

    /* 1.6.0：排版、快捷键、老板键也都在这里初始化。
     * LayoutFromSettings() 先把 g_lay 铺成"新书默认值"（还没开书时书架/欢迎页也用这套）；
     * 打开书之后 UiOpenFile 里的 LayoutLoad 会用这本书自己的设置覆盖它。
     * KeysLoad 必须早于 UiRebuildAccel —— 表里要用的就是 ini 里的键位。 */
    LayoutFromSettings();
    KeysLoad();
    UiRebuildAccel();
    UiApplyBossKey();     /* 老板键是全局热键，注册失败也不影响主流程 */

    /* 状态栏属于通用控件，必须先 InitCommonControlsEx 才能 CreateWindowExW 成功 */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    /* 注册主窗口类。hbrBackground 用 COLOR_3DFACE 这种「系统对话框灰」，它是子窗口
     * 之间露出来的缝的颜色，日间/夜间下都能接受，不会显得突兀。
     */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = L"NRMain";
    RegisterClassExW(&wc);

    /* 默认 1060×800 是「侧栏 232 + 正文 828、八十来字换行」的舒适区间；
     * 但在 1366×768 这类小屏上会超出工作区，所以下面按 88%/90% 收一次。
     */
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int wW = 1060, wH = 800;
    int maxW = (work.right - work.left) * 88 / 100;
    int maxH = (work.bottom - work.top) * 90 / 100;
    /* 下限 640×480：比最小跟踪尺寸（600×420）略大，保证首次启动时布局就是合理的 */
    if (maxW < 640) maxW = 640;
    if (maxH < 480) maxH = 480;
    if (wW > maxW) wW = maxW;
    if (wH > maxH) wH = maxH;
    int wx = work.left + ((work.right - work.left) - wW) / 2;
    int wy = work.top + ((work.bottom - work.top) - wH) / 2;

    /* 有上次的窗口矩形就用它（位置 + 大小一起恢复），但先验一下尺寸：ini 是纯文本，
     * 可能被手工改成荒谬的值。
     */
    if (g_set.hasRect) {
        RECT r = g_set.winRect;
        if (r.right - r.left >= S(520) && r.bottom - r.top >= S(360)) {
            wx = r.left; wy = r.top;
            wW = r.right - r.left; wH = r.bottom - r.top;
        }
    }

    /* WS_CLIPCHILDREN：绘制父窗口时跳过子窗口占的区域，否则每次重绘都要先刷背景、
     * 再让子窗口盖上去，滚动时会闪。
     */
    g_hMain = CreateWindowExW(0, L"NRMain", L"小说阅读器",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              wx, wy, wW, wH, nullptr, nullptr, inst, nullptr);
    if (!g_hMain) return 1;

    /* 控件 → 菜单 → 最近打开（依赖菜单）→ 菜单勾选状态 → 布局（依赖全部控件） */
    UiCreateChildren(inst);
    /* 菜单**不挂到窗口上**：SetMenu 那条系统灰菜单栏和这套纸感界面太不搭，
     * 而且在夜间模式下它是全窗口唯一一块亮色。改成"点左上角三条横线 / 右键"弹出同一个菜单，
     * 快捷键（BuildAccel 那些）照旧有效，功能一个不少。 */
    s_hAppMenu = BuildMenu();
    UiRebuildRecentMenu();
    AutoSyncMenu();
    UiLayout();

    ShowWindow(g_hMain, SW_SHOW);
    UpdateWindow(g_hMain);
    UiUpdateStatus();

    /* 启动时打开哪本书：只有命令行明确给的文件才直接打开（双击/拖到图标上 = 明确的意图）。
     * 没有命令行文件就停在首页书架：书架第一行就是上次读的书，带章名和百分比，
     * 单击即可续读 —— 这是"打开应用先看首页"的要求（m01134）与"记住位置"之间的折中：
     * 位置照记，但入口从"直接跳进去"改成了"首页上点一下"。
     */
    std::wstring target = FirstFileArg();
    if (!target.empty() && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        UiOpenFile(target);
    } else {
        ViewFocus();
        InvalidateRect(g_hMain, nullptr, TRUE);
    }

    /* 标准消息循环：GetMessageW 返回 0 表示收到 WM_QUIT（由 WM_DESTROY 里的
     * PostQuitMessage 发出），> 0 才是正常消息；acc 判空是给加速键表创建失败留的退路。
     */
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        /* 每轮都问一次当前加速键表：用户改了快捷键会重建表（UiRebuildAccel），
         * 本地变量 acc 会变成旧表的悬空句柄，所以不能缓存。 */
        HACCEL cur = UiAccelHandle();
        if (cur && TranslateAcceleratorW(g_hMain, cur, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
