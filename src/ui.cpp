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
#include "pdf.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
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
/* 「最近打开」子菜单句柄：BuildMenu 建好后存下来，UiRebuildRecentMenu 只动它，不动整个菜单栏 */
static HMENU s_hRecentMenu = nullptr;
static HFONT s_uiFont = nullptr;
/* ---- 界面瞬时状态（都不进 ini，退出即丢）----
 * s_searchShown：顶部搜索条是否展开，影响 UiLayout 里正文顶部要让出多少高度。
 * s_panel：侧栏当前显示哪一页，0 = 目录、1 = 搜索结果。搜索结果条目更长，所以更宽。
 */
static bool s_searchShown = false;
static int  s_panel = 0;                 /* 0 目录, 1 搜索结果 */
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

/* ---- 定时器 id（SetTimer 的第二个参数，同一窗口内必须唯一）----
 * TIMER_POS ：阅读进度防抖保存（见 WM_APP_STATUS 与 WM_TIMER）。
 * TIMER_AUTO：自动下滑。
 */
#define TIMER_POS  1
#define TIMER_AUTO 2

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

    /* 正文视图由 view.cpp 创建（它自己注册窗口类 NRView、自绘、处理滚轮键盘选中） */
    s_hView = ViewCreate(g_hMain);

    /* 状态栏：SBARS_SIZEGRIP 给右下角加拖拽握把；高度由系统算，所以布局时要先 WM_SIZE 再量 */
    s_hStatus = MakeChild(STATUSCLASSNAMEW, WS_VISIBLE | SBARS_SIZEGRIP, 0, IDC_STATUS, nullptr);
    /* 目录列表：LBS_NOTIFY 让选中变化能通知父窗口（WM_COMMAND + LBN_SELCHANGE）；
     * LBS_NOINTEGRALHEIGHT 允许高度不是行高的整数倍，避免最后一行被裁掉；
     * WS_EX_CLIENTEDGE 给个凹陷边框，和正文区域在视觉上分开。
     */
    s_hToc = MakeChild(L"LISTBOX", WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                       WS_EX_CLIENTEDGE, IDC_TOC, nullptr);
    /* 搜索结果列表：与目录同一块区域、同样样式，初始不带 WS_VISIBLE，切换靠 ShowWindow */
    s_hHits = MakeChild(L"LISTBOX", WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                        WS_EX_CLIENTEDGE, IDC_HITS, nullptr);
    /* 搜索条：ES_AUTOHSCROLL 让超长关键词横向滚动，不加 ES_MULTILINE 是因为
     * 搜索词不会有换行；三个按钮都带 WS_TABSTOP，保证 Tab 键能在它们之间走。
     */
    s_hFind = MakeChild(L"EDIT", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_FINDEDIT, L"");
    s_bFind = MakeChild(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BFIND, L"搜索");
    s_bNext = MakeChild(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BNEXT, L"下一个");
    s_bClose = MakeChild(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_BCLOSE, L"关闭");

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
 * 配色特意用系统灰（COLOR_BTNFACE / COLOR_BTNTEXT）而不是日间/夜间的米色，
 * 因为侧栏本身是系统 LISTBOX（也是灰的），硬套主题色反而突兀；它属于"窗口框架"，
 * 和状态栏、菜单同族。
 * 边界：r 宽或高 <= 0 时直接返回（缩到极小的窗口里会出现），否则 FillRect 无意义、
 * 文字也没有地方放。
 */
static void DrawFoldBar(HDC dc, const RECT& r, bool pressed)
{
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return;

    /* 1) 底色 + 分隔线：横条在顶边画线，竖条在右边画线，与相邻的列表分开 */
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, r.left, r.top, nullptr);
    LineTo(dc, r.right, r.top);
    if (r.right - r.left < S(30)) {
        MoveToEx(dc, r.right - 1, r.top, nullptr);
        LineTo(dc, r.right - 1, r.bottom);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    /* 2) 文字：横条居中显示整句，竖条只靠上画一个箭头 */
    HFONT oldFont = (HFONT)SelectObject(dc, s_uiFont);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dc, pressed ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_BTNTEXT));

    if (r.right - r.left >= S(30)) {
        /* 横条：居中，文字左右各留 6，放不下就截断（用 DT_END_ELLIPSIS 让它自己加省略号） */
        RECT tr = r;
        tr.left += S(6);
        tr.right -= S(6);
        /* 箭头用半角 "<" / ">" 而不是 U+2039 之类的印刷引号：按钮用的是系统界面字体
         * （MS Shell Dlg / Tahoma），这类字体里没有 ‹ › 字形，会画成方框或怪字符。 */
        DrawTextW(dc, L"<  收起目录", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        /* 竖条：箭头放在顶端往下 S(4) 处，水平居中 */
        RECT tr = r;
        tr.top += S(4);
        tr.bottom = tr.top + S(20);
        DrawTextW(dc, L">", -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
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

    /* 先让状态栏吃一次 WM_SIZE，系统才会算出「含边距和握把」的真实高度 */
    if (s_hStatus) {
        SendMessageW(s_hStatus, WM_SIZE, 0, 0);
        RECT sr;
        GetWindowRect(s_hStatus, &sr);
        int h = sr.bottom - sr.top;
        if (h > 4) s_sbH = h;
        /* 三段宽度用比例而不是像素：窗口拉宽时三格一起变宽。第一格到 34%，
         * 第二格 34%→66%，第三格 66%→右边界（-1 是「到右边界」的约定写法）。
         */
        int parts[3];
        parts[0] = W * 34 / 100;
        parts[1] = W * 66 / 100;
        parts[2] = -1;
        SendMessageW(s_hStatus, SB_SETPARTS, 3, (LPARAM)parts);
    }

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
    int top = searchH;
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
        if (s_hToc)  MoveWindow(s_hToc, 0, top, sideW, sh, TRUE);
        if (s_hHits) MoveWindow(s_hHits, 0, top, sideW, sh, TRUE);
    }
    if (s_hToc)  ShowWindow(s_hToc,  (sideW > 0 && s_panel == 0) ? SW_SHOW : SW_HIDE);
    /* s_panel==2（书签面板）也复用 s_hHits 这个列表，所以要一起显示，
     * 否则切到书签页会看到一块空白（列表被隐藏、目录也不该显示）。 */
    if (s_hHits) ShowWindow(s_hHits, (sideW > 0 && s_panel != 0) ? SW_SHOW : SW_HIDE);

    /* 搜索条内部：以右边界为基准逐个往左挤。按钮之间留 6，按钮与输入框之间留 12，
     * 输入框拿走剩余宽度。这样无论窗口多宽，按钮组始终贴着右边、不会乱飘。
     */
    if (s_searchShown) {
        int y = S(5), hh = S(26);
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
        p0 = g_book.title;
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
    SendMessageW(s_hStatus, SB_SETTEXTW, 0, (LPARAM)p0.c_str());
    SendMessageW(s_hStatus, SB_SETTEXTW, 1, (LPARAM)p1.c_str());
    SendMessageW(s_hStatus, SB_SETTEXTW, 2, (LPARAM)p2.c_str());
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
    lf.lfHeight = -MulDiv(g_set.fontPt, g_dpi, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, g_set.fontFace.empty() ? L"Microsoft YaHei" : g_set.fontFace.c_str(), LF_FACESIZE);

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

    g_set.fontFace = lf.lfFaceName;
    SettingsSave();          /* 立刻落盘：换字体是"设置"，不该等到关窗口才存 */
    ViewMetricsChanged(true);   /* 字体变了：行高、换行位置全部重算（传 true 让段落行数缓存作废） */
    ViewInvalidate();
    s_statusExtra = std::wstring(L"字体：") + g_set.fontFace;
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

    ClearSearchState();
    ViewSetBook();
    FillToc();
    /* 书签是按书存的：换书后立刻读新书的书签，否则会在新书里看到上一本的书签标记 */
    BmReload();

    int off = PosLoad(path);
    if (off > 0 && off < (int)g_book.text.size()) ViewGotoOffset(off, 0);
    else ViewGotoOffset(0, 0);

    std::wstring cap = L"小说阅读器 - ";
    cap += g_book.title;
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
    if (s_hStatus) SendMessageW(s_hStatus, SB_SETTEXTW, 0, (LPARAM)s.c_str());

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
        L"  Ctrl+O       打开文件（txt / epub / html / md / docx）\n"
        L"  Ctrl+W       关闭当前书，回到首页书架\n"
        L"  Ctrl+F       全文搜索（回车开始搜索）\n"
        L"  F3           下一个结果\n"
        L"  Ctrl+C       复制选中文字\n"
        L"  Ctrl+T       显示/隐藏章节目录（也可点侧栏底部那条「收起目录」）\n"
        L"  F2           日间 / 夜间切换\n"
        L"  F5           重新载入当前文件\n"
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
 * ViewMetricsChanged(true)：true 表示「换行会变」，必须作废所有段落的行数缓存；
 * 这和只换主题（传 false）是两回事，别传错。
 */
void UiSetFontDelta(int d)
{
    int f = g_set.fontPt + d;
    if (f < 9) f = 9;
    if (f > 42) f = 42;
    if (f == g_set.fontPt) return;
    g_set.fontPt = f;
    ViewMetricsChanged(true);
    IniSetInt(L"settings", L"fontPt", g_set.fontPt);
}

/* 行距在 紧凑 → 标准 → 宽松 之间循环（0/1/2，对应 view.cpp 的 kSpacing[]）。
 * 取模 3 保证永远落在 0..2，不用写 if 分支；行距一变换行位置也变，
 * 所以 ViewMetricsChanged(true)。
 */
void UiCycleSpacing()
{
    g_set.spacing = (g_set.spacing + 1) % 3;
    ViewMetricsChanged(true);
    IniSetInt(L"settings", L"spacing", g_set.spacing);
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
    case IDM_PHONE:       UiShowPhone(); break;
    case IDM_PHONE_JUMP:  UiJumpPhoneProgress(); break;
    case IDM_AUTO_TOGGLE: UiAutoToggle(); break;
    case IDM_AUTO_FAST:   UiAutoFaster(); break;
    case IDM_AUTO_SLOW:   UiAutoSlower(); break;
    case IDM_BM_ADD:      UiAddBookmark(); break;
    case IDM_BM_LIST:     UiShowBookmarks(); break;
    case IDM_BM_DEL:      UiDelBookmark(); break;
    case IDM_PDF_MODE:    UiTogglePdfMode(); break;
    case IDM_OPEN_URL:    UiOpenWebNovel(); break;
    case IDM_PICK_FONT:   UiPickFont(); break;
    case IDM_IMPORT_FONT: UiImportFont(); break;
    case IDM_PICK_COLORS: UiPickColors(); break;

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
            /* 这个 LISTBOX 是两个面板共用的：s_panel==2 时它显示书签，其余显示搜索结果。
             * 所以点击后的动作要按当前面板分开处理，否则点书签会去查搜索结果数组。 */
            if (s_panel == 2) {
                if (sel >= 0 && sel < (int)s_bms.size()) {
                    int off = BmParseOffset(s_bms[(size_t)sel]);
                    if (off >= 0) {
                        ViewGotoOffset(off, 0);
                        UiUpdateStatus();
                    }
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

    case WM_DRAWITEM: {
        /* 只有侧栏折叠条是自绘的（BS_OWNERDRAW）。其它控件不会发这个通知，
         * 但仍然判一下 CtlID，将来加自绘控件时不会串味。 */
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)l;
        if (!dis || dis->CtlID != IDC_FOLD) break;
        DrawFoldBar(dis->hDC, dis->rcItem, (dis->itemState & ODS_SELECTED) != 0);
        return TRUE;
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
static HACCEL BuildAccel()
{
    ACCEL a[] = {
        { FVIRTKEY | FCONTROL,             'O',         IDM_OPEN },
        { FVIRTKEY | FCONTROL,             'W',         IDM_CLOSEFILE },
        { FVIRTKEY | FCONTROL,             'F',         IDM_FIND },
        { FVIRTKEY | FCONTROL,             'T',         IDM_TOC },
        { FVIRTKEY | FCONTROL,             'D',         IDM_BM_ADD },
            { FVIRTKEY | FCONTROL,             'U',         IDM_OPEN_URL },
            { FVIRTKEY | FCONTROL,             'G',         IDM_PHONE },
            { FVIRTKEY | FCONTROL | FSHIFT,    'G',         IDM_PHONE_JUMP },
{ FVIRTKEY | FCONTROL,             'B',         IDM_BM_LIST },
        { FVIRTKEY,                        VK_F2,       IDM_THEME },
        { FVIRTKEY,                        VK_F3,       IDM_FIND_NEXT },
        { FVIRTKEY,                        VK_F5,       IDM_RELOAD },
        { FVIRTKEY,                        VK_F6,       IDM_AUTO_TOGGLE },
        { FVIRTKEY,                        VK_F7,       IDM_AUTO_SLOW },
        { FVIRTKEY,                        VK_F8,       IDM_AUTO_FAST },
        { FVIRTKEY,                        VK_F9,       IDM_PDF_MODE },
        { FVIRTKEY | FCONTROL,             VK_OEM_PLUS, IDM_FONT_UP },
        { FVIRTKEY | FCONTROL,             VK_ADD,      IDM_FONT_UP },
        { FVIRTKEY | FCONTROL,             VK_OEM_MINUS, IDM_FONT_DOWN },
        { FVIRTKEY | FCONTROL,             VK_SUBTRACT, IDM_FONT_DOWN },
    };
    return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
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
    SetMenu(g_hMain, BuildMenu());
    UiRebuildRecentMenu();
    AutoSyncMenu();
    UiLayout();

    HACCEL acc = BuildAccel();

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
        if (acc && TranslateAcceleratorW(g_hMain, acc, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
