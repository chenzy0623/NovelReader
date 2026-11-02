/* layout.cpp - 每本书的排版设置、高亮笔记、可自定义快捷键的存取
 *
 * 这个文件只做"数据"这一层：把结构体跟 ini 来回倒，以及把 "Ctrl+Alt+H" 这种
 * 人能读的字符串翻译成 Windows 要的 (虚拟键码, 修饰键)。界面和排版算法分别
 * 在 ui.cpp / view.cpp 里，互不掺和。
 *
 * 三段 ini 数据，全部按"每本书"存（段名都基于 PosSectionFor 的路径哈希）：
 *   [f1a2b3c4]     书的阅读进度（ini.cpp 管）+ lay_* 排版键（本文件管）
 *   [hlf1a2b3c4]   这本书的高亮与笔记
 *   [keys]         全局快捷键表（不是每本书的，因为换书不该换肌肉记忆）
 */

#include "common.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

/* ---------------- 全局：当前书的排版与高亮 ---------------- */

/* 当前生效的排版。打开书时 LayoutLoad 会把它填成"全局默认值 + 这本书的覆盖"，
 * 之后 UI 上改字号/行距/页边距都直接改这个结构体再 LayoutSave。 */
BookLayout g_lay;

/* 当前书的高亮，按 off 升序排列（绘制时用二分找本行命中的那些）。
 * 换书时由 HighlightLoad 整体替换。 */
std::vector<Highlight> g_hls;

/* ---------------- 排版设置 ---------------- */

/* 用 [settings] 里的全局值当"新书默认排版"。
 *
 * 只有字号、行距、字体名从全局继承 —— 页边距/缩进/分页/双栏/强制字体这几项
 * 是 1.6.0 才有的新概念，全局里没有对应项，直接用固定的出厂值（标准边距、
 * 缩进两字、连续滚动、单栏、不强制字体）。 */
void LayoutFromSettings()
{
    g_lay.fontPt    = g_set.fontPt;
    g_lay.spacing   = g_set.spacing;
    g_lay.fontFace  = g_set.fontFace;
    g_lay.margin    = 1;
    g_lay.indent    = 1;
    g_lay.paged     = 0;
    g_lay.cols      = 1;
    g_lay.forceFont = 0;
}

/* 读一本书的排版：先铺全局默认，再用 ini 里这本书自己的键覆盖。
 *
 * 为什么不给"这本书还没读过"另写一套：LayoutFromSettings 铺完就是默认值，
 * 没覆盖就等于没改过，逻辑只有一条路径。
 * 数值都要夹：ini 是文本文件，用户可能手改成 99 号字或者 7 栏，读进来直接
 * 交给排版会算了半天算不出东西。 */
void LayoutLoad(const std::wstring& path)
{
    LayoutFromSettings();
    if (path.empty()) return;

    std::wstring sec = PosSectionFor(path);
    const wchar_t* s = sec.c_str();

    int v;
    v = IniGetInt(s, L"lay_fontPt", g_lay.fontPt);
    if (v < 10) v = 10;
    if (v > 48) v = 48;
    g_lay.fontPt = v;

    v = IniGetInt(s, L"lay_spacing", g_lay.spacing);
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    g_lay.spacing = v;

    v = IniGetInt(s, L"lay_margin", g_lay.margin);
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    g_lay.margin = v;

    v = IniGetInt(s, L"lay_indent", g_lay.indent);
    g_lay.indent = (v != 0) ? 1 : 0;

    v = IniGetInt(s, L"lay_paged", g_lay.paged);
    g_lay.paged = (v != 0) ? 1 : 0;

    v = IniGetInt(s, L"lay_cols", g_lay.cols);
    g_lay.cols = (v >= 2) ? 2 : 1;
    /* 双栏只在分页模式下说得通：连续滚动 + 双栏时，两栏各自滚到哪一行
     * 根本没法同步（往下滚三行，第一栏末尾的字会跑到第二栏顶上去）。
     * 所以读到"双栏"就强制变成分页，跟界面上的行为保持一致。 */
    if (g_lay.cols == 2) g_lay.paged = 1;

    v = IniGetInt(s, L"lay_force", g_lay.forceFont);
    g_lay.forceFont = (v != 0) ? 1 : 0;

    std::wstring face = IniGetStr(s, L"lay_face", L"");
    if (!face.empty()) g_lay.fontFace = face;
}

/* 把当前排版写回这本书的 ini 段。
 *
 * 顺手也写一份到 [settings]：g_set.fontPt / g_set.spacing / g_set.fontFace
 * 是"下一本新书的默认值"，用户把某本书调成 22 号字，多半希望以后新开的书也
 * 是这个字号，否则每开一本都要重新调。 */
void LayoutSave(const std::wstring& path)
{
    g_set.fontPt   = g_lay.fontPt;
    g_set.spacing  = g_lay.spacing;
    g_set.fontFace = g_lay.fontFace;

    if (path.empty()) return;

    std::wstring sec = PosSectionFor(path);
    const wchar_t* s = sec.c_str();
    IniSetInt(s, L"lay_fontPt",  g_lay.fontPt);
    IniSetInt(s, L"lay_spacing", g_lay.spacing);
    IniSetInt(s, L"lay_margin",  g_lay.margin);
    IniSetInt(s, L"lay_indent",  g_lay.indent);
    IniSetInt(s, L"lay_paged",   g_lay.paged);
    IniSetInt(s, L"lay_cols",    g_lay.cols);
    IniSetInt(s, L"lay_force",   g_lay.forceFont);
    IniSetStr(s, L"lay_face",    g_lay.fontFace);
}

/* ---------------- 高亮与笔记 ---------------- */

/* 高亮段名：在书本身的段名前面加 "hl"，例如 [hlf1a2b3c4]。
 * 这样既能一眼看出是标注，又不用再算一次哈希。 */
static std::wstring HlSectionFor(const std::wstring& path)
{
    return L"hl" + PosSectionFor(path);
}

/* 把一条高亮的"偏移,长度,颜色"拼成 ini 值；笔记单独一个键。
 * 之所以分开存：笔记里用户可能打逗号、分号、等号，塞进同一个值里迟早会
 * 被解析错，分开存就只需要"真"和"假"两次读取。 */
static std::wstring HlEncode(const Highlight& h)
{
    wchar_t buf[64];
    wsprintfW(buf, L"%d,%d,%d", h.off, h.len, h.color);
    return buf;
}

/* "偏移,长度,颜色" → Highlight。解析不出来就返回 false（这条记录跳过，
 * 不让一条坏数据把整本书的标注全丢掉）。 */
static bool HlDecode(const std::wstring& text, Highlight* out)
{
    int off = 0, len = 0, color = 0;
    if (swscanf_s(text.c_str(), L"%d,%d,%d", &off, &len, &color) != 3) return false;
    if (off < 0 || len <= 0) return false;
    if (color < 0) color = 0;
    if (color > 3) color = 3;
    out->off = off;
    out->len = len;
    out->color = color;
    return true;
}

/* 读这本书的全部高亮。上限 2000 条：正常用户标不出更多，而 ini 是顺序解析的
 * 文本文件，条数太多（比如被脚本灌了十万条）启动时会明显变慢。 */
void HighlightLoad(const std::wstring& path)
{
    g_hls.clear();
    if (path.empty()) return;

    std::wstring sec = HlSectionFor(path);
    const wchar_t* s = sec.c_str();
    int n = IniGetInt(s, L"n", 0);
    if (n < 0) n = 0;
    if (n > 2000) n = 2000;

    wchar_t key[16];
    for (int i = 0; i < n; i++) {
        wsprintfW(key, L"h%d", i);
        std::wstring enc = IniGetStr(s, key, L"");
        if (enc.empty()) continue;
        Highlight h;
        if (!HlDecode(enc, &h)) continue;
        wsprintfW(key, L"t%d", i);
        h.note = IniGetStr(s, key, L"");
        g_hls.push_back(h);
    }
    /* 排序：绘制时按行二分查找，必须有序。用户在文中间插一条标注时
     * 顺序会乱，所以存完也排一次。 */
    std::sort(g_hls.begin(), g_hls.end(),
              [](const Highlight& a, const Highlight& b) { return a.off < b.off; });
}

/* 写回全部高亮。
 *
 * 先读旧的条数，把"变短之后多出来的"键写空串删掉 —— 否则删掉一条标注、
 * 重启之后那条又冒出来（ini 里还留着 h5/t5）。 */
void HighlightSave(const std::wstring& path)
{
    if (path.empty()) return;

    std::wstring sec = HlSectionFor(path);
    const wchar_t* s = sec.c_str();
    int oldN = IniGetInt(s, L"n", 0);
    int newN = (int)g_hls.size();

    wchar_t key[16];
    for (int i = 0; i < newN; i++) {
        wsprintfW(key, L"h%d", i);
        IniSetStr(s, key, HlEncode(g_hls[(size_t)i]));
        wsprintfW(key, L"t%d", i);
        IniSetStr(s, key, g_hls[(size_t)i].note);
    }
    if (oldN > newN) {
        for (int i = newN; i < oldN; i++) {
            wsprintfW(key, L"h%d", i);
            IniSetStr(s, key, L"");
            wsprintfW(key, L"t%d", i);
            IniSetStr(s, key, L"");
        }
    }
    IniSetInt(s, L"n", newN);
}

/* ---------------- 可自定义快捷键 ---------------- */

/* 内置的快捷键表。改这里就等于改出厂默认：ini 里没记录的项一律用 def。
 *
 * 只收"用户最可能想改"的功能，不把每个菜单项都放进来 —— 一个三十行的列表
 * 没人愿意看。表格顺序就是对话框里的顺序，按使用频率从高到低排。 */
static KeyBind kDefBinds[] = {
    { IDM_OPEN,        L"打开文件",        L"Ctrl+O",       L"" },
    { IDM_CLOSEFILE,   L"关闭当前书",      L"Ctrl+W",       L"" },
    { IDM_FIND,        L"全文搜索",        L"Ctrl+F",       L"" },
    { IDM_FIND_NEXT,   L"下一个命中",      L"F3",           L"" },
    { IDM_TOC,         L"显示/隐藏目录",   L"Ctrl+T",       L"" },
    { IDM_PREV_CH,     L"上一章",          L"Alt+Left",     L"" },
    { IDM_NEXT_CH,     L"下一章",          L"Alt+Right",    L"" },
    { IDM_AUTO_TOGGLE, L"自动下滑开关",    L"F6",           L"" },
    { IDM_AUTO_FAST,   L"自动下滑加速",    L"F8",           L"" },
    { IDM_AUTO_SLOW,   L"自动下滑减速",    L"F7",           L"" },
    { IDM_FONT_UP,     L"字号变大",        L"Ctrl+=",       L"" },
    { IDM_FONT_DOWN,   L"字号变小",        L"Ctrl+-",       L"" },
    { IDM_THEME,       L"日间/夜间切换",   L"F2",           L"" },
    { IDM_LINE_SPACE,  L"行距切换",        L"Ctrl+1",       L"" },
    { IDM_BM_ADD,      L"添加书签",        L"Ctrl+D",       L"" },
    { IDM_BM_LIST,     L"书签列表",        L"Ctrl+B",       L"" },
    { IDM_HL_ADD,      L"高亮选中文字",    L"Ctrl+H",       L"" },
    { IDM_HL_NOTE,     L"给选中文字写笔记",L"Ctrl+Shift+N", L"" },
    { IDM_HL_LIST,     L"高亮与笔记列表",  L"Ctrl+L",       L"" },
    { IDM_LAY_PAGED,   L"分页/滚动切换",   L"Ctrl+Shift+P", L"" },
    { IDM_LAY_COLS,    L"单栏/双栏切换",   L"Ctrl+Shift+C", L"" },
    { IDM_PRIVACY,     L"隐私模式开关",    L"Ctrl+Shift+H", L"" },
    { IDM_PDF_MODE,    L"PDF 原始版式切换",L"F9",           L"" },
    { IDM_KEYS_EDIT,   L"快捷键设置",      L"Ctrl+F12",     L"" },
    { IDM_KEYS,        L"快捷键说明",      L"F1",           L"" },
    { IDM_MENU,        L"打开主菜单",      L"Ctrl+F10",     L"" },
    /* 1.7.0 修复：下面这些命令原来只在菜单文字里写了快捷键，从没登记进这张表，
     * 于是 BuildAccel() 建不出对应的加速键，按了完全没反应；老板键连"改"都改不了。 */
    { IDM_BOSS,        L"老板键（隐藏/唤回窗口）", L"Ctrl+Alt+H", L"" },
    { IDM_OPEN_URL,    L"打开网页小说",    L"Ctrl+U",       L"" },
    { IDM_SOURCE,      L"书源搜索",        L"Ctrl+S",       L"" },
    { IDM_PHONE,       L"手机阅读（局域网）", L"Ctrl+G",   L"" },
    { IDM_PHONE_JUMP,  L"跳到手机读到的位置", L"Ctrl+Shift+G", L"" },
    { IDM_LAY_INDENT,  L"首行缩进开关",    L"Ctrl+Shift+I", L"" },
    { IDM_HL_DEL,      L"删除此处高亮",    L"Ctrl+Shift+X", L"" },
};

/* 表 + 当前值。第一次调用时把 def 填进 cur，之后 KeysLoad 覆盖成 ini 里的值。
 * 用函数内静态而不是全局对象：静态初始化顺序不由我控制，而这张表在
 * wWinMain 早期就会被用到。 */
std::vector<KeyBind>& KeyBinds()
{
    static std::vector<KeyBind> binds;
    if (binds.empty()) {
        int n = (int)(sizeof(kDefBinds) / sizeof(kDefBinds[0]));
        binds.reserve((size_t)n);
        for (int i = 0; i < n; i++) {
            KeyBind b = kDefBinds[i];
            b.cur = b.def;
            binds.push_back(b);
        }
    }
    return binds;
}

/* 键名表：字符串 → 虚拟键码。只收阅读器用得到的键，既不支持 PrintScreen 那种
 * 奇怪的东西，也没必要把整个键盘的 VK 都列出来。
 *
 * 注意 '=' 和 '-' 这两个：用户按 Ctrl+= 打出来的是 VK_OEM_PLUS，但人写配置时
 * 会写 "Ctrl+="。所以键名表里同时认 "=" 和 "OEM_PLUS" 两种写法。 */
struct KeyName { const wchar_t* name; BYTE vk; };
static const KeyName kKeyNames[] = {
    { L"LEFT",  VK_LEFT },  { L"RIGHT", VK_RIGHT }, { L"UP",    VK_UP },
    { L"DOWN",  VK_DOWN },  { L"PGUP",  VK_PRIOR }, { L"PGDN",  VK_NEXT },
    { L"HOME",  VK_HOME },  { L"END",   VK_END },   { L"INS",   VK_INSERT },
    { L"DEL",   VK_DELETE },{ L"SPACE", VK_SPACE }, { L"ENTER", VK_RETURN },
    { L"ESC",   VK_ESCAPE },{ L"TAB",   VK_TAB },   { L"BACK",  VK_BACK },
    { L"PLUS",  VK_OEM_PLUS }, { L"=", VK_OEM_PLUS }, { L"ADD", VK_ADD },
    { L"MINUS", VK_OEM_MINUS }, { L"-", VK_OEM_MINUS }, { L"SUB", VK_SUBTRACT },
};

/* 单个键名 → vk。认三种：单字符（A~Z / 0~9 / 标点，用 ASCII 大写）、
 * F1~F24、以及上面那张表里的名字（大小写不敏感）。 */
/* 十六进制数字 → 值（NameFromVk 的 "VK%02X" 输出要能被认回来）。 */
static int HexDigit(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return (int)(c - L'0');
    if (c >= L'A' && c <= L'F') return (int)(c - L'A' + 10);
    if (c >= L'a' && c <= L'f') return (int)(c - L'a' + 10);
    return -1;
}

static bool VkFromName(const std::wstring& raw, BYTE* vk)
{
    if (raw.empty()) return false;
    std::wstring up;
    up.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) up.push_back((wchar_t)towupper(raw[i]));

    /* F1~F24 */
    if (up.size() >= 2 && up[0] == L'F') {
        wchar_t* end = nullptr;
        long n = wcstol(up.c_str() + 1, &end, 10);
        if (end && *end == 0 && n >= 1 && n <= 24) {
            *vk = (BYTE)(VK_F1 + n - 1);
            return true;
        }
    }
    int cnt = (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0]));
    for (int i = 0; i < cnt; i++) {
        if (up == kKeyNames[i].name) { *vk = kKeyNames[i].vk; return true; }
    }
    /* "VK6F" 这种十六进制写法：NameFromVk 对表外的键就是这么输出的。
     * 认不回来的话，自定义到 OEM 键、小键盘键等存一次 ini 再读就失效（界面还会显示
     * "Ctrl+VKBF" 这种看不懂的名字）。 */
    if (up.size() == 4 && up[0] == L'V' && up[1] == L'K') {
        int hi = HexDigit(up[2]), lo = HexDigit(up[3]);
        if (hi >= 0 && lo >= 0 && hi * 16 + lo > 0) { *vk = (BYTE)(hi * 16 + lo); return true; }
    }
    /* 最后当单字符键：字母数字直接用 ASCII，这正好等于 VK 值 */
    if (up.size() == 1 && up[0] > 32 && up[0] < 127) {
        *vk = (BYTE)up[0];
        return true;
    }
    return false;
}

/* vk → 键名（写 ini、显示在对话框里都用它）。反向查上面那张表，
 * 找不到就按字母/数字/未知处理。 */
static std::wstring NameFromVk(BYTE vk)
{
    int cnt = (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0]));
    for (int i = 0; i < cnt; i++) {
        /* 同一 vk 可能有多个名字（"=" 和 "PLUS"），取第一个正式名（表里排前面那种），
         * 这样界面显示统一是 "Ctrl+=" 而不是 "Ctrl+OEM_PLUS"。 */
        if (kKeyNames[i].vk == vk && kKeyNames[i].name[0] >= L'A' && kKeyNames[i].name[0] <= L'Z')
            return kKeyNames[i].name;
    }
    for (int i = 0; i < cnt; i++) {
        if (kKeyNames[i].vk == vk) return kKeyNames[i].name;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        wsprintfW(buf, L"F%d", (int)(vk - VK_F1 + 1));
        return buf;
    }
    if (vk >= 'A' && vk <= 'Z') { wchar_t b[2] = { (wchar_t)vk, 0 }; return b; }
    if (vk >= '0' && vk <= '9') { wchar_t b[2] = { (wchar_t)vk, 0 }; return b; }
    wchar_t buf[16];
    wsprintfW(buf, L"VK%02X", (int)vk);
    return buf;
}

/* "Ctrl+Alt+H" → vk + MOD_*。
 *
 * 解析规则：按 '+' 切开，最后一段是主键，前面的段全是修饰键。
 * 这样写是为了让"Ctrl+="（主键本身是加号）也能解析 —— 切开之后最后一段是
 * 空串，特殊照顾一下，把空串当成 '+'。 */
bool KeysParse(const std::wstring& text, BYTE* vk, BYTE* mods)
{
    if (text.empty() || !vk || !mods) return false;
    BYTE v = 0, m = 0;
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (size_t i = 0; i < text.size(); i++) {
        wchar_t c = text[i];
        if (c == L'+') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    /* 结尾是空的（"Ctrl+=" 切完最后一段是 ""）：补一个 '+' 当主键 */
    if (parts.size() >= 2 && parts.back().empty()) parts.back() = L"=";

    for (size_t i = 0; i + 1 < parts.size(); i++) {
        std::wstring up = parts[i];
        for (size_t k = 0; k < up.size(); k++) up[k] = (wchar_t)towupper(up[k]);
        if (up == L"CTRL" || up == L"CONTROL") m |= MOD_CONTROL;
        else if (up == L"ALT") m |= MOD_ALT;
        else if (up == L"SHIFT") m |= MOD_SHIFT;
        else if (up == L"WIN") m |= MOD_WIN;
        else return false;
    }
    if (!VkFromName(parts.back(), &v)) return false;
    *vk = v;
    *mods = m;
    return true;
}

/* (vk, mods) → "Ctrl+Alt+H"。顺序固定 Ctrl→Alt→Shift→Win，
 * 这样同一条快捷键不会因为用户按下的顺序不同而写成两种字符串。 */
std::wstring KeysFormat(BYTE vk, BYTE mods)
{
    std::wstring out;
    if (mods & MOD_CONTROL) out += L"Ctrl+";
    if (mods & MOD_ALT)     out += L"Alt+";
    if (mods & MOD_SHIFT)   out += L"Shift+";
    if (mods & MOD_WIN)     out += L"Win+";
    out += NameFromVk(vk);
    return out;
}

/* 读 [keys]：键名用 "k<命令ID>"（纯 ASCII，不担心编码），值形如 "Ctrl+F6"。
 * 值写空串表示"用户主动取消了这个快捷键"，此时 cur 留空 —— 不能回退到默认值，
 * 否则用户永远取消不掉。 */
void KeysLoad()
{
    std::vector<KeyBind>& binds = KeyBinds();
    for (size_t i = 0; i < binds.size(); i++) {
        wchar_t key[16];
        wsprintfW(key, L"k%d", binds[i].id);
        std::wstring v = IniGetStr(L"keys", key, L"\x01");
        /* \x01 是"这个键在 ini 里根本不存在"的探针值：存在但为空串说明用户
         * 故意清掉了，要尊重；不存在才用出厂默认。 */
        if (v == L"\x01") binds[i].cur = binds[i].def;
        else binds[i].cur = v;
    }
}

/* 把当前快捷键全部写回 [keys]。没有删除逻辑 —— 表长固定，清空写空串即可。 */
void KeysSave()
{
    std::vector<KeyBind>& binds = KeyBinds();
    for (size_t i = 0; i < binds.size(); i++) {
        wchar_t key[16];
        wsprintfW(key, L"k%d", binds[i].id);
        IniSetStr(L"keys", key, binds[i].cur);
    }
}
