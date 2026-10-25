/* server.h - 局域网手机阅读服务
 *
 * 为什么是"网页服务"而不是 APK：
 *   本机没有 Java / Android SDK，也编不出能过签名的 APK，而且给用户装一个
 *   APK 比扫个网址麻烦得多。这个程序自带的 HTTP 服务在电脑上跑，手机连同一个
 *   WiFi 直接用浏览器打开就能读，书和进度都在电脑上，天然同步 —— 手机读到的
 *   位置会写进同一份 NovelReader.ini，电脑这边按 Ctrl+G 就能跳过去接着读。
 *
 * 只监听局域网，不做鉴权（同一 WiFi 内可见），默认端口 8231。
 */
#ifndef NR_SERVER_H
#define NR_SERVER_H

#include <string>
#include <vector>

// 启动手机阅读服务（已经在跑就返回 true 并保持原端口）。
// port = 0 表示用默认端口 8231。
bool ServerStart(int port, std::wstring& err);

// 服务是否在跑
bool ServerRunning();

// 正在监听的端口（没跑返回 0）
int ServerPort();

// 本机可以被手机访问的地址列表，形如 L"http://192.168.1.5:8231/"
std::vector<std::wstring> ServerUrls();

// 最近一次手机保存的进度（没有则返回 false）
bool ServerLastPhoneProgress(std::wstring& path, int& off, std::wstring& chapter, int& pct);

#endif // NR_SERVER_H
