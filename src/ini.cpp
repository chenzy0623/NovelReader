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
#include <ctime>
#include <algorithm>

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

/* 程序启动时调用一次：定下 ini 路径。必须在任何 IniXxx / SettingsXxx 调用之前执行。
 *
 * 这里除了"选一个可写的位置"，还多做了一件重要的事：**找回上一次的阅读记录**。
 * 起因是真实的用户反馈——"更新之后书架里的书没了"：
 * 绿色版把 NovelReader.ini 放在 exe 同目录，用户下新版往往解压到一个新文件夹，
 * 于是程序看到的是一份全新的空 ini，书架当然空空如也（书本身没丢，只是记录不在这儿）。
 *
 * 处理原则（宁可不完美，也不能让用户以为书丢了）：
 *   1) exe 同目录可写 → 以它为首选（保持绿色软件"拷走文件夹就带走进度"的特性）；
 *   2) 这个 ini 里没有阅读记录时，去几个已知位置找"有阅读记录的那份"：
 *      exe 上级目录、再上一级、这两层的子目录、%APPDATA%、%LOCALAPPDATA%、
 *      桌面的 NovelReader\程序、下载目录的 NovelReader\程序；
 *   3) 找到就直接**改用那一份**（不复制、不合并：任何合并都可能把用户手改过的
 *      规则弄乱，而且原来的目录还在，继续往那儿写最不容易出错）；
 *   4) 一个都找不到才用 exe 目录的新 ini。
 * 找不到也还有第二道保险：ShelfRefresh 会扫 ini 里所有 f* 段把书补回书架。
 */

/* 取 exe 所在目录的上 n 级（n=1 是父目录） */
static std::wstring DirUp(const std::wstring& dir, int n)
{
    std::wstring s = dir;
    for (int i = 0; i < n; i++) {
        size_t k = s.find_last_of(L"\\/");
        if (k == std::wstring::npos) return s;
        s = s.substr(0, k);
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    }
    return s;
}

/* 往候选表里追加一个 ini 路径（去重，且忽略空目录） */
static void AddCandidate(std::vector<std::wstring>& v, const std::wstring& dir)
{
    if (dir.size() < 4) return;                 /* "", "C:", "C:\" 之类都跳过 */
    std::wstring p = dir + L"\\NovelReader.ini";
    for (size_t i = 0; i < v.size(); i++) {
        if (_wcsicmp(v[i].c_str(), p.c_str()) == 0) return;
    }
    v.push_back(p);
}

/* 把 dir 下面一层子目录（最多 24 个）也当成候选目录，
 * 覆盖"用户把新版解压到 NovelReader-1.5.0\ 而旧版在 NovelReader\ "这种常见布局。 */
static void AddSubdirCandidates(std::vector<std::wstring>& v, const std::wstring& dir)
{
    if (dir.size() < 4) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (fd.cFileName[0] == L'.') continue;
        AddCandidate(v, dir + L"\\" + fd.cFileName);
        AddCandidate(v, dir + L"\\" + fd.cFileName + L"\\程序");
        if (++n >= 24) break;                   /* 目录很多的盘也不至于卡住 */
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 这份 ini 里到底有没有"读过书"的痕迹。
 *
 * 判据比"文件存不存在"严格得多：新解压出来的 ini 也会存在（程序写设置时创建），
 * 所以必须看 [recent] 有没有书、或者有没有任何一个 f* 段落里存了 path。 */
static bool IniHasReadingData(const std::wstring& path)
{
    wchar_t r0[2048] = { 0 };
    GetPrivateProfileStringW(L"recent", L"r0", L"", r0, 2048, path.c_str());
    if (r0[0]) return true;

    static std::vector<wchar_t> buf(64 * 1024);
    DWORD n = GetPrivateProfileSectionNamesW(&buf[0], (DWORD)buf.size(), path.c_str());
    if (n == 0) return false;
    const wchar_t* p = &buf[0];
    while (*p) {
        if (p[0] == L'f') {                     /* 只可能是书的位置段 [fXXXXXXXX] */
            wchar_t sec[64];
            lstrcpynW(sec, p, 64);
            wchar_t real[MAX_PATH * 2] = { 0 };
            GetPrivateProfileStringW(sec, L"path", L"", real, MAX_PATH * 2, path.c_str());
            if (real[0]) return true;
        }
        p += lstrlenW(p) + 1;
    }
    return false;
}

void IniInit()
{
    std::wstring dir = ExeDir();
    std::wstring p = dir + L"\\NovelReader.ini";

    /* 备选位置：%APPDATA%（普通用户一定有写权限） */
    wchar_t appdata[MAX_PATH * 2] = { 0 };
    std::wstring appDir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        appDir = std::wstring(appdata) + L"\\NovelReader";
        CreateDirectoryW(appDir.c_str(), nullptr);   /* 已存在会返回 FALSE，无所谓 */
    }
    wchar_t localdata[MAX_PATH * 2] = { 0 };
    std::wstring localDir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localdata))) {
        localDir = std::wstring(localdata) + L"\\NovelReader";
    }
    wchar_t desktop[MAX_PATH * 2] = { 0 };
    std::wstring deskDir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        deskDir = desktop;
    }

    /* 首选：exe 同目录。不可写就退回 %APPDATA%。 */
    bool exeWritable = EnsureUnicodeIni(p);
    std::wstring primary = exeWritable ? p : (appDir.empty() ? p : appDir + L"\\NovelReader.ini");
    if (!exeWritable && !appDir.empty()) EnsureUnicodeIni(primary);

    /* 首选这份里没有阅读记录 → 去找旧版本留下的那份 */
    if (!IniHasReadingData(primary)) {
        std::vector<std::wstring> cands;
        /* exe 目录自己先排除（它就是 primary） */
        std::wstring up1 = DirUp(dir, 1);
        std::wstring up2 = DirUp(dir, 2);
        AddCandidate(cands, up1);                       /* 老版在上一级目录 */
        AddCandidate(cands, up1 + L"\\程序");
        AddCandidate(cands, up1 + L"\\NovelReader");
        AddCandidate(cands, up1 + L"\\NovelReader\\程序");
        AddCandidate(cands, up2);
        AddCandidate(cands, up2 + L"\\NovelReader");
        AddCandidate(cands, up2 + L"\\NovelReader\\程序");
        AddSubdirCandidates(cands, up1);                /* 解压成 NovelReader-1.4.0\ 这类情况 */
        AddSubdirCandidates(cands, dir);
        AddCandidate(cands, appDir);
        AddCandidate(cands, localDir);
        if (!deskDir.empty()) {
            AddCandidate(cands, deskDir + L"\\NovelReader\\程序");
            AddCandidate(cands, deskDir + L"\\NovelReader");
            AddCandidate(cands, deskDir + L"\\小说阅读器\\程序");
        }

        for (size_t i = 0; i < cands.size(); i++) {
            if (_wcsicmp(cands[i].c_str(), primary.c_str()) == 0) continue;
            if (GetFileAttributesW(cands[i].c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (IniHasReadingData(cands[i])) {
                s_iniPath = cands[i];    /* 直接改用那一份，之后继续往那儿写 */
                return;
            }
        }
    }

    s_iniPath = primary;
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
    /* 不能用定长 2048：字体文件列表这类值会越攒越长（十几条就超），被静默截断。
     * GetPrivateProfileStringW 返回实际写入的字符数，等于缓冲区大小-1 就是被截断了，
     * 加倍重试到 1M 字符为止（防止异常 ini 死循环）。 */
    std::vector<wchar_t> buf(512);
    for (;;) {
        DWORD n = GetPrivateProfileStringW(sec, key, def ? def : L"", &buf[0],
                                           (DWORD)buf.size(), s_iniPath.c_str());
        if ((size_t)n + 1 < buf.size()) return std::wstring(&buf[0], (size_t)n);
        if (buf.size() >= (size_t)1 << 20) return std::wstring(&buf[0], (size_t)n);
        buf.resize(buf.size() * 2);
    }
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

    /* ---- 1.6.0：隐私模式与老板键 ----
     * privacy 决定窗口标题里显不显示书名；bossKey 是全局热键的组合字符串
     * （"Ctrl+Alt+H" 这种写法，键名解析在 layout.cpp 的 KeysParse 里）。
     * 这两个都是"全局习惯"，不跟书走，所以放 [settings] 而不是每本书的段。 */
    g_set.privacy = IniGetInt(S0, L"privacy", 0);
    g_set.bossKey = IniGetStr(S0, L"bossKey", L"Ctrl+Alt+H");
    if (g_set.bossKey.empty()) g_set.bossKey = L"Ctrl+Alt+H";   /* 空串会注册成"无修饰的键"，太危险 */
    g_set.phoneSrv = IniGetInt(S0, L"phoneSrv", 0);   /* 手机同步：0 关（默认） 1 开 */

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
    /* 1.6.0：隐私模式与老板键（老板键可以在 Ctrl+F12 里改，改完要能记住） */
    IniSetInt(S0, L"privacy", g_set.privacy);
    IniSetStr(S0, L"bossKey", g_set.bossKey);
    IniSetInt(S0, L"phoneSrv", g_set.phoneSrv);
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

    /* 最后阅读时间（Unix 秒）。只用来给首页书架排序：
     * 书架同时从 [recent] 和 f* 段收集，没有时间戳就只能按"谁先被发现"排，
     * 补回来的老书会莫名其妙跑到最前面。 */
    IniSetInt(sec.c_str(), L"ts", (int)time(nullptr));

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
 * 数据来源有两个，缺一不可：
 *   1) [recent] 的 r0..r9 —— 最近打开的顺序（用户主动打开过哪些书）；
 *   2) ini 里所有 f* 段落 —— 只要这本书存过进度，就说明读过它。
 * 为什么两处都要：老版本只写 [recent]，一旦这个段被写坏/被清空/被新版 ini
 * 覆盖，书架就空了（"更新之后书没了"的报告就是这么来的）。而 f* 段落里有
 * path 键，能独立把书补回来，等于给书架加了一道自愈能力。
 *
 * 排序：每本书的段里有 ts（最后阅读时间，Unix 秒），按 ts 从新到旧排；
 * 没有 ts 的老记录排在有 ts 的后面，保持它们原来的相对顺序。
 * 排完把顺序写回 [recent]，这样下次就算只读 [recent] 也是对的。
 *
 * 两处细节：
 *   1) 文件已经被删/改名时标 missing，首页上灰掉且不给点（否则点了只会弹错误框）；
 *   2) 老 ini 没有 chname/title 键时，用"文件名去扩展名"当书名，章名留空。
 */
void ShelfRefresh()
{
    s_shelf.clear();

    /* 把一条路径加进书架（去重）。pshelf 里可能已经有它了。 */
    struct Helper {
        static bool Add(std::vector<ShelfItem>& out, const std::wstring& p)
        {
            if (p.empty()) return false;
            for (size_t i = 0; i < out.size(); i++) {
                if (lstrcmpiW(out[i].path.c_str(), p.c_str()) == 0) return false;
            }
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
            it.ts   = IniGetInt(sec.c_str(), L"ts", 0);
            it.missing = (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES);
            out.push_back(it);
            return true;
        }
    };

    /* 1) 先按最近打开顺序加入 */
    for (int i = 0; i < IDM_RECENT_MAX; i++) {
        Helper::Add(s_shelf, RecentGet(i));
    }

    /* 2) 再扫所有 f* 段落，把 [recent] 里没提到的书补回来 */
    {
        static std::vector<wchar_t> names(64 * 1024);
        DWORD n = GetPrivateProfileSectionNamesW(&names[0], (DWORD)names.size(),
                                                 s_iniPath.c_str());
        if (n > 0) {
            const wchar_t* p = &names[0];
            while (*p) {
                if (p[0] == L'f') {
                    wchar_t sec[64];
                    lstrcpynW(sec, p, 64);
                    std::wstring path = IniGetStr(sec, L"path", L"");
                    Helper::Add(s_shelf, path);
                }
                p += lstrlenW(p) + 1;
            }
        }
    }

    /* 3) 按最后阅读时间倒序。stable_sort 保证"没有 ts 的老记录"之间不乱序。
     * ts 只用来排序，不参与界面显示。 */
    std::stable_sort(s_shelf.begin(), s_shelf.end(),
                     [](const ShelfItem& a, const ShelfItem& b) { return a.ts > b.ts; });

    /* 4) 把书架顺序回写 [recent]：下次启动即使只读 [recent] 也是最新的顺序。
     * 只写前 IDM_RECENT_MAX 条（菜单里也只有这么多），其余的书仍然靠 f* 段补回来。 */
    for (int i = 0; i < IDM_RECENT_MAX; i++) {
        wchar_t key[16];
        wsprintfW(key, L"r%d", i);
        IniSetStr(L"recent", key, i < (int)s_shelf.size() ? s_shelf[(size_t)i].path : L"");
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

