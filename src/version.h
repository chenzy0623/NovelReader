/* version.h - 版本号唯一来源
 * ============================================================================
 * 以前版本号只出现在 README 和 git tag 里，代码里根本没有"我是哪个版本"这个概念，
 * 于是：关于对话框没法显示版本、检查更新没法比较、崩溃日志也说不清是哪个版本崩的。
 * 现在统一在这里定义，其他地方一律引用这几个宏，不要再写字面量。
 *
 * 加版本号时的规则：只改 NR_VERSION_* 这三个宏 + README/说明文档，
 * 别在别的 .cpp 里硬写 "1.8.0"。
 * ============================================================================ */
#ifndef NR_VERSION_H
#define NR_VERSION_H

#define NR_VERSION_MAJOR 1
#define NR_VERSION_MINOR 8
#define NR_VERSION_PATCH 0

/* 显示用字符串，例如 L"1.8.0" */
#define NR_VERSION_STR L"1.8.0"

/* 比较用的整数：主*10000 + 次*100 + 修订。
 * 只为了"谁更新"这种比较，别拿它做位运算。 */
#define NR_VERSION_NUM (NR_VERSION_MAJOR * 10000 + NR_VERSION_MINOR * 100 + NR_VERSION_PATCH)

/* 把 "1.8.0" / "v1.8.0" / "1.8" 这类字符串解析成 NR_VERSION_NUM 同量纲的整数。
 * 解析不出来返回 -1。实现在 update.cpp（检查更新要用它比 tag 名）。 */
int VersionParse(const wchar_t* text);

#endif /* NR_VERSION_H */
