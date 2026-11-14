/* update.h - 检查更新（问 GitHub Releases 有没有新版本）
 * ============================================================================
 * 做得很克制，只干三件事：
 *   1. 用和抓书完全相同的通道（web.cpp 的 WebFetchUrl：WinInet → 直连 → 系统 curl）
 *      去请求 https://api.github.com/repos/<owner>/<repo>/releases/latest；
 *   2. 从返回的 JSON 里抠出 tag_name（例如 "v1.8.0"）和 html_url；
 *   3. 和 version.h 里的 NR_VERSION_NUM 比大小。
 *
 * 为什么不做"自动下载并替换 exe"：
 *   替换正在运行的程序需要写 exe 所在目录（可能装在 Program Files，没有权限）、
 *   还要处理"重启后替换"的批处理，出错就把用户程序搞没了。打开浏览器让用户
 *   自己点下载，比"自作聪明地自更新"安全得多，代码也少一个数量级。
 *
 * 为什么不带 token：
 *   未登录的 GitHub API 每小时 60 次（按 IP），手动点"检查更新"根本用不完；
 *   而把 token 编译进程序等于公开泄漏，绝对不做。
 * ============================================================================ */
#ifndef NR_UPDATE_H
#define NR_UPDATE_H

#include <string>

struct UpdateInfo {
    bool ok = false;            /* 网络请求 + 解析都成功 */
    int  latest = -1;           /* 线上最新版本，量纲同 NR_VERSION_NUM；解析不出来是 -1 */
    std::wstring tag;           /* 原始 tag 名，例如 "v1.8.0" */
    std::wstring url;           /* release 页面地址（用户点"打开"时用） */
    std::wstring name;          /* release 标题，可为空 */
    std::wstring err;           /* 失败原因（中文） */
};

/* 从 GitHub API 的 release JSON 里解析出版本信息。纯函数，不联网，单元测试直接调。
 * 解析不出来时 out.ok=false 且 out.err 写原因。 */
bool UpdateParseReleaseJson(const std::wstring& json, UpdateInfo& out);

/* 联网检查。会阻塞（默认 12 秒超时），所以界面里必须放到后台线程调用。 */
UpdateInfo UpdateCheck(int timeoutMs = 12000);

#endif /* NR_UPDATE_H */
