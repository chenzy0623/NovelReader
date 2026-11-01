/* sync.h —— 电脑 ↔ 手机的阅读进度同步（1.7.0）
 *
 * ============================ 为什么要这么设计 ============================
 *
 * 需求原话是："在同一网络下实现阅读同步（手机上读到那，电脑上打开的时候就显示
 * 读到那），这个功能由用户自己开启"，并且明确"不要以我的电脑为服务器"。
 *
 * 所以这里**没有固定服务端**：电脑和手机各开一个很小的 TCP 监听（只在用户打开
 * 同步后才开），同时各往 255.255.255.255 广播自己的位置。两边互相发现之后就
 * 直接对话 —— 谁在线谁就能被找到，关机了也不影响另一边继续读。
 *
 * 为什么不搞成"手机当服务端、电脑当客户端"：手机经常换 IP（DHCP），
 * 而且手机端一旦被杀后台服务就没了；反过来"电脑当服务端"又是用户明确否掉的。
 * 广播 + 各自的监听对两边都公平，也不依赖任何第三方服务器。
 *
 * ============================== 数据怎么流 ==============================
 *
 *  - 每次打开一本书，电脑先按自己的记录恢复位置，然后问对端："这本书你读到哪？"
 *  - 对端回一行记录（书名 + 字符偏移 + 百分比 + 时间戳）。
 *  - 两边**比字符偏移**：谁的偏移大，谁就读得更靠后（"进度快的那一边"）。
 *  - 如果对端比本地更靠后，弹一个问句让用户确认 —— 同步是改用户的阅读位置，
 *    不问就跳会很唐突（用户可能只是想在电脑上回看前面一段）。
 *  - 用户点"是"才跳过去，跳完立刻按本地进度存一次，于是两边都对齐到最靠后的位置。
 *  - 本地滚动时不再实时推送（读的时候弹窗/网络抖动都很烦），只在下一次打开书
 *    或者手动"立即同步"时才对账。
 *
 * ============================== 协议长什么样 ==============================
 *
 * 传输就是最朴素的 HTTP/1.1，端口 TCP 8233（和 1.1.0 那个"手机阅读"网页服务
 * 的 8231 分开，互不影响）。之所以用 HTTP 而不是自定义二进制协议：手机端用
 * 现成的 HttpURLConnection 就能说，电脑端用 Winsock 手写解析也不难。
 *
 *   GET  /peer/ping       -> "NRPEER 1\n"                     用来探活
 *   GET  /peer/books      -> 每本一行："key<TAB>title<TAB>off<TAB>pct<TAB>ts<TAB>chapter"
 *   GET  /peer/book?key=  -> 正文（UTF-8），key 不存在回 404  给手机首次取书用
 *   POST /peer/progress   -> 请求体同样是一行记录，对端写自己的 ini
 *
 * 所有字段里的制表符/换行都会被百分号编码（%09/%0A/%25），所以书名里带奇怪的
 * 字符也不会把协议拆坏。
 */
#ifndef NR_SYNC_H
#define NR_SYNC_H

#include <string>
#include <vector>

/* 一条阅读记录。key 是书的"身份指纹"，电脑和手机各自算出来的同一个 key
 * 就认为是同一本书（算法见 sync.cpp 的 SyncBookKey）。 */
struct SyncRec
{
    std::wstring key;      /* 16 进制指纹（16 字符） */
    std::wstring title;    /* 书名，只用来显示和问用户 */
    std::wstring chapter;  /* 章名，同上 */
    int off = 0;           /* 字符偏移：比对"谁读得靠后"就看它 */
    int pct = 0;           /* 千分数，纯展示 */
    int ts  = 0;           /* 保存时间（Unix 秒），同偏移时用它挑新的 */
};

/* 打开/关闭同步。关闭后 TCP/UDP 都关掉，局域网里再也看不到这台机器。 */
bool SyncStart(std::wstring& err);
void SyncStop();
bool SyncRunning();

/* 现在能看到的对端（"192.168.1.23:8233  手机  3 秒前"）。没发现就是空。 */
std::vector<std::wstring> SyncPeers();

/* 取对端最近一次广播的地址（"http://192.168.1.23:8233"）。没有返回空串。 */
std::wstring SyncPeerUrl();

/* 本机所有书的进度记录（从 ini 的 f* 段读，和书架同源）。 */
void SyncLocalRecords(std::vector<SyncRec>& out);

/* 当前这本书的进度记录（没开书 / 书太短返回 false）。 */
bool SyncLocalRecord(SyncRec& r);

/* 按指纹取一本书的正文：先看是不是当前打开的书（直接给内存里的那份），
 * 不是的话按 ini 记录里的路径现读一份。手机端第一次打开某本书时用它。 */
bool SyncServeBook(const std::wstring& key, std::wstring& title, std::wstring& text);

/* 从 ini 的 [sync] 段取对端上次推过来的记录（"手机上读到哪"）。
 * 打开书时用她来比较谁读得靠后。 */
bool SyncPeerRecord(const std::wstring& key, SyncRec& out);

/* 取对端的进度列表。失败返回 false 并填 err（网络断了、对面关了同步等）。 */
bool SyncFetch(const std::wstring& baseUrl, std::vector<SyncRec>& out, std::wstring& err);

/* 把本地记录推给对端（对端只写它自己的 ini，不会反过来改我们）。 */
bool SyncPush(const std::wstring& baseUrl, const SyncRec& rec, std::wstring& err);

/* 书的指纹：书名 + 长度 + 开头 4096 个 UTF-16 字符，走 FNV-1a 64 位。
 *
 * 为什么不直接按书名匹配：不同版本的同名小说章节划分可能完全不同（有人说
 * "第1章"有人合并成一卷），按书名同步必然把偏移跳到莫名其妙的地方；而同一份
 * 文本的开头 4K 字符几乎必然一致。文本太短（< 64 字符）的书不参与同步，返回空串。 */
std::wstring SyncBookKey(const std::wstring& title, const std::wstring& text);

/* 当前打开的这本书的正文（给 /peer/book 用）。 */
bool SyncCurrentBook(std::wstring& key, std::wstring& title, std::wstring& text);

#endif /* NR_SYNC_H */
