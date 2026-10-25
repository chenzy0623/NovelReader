// =============================================================================
// zip.cpp —— ZIP 中央目录解析 + 原始 DEFLATE 解压（RFC 1951）的实现
//
// 为什么自己写：EPUB / DOCX 都是 ZIP 包，为了保持"单 exe、无 DLL 依赖"，
// 不引入 zlib / minizip，这里按规范手写一份最小的只读实现。
//
// 整体流程（读一个 EPUB 里的一章）：
//   1) 调用方把整个 .epub 读进内存，交给 ZipArchive::Load；
//   2) Load 从文件末尾倒着找 EOCD（"End Of Central Directory"），
//      用里面的"目录偏移 + 条目数"逐条解析中央目录，得到每个条目的
//      名字、压缩方法、压缩后大小、原始大小、本地头偏移；
//   3) ReadName/ReadIndex 按本地头偏移跳到数据区，stored 直接拷贝，
//      deflate 交给 InflateRaw 解压。
//
// 本文件不做：ZIP64、多卷(分卷)、加密、写 zip。
// =============================================================================

#include "zip.h"

#include <cstddef>   // size_t
#include <utility>   // std::move

// 所有细节实现都塞进匿名 namespace：一来符号不外泄（避免和别处重名），
// 二来满足"无全局可变状态"的要求——这里的表全是 const，状态全在函数内的
// struct 里，多线程同时解压两个条目互不影响。
namespace {

// -----------------------------------------------------------------------------
// 小工具：按小端序读 16/32 位整数
//
// ZIP 里所有多字节字段都是小端序（低字节在前），这是当年 PKZIP 在 DOS/Intel
// 上的产物。这里显式拼字节而不是直接把指针 reinterpret_cast 成 uint32_t*：
// 一是避免未对齐访问（x86 虽然能跑，但属于未定义行为），二是避免大小端依赖。
// -----------------------------------------------------------------------------
inline uint16_t ReadU16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

inline uint32_t ReadU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 判断 p 处 4 个字节是不是给定的 ZIP 签名（PK\x03\x04 等）
inline bool HasSignature(const uint8_t* p, uint8_t b2, uint8_t b3) {
    // 所有 ZIP 签名的前两个字节都是 'P' 'K'，只有后两个字节区分用途
    return p[0] == 0x50 && p[1] == 0x4B && p[2] == b2 && p[3] == b3;
}

// -----------------------------------------------------------------------------
// 位读取器：DEFLATE 的位流
//
// DEFLATE 的位序是"反的"：一个字节里先出来的是最低位（LSB-first），
// 与我们在 paper 上写二进制数的习惯（高位在左）正好相反。原因很实际——
// 压缩端一边产生位一边往字节低端填，填满 8 位就吐出一个字节，这样不用
// 预先知道总长度、也不用回填，最省事。Huffman 码字也顺着这个位序发送，
// 所以"规范码字"在流里是"位反转"过的，下面 Huffman::Decode 里那种
// 逐位 code |= bit 的写法处理的就是这件事。
//
// 实现上用 64 位缓冲：一次 refill 就把能读的字节塞进 buf 低位，取位时
// 从低位取、整体右移。约束：GetBits 的 n <= 32，Refill 只在 count < need
// 时补字节，所以 count 最多到 need-1+8 <= 39，不会把 buf 撑爆 64 位。
// -----------------------------------------------------------------------------
struct BitReader {
    const uint8_t* src = nullptr;  // 压缩数据起点
    size_t         srcLen = 0;     // 压缩数据总长（所有读取都必须受它限制）
    size_t         pos = 0;        // 下一个"尚未装进缓冲"的字节下标
    uint64_t       buf = 0;        // 位缓冲：低位是下一个要吐出的位
    int            count = 0;      // buf 里还有多少有效位

    void Init(const uint8_t* s, size_t n) {
        src = s;
        srcLen = n;
        pos = 0;
        buf = 0;
        count = 0;
    }

    // 确保缓冲里至少有 need 位。返回 false 表示压缩数据已经用完——
    // 这是"绝不读越界内存"的唯一把关点：先看 pos 是否到头，再取字节。
    bool Refill(int need) {
        while (count < need) {
            if (pos >= srcLen) return false;
            buf |= (uint64_t)src[pos++] << count;
            count += 8;
        }
        return true;
    }

    // 取出 n 位（n <= 32），结果放在 v 里。n == 0 时合法地返回 0 位，
    // 用于"额外位数为 0"的长度/距离码。
    bool GetBits(int n, uint32_t& v) {
        v = 0;
        if (n <= 0) return true;
        if (!Refill(n)) return false;
        v = (uint32_t)(buf & (((uint64_t)1 << n) - 1));
        buf >>= n;
        count -= n;
        return true;
    }

    // 取 1 位，失败返回 -1（Huffman 解码时用它判断数据是否耗尽）
    int GetBit() {
        uint32_t v = 0;
        if (!GetBits(1, v)) return -1;
        return (int)v;
    }

    // 对齐到字节边界：丢掉缓冲里那些"不足一个字节"的零散位。
    // stored 块的数据是按字节存的，必须先把位流对齐，否则会从半截字节开始读。
    void AlignToByte() {
        int drop = count & 7;
        buf >>= drop;
        count -= drop;
    }

    // 已经消耗的压缩字节数：已读位数向上取整到字节。
    // 已读位数 = pos*8 - count（pos 是搬进缓冲的字节数，count 是还没用掉的位）；
    // 对它向上取整，等价于 pos - count/8（count 除 8 是缓冲里没动过的整字节）。
    size_t ConsumedBytes() const { return pos - (size_t)(count >> 3); }
};

// -----------------------------------------------------------------------------
// 规范 Huffman 表
//
// DEFLATE 用的是"规范 Huffman 码"（canonical code）：传输时只给每个符号的
// 码长，码字本身按一套固定规则现推：
//   * 码长相同的符号，按符号编号从小到大、码字从小到大分配；
//   * 长度为 len 的第一个码字 = (长度为 len-1 的最后一个码字 + 1) << 1。
// 所以建表根本不需要建树，只要 count[码长] 和一份"按码字顺序排好的符号表"。
//
// 建表复杂度 O(n)：先数出每个码长有多少个（计数排序的"计数"那一步），
// 再用前缀和算出每个码长的起始下标 offsets[]，最后把符号按码长塞进
// symbol[] 的对应区间。解码时维护 (code, first, index) 三个量做二分式的
// 逐位下降，见 Decode。
// -----------------------------------------------------------------------------
struct Huffman {
    uint16_t count[16];    // count[len] = 码长为 len 的符号个数（len 取 1..15）
    uint16_t symbol[288];  // 所有符号按"码字递增"顺序排列（literal 表最多 288 项）

    // 用码长数组 lengths[0..n-1] 建表。返回 false 表示这组码长非法。
    bool Build(const uint8_t* lengths, int n) {
        for (int i = 0; i < 16; i++) count[i] = 0;
        for (int i = 0; i < n; i++) count[lengths[i]]++;

        // 全 0 长度：这是一张空表（合法的，例如一个只有字面量、没有回溯的
        // 动态块可以给距离树全 0 长度）。允许建表，但解码时任何码都算非法。
        if (count[0] == n) return true;
        count[0] = 0;

        // 检验码长是否"超额订阅"：把 count 当作一棵满二叉树的叶子分布，
        // 每往下一层可用的坑位翻倍，挖掉该层用掉的叶子；如果坑位变成负数，
        // 说明这组码长编不出前缀码（数据损坏或不是 DEFLATE 流）。
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        // 注意 left > 0 是允许的（存在"不完整码"，即有些码字没分配给任何
        // 符号）。这种表在解码时遇到未分配的码字会返回 -1，由调用方判失败。

        // 前缀和：算出每个码长在 symbol[] 里的起始下标。
        // 因为码字按码长递增，长度 len 的符号紧跟在所有更短的符号之后。
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 2; len < 16; len++) offs[len] = (uint16_t)(offs[len - 1] + count[len - 1]);

        // 按符号编号顺序填表：编号小的先占码字小的位置，这正是规范码的定义。
        for (int i = 0; i < n; i++) {
            if (lengths[i] != 0) symbol[offs[lengths[i]]++] = (uint16_t)i;
        }
        return true;
    }

    // 从位流里解出一个符号；数据耗尽或遇到未分配码字返回 -1。
    //
    // 解码思路（不建树，直接沿"码字区间"下降）：
    //   first  = 当前码长下第一个码字的数值
    //   index  = 当前码长下第一个符号在 symbol[] 里的下标
    //   code   = 已经读到的位（逐位左移累积，新位放最低位）
    // 每层先比一下 code 是否落在 [first, first+count[len]) 内，落进去
    // 就说明长度为 len 的码字匹配成功了；否则把该层的 count 个符号跳过、
    // first 进位到下一层，继续读下一位。
    int Decode(BitReader& br) const {
        int code = 0;   // 已读位累积成的码字值
        int first = 0;  // 当前码长下最小的码字
        int index = 0;  // 当前码长下最小的符号在 symbol[] 中的位置
        for (int len = 1; len < 16; len++) {
            int bit = br.GetBit();
            if (bit < 0) return -1;
            code |= bit;  // 新读的位是"最低位"：这是 LSB-first 位序的直接体现
            int cnt = count[len];
            if (code - first < cnt) return symbol[index + (code - first)];
            index += cnt;          // 跳过这一层的符号
            first = (first + cnt) << 1;  // 下一层的起始码字
            code <<= 1;            // 已读的码字整体左移，给下一位腾位置
        }
        return -1;  // 15 位还没匹配上：不可能是合法的 DEFLATE 码
    }
};

// -----------------------------------------------------------------------------
// 长度码 / 距离码的基值与额外位数（RFC 1951 3.2.5 的两张表，完整列出）
//
// 符号 257..285 表示"匹配长度"：真正的长度 = 基值 + 从流里再读的 extra 位。
// 这样设计是为了用 29 个 Huffman 符号覆盖 3..258 的全部长度，出现频繁的
// 短长度用短码字，长长度用长码字。
// -----------------------------------------------------------------------------
const uint16_t kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const uint8_t kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

// 符号 0..29 表示回溯距离：真正的距离 = 基值 + extra 位。
// 距离 1 是"上一个字节"，在小说文本里最常出现（重复的标点、缩进空格），
// 所以 1 拿到了最短的码字位置。
const uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
const uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// 动态块里"码长码长表"的读出顺序（RFC 1951 3.2.7）。
// 19 个码长的排列是规范定死的，因为它们通常是"前几个有用、后面的全 0"，
// 按这个顺序摆能让 0 尽量排在后面，从而少读几位（HCLEN 只描述前多少个）。
const uint8_t kCodeLenOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// -----------------------------------------------------------------------------
// 输出缓冲：一次性 resize 到 expectedSize，之后按下标写入
//
// 不用 push_back 的原因：push_back 每次都要比较 size 和 capacity，并且可能
// 触发扩容（新分配 + 整体拷贝）；而本项目要解压的 word/styles.xml 有 349KB、
// 章节 XHTML 也有几十 KB，逐字节 push_back 的常数开销很可观。既然中央目录
// 已经告诉了我们原始大小，直接预分配然后再写就省掉了所有扩容和边界重算。
// -----------------------------------------------------------------------------
struct OutBuf {
    std::string& s;
    size_t       pos = 0;  // 当前写到哪里
    size_t       cap = 0;  // 容量 = expectedSize

    explicit OutBuf(std::string& str) : s(str) {}

    void Init(size_t n) {
        s.assign(n, '\0');
        cap = n;
        pos = 0;
    }

    // 写一个字节；超过预期长度说明压缩数据比声明的长，属于损坏，直接失败。
    bool Put(uint8_t b) {
        if (pos >= cap) return false;
        s[pos++] = (char)b;
        return true;
    }

    // LZ77 的回溯拷贝：从"当前位置 - dist"处逐字节拷 len 个字节。
    // 必须逐字节拷贝、不能 memcpy，因为 dist 可能小于 len（重叠拷贝），
    // 那正是"重复重复重复..."这种长串能压到极小的原理。
    bool CopyBack(size_t dist, size_t len) {
        if (dist == 0 || dist > pos) return false;   // 距离越界：非法回溯
        if (len > cap - pos) return false;           // 输出会超出声明的原始大小
        for (size_t k = 0; k < len; k++) {
            s[pos] = s[pos - dist];
            pos++;
        }
        return true;
    }

    // 收尾：把临时预分配的多余尾部裁掉，让 size() 反映真实解压长度，
    // 调用方（ReadIndex）再用它和 uncompSize 比对。
    void Trim() { s.resize(pos); }
};

// 建固定 Huffman 表（BTYPE=01 用的那张，RFC 1951 3.2.6）。
// 码长分布是规范写死的：0-143 用 8 位、144-255 用 9 位、256-279 用 7 位、
// 280-287 用 8 位；距离码 0-29 一律 5 位。所以固定块不需要传任何表。
bool BuildFixedTables(Huffman& lit, Huffman& dist) {
    uint8_t lengths[288];
    for (int i = 0; i < 288; i++) {
        if (i < 144)      lengths[i] = 8;
        else if (i < 256) lengths[i] = 9;
        else if (i < 280) lengths[i] = 7;
        else              lengths[i] = 8;
    }
    if (!lit.Build(lengths, 288)) return false;
    // 距离表复用同一个数组的前 30 项：固定块里 30 个距离码全是 5 位
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    return dist.Build(lengths, 30);
}

// 读并建立动态 Huffman 表（BTYPE=10）。
// 格式：HLIT(5 位)+257 个 literal/length 码长、HDIST(5 位)+1 个距离码长、
// HCLEN(4 位)+4 个"码长码"的码长；后者的码长按 kCodeLenOrder 的顺序存，
// 因为只有前若干个非零，前面的先读、后面缺的按 0 处理。
bool BuildDynamicTables(BitReader& br, Huffman& lit, Huffman& dist) {
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!br.GetBits(5, hlit)) return false;
    if (!br.GetBits(5, hdist)) return false;
    if (!br.GetBits(4, hclen)) return false;

    int nlit  = (int)hlit + 257;   // literal/length 码长个数（257..286）
    int ndist = (int)hdist + 1;    // 距离码长个数（1..30）
    int nclen = (int)hclen + 4;    // 实际读出的码长码个数（4..19）
    // 规范上限：literal/length 最多 286 个（287 是保留码），距离最多 30 个。
    // 超出说明数据损坏，直接拒绝，避免后面写 lengths[] 越界。
    if (nlit > 286 || ndist > 30 || nclen > 19) return false;

    uint8_t clens[19];
    for (int i = 0; i < 19; i++) clens[i] = 0;
    for (int i = 0; i < nclen; i++) {
        uint32_t v = 0;
        if (!br.GetBits(3, v)) return false;   // 每个码长占 3 位
        clens[kCodeLenOrder[i]] = (uint8_t)v;
    }

    // 第一层：用这 19 个码长建"码长码"的 Huffman 表
    Huffman clh;
    if (!clh.Build(clens, 19)) return false;

    // 第二层：用码长码解出 literal/length + distance 共 nlit+ndist 个码长。
    // 符号 0..15 是字面码长；16 表示"重复上一个码长 3~6 次"；
    // 17/18 表示"重复 0（即一整段没有码字的符号）3~10 / 11~138 次"。
    // 这两条压缩手段对"大段连续 0 长度"特别有效：动态块里通常只有二十几个
    // 符号真的用到，其余几百个位置全是 0 长度。
    uint8_t lengths[286 + 30];
    int total = nlit + ndist;
    int i = 0;
    while (i < total) {
        int sym = clh.Decode(br);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int repeat = 0;
        uint8_t value = 0;
        if (sym == 16) {
            if (i == 0) return false;   // 16 没有"上一个码长"可复用，非法
            value = lengths[i - 1];
            uint32_t v = 0;
            if (!br.GetBits(2, v)) return false;
            repeat = 3 + (int)v;
        } else if (sym == 17) {
            uint32_t v = 0;
            if (!br.GetBits(3, v)) return false;
            repeat = 3 + (int)v;
        } else {  // sym == 18
            uint32_t v = 0;
            if (!br.GetBits(7, v)) return false;
            repeat = 11 + (int)v;
        }
        // 重复次数必须正好落在剩余槽位内，越界就是损坏（也防止写越界）
        if (repeat > total - i) return false;
        for (int k = 0; k < repeat; k++) lengths[i++] = value;
    }

    if (!lit.Build(lengths, nlit)) return false;
    return dist.Build(lengths + nlit, ndist);
}

// 解一个 Huffman 数据块（固定/动态共用这段主循环）。
// 语法：一串"字面量 / 长度-距离对"，直到读到一个 end-of-block（符号 256）。
bool InflateHuffmanBlock(BitReader& br, const Huffman& lit, const Huffman& dist, OutBuf& ob) {
    for (;;) {
        int sym = lit.Decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            if (!ob.Put((uint8_t)sym)) return false;   // 普通字面量：直接吐出这个字节
            continue;
        }
        if (sym == 256) return true;   // 块结束符

        // 257..285 是长度码；286/287 在流里出现即非法（保留码）
        sym -= 257;
        if (sym >= 29) return false;

        uint32_t extra = 0;
        if (!br.GetBits(kLenExtra[sym], extra)) return false;
        size_t len = (size_t)kLenBase[sym] + extra;

        int dsym = dist.Decode(br);
        if (dsym < 0 || dsym >= 30) return false;
        if (!br.GetBits(kDistExtra[dsym], extra)) return false;
        size_t d = (size_t)kDistBase[dsym] + extra;

        if (!ob.CopyBack(d, len)) return false;
    }
}

// 真正的解压主循环。返回 false 时 out 已被清空（调用方不必自己清）。
bool InflateImpl(const uint8_t* src, size_t srcLen, std::string& out,
                 size_t expectedSize, size_t* consumed) {
    out.clear();
    if (consumed) *consumed = 0;
    if (src == nullptr) return false;

    BitReader br;
    br.Init(src, srcLen);
    OutBuf ob(out);
    ob.Init(expectedSize);

    int last = 0;   // BFINAL：1 表示这是最后一块
    do {
        uint32_t bfinal = 0, btype = 0;
        if (!br.GetBits(1, bfinal)) return false;
        if (!br.GetBits(2, btype)) return false;
        last = (bfinal != 0) ? 1 : 0;

        if (btype == 0) {
            // ---- 类型 00：stored（不压缩，原样存放）----
            // 先对齐到字节边界：上面刚读了 3 个控制位，而 LEN/NLEN 和数据
            // 是整字节的，不对齐会从半截字节开始读。
            br.AlignToByte();
            // 对齐后缓冲里可能还压着几个完整的字节（refill 时提前装的），
            // 把它们"退回去"再按偏移直接访问，避免两套读取路径。
            br.pos = br.ConsumedBytes();
            br.count = 0;
            br.buf = 0;
            if (br.pos + 4 > srcLen) return false;
            uint32_t len  = ReadU16(src + br.pos);
            uint32_t nlen = ReadU16(src + br.pos + 2);
            // LEN 与其反码 NLEN 必须逐位互补，否则块长被破坏
            if ((len ^ nlen) != 0xFFFFu) return false;
            br.pos += 4;
            if (len > srcLen - br.pos) return false;
            for (uint32_t k = 0; k < len; k++) {
                if (!ob.Put(src[br.pos + k])) return false;
            }
            br.pos += len;
        } else if (btype == 1) {
            // ---- 类型 01：固定 Huffman ----
            Huffman lit, dist;
            if (!BuildFixedTables(lit, dist)) return false;
            if (!InflateHuffmanBlock(br, lit, dist, ob)) return false;
        } else if (btype == 2) {
            // ---- 类型 10：动态 Huffman ----
            Huffman lit, dist;
            if (!BuildDynamicTables(br, lit, dist)) return false;
            if (!InflateHuffmanBlock(br, lit, dist, ob)) return false;
        } else {
            return false;   // 类型 11 在 RFC 1951 里保留未用
        }
    } while (!last);

    ob.Trim();
    if (consumed) *consumed = br.ConsumedBytes();
    return true;
}

// -----------------------------------------------------------------------------
// 名字比较：忽略大小写 + 把反斜杠当正斜杠
//
// EPUB/DOCX 里的路径大小写经常不统一（有的写作 OEBPS/Text/Ch1.xhtml），
// 而 zip 规范本身是区分大小写的。阅读器按名字找章节时宽容一点更实用。
// 只对 ASCII 字母做大小写折叠：UTF-8 的多字节序列里每个字节都 >= 0x80，
// 不会误伤中文。
// -----------------------------------------------------------------------------
inline char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

inline char SlashNorm(char c) {
    return (c == '\\') ? '/' : c;   // 有的写入方用 Windows 反斜杠
}

bool NameEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (LowerAscii(SlashNorm(a[i])) != LowerAscii(SlashNorm(b[i]))) return false;
    }
    return true;
}

}  // namespace

// =============================================================================
// ZipArchive 的实现
// =============================================================================

// 解析中央目录。
//
// ZIP 的目录结构是"从后往前"的：文件末尾是 EOCD 记录，里面写着中央目录
// 的起始偏移和条目数；中央目录本身是一串固定 46 字节头 + 变长字段的记录。
// 必须从尾巴找 EOCD 而不是从头部顺序扫，原因是 zip 允许在文件最前面塞
// 自解压代码（SFX），也可能带注释，头部偏移并不固定。
bool ZipArchive::Load(const uint8_t* data, size_t size) {
    m_data = data;
    m_size = size;
    m_ok = false;
    m_entries.clear();
    if (data == nullptr || size < 22) return false;   // 22 = EOCD 的最小长度

    // EOCD 后面可能跟着最长 65535 字节的注释，所以从末尾往前最多回退
    // 65535+22 字节就够了；再往前不可能有合法的 EOCD。
    const size_t kMaxComment = 65535;
    size_t tail = (size > kMaxComment + 22) ? (size - (kMaxComment + 22)) : 0;

    size_t eocd = SIZE_MAX;
    // 从 size-22 处开始往前找（i-- > tail 的写法让 i 从 size-22 递减到 tail）
    for (size_t i = size - 22 + 1; i-- > tail; ) {
        if (HasSignature(data + i, 0x05, 0x06)) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) return false;   // 根本不是 zip（或已经损坏）
    if (eocd + 22 > size) return false;

    // EOCD 字段布局（相对 EOCD 起点）：
    //   +8  本卷条目数(2)  +10 总条目数(2)
    //   +12 中央目录大小(4) +16 中央目录偏移(4)  +20 注释长度(2)
    uint32_t total  = ReadU16(data + eocd + 10);
    uint32_t cdSize = ReadU32(data + eocd + 12);
    uint32_t cdOff  = ReadU32(data + eocd + 16);

    // ZIP64 的标记是这些字段取最大值（0xFFFF / 0xFFFFFFFF），真实值放在
    // ZIP64 扩展记录里。小说/EPUB 不可能超过 4GB，明确不支持，直接失败。
    if (total == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return false;
    if (cdOff > size) return false;
    if (cdSize > size - cdOff) return false;   // 目录本身越界，文件被截断了

    size_t p = cdOff;
    size_t cdEnd = (size_t)cdOff + cdSize;
    m_entries.reserve(total);

    for (uint32_t k = 0; k < total; k++) {
        // 中央目录记录固定头 46 字节，签名 "PK\x01\x02"
        if (cdEnd - p < 46) return false;
        if (!HasSignature(data + p, 0x01, 0x02)) return false;

        ZipEntry e;
        // 字段布局（相对记录起点）：
        //   +10 压缩方法(2)     +20 压缩后大小(4)  +24 原始大小(4)
        //   +28 名字长度(2)     +30 扩展字段长度(2) +32 注释长度(2)
        //   +42 本地文件头偏移(4)  +46 名字
        e.method     = ReadU16(data + p + 10);
        e.compSize   = ReadU32(data + p + 20);
        e.uncompSize = ReadU32(data + p + 24);
        uint32_t nameLen    = ReadU16(data + p + 28);
        uint32_t extraLen   = ReadU16(data + p + 30);
        uint32_t commentLen = ReadU16(data + p + 32);
        e.localOff   = ReadU32(data + p + 42);

        // 单条目级别的 ZIP64 标记同样不支持（大文件才需要）
        if (e.compSize == 0xFFFFFFFFu || e.uncompSize == 0xFFFFFFFFu ||
            e.localOff == 0xFFFFFFFFu) {
            return false;
        }

        // 变长部分：名字 + 扩展字段 + 注释，三段紧挨着，长度都记录在头上
        size_t rec = (size_t)46 + nameLen + extraLen + commentLen;
        if (rec > cdEnd - p) return false;
        e.name.assign(reinterpret_cast<const char*>(data + p + 46), nameLen);

        // 统一斜杠方向：后面拼路径、比较名字都只用正斜杠
        for (size_t c = 0; c < e.name.size(); c++) {
            if (e.name[c] == '\\') e.name[c] = '/';
        }

        m_entries.push_back(std::move(e));
        p += rec;
    }

    m_ok = true;
    return true;
}

// 按名字找条目下标；忽略大小写和斜杠方向，找不到返回 -1。
int ZipArchive::Find(const std::string& name) const {
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (NameEquals(m_entries[i].name, name)) return (int)i;
    }
    return -1;
}

// 按下标解压一个条目。
//
// 为什么还要再跳一次本地文件头：中央目录里存了 localOff（本地头的偏移），
// 但数据并不在 localOff 处，而是 localOff + 30 + 名字长度 + 扩展长度。
// 中央目录的 compSize 才是权威值（用 data descriptor 写入时本地头的
// 大小字段可能是 0），所以这里只用本地头来算数据起点。
bool ZipArchive::ReadIndex(size_t i, std::string& out) const {
    out.clear();
    if (!m_ok || i >= m_entries.size() || m_data == nullptr) return false;
    const ZipEntry& e = m_entries[i];

    // 本地文件头固定 30 字节，签名 "PK\x03\x04"
    size_t off = e.localOff;
    if (off > m_size || m_size - off < 30) return false;
    if (!HasSignature(m_data + off, 0x03, 0x04)) return false;

    // 本地头的名字/扩展字段长度可能与中央目录不同（尤其是扩展字段），
    // 所以必须按本地头自己的值来跳过，不能用中央目录的。
    uint32_t nameLen  = ReadU16(m_data + off + 26);
    uint32_t extraLen = ReadU16(m_data + off + 28);
    size_t dataOff = off + 30 + (size_t)nameLen + extraLen;
    if (dataOff > m_size) return false;
    if ((size_t)e.compSize > m_size - dataOff) return false;

    if (e.method == 0) {
        // stored：数据区就是原始内容，直接按中央目录记录的长度拷出来
        out.assign(reinterpret_cast<const char*>(m_data + dataOff), e.compSize);
        return true;
    }
    if (e.method == 8) {
        // deflate：解压后必须恰好等于中央目录声明的原始大小，否则视为损坏。
        // （这个校验很关键：截断的文件也能解出"一部分正确内容"，不校验就会
        //   把半章文本当成完整章节显示出来。）
        size_t consumed = 0;
        if (!InflateRaw(m_data + dataOff, e.compSize, out, e.uncompSize, &consumed)) {
            out.clear();
            return false;
        }
        if (out.size() != (size_t)e.uncompSize) {
            out.clear();
            return false;
        }
        return true;
    }
    // 其它压缩方法（bzip2 / lzma / 加密等）本项目不支持
    return false;
}

// 按名字解压：先查下标，再走 ReadIndex
bool ZipArchive::ReadName(const std::string& name, std::string& out) const {
    int idx = Find(name);
    if (idx < 0) {
        out.clear();
        return false;
    }
    return ReadIndex((size_t)idx, out);
}

// 公开入口：把实现细节留在匿名 namespace 里，这里只做一层薄包装
bool InflateRaw(const uint8_t* src, size_t srcLen, std::string& out,
                size_t expectedSize, size_t* consumed) {
    return InflateImpl(src, srcLen, out, expectedSize, consumed);
}
