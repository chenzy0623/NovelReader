/* tests.cpp - 纯逻辑单元测试（不弹窗、不联网、不需要 ini）
 * ============================================================================
 * 只测"输入 → 输出"能完全确定的那部分逻辑：
 *   - version.h / update.cpp 的版本号解析与 GitHub JSON 解析；
 *   - sourcejson.cpp 的书源 JSON 导出 / 导入（含各种畸形输入）。
 * 界面、渲染、抓网页这些需要窗口和网络的都不在这里测 —— 它们靠人工回归，
 * 硬塞进单元测试只会写成"把实现抄一遍"的假测试。
 *
 * 为什么单独一个 exe 而不是塞进主程序：主程序是 -mwindows 无控制台，
 * 测试要往 stdout 打结果并被 build 脚本判 exit code，只能是 -mconsole。
 *
 * 运行：tests\tests.bat（编出 tests\tests.exe 并跑一遍，失败返回 1）
 * ============================================================================ */
#include "../src/sourcejson.h"
#include "../src/version.h"
#include "../src/update.h"
#include "../src/web.h"          /* 只为了拿 WebFetchUrl 的签名来打桩 */

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

/* ---- 打桩：update.cpp 的 UpdateCheck 会调 WebFetchUrl，但测试只测解析，不联网。
 * 真实实现在 src\web.cpp 里，这里不链接它（链接它会把整个抓书模块拖进来），
 * 于是提供一个同签名的桩，永远失败。UpdateCheck 本身不在这里测。 */
bool WebFetchUrl(const std::wstring&, std::string&, std::wstring&, std::wstring& err, int)
{
    err = L"（单元测试里不联网）";
    return false;
}

static int g_fail = 0;
static int g_pass = 0;

static void Check(bool cond, const char* what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        std::printf("FAIL: %s\n", what);
    }
}

static void CheckEq(int got, int want, const char* what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        std::printf("FAIL: %s  (got %d, want %d)\n", what, got, want);
    }
}

static void CheckEqW(const std::wstring& got, const std::wstring& want, const char* what)
{
    if (got == want) {
        g_pass++;
        return;
    }
    g_fail++;
    /* 宽字符直接 printf 在控制台代码页下会乱码，转成 UTF-8 再打 */
    char buf[1024];
    buf[0] = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, got.c_str(), (int)got.size(), buf, (int)sizeof(buf) - 1,
                                nullptr, nullptr);
    if (n > 0) buf[n] = 0;
    std::printf("FAIL: %s  (got \"%s\")\n", what, buf);
}

/* ------------------------------------------------------------------ 版本号 */
static void TestVersion()
{
    CheckEq(VersionParse(L"1.8.0"), 10800, "VersionParse 1.8.0");
    CheckEq(VersionParse(L"v1.8.0"), 10800, "VersionParse v1.8.0");
    CheckEq(VersionParse(L"1.8"), 10800, "VersionParse 1.8（缺第三段按 0）");
    CheckEq(VersionParse(L"1"), 10000, "VersionParse 1");
    CheckEq(VersionParse(L"v1.7.10"), 10710, "VersionParse v1.7.10（10 不能变成 1）");
    CheckEq(VersionParse(L"release-1.10.0"), 11000, "VersionParse release-1.10.0");
    CheckEq(VersionParse(L"NovelReader 2.0.1"), 20001, "VersionParse 带前缀文字");
    CheckEq(VersionParse(L"1.8.0-beta"), 10800, "VersionParse 后缀忽略");
    CheckEq(VersionParse(L"abc"), -1, "VersionParse 没有数字");
    CheckEq(VersionParse(L""), -1, "VersionParse 空串");
    CheckEq(VersionParse(nullptr), -1, "VersionParse nullptr");
    /* 超长数字不能撑爆 int（内部夹在 100000 以内） */
    Check(VersionParse(L"999999999999.0.0") > 0, "VersionParse 超长数字不溢出");

    Check(NR_VERSION_NUM == VersionParse(NR_VERSION_STR), "NR_VERSION_NUM 与 NR_VERSION_STR 一致");
}

/* ------------------------------------------------------------------ JSON 转义 */
static void TestQuote()
{
    CheckEqW(SourceJsonQuote(L"abc"), L"\"abc\"", "Quote 普通串");
    CheckEqW(SourceJsonQuote(L"a\"b"), L"\"a\\\"b\"", "Quote 引号");
    CheckEqW(SourceJsonQuote(L"a\\b"), L"\"a\\\\b\"", "Quote 反斜杠");
    CheckEqW(SourceJsonQuote(L"a\nb"), L"\"a\\nb\"", "Quote 换行");
    CheckEqW(SourceJsonQuote(L"a\tb"), L"\"a\\tb\"", "Quote 制表符");
    CheckEqW(SourceJsonQuote(L"a\rb"), L"\"a\\rb\"", "Quote 回车");
}

/* ------------------------------------------------------------------ 导出/导入往返 */
static BookSource MakeSrc(const std::wstring& name, const std::wstring& search)
{
    BookSource s;
    s.name = name;
    s.search = search;
    s.listRe = L"<a href=\"([^\"]+)\"[^>]*>([^<]+)</a>";
    s.tocStart = L"toc-list";
    s.tocRe = L"<a href=\"([^\"]+[.]html)\"[^>]*>([^<]{1,60})</a>";
    s.contentRe = L"<div[^>]*id=\"neirong\"[^>]*>([\\s\\S]*?)</div>";
    s.builtin = false;
    return s;
}

static void TestRoundtrip()
{
    std::vector<BookSource> list;
    list.push_back(MakeSrc(L"站点甲", L"https://a.example/search?q={kw}"));
    list.push_back(MakeSrc(L"站点\"乙\"", L"https://b.example/s?q={kw}"));

    std::wstring json = SourceExportJson(list, false);
    Check(json.find(L"\"sources\"") != std::wstring::npos, "导出带 sources 键");
    Check(json.find(L"站点\\\"乙\\\"") != std::wstring::npos, "导出时引号被转义");

    std::vector<BookSource> back;
    std::wstring err;
    bool ok = SourceImportJson(json, back, err);
    Check(ok, "导出再导入成功");
    if (ok) {
        CheckEq((int)back.size(), 2, "往返后条数不变");
        CheckEqW(back[0].name, L"站点甲", "往返后名字 0");
        CheckEqW(back[1].name, L"站点\"乙\"", "往返后名字 1（引号还原）");
        CheckEqW(back[1].search, L"https://b.example/s?q={kw}", "往返后 search");
        CheckEqW(back[0].tocRe, list[0].tocRe, "往返后 tocRe（含反斜杠）");
        CheckEqW(back[0].contentRe, list[0].contentRe, "往返后 contentRe（含 \\s\\S）");
    }

    /* 内置源不导出：导出时 includeBuiltin=false，即使传了内置源也应当跳过 */
    std::vector<BookSource> withBuiltin;
    BookSource bi = MakeSrc(L"内置示例源", L"");
    bi.builtin = true;
    withBuiltin.push_back(bi);
    withBuiltin.push_back(list[0]);
    std::wstring j2 = SourceExportJson(withBuiltin, false);
    std::vector<BookSource> b2;
    Check(SourceImportJson(j2, b2, err), "跳过内置源后仍能导入");
    CheckEq((int)b2.size(), 1, "内置源没被导出");

    /* includeBuiltin=true 时应当带上 */
    std::wstring j3 = SourceExportJson(withBuiltin, true);
    std::vector<BookSource> b3;
    Check(SourceImportJson(j3, b3, err), "includeBuiltin 导出可导入");
    CheckEq((int)b3.size(), 2, "includeBuiltin 时内置源也在");

    /* 空列表：导出一个合法的空数组，导入回来是 0 条且不算错 */
    std::vector<BookSource> none;
    std::wstring j4 = SourceExportJson(none, false);
    std::vector<BookSource> b4;
    Check(SourceImportJson(j4, b4, err), "空列表可导入");
    CheckEq((int)b4.size(), 0, "空列表导入后 0 条");
}

/* ------------------------------------------------------------------ 各种写法都能吃 */
static void TestFlexible()
{
    std::wstring err;
    std::vector<BookSource> out;

    /* 1) 顶层直接是数组 */
    bool ok = SourceImportJson(L"[{\"name\":\"甲\",\"search\":\"https://a/?q={kw}\"}]", out, err);
    Check(ok, "数组形式可导入");
    CheckEq((int)out.size(), 1, "数组形式条数");

    /* 2) 对象里嵌 sources */
    ok = SourceImportJson(L"{\"sources\":[{\"name\":\"乙\",\"search\":\"https://b/\"}]}", out, err);
    Check(ok, "sources 包裹可导入");
    CheckEqW(out.empty() ? L"" : out[0].name, L"乙", "sources 包裹的名字");

    /* 3) 只有一个书源的对象（没有 sources 键） */
    ok = SourceImportJson(L"{\"name\":\"丙\",\"search\":\"https://c/\"}", out, err);
    Check(ok, "单个对象可导入");
    CheckEq((int)out.size(), 1, "单个对象条数");

    /* 4) 字段别名：searchUrl / listRe / contentRe ... */
    ok = SourceImportJson(
        L"[{\"name\":\"丁\",\"searchUrl\":\"https://d/?q={kw}\",\"listRe\":\"L\","
        L"\"tocContainer\":\"C\",\"tocRe\":\"T\",\"bodyRe\":\"B\"}]",
        out, err);
    Check(ok, "别名字段可导入");
    if (ok && !out.empty()) {
        CheckEqW(out[0].search, L"https://d/?q={kw}", "别名 searchUrl");
        CheckEqW(out[0].listRe, L"L", "别名 listRe");
        CheckEqW(out[0].tocStart, L"C", "别名 tocContainer");
        CheckEqW(out[0].tocRe, L"T", "别名 tocRe");
        CheckEqW(out[0].contentRe, L"B", "别名 bodyRe");
    }

    /* 5) 键名大小写不敏感 + 空对象被跳过 + 未知键忽略 */
    ok = SourceImportJson(
        L"[{\"NAME\":\"戊\",\"SEARCH\":\"https://e/\"},{},{\"name\":\"\",\"search\":\"x\"}]", out, err);
    Check(ok, "大小写/空对象/空名字混在一起仍可导入");
    CheckEq((int)out.size(), 1, "空对象与空名字被跳过");

    /* 6) 转义：\u4e2d = 中，以及 \\ 与 \n */
    ok = SourceImportJson(L"[{\"name\":\"\\u4e2d\\u6587\",\"search\":\"a\\\\b\\nc\"}]", out, err);
    Check(ok, "转义可解析");
    if (ok && !out.empty()) {
        CheckEqW(out[0].name, L"中文", "\\uXXXX 解码");
        CheckEqW(out[0].search, L"a\\b\nc", "\\\\ 与 \\n 解码");
    }
}

/* ------------------------------------------------------------------ 畸形输入必须是"报错"而不是崩溃 */
static void TestBad()
{
    std::wstring err;
    std::vector<BookSource> out;

    Check(!SourceImportJson(L"", out, err), "空串导入失败");
    Check(!err.empty(), "空串有错误信息");

    Check(!SourceImportJson(L"{", out, err), "截断的对象失败");
    Check(!SourceImportJson(L"[{\"name\":\"x\"}", out, err), "缺右括号失败");
    Check(SourceImportJson(L"[]", out, err), "空数组不算错（0 条）");
    Check(!SourceImportJson(L"hello", out, err), "纯文字失败");
    Check(!SourceImportJson(L"{\"sources\":\"不是数组\"}", out, err), "sources 不是数组失败");

    /* 深层嵌套不应该把栈打爆：内部有 kMaxDepth 限制，超过就报错 */
    std::wstring deep;
    for (int i = 0; i < 200; i++) deep += L"[";
    for (int i = 0; i < 200; i++) deep += L"]";
    (void)SourceImportJson(deep, out, err);   /* 只要不崩就算过 */
    Check(true, "深层嵌套不崩溃");

    /* 超长单键：8MB 上限之内应当能读，超过上限报错（这里只测不崩） */
    std::wstring big = L"[{\"name\":\"";
    big.append(100000, L'a');
    big += L"\"}]";
    Check(SourceImportJson(big, out, err), "10 万字的名字仍可导入");
}

/* ------------------------------------------------------------------ 文件读写 */
static void TestFileIo()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"nr_tests_sources.json";

    std::vector<BookSource> list;
    list.push_back(MakeSrc(L"文件测试", L"https://f/?q={kw}"));
    std::wstring err;
    Check(SourceExportJsonFile(path, list, false, err), "导出到文件");

    std::vector<BookSource> back;
    Check(SourceImportJsonFile(path, back, err), "从文件导入");
    CheckEq((int)back.size(), 1, "文件导入条数");
    CheckEqW(back.empty() ? L"" : back[0].name, L"文件测试", "文件导入名字");

    DeleteFileW(path.c_str());

    /* 文件不存在：报错但不崩 */
    std::wstring bad = std::wstring(tmp) + L"nr_tests_not_exist_12345.json";
    DeleteFileW(bad.c_str());
    std::vector<BookSource> none;
    Check(!SourceImportJsonFile(bad, none, err), "不存在的文件报错");
    Check(!err.empty(), "不存在文件有错误信息");

    /* 目录不存在时要能自动建（导出路径带一层新目录） */
    std::wstring d = std::wstring(tmp) + L"nr_tests_dir\\sub\\s.json";
    Check(SourceExportJsonFile(d, list, false, err), "自动建目录后导出");
    Check(SourceImportJsonFile(d, none, err), "自动建目录后能读回");
    DeleteFileW(d.c_str());
    DeleteFileW((std::wstring(tmp) + L"nr_tests_dir\\sub").c_str());
    DeleteFileW((std::wstring(tmp) + L"nr_tests_dir").c_str());
}

/* ------------------------------------------------------------------ GitHub JSON 解析 */
static void TestUpdateParse()
{
    UpdateInfo u;
    const wchar_t* good =
        L"{\"url\":\"https://api.github.com/repos/x/y/releases/1\","
        L"\"html_url\":\"https://github.com/x/y/releases/tag/v1.9.0\","
        L"\"tag_name\":\"v1.9.0\",\"name\":\"1.9.0 听书\",\"body\":\"说明\"}";
    Check(UpdateParseReleaseJson(good, u), "正常 release JSON");
    Check(u.ok, "ok 为真");
    CheckEq(u.latest, 10900, "解析出的版本号");
    CheckEqW(u.tag, L"v1.9.0", "tag");
    CheckEqW(u.url, L"https://github.com/x/y/releases/tag/v1.9.0", "html_url");

    /* 限流 / 出错时 GitHub 返回 message */
    Check(!UpdateParseReleaseJson(L"{\"message\":\"API rate limit exceeded\"}", u),
          "限流 JSON 解析失败");
    Check(u.err.find(L"rate limit") != std::wstring::npos, "限流原因带出来");

    Check(!UpdateParseReleaseJson(L"", u), "空 JSON 失败");
    Check(!UpdateParseReleaseJson(L"<html>404</html>", u), "HTML 失败");

    /* 有 tag 但版本号看不懂 */
    Check(!UpdateParseReleaseJson(L"{\"tag_name\":\"nightly\"}", u), "看不懂的 tag 失败");

    /* 缺 html_url 时回退到 releases 页面 */
    Check(UpdateParseReleaseJson(L"{\"tag_name\":\"v2.0.0\"}", u), "缺 html_url 也算成功");
    Check(u.url.find(L"/releases") != std::wstring::npos, "回退到 releases 页");
    CheckEq(u.latest, 20000, "回退时版本号仍正确");
}

/* ------------------------------------------------------------------ 比较逻辑（与界面一致） */
static void TestCompare()
{
    /* 界面里用的是 latest <= NR_VERSION_NUM 判"已是最新"，这里把这个口径固定下来。
     * 注意别把版本号写死：以前写死 "v1.8.1"，等真发到 1.8.1 时这条就恒假了。
     * 用 NR_VERSION_STR 拼当前版本，高/低版本取足够远的固定值。 */
    UpdateInfo u;
    Check(UpdateParseReleaseJson(L"{\"tag_name\":\"v" NR_VERSION_STR L"\"}", u), "同版本");
    Check(u.latest == NR_VERSION_NUM, "同版本号解析一致");
    Check(u.latest <= NR_VERSION_NUM, "同版本视为已最新");
    Check(UpdateParseReleaseJson(L"{\"tag_name\":\"v99.0.0\"}", u), "高版本");
    Check(!(u.latest <= NR_VERSION_NUM), "高版本视为有新版本");
    Check(UpdateParseReleaseJson(L"{\"tag_name\":\"v1.0.0\"}", u), "低版本");
    Check(u.latest <= NR_VERSION_NUM, "低版本视为已最新");
}

/* 调试用：把导出的 JSON 读回来，逐条打印字段。规则里有反斜杠/引号，靠肉眼看
 * JSON 很容易漏转义，用程序读一遍最靠谱。
 *   tests.exe --import <文件> */
static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static int ImportDump(const std::wstring& path)
{
    std::vector<BookSource> list;
    std::wstring err;
    if (!SourceImportJsonFile(path, list, err)) {
        std::printf("import failed: %s\n", Narrow(err).c_str());
        return 1;
    }
    std::printf("count=%zu\n", list.size());
    for (size_t i = 0; i < list.size(); i++) {
        const BookSource& s = list[i];
        std::printf("[%zu] name=%s\n    search=%s\n    list=%s\n    tocStart=%s\n    toc=%s\n    content=%s\n",
                    i, Narrow(s.name).c_str(), Narrow(s.search).c_str(), Narrow(s.listRe).c_str(),
                    Narrow(s.tocStart).c_str(), Narrow(s.tocRe).c_str(), Narrow(s.contentRe).c_str());
    }
    return 0;
}

int main()
{
    /* 带参数时只做"把 JSON 读回来看一眼"，不跑测试：参数用宽字符版本取，
     * 免得中文路径被 ANSI 转换搞坏。 */
    if (__argc > 1) {
        int n = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
        if (argv && n > 2 && std::wstring(argv[1]) == L"--import") {
            int rc = ImportDump(argv[2]);
            LocalFree(argv);
            return rc;
        }
        if (argv) LocalFree(argv);
    }
    std::printf("NovelReader %s 单元测试\n", "tests");
    std::printf("版本常量：%d\n\n", NR_VERSION_NUM);

    TestVersion();
    TestQuote();
    TestRoundtrip();
    TestFlexible();
    TestBad();
    TestFileIo();
    TestUpdateParse();
    TestCompare();

    std::printf("\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("全部通过\n");
    return g_fail == 0 ? 0 : 1;
}
