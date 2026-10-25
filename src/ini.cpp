/* ini.cpp - 配置、阅读进度、最近打开的持久化
 *
 * 所有"下次打开还要记得"的东西都写在这个文件里，落盘位置是：
 *   1) 优先 exe 同目录的 NovelReader.ini（绿色软件的做法，拷走整个文件夹就能带走进度）
 *   2) 那里不可写（比如放在 C:\Program Files）就退回 %APPDATA%\NovelReader\NovelReader.ini
 *
 * 文件内容大致长这样（中文路径会因为写了 BOM 而保存为 UTF-16LE）：
 *   [settings]
 *   fontPt=17  theme=0  sidebar=1  spacing=1  autoSpeed=4
 *   lastFile=D:\小说\某某.txt  winL=153 winT=36 winR=1213 winB=691
 *   [recent]
 *   r0=D:\小说\某某.txt  r1=...
 *   [fd9ade76a]              ← 一本书一个段，段名 = "f" + 路径哈希
 *   off=1349                 ← 读到第几个字符
 *   path=D:\小说\某某.txt    ← 真实路径（只为方便人肉查看）
 *
 * 实现上直接用了 Win32 的私有的 ini API（Get/WritePrivateProfileString），
 * 好处是不用自己解析文本；坑在于它对"已存在但大小为 0 的文件"会按 ANSI 写，
 * 所以这里自己先把 BOM 塞进去（见 EnsureUnicodeIni）。
 */
#include "common.h"
#include <shlobj.h>
#include <cwchar>

/* 全局唯一的设置对象，界面各处直接读写 g_set，改动后由 SettingsSave 落盘 */
Settings g_set;

/* 当前 ini 的完整路径，IniInit 决定，之后只读 */
static std::wstring s_iniPath;

/* 取 exe 所在目录（不带结尾反斜杠）。用 GetModuleFileNameW 而不是
 * 当前工作目录，因为用户可能从资源管理器/命令行/快捷方式以不同 cwd 启动。 */
static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    if (k != std::wstring::npos) s = s.substr(0, k);
    return s;
}

/* 确保 ini 文件是 Unicode 的。
 *
 * 为什么需要这么一步（踩过的坑）：
 *   WritePrivateProfileStringW 的行为是"文件不存在就新建一个 ANSI 文件"，
 *   而且判断依据是文件大小——空文件依然被当成 ANSI。于是第一次运行时，
 *   中文路径/书名会被按系统代码页（GBK）写进去，读回来就乱码。
 *   这里在文件为空时先写一个 UTF-16LE BOM（0xFEFF），Windows 之后就会
 *   按 UTF-16 读写，中文路径就安全了。
 *
 * 返回值：能打开（说明这个目录可写）返回 true，否则 false（会退回 %APPDATA%）。 */
static bool EnsureUnicodeIni(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart == 0) {
        const wchar_t bom = 0xFEFF;
        DWORD wrote = 0;
        WriteFile(h, &bom, sizeof(bom), &wrote, nullptr);
    }
    CloseHandle(h);
    return true;
}

/* 程序启动时调用一次：定下 ini 路径。必须在任何 IniXxx / SettingsXxx 调用之前执行。 */
void IniInit()
{
    std::wstring dir = ExeDir();
    std::wstring p = dir + L"\\NovelReader.ini";
    if (EnsureUnicodeIni(p)) { s_iniPath = p; return; }   /* 首选：exe 同目录 */

    /* 备选：%APPDATA%\NovelReader\NovelReader.ini（普通用户一定有写权限） */
    wchar_t appdata[MAX_PATH * 2] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring d = std::wstring(appdata) + L"\\NovelReader";
        CreateDirectoryW(d.c_str(), nullptr);   /* 已存在会返回 FALSE，无所谓 */
        s_iniPath = d + L"\\NovelReader.ini";
        EnsureUnicodeIni(s_iniPath);
    } else {
        s_iniPath = p;   /* 实在不行就用原路径，至少程序不会崩 */
    }
}

/* 当前 ini 路径，"关于"对话框里展示给用户 */
std::wstring IniFilePath()
{
    return s_iniPath;
}

/* 读字符串键，没有就用 def。缓冲区 2048 字符，足够放长路径；
 * 超长会被 API 截断（私有的 ini API 就是这么设计的）。 */
std::wstring IniGetStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[2048] = { 0 };
    GetPrivateProfileStringW(sec, key, def ? def : L"", buf, 2048, s_iniPath.c_str());
    return std::wstring(buf);
}

/* 写字符串键。注意：传空串等于删除这个键。 */
void IniSetStr(const wchar_t* sec, const wchar_t* key, const std::wstring& val)
{
    WritePrivateProfileStringW(sec, key, val.c_str(), s_iniPath.c_str());
}

/* 读整数键，没有就用 def */
int IniGetInt(const wchar_t* sec, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, s_iniPath.c_str());
}

/* 写整数键（私有的 ini API 只收字符串，所以这里自己格式化） */
void IniSetInt(const wchar_t* sec, const wchar_t* key, int v)
{
    wchar_t buf[32];
    wsprintfW(buf, L"%d", v);
    WritePrivateProfileStringW(sec, key, buf, s_iniPath.c_str());
}

/* 读 [settings] 段。
 * 每一项都做了范围夹取——ini 是用户可以手改的文本文件，
 * 万一被改坏了（比如 fontPt=9999），这里要能兜住，不能让程序排版炸掉。 */
void SettingsLoad()
{
    const wchar_t* S0 = L"settings";
    g_set.fontPt  = IniGetInt(S0, L"fontPt", 17);
    if (g_set.fontPt < 9)  g_set.fontPt = 9;      /* 下限 9 磅：再小就看不清了 */
    if (g_set.fontPt > 42) g_set.fontPt = 42;     /* 上限 42 磅：再大一行放不下几个字 */
    g_set.theme   = IniGetInt(S0, L"theme", 0);   /* 0 日间 1 夜间 */
    g_set.sidebar = IniGetInt(S0, L"sidebar", 1); /* 侧栏默认显示 */
    g_set.spacing = IniGetInt(S0, L"spacing", 1); /* 行距档位 0/1/2 */
    if (g_set.spacing < 0) g_set.spacing = 0;
    if (g_set.spacing > 2) g_set.spacing = 2;
    g_set.autoSpeed = IniGetInt(S0, L"autoSpeed", 4);   /* 自动下滑速度 1~10 */
    if (g_set.autoSpeed < 1)  g_set.autoSpeed = 1;
    if (g_set.autoSpeed > 10) g_set.autoSpeed = 10;
    g_set.lastFile = IniGetStr(S0, L"lastFile", L"");   /* 上次打开的书 */

    /* ---- 外观：字体与自定义配色 ---- */
    g_set.theme = g_set.theme < 0 ? 0 : (g_set.theme > 2 ? 2 : g_set.theme);
    /* 字体名：空串在 view.cpp 里当作"用内置默认字体"，这里不强行填默认值 ——
     * 将来改了默认字体，老用户 ini 里的空串就会自动跟着变。 */
    g_set.fontFace  = IniGetStr(S0, L"fontFace", L"Microsoft YaHei");
    g_set.fontFiles = IniGetStr(S0, L"fontFiles", L"");
    /* 自定义配色：存的是 COLORREF 整数（0x00BBGGRR）。缺省值就是日间配色，
     * 这样第一次切到"自定义"时不会看到一片黑或一片白。 */
    g_set.cBg    = (COLORREF)(uint32_t)IniGetInt(S0, L"cBg",    (int)RGB(0xF7, 0xF3, 0xE9));
    g_set.cFg    = (COLORREF)(uint32_t)IniGetInt(S0, L"cFg",    (int)RGB(0x31, 0x2D, 0x27));
    g_set.cTitle = (COLORREF)(uint32_t)IniGetInt(S0, L"cTitle", (int)RGB(0x8A, 0x5A, 0x2B));
    g_set.cSel   = (COLORREF)(uint32_t)IniGetInt(S0, L"cSel",   (int)RGB(0xBF, 0xDC, 0xF4));
    g_set.cHov   = (COLORREF)(uint32_t)IniGetInt(S0, L"cHov",   (int)RGB(0xEC, 0xE6, 0xD8));

    /* 窗口位置：四个值缺一不可，而且要有意义的大小（right>left、bottom>top）。
     * 用 -100000 当"没有记录"的哨兵，比 0 更安全（多显示器可能出现负数坐标）。 */
    int l = IniGetInt(S0, L"winL", -100000);
    int t = IniGetInt(S0, L"winT", -100000);
    int r = IniGetInt(S0, L"winR", -100000);
    int b = IniGetInt(S0, L"winB", -100000);
    if (r > l && b > t) {
        g_set.winRect.left = l; g_set.winRect.top = t;
        g_set.winRect.right = r; g_set.winRect.bottom = b;
        g_set.hasRect = true;
    }
}

/* 写 [settings] 段。只在关窗口时调一次（外加首次启动失败时的兜底），
 * 所以不用考虑性能；窗口没记录过位置就不写那四个键。 */
void SettingsSave()
{
    const wchar_t* S0 = L"settings";
    IniSetInt(S0, L"fontPt", g_set.fontPt);
    IniSetInt(S0, L"theme", g_set.theme);
    IniSetInt(S0, L"sidebar", g_set.sidebar);
    IniSetInt(S0, L"spacing", g_set.spacing);
    IniSetInt(S0, L"autoSpeed", g_set.autoSpeed);
    IniSetStr(S0, L"lastFile", g_set.lastFile);
    /* 外观：字体名 / 导入的字体文件 / 自定义配色。
     * 每次关窗口都写，哪怕用户没改 —— 反正只有十几行文本，省得再维护"脏标记"。 */
    IniSetStr(S0, L"fontFace", g_set.fontFace);
    IniSetStr(S0, L"fontFiles", g_set.fontFiles);
    IniSetInt(S0, L"cBg",    (int)g_set.cBg);
    IniSetInt(S0, L"cFg",    (int)g_set.cFg);
    IniSetInt(S0, L"cTitle", (int)g_set.cTitle);
    IniSetInt(S0, L"cSel",   (int)g_set.cSel);
    IniSetInt(S0, L"cHov",   (int)g_set.cHov);
    if (g_set.hasRect) {
        IniSetInt(S0, L"winL", g_set.winRect.left);
        IniSetInt(S0, L"winT", g_set.winRect.top);
        IniSetInt(S0, L"winR", g_set.winRect.right);
        IniSetInt(S0, L"winB", g_set.winRect.bottom);
    }
}

/* ---------------- 阅读位置 ---------------- */

/* FNV-1a 哈希（32 位），把"书的完整路径"压成 8 位十六进制段名。
 * 为什么用哈希而不是路径本身：
 *   ini 的段名不能含 ] 和换行，中文长路径直接当段名既难看又有风险；
 * 为什么要先把 ASCII 大写转小写：
 *   Windows 路径大小写不敏感，同一条路径用户可能写成 D:\Book 或 d:\book，
 *   统一小写后两者才会命中同一个段（中文没有大小写，不受影响）。 */
static uint32_t Fnv1a(const std::wstring& s)
{
    uint32_t h = 2166136261u;                 /* FNV-1a 32 位的标准初始值 */
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        h ^= (uint32_t)(c & 0xFFFF);          /* 异或进一个字符（只取低 16 位） */
        h *= 16777619u;                       /* 乘 FNV 质数 */
    }
    return h;
}

/* 书路径 → ini 段名，形如 L"fd9ade76a"（1 个 f + 8 位十六进制）。
 * 前面那个 f 只是让段名一眼能看出是"某本书"，避免和 settings/recent 混淆。 */
std::wstring PosSectionFor(const std::wstring& path)
{
    wchar_t buf[64];
    wsprintfW(buf, L"f%08x", Fnv1a(path));
    return std::wstring(buf);
}

/* 读这本书上次读到的字符偏移。
 * 返回值：>=0 表示有记录（0 就是开头）；-1 表示没有记录（第一次读这本书）。 */
int PosLoad(const std::wstring& path)
{
    if (path.empty()) return -1;
    std::wstring sec = PosSectionFor(path);
    int off = IniGetInt(sec.c_str(), L"off", -1);
    return off;
}

/* 保存阅读进度。off 是"顶行第一个字符在全文中的下标"（不是行号、不是百分比），
 * 好处是换字号、改窗口大小之后依然能精确回到同一句话。
 *
 * 除了 off，还顺手记下章节号/章数/百分比/章名/书名。这几项**不是**给本次阅读用的，
 * 而是留给首页书架（见 common.h 里 ShelfItem 的注释）：首页在没加载任何书的情况下
 * 也要显示"读到第 3/120 章，3.2%"，只能靠 ini 里预先存好的这几项。
 * 老版本 ini 里没有这些键也没关系，读的时候缺省成 0/空串（见 ShelfRefresh）。
 */
void PosSaveMeta(const std::wstring& path, int off, const std::wstring& chname,
                 int ch, int chn, int pct)
{
    if (path.empty() || off < 0) return;
    std::wstring sec = PosSectionFor(path);
    IniSetInt(sec.c_str(), L"off", off);
    IniSetStr(sec.c_str(), L"path", path);              /* 真实路径，方便人肉看 ini */
    IniSetStr(sec.c_str(), L"chname", chname);          /* 章名，例如"第3章 三年之约" */
    IniSetInt(sec.c_str(), L"ch",  ch);
    IniSetInt(sec.c_str(), L"chn", chn);
    IniSetInt(sec.c_str(), L"pct", pct);                /* 千分数：1000 = 100.0% */

    /* 书名单独存一份：首页显示时不必再去解析路径（也避免路径里有各种怪字符）。
     * 只有 .txt 才把扩展名去掉：纯文本是默认格式，去掉更清爽；
     * .epub/.docx/.html/.md 则保留扩展名，因为"张三.epub"和"张三.txt"
     * 很可能是同一本书的两个版本，首页上得能分得出来。 */
    std::wstring title;
    size_t slash = path.find_last_of(L"\\/");
    title = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = title.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        std::wstring ext = title.substr(dot);
        for (size_t q = 0; q < ext.size(); q++) ext[q] = (wchar_t)towlower(ext[q]);
        if (ext == L".txt") title = title.substr(0, dot);
    }
    IniSetStr(sec.c_str(), L"title", title);
}

/* ---------------- 首页书架 ---------------- */

/* 书架列表本体。放在 ini.cpp 而不是 ui.cpp，是因为它完全由 ini 内容决定，
 * 和窗口/控件无关；ui.cpp 只管把它画出来。 */
static std::vector<ShelfItem> s_shelf;

/* 重新扫一遍 ini 重建书架列表。
 *
 * 数据来源：最近打开列表（[recent] 的 r0..r9）给出"读过哪些书、什么顺序"，
 * 每本书自己的段给出进度。所以书架天然是"按最近阅读时间倒序"的。
 * 两处细节：
 *   1) 文件已经被删/改名时标 missing，首页上灰掉且不给点（否则点了只会弹错误框）；
 *   2) 老 ini 没有 chname/title 键时，用"文件名去扩展名"当书名，章名留空。
 */
void ShelfRefresh()
{
    s_shelf.clear();
    for (int i = 0; i < IDM_RECENT_MAX; i++) {
        std::wstring p = RecentGet(i);
        if (p.empty()) continue;

        ShelfItem it;
        it.path = p;
        std::wstring sec = PosSectionFor(p);

        it.title = IniGetStr(sec.c_str(), L"title", L"");
        if (it.title.empty()) {
            /* 老记录没有 title 键：从路径里抠出文件名当书名（.txt 去扩展名，其它格式留着） */
            size_t slash = p.find_last_of(L"\\/");
            it.title = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
            size_t dot = it.title.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) {
                std::wstring ext = it.title.substr(dot);
                for (size_t q = 0; q < ext.size(); q++) ext[q] = (wchar_t)towlower(ext[q]);
                if (ext == L".txt") it.title = it.title.substr(0, dot);
            }
        }
        it.chapter = IniGetStr(sec.c_str(), L"chname", L"");
        it.ch   = IniGetInt(sec.c_str(), L"ch", 0);
        it.chn  = IniGetInt(sec.c_str(), L"chn", 0);
        it.pct  = IniGetInt(sec.c_str(), L"pct", -1);   /* -1 = 这本书从没存过进度 */
        it.missing = (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES);
        s_shelf.push_back(it);
    }
}

/* 给界面取书架内容用（只读引用，避免每次绘制都拷贝一堆字符串） */
const std::vector<ShelfItem>& ShelfItems()
{
    return s_shelf;
}

/* ---------------- 最近打开 ---------------- */

/* 取第 idx 条最近打开（0 是最新的）。idx 越界或没有记录返回空串。 */
std::wstring RecentGet(int idx)
{
    if (idx < 0 || idx >= IDM_RECENT_MAX) return L"";
    wchar_t key[16];
    wsprintfW(key, L"r%d", idx);
    return IniGetStr(L"recent", key, L"");
}

/* 把一本书插到最近打开列表的最前面，然后整体重写 [recent] 段。
 *
 * 去重规则：同一路径（大小写不敏感）只保留一条，并且被重新打开过的书
 * 会被"提到最前面"，这样菜单里的顺序永远是最近使用过的在前面。
 * 列表满 10 条就把最旧的挤掉。 */
void RecentAdd(const std::wstring& path)
{
    if (path.empty()) return;

    std::vector<std::wstring> list;
    list.push_back(path);                       /* 新的排第一 */
    for (int i = 0; i < IDM_RECENT_MAX; i++) {
        std::wstring p = RecentGet(i);
        if (p.empty()) continue;                /* 空位跳过，列表自然就短了 */
        bool dup = false;
        for (size_t k = 0; k < list.size(); k++) {
            if (lstrcmpiW(list[k].c_str(), p.c_str()) == 0) { dup = true; break; }
        }
        if (!dup) list.push_back(p);
        if ((int)list.size() >= IDM_RECENT_MAX) break;
    }

    /* 整体重写 r0..r9：多出来的位置写空串（等于删除该键），
     * 这样列表变短时不会残留旧记录 */
    for (int i = 0; i < IDM_RECENT_MAX; i++) {
        wchar_t key[16];
        wsprintfW(key, L"r%d", i);
        IniSetStr(L"recent", key, i < (int)list.size() ? list[(size_t)i] : L"");
    }
}

/* ---------------- 书签 ---------------- */

/* 一本书的书签段名："bm" + 该书进度段的段名（f + 8 位哈希）。
 * 为什么不直接用路径：同 PosSectionFor 的理由（段名不能含 ] 和换行）。
 * 为什么加 "bm" 前缀：和进度段区分开，两者能并存且互不覆盖。 */
static std::wstring BmSectionFor(const std::wstring& path)
{
    return L"bm" + PosSectionFor(path);
}

/* 读一本书的书签。返回的每个元素是 "字符偏移|标题"（偏移是十进制文本），
 * 顺序就是用户添加的顺序（不排序 —— 列表里按"我加的顺序"看更符合直觉）。
 * 上限 200 条：一本小说真有 200 个书签已经远超正常使用，留个硬上限防止
 * ini 被写坏后无限循环。 */
std::vector<std::wstring> BookmarkLoad(const std::wstring& path)
{
    std::vector<std::wstring> v;
    if (path.empty()) return v;
    std::wstring sec = BmSectionFor(path);
    int n = IniGetInt(sec.c_str(), L"n", 0);
    if (n < 0) n = 0;
    if (n > 200) n = 200;
    for (int i = 0; i < n; i++) {
        wchar_t key[16];
        wsprintfW(key, L"b%d", i);
        std::wstring s = IniGetStr(sec.c_str(), key, L"");
        if (!s.empty()) v.push_back(s);
    }
    return v;
}

/* 覆盖写一本书的全部书签。先写 n 再写 b0..bN；列表变短时把多余的旧键写空串
 * （WritePrivateProfileStringW 传空串就是删键），否则删掉书签后重新读又会冒出来。 */
void BookmarkSave(const std::wstring& path, const std::vector<std::wstring>& items)
{
    if (path.empty()) return;
    std::wstring sec = BmSectionFor(path);
    /* 先记下上次写了几条，用于把多出来的旧键清空 */
    int old = IniGetInt(sec.c_str(), L"n", 0);
    if (old < 0) old = 0;
    if (old > 200) old = 200;

    int n = (int)items.size();
    if (n > 200) n = 200;
    IniSetInt(sec.c_str(), L"n", n);
    for (int i = 0; i < n; i++) {
        wchar_t key[16];
        wsprintfW(key, L"b%d", i);
        IniSetStr(sec.c_str(), key, items[(size_t)i]);
    }
    for (int i = n; i < old; i++) {
        wchar_t key[16];
        wsprintfW(key, L"b%d", i);
        IniSetStr(sec.c_str(), key, L"");
    }
}

