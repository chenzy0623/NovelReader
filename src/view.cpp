/* view.cpp - 阅读视图：自适应排版、滚动、选择、绘制 */
/* =====================================================================
 * view.cpp —— 阅读视图（自绘控件，窗口类名 NRView）
 * ---------------------------------------------------------------------
 * 职责：把 g_book.text 这一份 UTF-16 全文，按"字号 + 行距档位 + 客户区宽度"
 *   排成视觉行，只绘制屏幕上看得见的那几十行；并负责滚动、选中、复制、翻章、
 *   日夜配色和欢迎页。之所以自绘而不塞一个 RichEdit：段落级懒排版、字符偏移
 *   级选区、精确滚动锚点这三件事 RichEdit 都做得又慢又不可控，自绘之后滚动
 *   一屏只算一屏。
 *
 * 关键静态变量
 *   s_dc / s_bmp / s_bmpOld / s_bmpW / s_bmpH：内存 DC + 后备位图（双缓冲）及其
 *      尺寸；尺寸不变就复用，避免每帧重建。
 *   s_fontN / s_fontB / s_fontPx：正文/标题字体、建字体时的负高度（判断字号变
 *      没变，没变就不重建 GDI 对象）。
 *   s_lineH / s_paraGap / s_titleGap / kSpacing[]：行高、段间距、标题留白；行高
 *      = 字号 * kSpacing[行距档位]，三者都在 EnsureFonts 里算好。
 *   s_padL / s_padT / s_textW：左/上内边距、可用排版宽度；s_textW 是"一段切成
 *      几行"的唯一输入。
 *   s_lineCnt[]：每段视觉行数缓存，-1 = 未计算（懒排版，失效入口 InvalidCounts）。
 *   s_aPara / s_aLine / s_aOff：视口顶行锚点三元组，详见"三"。
 *   s_selA / s_selB：选区起止字符偏移，恒有 s_selA < s_selB，都为 -1 表示无选区。
 *   s_dragChar / s_dragging：拖选起点与拖动标志；起点要单独记，鼠标越到它上面时
 *      只交换 A/B。
 *
 * 一、绘制数据流：WM_PAINT -> PaintAll()：对齐 DC/字体/宽度 -> EnsureBackBuffer
 *   -> 主题色 FillRect 整个客户区（故 WM_ERASEBKGND 返回 1，免白闪）-> 从锚点
 *   逐段逐行画，y 越过客户区高度就 break -> BitBlt -> EndPaint。
 *   「只画可见部分」靠循环里 y < H 提前退出，不是先算总行数再裁剪：每段都要重排
 *   才知道有几行，先算全书行数是 O(全书) 的浪费。
 *
 * 二、排版数据流（懒排版）：LayoutPara 把一段按 s_textW 切成视觉行，starts[i] 是
 *   第 i 行首字符偏移，末尾多压一个段尾偏移当哨兵 —— 于是行数 = size()-1、第 i
 *   行 = [starts[i], starts[i+1])，调用方不用特判最后一行。一行能放几个字由
 *   GetTextExtentExPointW 一次问出（nFit），再套两条中文规则修正：标点禁则
 *   NoLineStart（，。）」等不许在行首，断点往回退）、英文单词不拆 WordChar
 *   （断点落在单词中间就退到单词开头）。行数在 ParaLineCount 里首次用时才算。
 *
 * 三、滚动锚点：为什么需要三个变量 —— s_aPara + s_aLine 精确到"第几段第几行"，
 *   绘制/命中/翻行全靠它，但它依赖排版结果：字号一变、窗口一窄，同一个 s_aLine
 *   指向的文字就换了。s_aOff 是顶行字符偏移，与排版无关，是"真相"；重排后用它
 *   反算出 (s_aPara, s_aLine)（RestoreAnchorFromOffset），文字就被钉住了。两者
 *   双向同步：改完 s_aPara/s_aLine 必须 SyncAnchorOffset() 写回 s_aOff，否则下次
 *   重排会按旧偏移把阅读位置倒回去。
 *
 * 四、滚动条：为什么 range 用字符数而不是行号 —— range = 全文字符数，pos = 顶行
 *   字符偏移，page = 一屏字符数。① 滑块大小天然反映"一屏占全文多少"，几百万字也
 *   不撑爆精度；② 拖动时直接把偏移丢给 ViewGotoOffset，不必先算全书行数。代价是
 *   滑块与"行"不是严格线性（长段落里挪得慢），阅读场景够用。
 *
 * 注意事项：· 像素一律过 S() 做 DPI 换算；LayoutPara 用全局 s_dc 的当前字体，
 *   调用前必须 SelectObject 选好字体，否则量出的行数是错的。· "从锚点往下逐段
 *   遍历"的循环都带 guard 计数（5000/6000），防数据异常时死循环卡死界面。
 *   · 本文件不缓存文本，一律从 g_book 现取；换书后必须调 ViewSetBook()。
 * ===================================================================== */
#include "common.h"
#include "pdf.h"
#include "pdfrender.h"
#include <windowsx.h>
#include <algorithm>
#include <cwchar>

/* ---------------- 主题 ---------------- */
/* 日间主题：米色纸张 + 深棕文字 + 褐色标题。字段顺序对应 common.h 的 Theme：
   bg / fg / title / selBg / selFg / dim / hov。颜色写死 RGB 而不取系统主题色，是为了
   日夜两套外观在任何 Windows 版本与配色方案下都保持一致。
   最后一个 hov 比 bg 稍深一点点（不是 selBg 那种蓝）：首页书架悬停时整行只轻微加深，
   仍像"纸"，不像控件。 */
static const Theme kDay   = { RGB(0xF7,0xF3,0xE9), RGB(0x31,0x2D,0x27), RGB(0x8A,0x5A,0x2B),
                              RGB(0xBF,0xDC,0xF4), RGB(0x10,0x10,0x10), RGB(0x9A,0x92,0x84),
                              RGB(0xEC,0xE6,0xD8) };
/* 夜间主题：深灰底 + 浅灰字，避免纯黑配纯白的高对比在暗环境下刺眼；标题用偏冷的天蓝、选中底色用暗蓝，
   都比日间压暗，防止夜间被亮色块晃到。 */
static const Theme kNight = { RGB(0x18,0x19,0x1A), RGB(0xC4,0xC6,0xC8), RGB(0x84,0xA6,0xD0),
                              RGB(0x33,0x4E,0x6B), RGB(0xEE,0xEE,0xEE), RGB(0x74,0x76,0x7A),
                              RGB(0x24,0x26,0x29) };
/* 自定义配色的拼装结果：由 g_set 里的 cBg / cFg / cTitle / cSel / cHov 在 CurTheme()
 * 里即时算出来，放在静态变量里返回引用 —— 每帧要调几十次，没必要每次构造一个 Theme。 */
static Theme s_custom;

/* 颜色混合：把 a 与 b 按 pa:(100-pa) 的比例混合（pa 是 a 的百分比）。
 * 用途：从"背景色 + 正文色"推算出次要文字色（dim）和分隔线色，这样用户只选
 * 两个主色，其余层次自动跟着走，不会出现"自己配了一套但次要文字还是原来的棕色"。 */
static COLORREF Mix(COLORREF a, COLORREF b, int pa)
{
    if (pa < 0) pa = 0;
    if (pa > 100) pa = 100;
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg2 = GetGValue(b), bb = GetBValue(b);
    int r = (ar * pa + br * (100 - pa)) / 100;
    int g = (ag * pa + bg2 * (100 - pa)) / 100;
    int bl = (ab * pa + bb * (100 - pa)) / 100;
    return RGB(r, g, bl);
}

/* 感知亮度（0~255，整数近似 Rec.601 权重）。
 * 用途：选中色可能被用户选成深蓝或浅黄，选中文字究竟该用黑还是白，
 * 只能看底色深浅来定，否则会出现"深底深字"看不见。 */
static int Luma(COLORREF c)
{
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

/* 当前主题：g_set.theme 三种取值 —— 0 日间、1 夜间、2 自定义。
 * 自定义时用用户选的颜色现拼一套；选中文字的前景色按选中底色的亮度自动取黑或白，
 * 次要文字色按"正文色 55% + 背景色 45%"混合出来，保证在任何配色下都能看出层次。 */
const Theme& CurTheme()
{
    if (g_set.theme == 1) return kNight;
    if (g_set.theme != 2) return kDay;
    s_custom.bg    = g_set.cBg;
    s_custom.fg    = g_set.cFg;
    s_custom.title = g_set.cTitle;
    s_custom.selBg = g_set.cSel;
    s_custom.selFg = Luma(g_set.cSel) > 140 ? RGB(0x10, 0x10, 0x10) : RGB(0xF2, 0xF2, 0xF2);
    s_custom.dim   = Mix(g_set.cFg, g_set.cBg, 55);
    s_custom.hov   = g_set.cHov;
    return s_custom;
}


/* ---------------- 状态 ---------------- */
/* 视图窗口句柄。状态直接用文件级 static，不包类 —— 窗口过程和十几个辅助函数
   到处都要用，封成类只是多一层间址。 */
static HWND      s_wnd = nullptr;
/* 双缓冲用的内存 DC。第一次需要时创建（EnsureDC），之后终身不销毁：进程退出时系统会回收 GDI
   对象，比在 WM_DESTROY 里小心翼翼逐个释放更省事。 */
static HDC       s_dc = nullptr;
/* 兼容位图（后备缓冲）及它"换出"前 DC 里原有的位图对象。s_bmpOld
   必须记下来：换入新位图后旧位图被挤出，销毁前要选回 DC，否则 DeleteObject 会因"仍被占用"而失败并泄漏
   GDI。s_bmpW / s_bmpH 只在客户区尺寸变化时才需要重建。 */
static HBITMAP   s_bmp = nullptr;
static HGDIOBJ   s_bmpOld = nullptr;
static int       s_bmpW = 0, s_bmpH = 0;
/* 正文字体 / 标题字体（同族加粗）/ 建字体时用的负高度（= -像素高）。拿 s_fontPx
   和当前字号比较，相等就直接复用，避免每次重绘都重建 GDI 字体。 */
static HFONT     s_fontN = nullptr, s_fontB = nullptr;
static int       s_fontPx = 0;
/* 建这两个字体时用的字体名。只有名字变了才需要重建 —— 用户换字体时字号可能没变，
 * 光比较 s_fontPx 会"换了字体画面不动"。用 wstring 存，比较起来直观。 */
static std::wstring s_fontFace;
/* 视觉行高 / 段间距 / 章标题上方留白（像素，已含 DPI 换算）。这里的初值只是"还没量过"时的兜底，真正的
   值在 EnsureFonts 里按字号和行距档位算出来。 */
static int       s_lineH = 20, s_paraGap = 8, s_titleGap = 14;
/* 正文左内边距 / 上内边距 / 可用排版宽度。s_textW 由 UpdateMetrics 按客户区宽度算出来；初值给 100
   只是保证在还没量过窗口时 LayoutPara 也有个合理的宽度，不会退化成 0。 */
static int       s_padL = 30, s_padT = 12;
static int       s_textW = 100;

/* 每段视觉行数缓存：下标 = 段落序号，值 = 该段行数，-1 = 还没算。缓存的意义：绘制/命中/统计每帧都要问"
   这段几行"，而排版一段要调一次 GetTextExtentExPointW，几万段的书每帧全算一遍会卡；作废时机见
   InvalidCounts()。 */
static std::vector<int> s_lineCnt;          /* 每段视觉行数缓存，-1 未计算 */
/* 视口顶部锚点：第 s_aPara 段的第 s_aLine 个视觉行，画在 y = s_padT 处。它和 s_aOff
   的分工见文件头第三节。 */
static int s_aPara = 0, s_aLine = 0;        /* 视口顶部锚点 */
/* 选区起止字符偏移，区间是左闭右开 [s_selA, s_selB)，绘制和复制都按它取字。始终保证 A < B，两个都为
   -1 表示无选区 —— 于是判断"有没有选中"只要一句 s_selB > s_selA，不必再额外维护一个 bool。 */
static int s_selA = -1, s_selB = -1;        /* 选区字符偏移，A < B */
/* 拖选起点（按下左键那一刻命中的字符偏移）。拖动过程中它固定不变；鼠标越过起点时，只是把起点和目标位置
   排序后写进 s_selA / s_selB。 */
static int s_dragChar = -1;
/* 是否正在拖选。只有"按下过左键"才响应 WM_MOUSEMOVE，否则鼠标单纯路过窗口也会把选区改掉。 */
static bool s_dragging = false;

/* 首页书架：鼠标当前悬停在第几行（-1 = 没悬停在任何行上）。
 * 只为一个"悬停高亮"效果服务 —— 让用户看得出哪些行是可以点的。 */
static int s_homeHover = -1;
/* 是否已经请求过 WM_MOUSELEAVE 通知。TrackMouseEvent 只对"下一次"离开有效，
 * 所以每次进入窗口时都得重新请求一次，这个标志就是用来避免重复请求的。 */
static bool s_trackLeave = false;

/* ---------------- PDF「原始版式」模式 ---------------- *
 * 打开 PDF 时默认走"抽文字重排"（能搜索、能换字体、能配色）。用户按 F9 切到
 * "原始版式"后，改成用系统 Windows.Data.Pdf 把整页渲染成位图直接贴上去：
 * 版式、图片、字体效果 100% 还原，但这一屏里的文字是图，选不中、搜不了。
 * 两种模式共用同一份 g_book（同一份抽出来的文字），所以阅读位置、书签、
 * 章节列表都不受影响 —— 切换时靠 s_pdfPage 和字符偏移互相换算来保持位置。 */
static bool s_pdfMode = false;      /* 是否处于原始版式模式 */
static int  s_pdfPage = 0;          /* 当前显示的页号（0 基） */
static int  s_pdfScrollY = 0;       /* 页内纵向滚动量（像素）：整页比窗口高时要能往下看 */
static std::wstring s_pdfOpenedPath; /* 渲染器已经打开的 PDF 路径（避免重复打开） */

/* PDF 模式的前置声明（定义在"绘制"一节；ViewGotoOffset 之类要靠它同步页码） */
static bool PdfEnsureOpen();
static int  PdfPageFromOffset(int off);
static void PdfSyncPageFromAnchor();
static void DrawPdfPage(int W, int H);


/* 行距档位系数：0 紧凑 / 1 标准 / 2 宽松，乘以字号得到行高。用"系数"而不是固定像素，换字号时行距比例才
   保持稳定；下标由 g_set.spacing 提供，EnsureFonts 里会先夹到 0~2。 */
static const float kSpacing[] = { 1.34f, 1.62f, 1.94f };

/* 通知主窗口"视图状态变了"：发 WM_APP_STATUS，让 ui.cpp 刷新状态栏、同步目录高亮、重置"保存阅读进度"的
   防抖定时器。用 PostMessage 而不是 SendMessage，避免在绘制 / 滚动过程中同步重入主窗口。 */
static void Notify() { NrTrace(L"Notify post status"); if (s_wnd) PostMessageW(GetParent(s_wnd), WM_APP_STATUS, 0, 0); }
/* 请求异步重绘。用 InvalidateRect 而不是自己立刻刷，让系统把短时间内多次失效合并成一次
   WM_PAINT；bErase = FALSE，因为整块区域都会被背景色覆盖，不需要系统先擦一遍（擦了反而闪）。 */
static void InvalidateAll() { if (s_wnd) InvalidateRect(s_wnd, nullptr, FALSE); }

/* 惰性创建双缓冲用的内存 DC（整个进程只建一次）。参数 ref 是调用方借来的屏幕/窗口
   DC，只用来决定兼容格式，不保存；传 nullptr 时自己临时 GetDC(nullptr) 并立刻 ReleaseDC —— 借来的 DC
   绝不能长期持有。结果存在 s_dc，无返回值。这里顺手把背景模式设成 TRANSPARENT，后面所有 ExtTextOutW
   就不用逐行重复设置，也不必担心文字被背景色刷成方块。 */
static void EnsureDC(HDC ref)
{
    if (!s_dc) {
        HDC screen = ref ? ref : GetDC(nullptr);
        s_dc = CreateCompatibleDC(screen);
        if (!ref) ReleaseDC(nullptr, screen);
        SetBkMode(s_dc, TRANSPARENT);
    }
}

/* 保证后备位图尺寸等于客户区（双缓冲的核心）。参数 ref 是屏幕/窗口 DC，只用来
   建兼容位图；w / h 是目标尺寸。重建时机三种：① 第一次绘制；② 窗口尺寸变化
   （与 s_bmpW/s_bmpH 不等）；③ 上次创建失败。尺寸没变时第一行就 return，复用旧
   位图 —— 拖动窗口时每帧新建位图会明显卡顿。顺序讲究：必须先把旧位图 s_bmpOld
   选回 DC，才能 DeleteObject 释放新位图，否则位图仍被 DC 占用、删除失败，反复
   拖动窗口就会漏 GDI 句柄。重建后 s_bmpW / s_bmpH 要跟着更新，否则下一帧会误判
   "尺寸没变"。 */
static void EnsureBackBuffer(HDC ref, int w, int h)
{
    if (s_bmp && s_bmpW == w && s_bmpH == h) return;
    if (s_bmp && s_bmpOld) { SelectObject(s_dc, s_bmpOld); s_bmpOld = nullptr; }
    if (s_bmp) { DeleteObject(s_bmp); s_bmp = nullptr; }
    s_bmp = CreateCompatibleBitmap(ref, w, h);
    if (s_bmp) {
        s_bmpOld = SelectObject(s_dc, s_bmp);
        s_bmpW = w; s_bmpH = h;
    }
    SetBkMode(s_dc, TRANSPARENT);
}

/* 作废全部段落行数缓存：把 s_lineCnt 重新填成 -1，元素个数 = 当前书段落数。调用
   时机只有三个（所有让"段 -> 行"映射失效的事件）：建/换字体后、窗口宽度变了、
   换书了。为什么不立刻重算：那样每次拖动窗口都会把所有段落排一遍（几万段），而
   用户真正看到的只有几十行 —— 留 -1 把成本摊到"确实要显示"的时候。n < 0 时按 0
   处理（换书到一半可能为 0），保证按下标访问安全。 */
static void InvalidCounts()
{
    int n = g_book.ParaCount();
    if (n < 0) n = 0;
    s_lineCnt.assign((size_t)n, -1);
}

/* 按当前字号和 DPI 建字体，并顺手刷新行高、段间距、标题间距。
   早退条件：字体已存在且像素高没变 —— 此时行高、段间距都不会变，直接返回。这点
   很重要：EnsureFonts 在绘制、排版、命中测试里被频繁调用，若每次都重建字体并
   InvalidCounts()，就变成每帧重排全书的灾难。
   字号换算：磅 -> 像素用 MulDiv(fontPt, dpi, 72)；lfHeight 取负表示"字符高度"
   （不含内部行距），排版按字符高度对齐更符合预期；再夹 11 像素下限，太小看不清，
   也避免 0 高度字体让所有行叠在一起。
   行高 = 字号 * kSpacing[档位]，段间距/标题间距按字号比例算，三者都 +0.5f 再转 int
   手工四舍五入（C++ 的 float->int 是截断，少加这一下会让行高系统性偏小，长文累积
   后与滚动计算对不齐）。最后必须 InvalidCounts()：行高与宽度共同决定每段几行，
   字体变了，所有缓存行数全部失效。 */
static void EnsureFonts()
{
    int px = -MulDiv(g_set.fontPt, g_dpi, 72);
    if (px > -S(11)) px = -S(11);
    /* 字体名：用户在「阅读 → 选择字体」里挑的、或从导入的字体文件里选的；
     * 空串时退回内置默认字体，保证任何时候都有字可画。 */
    std::wstring face = g_set.fontFace.empty() ? std::wstring(L"Microsoft YaHei") : g_set.fontFace;
    if (s_fontN && px == s_fontPx && face == s_fontFace) return;
    if (s_fontN) DeleteObject(s_fontN);
    if (s_fontB) DeleteObject(s_fontB);
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = px;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    /* CLEARTYPE_QUALITY：亚像素抗锯齿，小字号中文的可读性明显比 DEFAULT_QUALITY 好；代价是文字必须画在不透明
       背景上，所以下面 SetBkMode(TRANSPARENT) 之外，背景由 PaintAll 一次性刷好。 */
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, face.c_str(), LF_FACESIZE);
    s_fontN = CreateFontIndirectW(&lf);
    /* 标题字体只改字重，其余字段复用同一个 LOGFONTW —— 保证标题和正文的字体族、宽度度量一致，"同一个字在标
       题行和正文行里宽度相同"，排版时不用为标题单独算一套宽度。 */
    lf.lfWeight = FW_BOLD;
    s_fontB = CreateFontIndirectW(&lf);
    s_fontPx = px;
    s_fontFace = face;
    /* 行距档位来自 ini，属于用户可改数据，用前先夹到合法区间，否则 kSpacing[] 会越界读到一个随机系数。 */
    int sp = g_set.spacing; if (sp < 0) sp = 0; if (sp > 2) sp = 2;
    s_lineH = (int)((-px) * kSpacing[sp] + 0.5f);
    s_paraGap = (int)((-px) * 0.42f + 0.5f);
    s_titleGap = (int)((-px) * 0.85f + 0.5f);
    InvalidCounts();
}

/* 当前字体的像素高度（正数）。标题行高、欢迎页行距、ui.cpp 里算侧栏边距都要用它；内部先
   EnsureFonts()，保证刚启动还没绘制过时也能拿到正确值。 */
int ViewFontPixel() { EnsureFonts(); return -s_fontPx; }

/* 载入用户导入的字体文件：把 g_set.fontFiles 里以 '|' 分隔的每个路径
 * 用 AddFontResourceExW(FR_PRIVATE) 私有加载一次。
 *
 * 为什么用 FR_PRIVATE：不写注册表、不装进系统「已安装字体」列表，只对本进程可见；
 * 用户"导入一个字体看小说"不该悄悄改动系统字体。进程退出后自动失效。
 * 为什么每次启动都要重新加载：字体资源是进程级的，不持久化。
 * 文件可能已被删/改名 —— 加载失败只是那个字体名回退成系统默认字，不影响其他功能，
 * 所以这里不报错、只记一条 trace。
 * 调用时机：必须在第一次 CreateFontIndirectW 之前（本次改动放在 wWinMain 里，
 * SettingsLoad 之后、建窗口之前），否则本进程内该字体名还不可见。 */
void FontsLoadImported()
{
    if (g_set.fontFiles.empty()) return;
    size_t start = 0;
    while (start <= g_set.fontFiles.size()) {
        size_t bar = g_set.fontFiles.find(L'|', start);
        std::wstring one = g_set.fontFiles.substr(
            start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
        if (!one.empty()) {
            int cnt = AddFontResourceExW(one.c_str(), FR_PRIVATE, nullptr);
            (void)cnt;   /* 正式版里 NrTrace 是空宏，不加这句会报"未使用变量" */
            NrTrace(L"字体文件 %s -> 载入 %d 个", one.c_str(), cnt);
        }
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
}

/* ---------------- 行排版 ---------------- */

/* 标点禁则（行首禁则）：这些字符不允许出现在一行开头 —— 中文排版硬规则，句号、逗号、右括号、右引号挂在
   上一行末尾才好看，单独掉到下一行行首很难看。表里是常见中文标点，外加省略号、破折号、间隔号和百分号。
   参数 c：待判定字符；返回 true 表示"不能放行首"。c == 0 时返回 false ——
   调用方会读到段尾哨兵位置，那里不是字符，不该当成禁则标点。 */
static inline bool NoLineStart(wchar_t c)
{
    static const wchar_t* t = L"，。、；：？！）】》」』”’…—·%";
    return c && wcschr(t, c) != nullptr;
}

/* 英文单词的组成字符：字母、数字、撇号、连字符。排版时若断点落在两个 WordChar 之间，说明把一个单词劈开
   了，要退回单词开头。撇号/连字符算单词内字符，所以"well-known"、"don't"
   会整体移动，不会被拆到两行。参数 c：待判定字符；返回 true 表示"属于单词内部"。 */
static inline bool WordChar(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') || c == L'\'' || c == L'-';
}

/* 计算段落视觉行，starts 依次为每行起始偏移，最后压入段落结尾偏移。
   参数 para：段落序号（0 起）；starts（输出）：每行首字符在全文中的偏移，末尾
   额外压入"段落结束偏移"作哨兵 —— 于是行数 = starts.size() - 1，第 i 行文字是
   区间 [starts[i], starts[i+1])。返回值：无；出错（无 DC、没打开书、段号越界、
   空段）时 starts 为空。
   边界：· 段尾的 '\n' 不计入行宽，否则每段最后都会多排出一个空白行；· 空段
   （len <= 0）直接返回空 starts，调用方按"0 行"处理（只占一点间距）；· 一行连
   一个字都放不下时 nFit 可能为 0，强制取 1，否则 i 不前进会死循环。
   注意：本函数用的是全局 s_dc 的"当前字体"，调用前必须 SelectObject 把正文/标题
   字体选好 —— 字体不同，量出来的宽度和行数就不同。 */
static void LayoutPara(int para, std::vector<int>& starts)
{
    starts.clear();
    if (!s_dc || !g_book.Ok()) return;
    if (para < 0 || para >= g_book.ParaCount()) return;

    /* 章标题用粗体、正文用常规体。字体必须在测量之前选好，下面 GetTextExtentExPointW 的结果完全取决于 DC
       里当前选中的字体。 */
    SelectObject(s_dc, BookIsChapterPara(g_book, para) ? s_fontB : s_fontN);

    /* 段落文本区间 [a, b)：b 是 paraOff 的哨兵项（下一段起点），正因为末尾多存了一个哨兵，最后一段的长度也
       能直接减出来。 */
    int a = (int)g_book.paraOff[para];
    int b = (int)g_book.paraOff[para + 1];
    int len = b - a;
    /* 摘掉段尾换行：书里段落之间用一个 '\n' 分隔，那个换行不属于正文，算进行宽的话每段末尾都会凭空多占一个
       字的宽度。 */
    if (len > 0 && g_book.text[(size_t)b - 1] == L'\n') len--;
    if (len <= 0) return;

    const wchar_t* t = g_book.text.c_str();
    const wchar_t* s = t + a;
    /* maxW 兜底 8 像素：客户区算成 0 或负数时（窗口刚建、最小化），GetTextExtentExPointW 传 0
       宽度行为不可靠，给个正数更安全。 */
    int maxW = s_textW > 8 ? s_textW : 8;
    int i = 0;
    while (i < len) {
        int nFit = 0;
        SIZE sz;
        sz.cx = sz.cy = 0;
        /* 一次问出"maxW 宽度里能塞下几个字"（nFit），顺便拿到实际占用宽度。这比自己逐字调用
           GetTextExtentPoint32W 累加少 N 次 GDI 调用，对几万段的书是决定性的性能差别。 */
        GetTextExtentExPointW(s_dc, s + i, len - i, maxW, &nFit, nullptr, &sz);
        if (nFit <= 0) nFit = 1;
        /* 剩下的文字一次全放得下：这就是最后一行，直接收尾。此时不做禁则 / 单词调整 ——
           调整只会让本行更短，没有收益。 */
        if (nFit >= len - i) { starts.push_back(a + i); i = len; break; }
        int br = i + nFit;
        /* 标点禁则：不让标点出现在行首。断点 br 指向"下一行的第一个字符"，若它是
           禁则标点就把断点往前挪，让标点留在本行末尾；br > i + 1 是保底 —— 一行
           至少得留一个字，否则会退到行首、甚至死循环。 */
        while (br > i + 1 && NoLineStart(s[br])) br--;
        /* 不拆英文单词：断点两侧（s[k-1] 与 s[k]）都是单词字符，说明单词被劈开了，
           从 br 往前找单词开头。k > i + 1 同样是"本行至少留一个字"的保底。 */
        int k = br;
        while (k > i + 1 && WordChar(s[k - 1]) && WordChar(s[k])) k--;
        if (k < br) br = k;
        starts.push_back(a + i);
        i = br;
    }
    /* 哨兵：段落结束偏移。有了它，行数 = size() - 1，第 i 行长度 = starts[i+1]
       - starts[i]，绘制和命中测试都不用特判最后一行。 */
    starts.push_back(a + len);
}

/* 取某段的视觉行数（带缓存）：第一次问时才真正 LayoutPara 排版，结果写进
   s_lineCnt[para]；之后同一宽度下再问就只是一次数组读取。参数 para：段落序号；
   返回：行数，段号非法时返回 0。注意：缓存里只存"行数"、不存"每行起始偏移"，
   因为存偏移才是真正耗时的部分；需要行偏移的地方（AnchorChar、DrawRun 的调用者）
   会自己再排一次当前段。 */
static int ParaLineCount(int para)
{
    if (para < 0 || para >= g_book.ParaCount()) return 0;
    /* 防御：缓存大小和当前书的段落数对不上，说明中途换过书，此时整表作废（不能只怪某一个下标，因为下标含义
       已经完全变了）。 */
    if ((int)s_lineCnt.size() != g_book.ParaCount()) InvalidCounts();
    if (s_lineCnt[(size_t)para] < 0) {
        std::vector<int> st;
        LayoutPara(para, st);
        int n = (int)st.size() - 1;
        if (n < 0) n = 0;
        s_lineCnt[(size_t)para] = n;
    }
    return s_lineCnt[(size_t)para];
}

/* ---------------- 尺寸换算 ---------------- */

/* 两个前置声明：ClampAnchor / AnchorChar 定义在下面"滚动"小节里，但它们在上面几个函数里已经被用到。先
   声明后定义，顺带说明了这个文件的逻辑分层：尺寸换算 -> 滚动 -> 跳转 -> 绘制。 */
static void ClampAnchor();
static int AnchorChar();

/* 顶行锚点的字符偏移，作为“真相”保存下来：窗口缩放 / 改字号后用它重新定位到同一段文字，避免正文突然跳
   走。初值 -1 表示"还没有有效锚点"（还没打开过书）；RestoreAnchorFromOffset 见到 -1
   会直接返回，避免拿 -1 去查段落导致越界。 */
static int s_aOff = -1;

/* 用 s_aOff 反算 (s_aPara, s_aLine)，即"字符偏移 -> 第几段第几行"。
   换算：1) 先夹住偏移（防越界）；2) BookCharToPara 二分查出落在哪一段；3) 重新
   排版这一段，顺序扫一遍行首偏移，最后一个仍 <= off 的行就是顶行。用顺序扫而非
   二分：一段最多几十行，重排都已经做过了，顺序扫更简单，还能顺带利用刚算好的
   starts。
   参数：无（读 s_aOff，写 s_aPara / s_aLine）；返回值：无。
   边界：书没打开或 s_aOff < 0 时什么都不做（保持调用前的锚点）；末尾 ClampAnchor
   兜住"重排后行数变少导致 s_aLine 越界"。
   调试：调试版（NR_TRACE）会打印还原结果，正式版这行宏展开成空语句。 */
static void RestoreAnchorFromOffset()
{
    if (!g_book.Ok() || s_aOff < 0) return;
    EnsureDC(nullptr);
    int off = s_aOff;
    int total = (int)g_book.text.size();
    /* 夹到最后一个字符：s_aOff 有可能等于全文长度（旧版本存过、或书被换过），而 BookCharToPara
       期望的是合法下标。 */
    if (off > total - 1) off = total - 1;
    if (off < 0) off = 0;
    int p = BookCharToPara(g_book, off);
    s_aPara = p;
    s_aLine = 0;
    std::vector<int> st;
    LayoutPara(p, st);
    int n = (int)st.size() - 1;
    for (int i = 0; i < n; i++) {
        if (st[(size_t)i] <= off) s_aLine = i;
        else break;
    }
    ClampAnchor();
    NrTrace(L"anchor restore off=%d -> para=%d line=%d", off, s_aPara, s_aLine);
}

/* 把当前 (s_aPara, s_aLine) 换算成字符偏移存进 s_aOff。一切改动了锚点的操作（滚行、翻页、跳转）末尾都
   必须调用它，否则下一次重排（改字号 / 缩放窗口）会按旧的 s_aOff 把阅读位置倒回去。书没打开时置
   -1，表示"没有有效锚点"。 */
static void SyncAnchorOffset() { s_aOff = g_book.Ok() ? AnchorChar() : -1; }

/* 重新计算与排版相关的尺寸：左边距、上边距、可用排版宽度。
   参数 layoutDirty：调用方是否确认"换行可能变了"（字号变、行距变、刚换书）。为
   true 时无条件作废行数缓存；为 false 时只在 s_textW 真的变了才作废 —— 这样单纯
   切换主题、单纯重绘就不会触发几万段的重新排版。
   只有 s_textW 真的变化（或 layoutDirty）才 RestoreAnchorFromOffset：宽度没变行数
   就没变，锚点自然有效，没必要多算一次。窄窗口（< 320 设计像素）把边距收窄到 14：
   侧栏展开后正文区域明显变窄，继续用 28 的边距会让每行只剩几个字。s_padT 固定 12：
   上边距不随窗口变化，保证首行不贴边。 */
static void UpdateMetrics(bool layoutDirty)
{
    if (!s_wnd) return;
    RECT rc;
    GetClientRect(s_wnd, &rc);
    int pad = S(28);
    if (rc.right < S(320)) pad = S(14);
    int w = rc.right - pad * 2;
    /* 宽度下限：窗口极窄时 w 可能 <= 0，交给 LayoutPara 的 maxW 兜底之前先在这里给个 80
       使排版结果至少是"每行几个字"，不会算出 0 行。 */
    if (w < S(80)) w = S(80);
    s_padL = pad;
    s_padT = S(12);
    if (w != s_textW || layoutDirty) {
        s_textW = w;
        InvalidCounts();
        RestoreAnchorFromOffset();   /* 重新排版后回到同一段文字 */
    }
}

/* ---------------- 滚动 ---------------- */

/* 当前顶行的字符偏移 —— 这个值同时充当滚动条的 pos 和"阅读进度"。
   参数：无（读 s_aPara / s_aLine）；返回：顶行首字符在全文中的偏移。
   边界：书没打开时返回 0；该段没排出行（空段）时退化成返回段首偏移；s_aLine 越界
   时夹到 [0, 行数-1]。
   注意：必须重新 LayoutPara 一次，因为 s_lineCnt 只缓存"行数"、没缓存每行起始偏移
   （缓存行首偏移要为每行多存一个 int，几万段乘几十行就是几十 MB，不划算）。重排
   一段只要几微秒，可以接受。 */
static int AnchorChar()
{
    if (!g_book.Ok()) return 0;
    std::vector<int> st;
    LayoutPara(s_aPara, st);
    int n = (int)st.size() - 1;
    if (n <= 0) return (int)g_book.paraOff[(size_t)s_aPara];
    int line = s_aLine;
    if (line < 0) line = 0;
    if (line >= n) line = n - 1;
    return st[(size_t)line];
}

/* 把锚点夹回合法范围：段号 [0, 段数-1]，行号 [0, 该段行数-1]。每次滚动、跳转、重排之后都调一次，避免绘
   制时按越界的段号去读 paraOff。边界：空书（段数 <= 0）时全部归零；该段排不出行（空段落）时行号置
   0。注意：这里调 ParaLineCount 会触发该段的懒排版（如果还没算过）——
   这既是必要的（得知道行数上限），也正好把即将显示的那一段预热了。 */
static void ClampAnchor()
{
    int pc = g_book.ParaCount();
    if (pc <= 0) { s_aPara = 0; s_aLine = 0; return; }
    if (s_aPara >= pc) s_aPara = pc - 1;
    if (s_aPara < 0) s_aPara = 0;
    int n = ParaLineCount(s_aPara);
    if (n <= 0) { s_aLine = 0; return; }
    if (s_aLine >= n) s_aLine = n - 1;
    if (s_aLine < 0) s_aLine = 0;
}

/* 统计"从当前锚点往下，一屏能装几行、几个字符"。
   参数 chars：可为 nullptr；不为空时写入这一屏覆盖的字符总数，用于滚动条的 nPage
   （滑块大小）。同时返回行数和字符数，是因为滚动条用"字符偏移"做 range，nPage 也
   必须用字符数，滑块长度才代表"一屏占全文的比例"。
   返回：可见行数，至少为 1 —— 调用方（ViewScrollPages）拿它算翻页步长，返回 0 会
   导致翻页原地不动甚至除零。
   实现要点：y 的累加规则必须和 PaintAll 完全一致（s_lineH / s_paraGap / s_titleGap
   / 空段半行高），否则"一屏几行"和"实际画几行"对不上，翻页就会重复或漏行。
   guard 5000：段落数据异常时防止死循环。 */
static int LinesInView(int* chars)
{
    int cnt = 0, chs = 0;
    if (!g_book.Ok() || !s_wnd) { if (chars) *chars = 1; return 1; }
    EnsureFonts();
    RECT rc; GetClientRect(s_wnd, &rc);
    int H = rc.bottom;
    int y = s_padT;
    std::vector<int> st;
    int p = s_aPara, line = s_aLine, guard = 0;
    while (p < g_book.ParaCount() && guard++ < 5000) {
        bool title = BookIsChapterPara(g_book, p);
        /* 排版前必须选中该段真正会用到的字体，否则粗体标题会被按正文字体量宽度，行数偏少，一屏能装的字符数也就
           跟着算错。 */
        SelectObject(s_dc, title ? s_fontB : s_fontN);
        LayoutPara(p, st);
        int nl = (int)st.size() - 1;
        /* 空段落不占整行，只占半行高（大约等于一个段间距）。这个规则必须和 PaintAll
           里逐字一致，否则"可见行数"会偏大。 */
        if (nl <= 0) { y += s_lineH / 2; p++; line = 0; continue; }
        /* 章标题上方额外留白，只在"该段第 0 行可见"时加。滚动到标题中间时不再加，避免滚动过程中高度突然跳变。 */
        if (title && line == 0) y += s_titleGap;
        for (int l = line; l < nl; l++) {
            /* 放不下整行就停：宁可少算一行，也不要"半行"，否则 nPage 会偏大，滚动条滑块偏短。 */
            if (y + s_lineH > H) { if (chars) *chars = chs; return cnt > 0 ? cnt : 1; }
            cnt++;
            chs += st[(size_t)l + 1] - st[(size_t)l];
            y += s_lineH;
        }
        y += s_paraGap;
        p++; line = 0;
    }
    if (chars) *chars = chs > 0 ? chs : 1;
    return cnt > 0 ? cnt : 1;
}

/* 把锚点状态同步到垂直滚动条。range / pos 都用字符偏移（理由见文件头第四节），
   nPage 是"一屏字符数"；返回值：无。
   · total < 1 时强行置 1：nMax 不能等于 nMin，否则 SB_THUMBTRACK 会得到除零的位置
     计算，滑块乱跳。
   · SIF_DISABLENOSCROLL：内容不足一屏时让滚动条变灰而不是消失 —— 滚动条一出现一
     消失客户区宽度就变，会触发整本书重排，正文剧烈跳动。
   最后参数 TRUE 表示立即重画滚动条（否则要等下一次 WM_PAINT）。 */
static void UpdateScrollBar()
{
    if (!s_wnd) return;
    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;

    /* PDF 原始版式：滚动条改用"页"作单位，一格 = 一页 */
    if (s_pdfMode && PdfRenderOk()) {
        int pages = PdfRenderPageCount();
        if (pages < 1) pages = 1;
        si.nMin = 0;
        si.nMax = pages - 1;
        si.nPage = 1;
        si.nPos = s_pdfPage;
        SetScrollInfo(s_wnd, SB_VERT, &si, TRUE);
        return;
    }

    int total = (int)g_book.text.size();
    if (total < 1) total = 1;
    int chars = 1;
    LinesInView(&chars);
    si.nMin = 0;
    si.nMax = total;
    si.nPage = chars > 0 ? chars : 1;
    /* nPage 不能超过 total：滑块长度 = page/(max-min+1)，超过 1 时滚动条会画不出来（Windows
       认为"全都在视野里"）。 */
    if ((int)si.nPage > total) si.nPage = total;
    si.nPos = g_book.Ok() ? AnchorChar() : 0;
    SetScrollInfo(s_wnd, SB_VERT, &si, TRUE);
}

/* 相对当前锚点滚动 n 个视觉行（正数向下，负数向上）。
   参数 n：要滚动的行数，可为负；n == 0 时直接返回（不做无谓的重绘）。返回值：无。
   实现是"逐段跨过去"：先在当前段内挪，挪出界就进/退到相邻段，直到消耗完 n。之所以
   不能直接算"目标行 = 当前行 + n"，是因为各段行数不等、而且依赖懒排版结果 —— 除了
   当前段，别的段行数是现问现算的。
   边界：到全书开头/结尾时把余下的 n 吃掉并停在边界，绝不越界；guard 20000 防止数据
   异常时死循环（正常一屏最多几十行）。
   收尾顺序：Clamp 夹住 -> 同步 s_aOff（否则重排会跳回旧位置）-> 刷滚动条 -> 重绘 ->
   通知主窗口刷新状态栏。 */
void ViewScrollLines(int n)
{
    if (!g_book.Ok() || n == 0) return;
    int guard = 0;
    while (n > 0 && guard++ < 20000) {
        int cnt = ParaLineCount(s_aPara);
        /* 当前段还够放：行号直接加，滚动结束。 */
        if (s_aLine + n < cnt) { s_aLine += n; n = 0; break; }
        /* 不够：把本段剩下的行数用掉，带着剩余的 n 进入下一段的第 0 行。 */
        n -= (cnt - s_aLine);
        /* 已经是最后一段：停在最后一行，余下的 n 作废。 */
        if (s_aPara + 1 >= g_book.ParaCount()) { s_aLine = cnt > 0 ? cnt - 1 : 0; n = 0; break; }
        s_aPara++; s_aLine = 0;
    }
    while (n < 0 && guard++ < 20000) {
        /* 往上滚：n 是负数，s_aLine + n >= 0 表示还在本段内。 */
        if (s_aLine + n >= 0) { s_aLine += n; n = 0; break; }
        /* 本段撑不住：n += s_aLine 相当于消耗掉"退到本段第 0 行"的那部分，剩下的继续往上退段。 */
        n += s_aLine;
        /* 已经是第一段：停在第一行，余下的 n 作废。 */
        if (s_aPara == 0) { s_aLine = 0; n = 0; break; }
        s_aPara--;
        /* 往上退到上一段时落在它的最后一行，不是第一行 —— 这样连续上滚才是"一行一行过"，不会跳过上一段内容。 */
        int cnt = ParaLineCount(s_aPara);
        s_aLine = cnt > 0 ? cnt - 1 : 0;
    }
    ClampAnchor();
    SyncAnchorOffset();
    UpdateScrollBar();
    InvalidateAll();
    Notify();
}

/* 翻页：n = +1 下一页，-1 上一页（0 时等于原地翻一页，实际不会这么调）。
   一页 = 屏幕可见行数 - 1。减 1 是为了留一行重叠：读者能靠这一行接上刚读到的位置，
   不会漏读 —— 这是阅读器翻页的通行做法，也是"翻页后重读一行"看起来像 bug、其实是
   刻意设计的原因。内部转调 ViewScrollLines，滚动条/锚点/重绘都由它统一处理。 */
void ViewScrollPages(int n)
{
    int cnt = LinesInView(nullptr);
    int step = cnt > 1 ? cnt - 1 : 1;
    ViewScrollLines(n * step);
}

/* 跳到指定字符偏移，并高亮其后 selLen 个字符（搜索结果跳转用）。
   参数：off 目标字符偏移，会被夹到 [0, 全文长度-1]；selLen 高亮长度，<= 0 表示普通
   跳转，顺带清掉旧选区。返回值：无。
   内部还会把锚点往上回退最多 2 行，让命中位置上方留一点上下文 —— 否则搜索跳转会把
   关键词顶在窗口最上面，读者看不出前后文；回退上限 2 行且不跨段，跨段会把上一段的
   尾巴也翻出来，反而打乱定位。
   开头先 UiAutoStop()：这是用户主动改变阅读位置，自动下滑必须立刻停下，否则刚跳过去
   就被自动下滑拖走。 */
void ViewGotoOffset(int off, int selLen)
{
    UiAutoStop();
    NrTrace(L"ViewGotoOffset off=%d len=%d", off, selLen);
    if (!g_book.Ok()) return;
    if (off < 0) off = 0;
    /* 夹到最后一个字符：传进来的可能是 text.size()（比如"跳到末尾"），size() 本身已经越界一格。 */
    if (off > (int)g_book.text.size()) off = (int)g_book.text.size() - 1;
    int p = BookCharToPara(g_book, off);
    s_aPara = p;
    s_aLine = 0;
    std::vector<int> st;
    LayoutPara(p, st);
    int n = (int)st.size() - 1;
    /* 在段内找包含 off 的视觉行：最后一个"行首 <= off"的行就是它。 */
    for (int i = 0; i < n; i++) {
        if (st[(size_t)i] <= off) s_aLine = i;
        else break;
    }
    /* 选中端点次序：搜索命中的区间天然是 [off, off+selLen)，直接就是 A < B，不用像鼠标拖选那样排序。 */
    if (selLen > 0) { s_selA = off; s_selB = off + selLen; }
    else { s_selA = -1; s_selB = -1; }
    /* 命中位置上方留一点上下文 */
    int up = s_aLine < 2 ? s_aLine : 2;
    /* 借 ViewScrollLines 完成回退：它内部会 Clamp、同步
       s_aOff、刷滚动条、重绘并通知主窗口，这里不再重复这些收尾动作。 */
    if (up > 0) ViewScrollLines(-up);
    SyncAnchorOffset();
    /* 原始版式模式下，跳到某处要连带把页面翻到对应那一页（搜索命中就是这样跳页的） */
    PdfSyncPageFromAnchor();
    UpdateScrollBar();
    InvalidateAll();
    Notify();
}

/* 跳到某一段的段首（目录点击、章节跳转最终都落到这里）。参数 para：目标段落，会被夹到 [0,
   段数-1]；返回值：无。与 ViewGotoOffset 的差别：不做"上方留白"（段落序号本身就是精确目标），并且顺手
   清掉选区 —— 跳转后残留的高亮会落在毫不相关的段落上。同样先 UiAutoStop()（跳转是用户的主动操作）。 */
void ViewGotoPara(int para)
{
    UiAutoStop();
    if (!g_book.Ok()) return;
    if (para < 0) para = 0;
    if (para >= g_book.ParaCount()) para = g_book.ParaCount() - 1;
    s_aPara = para;
    s_aLine = 0;
    s_selA = s_selB = -1;
    ClampAnchor();
    SyncAnchorOffset();
    PdfSyncPageFromAnchor();
    UpdateScrollBar();
    InvalidateAll();
    Notify();
}

/* 按章跳转：delta < 0 上一章，delta > 0 下一章。参数 delta 只有符号有意义（-1 /
   +1 由按键分支传入）；返回值：无。
   三个细节：1) 没有章节信息（ch < 0，例如开篇文字不在任何章节内、或书没识别出章节）
   时退化成"翻一页"，至少让按键有反馈；2) 向上翻时，如果当前段落已经越过本章标题
   （即正读到章中间），先回到本章标题，再按一次才去上一章 —— 这样"上一章"第一次是
   "回到本章开头"，符合阅读器习惯；3) 越过最后一章时跳到全文末尾，而不是卡在最后一
   章的标题上。同样先 UiAutoStop()。 */
void ViewGotoChapter(int delta)
{
    UiAutoStop();
    if (!g_book.Ok()) return;
    int cur = ViewCurrentPara();
    int ch = BookChapterAtPara(g_book, cur);
    int target;
    if (ch < 0) {
        /* 没有章节信息：按视口大小跳 */
        ViewScrollPages(delta > 0 ? 1 : -1);
        return;
    }
    /* 若当前正在章标题之上，上一章要回到本章标题 */
    if (delta < 0) {
        /* 本章标题行在 cur 之前（严格小于），说明已经读过了标题，先回到本章开头；否则才去上一章的标题。 */
        if (ch >= 0 && g_book.chapters[(size_t)ch].para < cur) target = ch;
        else target = ch - 1;
    } else {
        target = ch + 1;
    }
    if (target < 0) target = 0;
    if (target >= (int)g_book.chapters.size()) {
        /* 没有下一章了：跳到全文最后一个字符，比停在最后一章标题上更符合预期。 */
        ViewGotoOffset((int)g_book.text.size() - 1, 0);
        return;
    }
    ViewGotoPara(g_book.chapters[(size_t)target].para);
}

/* ===================== PDF 原始版式的对外接口 ===================== */

/* 切换「原始版式 / 文字重排」。参数 on：true 切到原始版式。
 * 切不过去（不是 PDF、或渲染器打不开）时保持现状 —— 调用方（ui.cpp）据此提示用户。
 * 切过去时立刻按当前阅读位置算出该显示第几页，两种模式看到的是同一处文字。 */
bool ViewSetPdfMode(bool on)
{
    if (!on) {
        if (s_pdfMode) {
            s_pdfMode = false;
            if (s_wnd) { UpdateScrollBar(); InvalidateAll(); }
        }
        return true;
    }
    if (!g_book.Ok()) return false;
    /* 先确保渲染器可用：打不开就当没这回事，界面上仍然是文字重排 */
    if (!PdfEnsureOpen()) return false;
    s_pdfMode = true;
    PdfSyncPageFromAnchor();
    if (s_wnd) { UpdateScrollBar(); InvalidateAll(); }
    return true;
}

bool ViewIsPdfMode() { return s_pdfMode; }

int ViewPdfPageCount() { return s_pdfMode ? PdfRenderPageCount() : 0; }

int ViewPdfPage() { return s_pdfPage; }

int ViewPdfPageFromOffset(int off) { return PdfPageFromOffset(off); }

/* 翻页：delta = +1 下一页 / -1 上一页。翻页同时把文字锚点移到该页开头，
 * 这样状态栏的百分比、书架进度、书签落点都跟着走，和文字模式口径一致。 */
void ViewPdfFlip(int delta)
{
    if (!s_pdfMode) return;
    int total = PdfRenderPageCount();
    if (total <= 0) return;
    int page = s_pdfPage + delta;
    if (page < 0) page = 0;
    if (page > total - 1) page = total - 1;
    if (page == s_pdfPage) return;

    const std::vector<uint32_t>& offs = PdfLastPageOffsets();
    if ((size_t)page < offs.size()) {
        /* 走 ViewGotoOffset 而不是只改 s_pdfPage：它会顺带同步锚点、进度和重绘 */
        ViewGotoOffset((int)offs[(size_t)page] + 1, 0);
        s_pdfPage = page;
        s_pdfScrollY = 0;
        if (s_wnd) { UpdateScrollBar(); InvalidateAll(); }
    } else {
        s_pdfPage = page;
        s_pdfScrollY = 0;
        if (s_wnd) { UpdateScrollBar(); InvalidateAll(); }
        Notify();
    }
}

/* 页内纵向滚动（整页比窗口高时用）。dy 为像素位移，正数向下。 */
void ViewPdfScroll(int dy)
{
    if (!s_pdfMode || dy == 0) return;
    s_pdfScrollY += dy;
    if (s_pdfScrollY < 0) s_pdfScrollY = 0;
    if (s_wnd) InvalidateAll();
}

/* 当前顶行的字符偏移 —— 对外接口，ui.cpp 用它算阅读进度百分比、保存阅读位置（PosSave）。内部直接转调
   AnchorChar。 */
int ViewAnchorOffset() { return AnchorChar(); }

/* 当前顶行所在的段落序号 —— ui.cpp 用它判断"现在是第几章"、同步目录高亮。返回 s_aPara
   本身而不做换算：锚点就是用段落号表示的。边界：没打开书时返回 0。 */
int ViewCurrentPara()
{
    if (!g_book.Ok()) return 0;
    return s_aPara;
}

/* 换书后调用：把所有与"上一本书"相关的状态清零并重建。
   顺序讲究：先清锚点和选区 -> 重建字体 -> 重算尺寸 -> 作废行数缓存 -> 夹锚点，最后
   才刷滚动条和重绘（此时 s_lineCnt 的大小已经和新书对齐）。s_aOff 置 -1 是关键：否则
   下一次 UpdateMetrics 会拿旧书的偏移去定位新书，出现"打开新书却停在奇怪位置"。
   s_dragging / s_dragChar 也要复位：换书可能发生在拖选途中（拖到一半按快捷键打开别的
   文件），不复位会留下指向旧书的起点。注意：这里 InvalidCounts 调了两次（EnsureFonts
   里一次、这里一次）是刻意的冗余 —— 保证不管字体有没有真的重建，缓存都与新书对齐。 */
void ViewSetBook()
{
    s_aPara = 0; s_aLine = 0;
    s_aOff = -1;
    s_selA = s_selB = -1;
    s_dragChar = -1;
    s_dragging = false;
    /* 首页书架的悬停行也清掉：换书之后这一行要么不存在（有书了）、要么是新书架的另一行 */
    s_homeHover = -1;
    /* PDF 原始版式状态一起复位：换书（包括关书回书架）后必须回到"文字重排"模式，
       并关掉上一本书的渲染器，否则会拿旧 PDF 的页面去画新书。 */
    s_pdfMode = false;
    s_pdfPage = 0;
    s_pdfScrollY = 0;
    if (!s_pdfOpenedPath.empty()) {
        PdfRenderClose();
        s_pdfOpenedPath.clear();
    }
    EnsureFonts();
    UpdateMetrics(false);
    InvalidCounts();
    ClampAnchor();
    if (s_wnd) {
        UpdateScrollBar();
        InvalidateAll();
    }
}

/* 字号 / 行距 / 主题 / 窗口宽度变化后由 ui.cpp 调用。参数 layoutDirty：true
   表示换行可能变了（字号、行距、刚换书），要作废所有行数缓存；false
   表示"只是外观变了"（比如切主题），不必重排。返回值：无。必须先 EnsureFonts()：字号变了行高也变了，得
   先把 s_lineH 更新到新值，再让 UpdateMetrics 判断宽度是否变化并使用新的行高去还原锚点。顺序：字体 ->
   尺寸（内部按需作废缓存 + 还原锚点）-> 夹锚点 -> 刷新界面。 */
void ViewMetricsChanged(bool layoutDirty)
{
    EnsureFonts();
    if (layoutDirty) InvalidCounts();
    UpdateMetrics(layoutDirty);
    ClampAnchor();
    if (s_wnd) { UpdateScrollBar(); InvalidateAll(); }
}

/* 请求重绘（异步，可在任何线程上下文安全调用；没有窗口时是空操作）。给 ui.cpp 用，避免外部直接碰
   s_wnd。 */
void ViewInvalidate() { InvalidateAll(); }
/* 把键盘焦点放到正文上，保证打开书 / 搜索跳转之后方向键能直接翻页 ——焦点如果还留在目录列表或搜索框里，
   方向键是给那些控件用的。没有窗口时是空操作。 */
void ViewFocus() { if (s_wnd) SetFocus(s_wnd); }

/* 把选区文字复制到剪贴板（CF_UNICODETEXT）。参数：无（取 s_selA / s_selB）。
   边界：没打开书、没有选区（s_selB <= s_selA）时直接返回；OpenClipboard 可能因为别的
   程序正占用剪贴板而失败，失败就静默放弃 —— 剪贴板是共享资源，抢不到时弹框报错反而
   打扰用户。内存所有权：SetClipboardData 成功之后这块内存归系统所有，所以把 h 置成
   nullptr，避免下面那句 GlobalFree 把已经交出去的内存再释放一次（那会导致双重释放 /
   堆损坏）；失败路径上才需要自己 GlobalFree。bytes 里多算了一个 wchar_t：剪贴板文本
   必须以 L'\0' 结尾。 */
void ViewCopySelection()
{
    if (!g_book.Ok() || s_selB <= s_selA) return;
    std::wstring s = g_book.text.substr((size_t)s_selA, (size_t)(s_selB - s_selA));
    if (s.empty()) return;
    if (!OpenClipboard(s_wnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
            h = nullptr;
        }
        if (h) GlobalFree(h);
    }
    CloseClipboard();
}

/* 全选正文：选区直接覆盖 [0, size())，把章节标题、空行也一并算进去 ——
   复制出来的是原始文本（段落之间保留 '\n'），粘到别处仍然是分行的。这里没有触发
   Notify()：全选不改变阅读位置，状态栏的进度百分比不变。 */
void ViewSelectAll()
{
    if (!g_book.Ok()) return;
    s_selA = 0;
    s_selB = (int)g_book.text.size();
    InvalidateAll();
}

/* ---------------- 绘制 ---------------- */

/* 画一行文字，并处理这一行内在的选区高亮。
   参数：x, y 这一行左上角绘制坐标（y 是行顶，不是基线 —— ExtTextOutW 的 y 表示"字符
   单元的顶部"）；start 行首字符在全文中的偏移；end 行尾偏移（不含），即本行是
   [start, end)。返回值：无。
   难点：一行里可能只有一部分被选中。做法是先求本行与选区的交集 [os, oe)，再用
   GetTextExtentPoint32W 量出"选区左侧那段"的像素宽度 pre.cx，才能把高亮矩形和选中
   文字精确对齐（中文全角等宽，但英文、数字、标点不是，必须实测而不能按字数乘）。然后
   分三段画：选中前（正常色）-> 选中段（高亮色）-> 选中后（正常色），x 依次累加
   pre.cx / sel.cx；没有选区时走最后的"整行一次画完"快路径。 */
static void DrawRun(int x, int y, int start, int end)
{
    const Theme& th = CurTheme();
    const wchar_t* s = g_book.text.c_str() + start;
    int len = end - start;
    if (len <= 0) return;
    /* 裁剪矩形：上下各放 2 像素余量（y - 2），右侧多给 3 像素。原因：中文字形可能
       略超 GetTextExtentExPointW 报告的宽度（标点悬挂 / 斜体），右侧不留余量会被裁掉
       最后一笔；上下留余量是为了行距紧凑时上一行的降部不被切。用 ETO_CLIPPED 把绘制
       限制在这个矩形里，防止某一行画歪后把整片背景涂花。 */
    RECT clip;
    clip.left = x;
    clip.top = y - 2;
    clip.right = x + s_textW + S(3);
    clip.bottom = y + s_lineH;
    /* 有选区才走分段绘制；没有选区时整行一次 ExtTextOutW 最快。 */
    if (s_selB > s_selA) {
        /* 本行与选区求交集：起点取两者较大者，终点取两者较小者。os < oe
           才是真的有重叠，"本行完全在选区外"时下面会跳过。 */
        int os = start > s_selA ? start : s_selA;
        int oe = end < s_selB ? end : s_selB;
        if (os < oe) {
            SIZE pre, sel;
            pre.cx = pre.cy = sel.cx = sel.cy = 0;
            /* 量"高亮左边那段"的宽度。os == start 时（高亮从行首开始）根本不用量，pre 保持 0。 */
            if (os > start) GetTextExtentPoint32W(s_dc, s, os - start, &pre);
            GetTextExtentPoint32W(s_dc, s + (os - start), oe - os, &sel);
            RECT r;
            r.left = x + pre.cx;
            r.top = y;
            /* 右边多 1 像素，避免高亮块和文字宽度完全相同导致最后一点字形露在白底上，看起来像缺了一块。 */
            r.right = r.left + sel.cx + S(1);
            r.bottom = y + s_lineH;
            /* 先用纯色块填背景，再往上写字（而不是 SetBkColor + ETO_OPAQUE），这样才能精确控制高亮只覆盖"选中文字
               的宽度"，不会顺带刷到行尾。 */
            HBRUSH b = CreateSolidBrush(th.selBg);
            FillRect(s_dc, &r, b);
            DeleteObject(b);
            SetTextColor(s_dc, th.fg);
            /* 段落一：高亮之前的部分，正常色，从行首画到 os。 */
            if (os > start) ExtTextOutW(s_dc, x, y, ETO_CLIPPED, &clip, s, os - start, nullptr);
            SetTextColor(s_dc, th.selFg);
            /* 段落二：高亮部分，高亮前景色，x 位移到 pre.cx 处。 */
            ExtTextOutW(s_dc, x + pre.cx, y, ETO_CLIPPED, &clip, s + (os - start), oe - os, nullptr);
            SetTextColor(s_dc, th.fg);
            /* 段落三：高亮之后剩下的部分。oe == end 时（选区一直到行尾）这段长度为 0，跳过。x 继续累加 sel.cx。 */
            if (end > oe) ExtTextOutW(s_dc, x + pre.cx + sel.cx, y, ETO_CLIPPED, &clip, s + (oe - start), end - oe, nullptr);
            /* 分段画完必须立刻返回：否则会落到下面那句整行绘制，同一行字被画两遍，笔画叠加会显得发粗、高亮段上还会
               盖一层正常色。 */
            return;
        }
    }
    SetTextColor(s_dc, th.fg);
    ExtTextOutW(s_dc, x, y, ETO_CLIPPED, &clip, s, len, nullptr);
}

/* ---------------- 首页：书架 ---------------- */

/* 前置声明：DrawHome 里"书架为空"时要把画面交回给原来的三行引导语，
 * 而 DrawWelcome 的实体在下面才定义，所以这里先声明一下。 */
static void DrawWelcome(int W, int H);

/* 书架行的几何量。绘制与点击命中**共用**下面几个函数，绝不各算一份 —— 两份规则一旦不同步，
 * 用户点到的行和看到的行就会错开（这种 bug 在自绘界面里非常难查）。全是 96 DPI 设计值。 */
static int HomeRowH()    { return S(74); }   /* 一行两排字（书名 + 章节信息），74 足够宽松 */
static int HomeListTop() { return S(108); }  /* 列表从"大标题 + 副标题"下面开始 */

/* 客户区 y 坐标 → 书架行号；不在任何行上返回 -1。
 * 边界：有书打开时首页不显示（返回 -1）；书架为空时也返回 -1（此时画的是引导语）。 */
static int HomeRowAt(int y)
{
    if (g_book.Ok()) return -1;
    const std::vector<ShelfItem>& items = ShelfItems();
    if (items.empty()) return -1;
    int top = HomeListTop();
    if (y < top) return -1;
    int idx = (y - top) / HomeRowH();
    if (idx < 0 || idx >= (int)items.size()) return -1;
    return idx;
}

/* 把文字裁到 maxW 像素以内（放不下就截断并补一个省略号）。
 * 首页书名可能很长（"斗破苍穹_全本精校版_含番外"），不裁会顶到右边的百分比上。
 * 二分找切点：标题最多几十个字符，线性也行，但二分更省事也不会慢。 */
static std::wstring FitText(HDC dc, const std::wstring& s, int maxW)
{
    if (s.empty() || maxW <= 0) return L"";
    SIZE sz; sz.cx = sz.cy = 0;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    if (sz.cx <= maxW) return s;                 /* 放得下，原样返回 */
    const std::wstring ell = L"…";
    int lo = 0, hi = (int)s.size();
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;             /* 向上取整，避免 lo==mid 死循环 */
        std::wstring t = s.substr(0, (size_t)mid) + ell;
        GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        if (sz.cx <= maxW) lo = mid; else hi = mid - 1;
    }
    if (lo <= 0) return L"";
    return s.substr(0, (size_t)lo) + ell;
}

/* 首页：没打开书时整屏显示。
 * 两种情况：
 *   1) 书架里有记录（读过书）→ 画"书架"标题 + 每本书一行（书名 / 百分比 / 章节进度），
 *      行可点击（命中判定见 HomeRowAt），悬停时整行轻微加深；
 *   2) 书架为空（第一次运行）→ 退回原来的三行引导语，视觉上和以前完全一致。
 * 参数 W / H 是客户区宽高，由 PaintAll 传入。 */
static void DrawHome(int W, int H)
{
    const Theme& th = CurTheme();
    EnsureFonts();

    const std::vector<ShelfItem>& items = ShelfItems();
    if (items.empty()) { DrawWelcome(W, H); return; }

    int x0 = S(40);                              /* 左边距：比正文的 s_padL 稍大，让书架看着独立 */
    int x1 = W - S(40);                          /* 右边距 */

    /* ---- 大标题 + 副标题 ---- */
    SelectObject(s_dc, s_fontB);
    SetTextColor(s_dc, th.fg);
    const wchar_t* big = L"书架";
    ExtTextOutW(s_dc, x0, S(34), 0, nullptr, big, (int)wcslen(big), nullptr);

    wchar_t sub[128];
    wsprintfW(sub, L"共 %d 本 · 单击书名继续阅读，或按 Ctrl+O 打开新文件", (int)items.size());
    SelectObject(s_dc, s_fontN);
    SetTextColor(s_dc, th.dim);
    ExtTextOutW(s_dc, x0, S(34) + s_lineH + S(4), 0, nullptr, sub, (int)wcslen(sub), nullptr);

    /* ---- 书列表 ---- */
    int top = HomeListTop();
    int rowH = HomeRowH();
    int n = (int)items.size();
    for (int i = 0; i < n; i++) {
        if (top + rowH > H) break;               /* 放不下了就停（不做滚动，书不会太多） */
        const ShelfItem& it = items[(size_t)i];

        /* 悬停底色：只给"能点"的行画（文件已经不在的行灰显且不可点） */
        if (i == s_homeHover && !it.missing) {
            RECT r = { S(24), top, W - S(24), top + rowH };
            HBRUSH hb = CreateSolidBrush(th.hov);
            FillRect(s_dc, &r, hb);
            DeleteObject(hb);
        }

        int tx = x0 + S(8);
        int tx1 = x1 - S(8);

        /* 右侧百分比：先量出宽度，给书名留出剩余空间（"100.0%" 和 "3.2%" 都要按右边界对齐） */
        wchar_t pct[32];
        if (it.pct < 0)            lstrcpynW(pct, L"未读", 32);
        else if (it.pct >= 1000)   lstrcpynW(pct, L"100%", 32);
        else                       wsprintfW(pct, L"%d.%d%%", it.pct / 10, it.pct % 10);
        SelectObject(s_dc, s_fontB);
        SIZE psz; psz.cx = psz.cy = 0;
        GetTextExtentPoint32W(s_dc, pct, (int)wcslen(pct), &psz);

        /* 第一排：书名（粗体），右边是百分比 */
        std::wstring title = FitText(s_dc, it.title, tx1 - psz.cx - S(16) - tx);
        SetTextColor(s_dc, it.missing ? th.dim : th.fg);
        ExtTextOutW(s_dc, tx, top + S(10), 0, nullptr, title.c_str(), (int)title.size(), nullptr);
        SetTextColor(s_dc, it.missing ? th.dim : th.title);
        ExtTextOutW(s_dc, tx1 - psz.cx, top + S(10), 0, nullptr, pct, (int)wcslen(pct), nullptr);

        /* 第二排：章节进度（次要信息，用 dim 色） */
        std::wstring info;
        if (it.missing) {
            info = L"文件已不在：";
            info += it.path;
        } else if (!it.chapter.empty()) {
            info = it.chapter;
            if (it.chn > 0) {
                wchar_t b[64];
                wsprintfW(b, L"   ·   共 %d 章", it.chn);
                info += b;
            }
        } else if (it.chn > 0) {
            wchar_t b[64];
            wsprintfW(b, L"共 %d 章 · 还没开始读", it.chn);
            info += b;
        } else {
            info = L"还没开始读";
        }
        SelectObject(s_dc, s_fontN);
        SetTextColor(s_dc, th.dim);
        info = FitText(s_dc, info, tx1 - tx);
        ExtTextOutW(s_dc, tx, top + S(10) + s_lineH + S(2), 0, nullptr, info.c_str(), (int)info.size(), nullptr);

        top += rowH;
    }

    /* ---- 底部提示 ---- */
    const wchar_t* hint = L"提示：Ctrl+W 关闭当前书回到书架 · 拖入文件也能打开";
    SelectObject(s_dc, s_fontN);
    SetTextColor(s_dc, th.dim);
    SIZE hz; hz.cx = hz.cy = 0;
    GetTextExtentPoint32W(s_dc, hint, (int)wcslen(hint), &hz);
    int hy = H - s_lineH - S(16);
    if (hy > top) ExtTextOutW(s_dc, (W - hz.cx) / 2, hy, 0, nullptr, hint, (int)wcslen(hint), nullptr);
}

/* 欢迎页：还没打开书时整屏显示的三行提示。参数：W / H 是客户区宽高（由 PaintAll
   传入，省得再取一次）；返回值：无。用 dim 色而不是正文色：视觉上弱化，让"拖一个文件
   进来"这句引导成为焦点；行距用 s_lineH + S(6)，比正文松一点，三行不至于挤成一坨。注意这里自己
   SelectObject 了正文字体，写完不用恢复 —— 每次绘制开头都会重新选字体，不会污染别的绘制分支。
   什么时候出现：只有"书架是空的"时（第一次运行、或最近打开被清空），见 DrawHome。 */
static void DrawWelcome(int W, int H)
{
    const Theme& th = CurTheme();
    EnsureFonts();
    SelectObject(s_dc, s_fontN);
    SetTextColor(s_dc, th.dim);
    const wchar_t* lines[] = {
        L"拖一个小说文件到这里，或按 Ctrl+O 打开",
        L"支持 TXT / EPUB / PDF / DOCX / HTML / Markdown，自动分章",
        L"关闭窗口会自动记住阅读位置"
    };
    int n = 3;
    int lh = s_lineH + S(6);
    /* 垂直居中：总高 lh * n，从屏幕中线往上退半个总高作为第一行的 y。 */
    int y = H / 2 - lh * n / 2;
    for (int i = 0; i < n; i++) {
        SIZE sz; sz.cx = sz.cy = 0;
        GetTextExtentPoint32W(s_dc, lines[i], (int)wcslen(lines[i]), &sz);
        /* 水平居中：(客户区宽 - 文字宽) / 2。 */
        ExtTextOutW(s_dc, (W - sz.cx) / 2, y, 0, nullptr, lines[i], (int)wcslen(lines[i]), nullptr);
        y += lh;
    }
}

/* ============================ PDF「原始版式」模式 ============================ *
 * 思路：用系统自带的 Windows.Data.Pdf 把整页渲染成位图，按"铺满窗口宽度"缩放后
 * 贴上去。整页比窗口高时用 s_pdfScrollY 往下看（滚轮），翻页用 PageUp/PageDown。
 * 之所以不缩放成"整页可见"：小说 PDF 正文本来就要大一点才看得清，铺满宽度更实用。
 * 页码与文字偏移的换算依赖 pdf.cpp 记录的"每页正文起点偏移"（PdfLastPageOffsets），
 * 这样搜索命中、书签、记忆位置在两种模式之间切换时都不会跳位置。
 * ============================================================================ */

/* 确保渲染器已经打开了当前这本书。返回 false 表示这本书根本不是 PDF、或者
 * 渲染失败（例如加密 PDF、系统组件缺失），调用方应当退回文字模式。 */
static bool PdfEnsureOpen()
{
    if (!g_book.Ok() || !PdfIsPdf(g_book.path)) return false;
    if (s_pdfOpenedPath == g_book.path && PdfRenderOk()) return true;

    /* 换了一本书（或上次没打开成功）：先关掉旧的，再开新的 */
    PdfRenderClose();
    s_pdfOpenedPath.clear();

    std::wstring err;
    if (!PdfRenderOpen(g_book.path, err)) return false;
    s_pdfOpenedPath = g_book.path;
    return true;
}

/* 字符偏移 → 页号。取"最后一个起点 ≤ off 的页"，因为页起点数组是单调递增的。 */
static int PdfPageFromOffset(int off)
{
    const std::vector<uint32_t>& offs = PdfLastPageOffsets();
    if (offs.empty()) return 0;
    int page = 0;
    for (size_t i = 0; i < offs.size(); i++) {
        if ((long long)offs[i] <= (long long)off) page = (int)i;
        else break;
    }
    return page;
}

/* 让"当前页"跟上文字模式下的阅读位置（切到原始版式、跳转章节/搜索命中时用）。 */
static void PdfSyncPageFromAnchor()
{
    if (!s_pdfMode) return;
    s_pdfPage = PdfPageFromOffset(ViewAnchorOffset());
    s_pdfScrollY = 0;
}

/* 画一页 PDF：整页按窗口宽度缩放，纵向用 s_pdfScrollY 滚动。 */
static void DrawPdfPage(int W, int H)
{
    const Theme& th = CurTheme();
    int total = PdfRenderPageCount();
    if (total <= 0) {
        const wchar_t* msg = L"这份 PDF 无法渲染成页面";
        SelectObject(s_dc, s_fontN);
        SetTextColor(s_dc, th.dim);
        ExtTextOutW(s_dc, s_padL, s_padT, 0, nullptr, msg, (int)wcslen(msg), nullptr);
        return;
    }
    if (s_pdfPage < 0) s_pdfPage = 0;
    if (s_pdfPage > total - 1) s_pdfPage = total - 1;

    int availW = W - s_padL * 2;
    if (availW < S(120)) availW = S(120);

    /* 渲染位图比显示宽度大一倍（内部 2 倍超采样），缩放后文字才不糊 */
    HBITMAP hbm = PdfRenderPageBitmap(s_pdfPage, availW);
    if (!hbm) {
        std::wstring err = L"页面渲染失败：";
        err += PdfRenderLastError();
        SelectObject(s_dc, s_fontN);
        SetTextColor(s_dc, th.dim);
        ExtTextOutW(s_dc, s_padL, s_padT, 0, nullptr, err.c_str(), (int)err.size(), nullptr);
        return;
    }

    BITMAP bm;
    ZeroMemory(&bm, sizeof(bm));
    GetObjectW(hbm, sizeof(bm), &bm);
    int srcW = bm.bmWidth > 0 ? bm.bmWidth : 1;
    int srcH = bm.bmHeight > 0 ? bm.bmHeight : 1;

    /* 显示尺寸：整页完整放进可视区（宽、高两个方向取更小的那个缩放比）。
     * 只按宽度铺满的话，A4 竖版页在宽屏上会比窗口高出一截，页底要滚动才看得到，
     * 而"看 PDF"的心理预期是"一屏一页"。等比缩放 + 水平居中，页面完整可见。 */
    int availH = H - s_padT * 2;
    if (availH < S(120)) availH = S(120);
    int dw = availW;
    int dh = (int)((long long)srcH * dw / srcW);
    if (dh > availH) {
        dh = availH;
        dw = (int)((long long)srcW * dh / srcH);
        if (dw < 1) dw = 1;
    }

    /* 页内滚动范围：整页已经被完整缩放进来，正常情况不需要滚；
     * 只有窗口被拉得极扁（availH 极小）时才有余量，逻辑保留以防万一。 */
    int maxScroll = dh - (H - s_padT * 2);
    if (maxScroll < 0) maxScroll = 0;
    if (s_pdfScrollY > maxScroll) s_pdfScrollY = maxScroll;
    if (s_pdfScrollY < 0) s_pdfScrollY = 0;

    /* 等比缩放后左右留白，页面居中更好看 */
    int dx = s_padL + (availW - dw) / 2;

    HDC mem = CreateCompatibleDC(s_dc);
    if (mem) {
        HGDIOBJ old = SelectObject(mem, hbm);
        SetStretchBltMode(s_dc, HALFTONE);
        SetBrushOrgEx(s_dc, 0, 0, nullptr);
        /* 位图右侧/底部可能露出的底色先补一下，避免 StretchBlt 边缘出现黑边 */
        StretchBlt(s_dc, dx, s_padT - s_pdfScrollY, dw, dh,
                   mem, 0, 0, srcW, srcH, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    /* 注意：这里**不能** DeleteObject(hbm)。
     * 这张位图归 pdfrender 的页面缓存所有，下一次重绘（滚动、改窗口大小、
     * 甚至只是刷新光标）还会再取一次；在这里删掉的话缓存里就留下一个野句柄，
     * 下一帧 GetObjectW 取不到尺寸、StretchBlt 也就画不出东西 ——
     * 表现正是"翻页后整块区域变成纯背景色，只剩右下角页码"。 */

    /* 右下角画一小行"第 n/N 页"，不依赖状态栏也能看清页码 */
    wchar_t pg[64];
    wsprintfW(pg, L"第 %d / %d 页", s_pdfPage + 1, total);
    SelectObject(s_dc, s_fontN);
    SetTextColor(s_dc, th.dim);
    SIZE sz;
    sz.cx = sz.cy = 0;
    GetTextExtentPoint32W(s_dc, pg, (int)wcslen(pg), &sz);
    ExtTextOutW(s_dc, W - sz.cx - s_padL, H - sz.cy - S(4), 0, nullptr, pg, (int)wcslen(pg), nullptr);
}

/* 处理 WM_PAINT：整屏重绘。流程见文件头"一、绘制数据流"。
   · 开头就把三样东西对齐：DC、字体、排版宽度（EnsureDC / EnsureFonts /
     UpdateMetrics）—— 任一样不对，画出来就是错的（行数、行高、宽度）。
   · WM_ERASEBKGND 返回 1，背景由这里的 FillRect 负责，避免系统先擦一次白底。
   · 位图只在客户区尺寸变化时重建（EnsureBackBuffer 内部判断），尺寸没变就复用。
   · 从锚点开始画，y 越过 H 立即 break —— 这就是"只画可见部分"的实现方式。
   · 任何一步失败（拿不到 DC 或位图）都要 EndPaint 再返回，否则窗口 DC 泄漏；无书时
     画欢迎页并立即呈现，同样要走 EndPaint。 */
static void PaintAll()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(s_wnd, &ps);
    RECT rc;
    GetClientRect(s_wnd, &rc);
    int W = rc.right, H = rc.bottom;
    /* 零尺寸（窗口最小化、或刚创建还没布局）：Begin/End 必须配对，直接返回，别去
       CreateCompatibleBitmap(0,0)。 */
    if (W <= 0 || H <= 0) { EndPaint(s_wnd, &ps); return; }

    EnsureDC(hdc);
    EnsureFonts();
    /* layoutDirty = false：绘制本身不会改变换行规则，只让
       UpdateMetrics 在"宽度和上次不一样"时自己作废缓存（用户可能刚拖过窗口边缘）。 */
    UpdateMetrics(false);
    EnsureBackBuffer(hdc, W, H);
    if (!s_dc || !s_bmp) { EndPaint(s_wnd, &ps); return; }

    const Theme& th = CurTheme();
    /* 一次性刷满客户区背景：整块都是不透明色，所以后面文字可以用 TRANSPARENT 背景模式（ClearType
       也需要不透明底）。 */
    HBRUSH bg = CreateSolidBrush(th.bg);
    FillRect(s_dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(s_dc, TRANSPARENT);

    if (!g_book.Ok()) {
        /* 首页：有书架记录就画书架，没有就画三行引导语（DrawHome 内部自己分派） */
        DrawHome(W, H);
        /* 双缓冲的一次性呈现：把整幅画面从内存 DC 拷到窗口 DC。 */
        BitBlt(hdc, 0, 0, W, H, s_dc, 0, 0, SRCCOPY);
        EndPaint(s_wnd, &ps);
        return;
    }

    /* PDF「原始版式」模式：整页位图直接贴上去，文字重排那条路整段跳过 */
    if (s_pdfMode && PdfRenderOk()) {
        DrawPdfPage(W, H);
        BitBlt(hdc, 0, 0, W, H, s_dc, 0, 0, SRCCOPY);
        EndPaint(s_wnd, &ps);
        return;
    }

    ClampAnchor();
    int x = s_padL;
    int y = s_padT;
    int p = s_aPara, line = s_aLine;
    std::vector<int> st;
    int guard = 0;
    /* 从锚点开始逐段逐行画。guard 6000：正常情况下画满一屏最多几十段就 break
       了，这个上限只是防数据异常时死循环。 */
    while (p < g_book.ParaCount() && y < H && guard++ < 6000) {
        bool title = BookIsChapterPara(g_book, p);
        SelectObject(s_dc, title ? s_fontB : s_fontN);
        SetTextColor(s_dc, title ? th.title : th.fg);
        LayoutPara(p, st);
        int nl = (int)st.size() - 1;
        /* 空段落：不画内容，但要占一点垂直空间，否则连续空行会被压成 0 高度，和 LinesInView
           里算出来的行数对不上（翻页会重复/漏行）。 */
        if (nl <= 0) {
            /* 只有"不是视口顶行"时才加这半行高：否则一打开书，顶行如果正好是空段，就会凭空多出半行空白，看起来像顶
               部边距变大了。 */
            if (p > s_aPara || line > 0) y += s_lineH / 2;
            p++; line = 0;
            continue;
        }
        /* 兜底：重排后行数变少时 line 可能越界（ClampAnchor 已经夹过一次，这里防
           的是绘制过程中数据被改动的情况）。 */
        if (line >= nl) line = nl - 1;
        /* 章标题上方额外留白，只在标题的第 0 行可见时加一次。 */
        if (title && line == 0) y += s_titleGap;
        /* 书签标记：这一段上有书签，就在正文左边缘画一道短竖条（用标题色），
           用户在正文里一眼就能看到"我在这儿标过"。用二分查 g_bmParas —— 一屏最多几十段，
           代价可忽略；g_bmParas 由 ui.cpp 在打开书、增删书签时重建。 */
        if (!g_bmParas.empty() && std::binary_search(g_bmParas.begin(), g_bmParas.end(), p)) {
            RECT rb;
            rb.left   = x - S(15);
            rb.right  = rb.left + S(4);
            rb.top    = y + S(3);
            rb.bottom = rb.top + s_lineH - S(6);
            if (rb.bottom < rb.top + S(4)) rb.bottom = rb.top + S(4);
            HBRUSH br = CreateSolidBrush(th.title);
            FillRect(s_dc, &rb, br);
            DeleteObject(br);
        }
        for (int l = line; l < nl; l++) {
            /* 一屏画满就停：后面几十万字一概不碰，这是自绘排版能扛大文件的关键。 */
            if (y >= H) break;
            DrawRun(x, y, st[(size_t)l], st[(size_t)l + 1]);
            y += s_lineH;
        }
        if (y >= H) break;
        y += s_paraGap;
        p++; line = 0;
    }
    BitBlt(hdc, 0, 0, W, H, s_dc, 0, 0, SRCCOPY);
    EndPaint(s_wnd, &ps);
}

/* ---------------- 命中测试 ---------------- */

/* 屏幕坐标 -> 字符偏移（鼠标选择、双击选段的基础）。
   参数：mx / my 是客户区坐标；返回：命中的字符偏移。点在正文之外（下方空白、右侧
   空白）时返回全文长度，这样拖选能一路拖到末尾；点在行首左侧时返回该行首字符。
   实现注意：必须和 PaintAll 用完全相同的"逐段逐行 + y 累加"规则，包括 s_titleGap、
   s_paraGap、空段半行高，甚至连那个 `if (p > s_aPara || line > 0)` 的顶行特判都要
   一致。两处一旦不同，点到的字就会整体偏移 —— 这也是这段代码看起来那么像 PaintAll
   的原因，改动时务必两处一起改。guard 6000 与 PaintAll 保持同样的上限。 */
static int HitTestChar(int mx, int my)
{
    if (!g_book.Ok()) return 0;
    EnsureFonts();
    int x = s_padL;
    int y = s_padT;
    int p = s_aPara, line = s_aLine;
    std::vector<int> st;
    int guard = 0;
    while (p < g_book.ParaCount() && guard++ < 6000) {
        bool title = BookIsChapterPara(g_book, p);
        SelectObject(s_dc, title ? s_fontB : s_fontN);
        LayoutPara(p, st);
        int nl = (int)st.size() - 1;
        if (nl <= 0) { y += s_lineH / 2; p++; line = 0; continue; }
        if (line >= nl) line = nl - 1;
        if (title && line == 0) y += s_titleGap;
        for (int l = line; l < nl; l++) {
            /* 鼠标落在这一行的垂直范围内：行顶是 y，行高 s_lineH。注意这里不再判 y < H ——
               鼠标在窗口下方空白处时，最后一次循环会自然把"全文长度"返回出去。 */
            if (my < y + s_lineH) {
                int ls = st[(size_t)l], le = st[(size_t)l + 1];
                int maxW = mx - x;
                /* 点在行首左边（含负坐标）：算作行首。 */
                if (maxW <= 0) return ls;
                int nFit = 0;
                SIZE sz; sz.cx = sz.cy = 0;
                /* 借用和排版同一个 API，问"这一行里、x 像素之内能放下几个字"。比按字数乘平均宽度准确得多（中英混排时字
                   宽差别很大）。 */
                GetTextExtentExPointW(s_dc, g_book.text.c_str() + ls, le - ls, maxW, &nFit, nullptr, &sz);
                /* 夹住 nFit：GetTextExtentExPointW 在宽度足够时会把 nFit 设成整段长度，理论上不会超过
                   le-ls，但夹一下更安全。 */
                return ls + (nFit > le - ls ? le - ls : nFit);
            }
            y += s_lineH;
        }
        y += s_paraGap;
        p++; line = 0;
    }
    /* 一路扫到书尾都没命中：返回全文长度，拖选时能选到最后一个字。 */
    return (int)g_book.text.size();
}

/* ---------------- 窗口过程 ---------------- */

/* 视图窗口的窗口过程。
   消息分工：WM_CREATE 记句柄、建内存 DC；WM_SIZE 客户区宽度变了要重排版
   （UpdateMetrics + 夹锚点 + 刷滚动条）；WM_ERASEBKGND 返回 1（背景由 PaintAll 填，
   杜绝白闪）；WM_PAINT 整屏重绘；WM_SETFOCUS / KILLFOCUS 重绘一次；WM_MOUSEWHEEL
   里 Ctrl+滚轮 = 调字号，否则滚 3 行；WM_VSCROLL 处理 SB_* 各通知码，拖动滑块走
   "偏移反查段落"；WM_KEYDOWN 处理快捷键与翻行 / 翻页 / 翻章；鼠标三条消息组成拖选，
   双击选整段。
   设计说明：本控件是"无焦点外观"的自绘控件，所有状态都在文件级 static 里，所以窗口
   过程本身很薄、只做分派，真正的逻辑都在上面的 View* 函数里 —— 这样 ui.cpp 能直接
   调用同一批函数（菜单、快捷键、目录点击都走它们）。空指针防御：收到这些消息时
   s_wnd 一定已被赋值（WM_CREATE 里设的），内部函数直接用 s_wnd，不再逐个判空。 */
static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        /* 窗口刚建好：记下自己的句柄（后面所有 GDI 操作和滚动条都要用），并建一个内存 DC。字体不在这里建 ——
           建字体依赖 g_dpi 和 g_set，而它们可能比窗口创建更晚初始化，交给 EnsureFonts 惰性创建更安全。 */
        s_wnd = h;
        EnsureDC(nullptr);
        return 0;

    case WM_SIZE:
        /* 客户区尺寸变了：可用宽度跟着变 -> 所有段落的行数缓存作废（layoutDirty= true）。UpdateMetrics
           内部会顺带 RestoreAnchorFromOffset，把锚点从字符偏移还原到新排版下的行，这样缩放窗口时阅读位置不会跳
           走。 */
        UpdateMetrics(true);
        ClampAnchor();
        UpdateScrollBar();
        InvalidateAll();
        return 0;

    case WM_ERASEBKGND:
        /* 直接吃掉"擦背景"：PaintAll 一开头就用主题色填满整块区域，让系统再擦一遍只会白闪一下（顺便省一次全窗
           口填充）。 */
        return 1;

    case WM_SETCURSOR:
        /* 首页书架：鼠标停在"能点的书行"上时换手形光标，暗示这里可以点。
         * 只换首页且确实命中一行的情况，其它一律交给 DefWindowProc —— 正文区是
         * 工字形光标（注册窗口类时设的 IDC_IBEAM），那种"可选字"的暗示要保持。 */
        if (!g_book.Ok() && LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            int idx = HomeRowAt(pt.y);
            const std::vector<ShelfItem>& items = ShelfItems();
            if (idx >= 0 && idx < (int)items.size() && !items[(size_t)idx].missing) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_PAINT:
        /* BeginPaint / EndPaint 必须成对，只能在 PaintAll 里做；PaintAll 保证每条分支都会 EndPaint。 */
        PaintAll();
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        /* 焦点变化时重绘一次：选中高亮用的是主题色，失焦后是否需要区别外观交给主题决定，这里保守地刷一遍，保证
           视觉效果与状态一致。 */
        InvalidateAll();
        return 0;

    case WM_MOUSEWHEEL: {
        short delta = GET_WHEEL_DELTA_WPARAM(w);
        /* Ctrl + 滚轮 = 调字号，这是阅读器里最常用的手势；注意这里 return 0 而不是走下面的滚动逻辑 ——
           调字号本身就会触发重排版和重绘（UiSetFontDelta 内部会调 ViewMetricsChanged）。 */
        if (GetKeyState(VK_CONTROL) < 0) {
            if (delta > 0) UiSetFontDelta(1); else UiSetFontDelta(-1);
            return 0;
        }
        UiAutoStop();                       /* 手动拨轮时停下自动下滑 */
        /* PDF 原始版式：滚轮优先在页内上下移动；已经滚到底（或整页装得下）才翻页。
           这样"整页比窗口高"的 PDF 也能顺畅看完全文，而不是一格就跳过一页。 */
        if (s_pdfMode) {
            int step = (int)(delta / WHEEL_DELTA) * S(60);
            /* 判断是否还能在页内继续滚：滚不动且方向向下 → 翻下一页 */
            int before = s_pdfScrollY;
            ViewPdfScroll(-step);
            if (s_pdfScrollY == before) ViewPdfFlip(step > 0 ? -1 : 1);
            return 0;
        }
        /* 一次滚动 delta / WHEEL_DELTA 个"格"，每格 3 行 —— 比滚 1
           行更符合"滚一格走一小段"的直觉。符号要取负：滚轮往上是 delta > 0，而 ViewScrollLines 里正数是向下。 */
        int lines = (int)(delta / WHEEL_DELTA) * 3;
        ViewScrollLines(-lines);
        return 0;
    }

    case WM_VSCROLL: {
        /* 没有书时忽略：否则 ViewScrollLines 会被空书拒掉，但 SB_THUMBTRACK 里的 GetScrollInfo 仍然白跑一趟。 */
        if (!g_book.Ok()) return 0;
        UiAutoStop();                       /* 手动拖动滚动条时停下自动下滑 */
        int code = LOWORD(w);
        /* PDF 原始版式：滚动条一格 = 一页 */
        if (s_pdfMode) {
            switch (code) {
            case SB_LINEUP:   case SB_PAGEUP:   ViewPdfFlip(-1); break;
            case SB_LINEDOWN: case SB_PAGEDOWN: ViewPdfFlip(1);  break;
            case SB_TOP:      ViewPdfFlip(-ViewPdfPageCount()); break;
            case SB_BOTTOM:   ViewPdfFlip(ViewPdfPageCount());  break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si; ZeroMemory(&si, sizeof(si));
                si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
                GetScrollInfo(h, SB_VERT, &si);
                ViewPdfFlip(si.nTrackPos - s_pdfPage);
                break;
            }
            default: break;
            }
            return 0;
        }
        switch (code) {
        case SB_LINEUP:   ViewScrollLines(-1); break;
        case SB_LINEDOWN: ViewScrollLines(1); break;
        case SB_PAGEUP:   ViewScrollPages(-1); break;
        case SB_PAGEDOWN: ViewScrollPages(1); break;
        /* 首 / 尾：直接按字符偏移跳，比"翻到不能再翻"的次数少得多。 */
        case SB_TOP:      ViewGotoOffset(0, 0); break;
        case SB_BOTTOM:   ViewGotoOffset((int)g_book.text.size() - 1, 0); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            /* 必须用 GetScrollInfo(SIF_TRACKPOS) 取拖动中的实时位置：wParam 高 16 位只有 16 位宽，字符数超过
               65535 的长篇就会截断，而且 SIF_TRACKPOS 给的才是"用户正在拖到的位置"。 */
            SCROLLINFO si; ZeroMemory(&si, sizeof(si));
            si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &si);
            /* 这里的 nTrackPos 是字符偏移，直接交给 ViewGotoOffset 反查段落
               ——这正是"滚动条用字符偏移"这个设计带来的便利（见文件头第四节）。 */
            ViewGotoOffset(si.nTrackPos, 0);
            break;
        }
        default: break;
        }
        return 0;
    }

    case WM_KEYDOWN: {
        /* 只在空格翻页里用到 Shift（Shift+空格 = 往回翻一页），先取出来。 */
        bool shift = (GetKeyState(VK_SHIFT) < 0);
        /* Ctrl 组合键优先处理：复制 / 全选 / 调字号。这些操作各有自己的收尾逻辑
           （复制不动视图、调字号会走 ViewMetricsChanged），处理完直接 return 0，
           不再往下走滚动分支。 */
        if (GetKeyState(VK_CONTROL) < 0) {
            switch (w) {
            case 'C': ViewCopySelection(); return 0;
            case 'A': ViewSelectAll(); return 0;
            case VK_OEM_PLUS: case VK_ADD:      UiSetFontDelta(1);  return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT: UiSetFontDelta(-1); return 0;
            default: break;
            }
        }
        /* 键盘翻页/跳转也停下自动下滑（不含 F6/F7/F8，那三个由快捷键表处理） */
        /* 分两段 switch：这一段只负责"停下自动下滑"，下一段才真正执行动作。好处是
           "用户手动操作就停止自动下滑"这条规则只写一次，不必在每个按键分支里重复，
           也就不会漏掉某个键。 */
        switch (w) {
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
        case VK_HOME: case VK_END: case VK_LEFT: case VK_RIGHT:
        case VK_SPACE:
            UiAutoStop();
            break;
        default: break;
        }
        switch (w) {
        case VK_UP:    if (s_pdfMode) { ViewPdfScroll(-S(56)); return 0; } ViewScrollLines(-1); return 0;
        case VK_DOWN:  if (s_pdfMode) { ViewPdfScroll(S(56)); return 0; } ViewScrollLines(1); return 0;
        case VK_PRIOR: if (s_pdfMode) { ViewPdfFlip(-1); return 0; } ViewScrollPages(-1); return 0;
        case VK_NEXT:  if (s_pdfMode) { ViewPdfFlip(1); return 0; } ViewScrollPages(1); return 0;
        case VK_HOME:  ViewGotoOffset(0, 0); return 0;
        case VK_END:   ViewGotoOffset((int)g_book.text.size() - 1, 0); return 0;
        /* ← / → 是"翻章"而不是"移动光标"：这是阅读器，没有光标。 */
        case VK_LEFT:  ViewGotoChapter(-1); return 0;
        case VK_RIGHT: ViewGotoChapter(1); return 0;
        /* 空格翻页（Shift 反向）：单手翻页比 PgDn 顺手。PDF 原始版式下按页翻。 */
        case VK_SPACE:
            if (s_pdfMode) ViewPdfFlip(shift ? -1 : 1);
            else ViewScrollPages(shift ? -1 : 1);
            return 0;
        /* Esc 只清选区，不关窗口（关窗口是 Alt+F4 / 标题栏的事）。 */
        case VK_ESCAPE: s_selA = s_selB = -1; InvalidateAll(); return 0;
        /* 吞掉 Tab：不让它把焦点跳到别的控件上，保证阅读时焦点稳定留在正文里，方向键和空格一直可用。 */
        case VK_TAB: return 0;
        default: break;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        /* 按下左键开始拖选：定位起点，把 A/B 都设成起点（此时选区长度为 0，绘制时不会显示高亮）。SetCapture 很
           关键：捕获鼠标后即使指针移到窗口外面，本窗口依然能收到 WM_MOUSEMOVE /WM_LBUTTONUP，拖选才不会中断。 */
        SetFocus(h);
        UiAutoStop();
        /* 首页书架：没打开书时点一行就是"打开这本书"。
         * 这里不进入拖选流程 —— 首页没有正文，选字毫无意义，直接返回更干净。
         * 文件已经不存在的行（missing）点了没有反应，视觉上也已经灰显过。 */
        if (!g_book.Ok()) {
            int idx = HomeRowAt(GET_Y_LPARAM(l));
            const std::vector<ShelfItem>& items = ShelfItems();
            if (idx >= 0 && idx < (int)items.size() && !items[(size_t)idx].missing)
                UiOpenFile(items[(size_t)idx].path);
            return 0;
        }
        int ch = HitTestChar(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        s_dragChar = ch;
        s_selA = s_selB = ch;
        s_dragging = true;
        SetCapture(h);
        InvalidateAll();
        return 0;
    }

    case WM_MOUSEMOVE: {
        /* 首页书架：悬停高亮。必须放在"拖选状态"判断之前 —— 首页永远没有拖选，
         * 若先判 s_dragging 就直接 return 了，悬停效果永远不出现。 */
        if (!g_book.Ok()) {
            int idx = HomeRowAt(GET_Y_LPARAM(l));
            if (idx != s_homeHover) { s_homeHover = idx; InvalidateAll(); }
            /* 请求一次"鼠标离开窗口"的通知：系统只发一次，所以每次进入都要重新登记 */
            if (!s_trackLeave) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = h;
                tme.dwHoverTime = 0;
                if (TrackMouseEvent(&tme)) s_trackLeave = true;
            }
            return 0;
        }
        /* 没在拖动就不理：鼠标单纯路过窗口不应该改选区。 */
        if (!s_dragging) return 0;
        int ch = HitTestChar(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        /* 防御：起点因为某种原因丢了（例如换书后没复位），用当前点补上，否则下面排序会拿 -1
           去比较，选区直接错乱。 */
        if (s_dragChar < 0) s_dragChar = ch;
        /* 选中端点次序：永远把"较小的偏移"放进 s_selA、"较大的"放进 s_selB，保证 A <
           B。这样无论用户往上拖还是往下拖，区间表示都一致，绘制和复制也不用再判方向。 */
        s_selA = s_dragChar < ch ? s_dragChar : ch;
        s_selB = s_dragChar < ch ? ch : s_dragChar;
        InvalidateAll();
        return 0;
    }

    /* 鼠标离开视图：清掉书架悬停行，否则那一行会一直保持高亮（看起来像卡住了） */
    case WM_MOUSELEAVE:
        s_trackLeave = false;
        if (s_homeHover != -1) { s_homeHover = -1; InvalidateAll(); }
        return 0;

    case WM_LBUTTONUP: {
        /* 松开左键：结束拖动并释放鼠标捕获（不释放的话后续鼠标消息会一直被本窗口吞掉，别的窗口点不动）。 */
        if (s_dragging) {
            s_dragging = false;
            ReleaseCapture();
            /* 只是点了一下没拖动（A == B）：当作"点空白处取消选区"。否则会留下一个长度为 0
               的假选区，用户既看不见也不好取消。 */
            if (s_selA == s_selB) { s_selA = s_selB = -1; InvalidateAll(); }
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        /* 双击选整段：命中字符 -> 段落 -> 用 paraOff 的段落区间 [a, b) 作选区。段尾的 '\n'
           要像排版时那样去掉，否则复制出来的文字末尾会多一个换行，粘到一行文本框里就会多出空行。 */
        int ch = HitTestChar(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        int p = BookCharToPara(g_book, ch);
        int a = (int)g_book.paraOff[(size_t)p];
        int b = (int)g_book.paraOff[(size_t)p + 1];
        if (b > a && g_book.text[(size_t)b - 1] == L'\n') b--;
        s_selA = a; s_selB = b;
        InvalidateAll();
        return 0;
    }

    default:
        break;
    }
    /* 未处理的消息交回系统默认处理（WM_DESTROY / WM_NCCREATE 之类必须走这里，否则子窗口创建会失败）。 */
    return DefWindowProcW(h, m, w, l);
}

/* 创建阅读视图子窗口。参数 parent：父窗口（主窗口）；返回：新建的子窗口句柄，同时
   写进 s_wnd，供后续所有视图函数使用。
   三个注册细节：· wc.style 必须带 CS_DBLCLKS，否则系统不会把第二次按下转成
   WM_LBUTTONDBLCLK，"双击选整段"就永远收不到消息；· hbrBackground 给 nullptr：背景
   完全由 PaintAll 自绘，让系统再刷一遍只会闪；· 类只注册一次（static bool reg），虽然
   本程序只创建一个视图，保留这个惯用法，将来要开多个阅读窗口也不用改这里。
   窗口样式：WS_VSCROLL 让系统画垂直滚动条并自动发 WM_VSCROLL；WS_TABSTOP 让它能通过
   Tab 拿到焦点（ViewFocus 的效果）。子窗口 ID 用 (HMENU)(INT_PTR)IDC_VIEW 传入：子
   窗口没有菜单句柄，这个位置被复用成控件 ID，主窗口靠 GetDlgItem(IDC_VIEW) 找到它。 */
HWND ViewCreate(HWND parent)
{
    static bool reg = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    if (!reg) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = inst;
        /* 工字形光标：向用户暗示"这里的文字可以被选中"。 */
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        wc.lpszClassName = L"NRView";
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
        reg = true;
    }
    s_wnd = CreateWindowExW(0, L"NRView", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                            0, 0, 100, 100, parent, (HMENU)(INT_PTR)IDC_VIEW, inst, nullptr);
    return s_wnd;
}
