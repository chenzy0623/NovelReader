// zip.h —— 极简 ZIP 读取器 + 原始 DEFLATE 解压（RFC 1951）
//
// 用途：EPUB 与 DOCX 本质都是 ZIP 包（内含 XML/XHTML），要读它们就得先能解压。
// 为了不引入任何第三方库（保持单 exe、无 DLL 依赖），这里自己实现：
//   1) ZIP 中央目录解析（只支持单卷、不支持 ZIP64 加密）；
//   2) DEFLATE 解压（stored / 固定 Huffman / 动态 Huffman 三种块）。
//
// 设计约定：
//   * 整个 zip 文件由调用方一次性读进内存，ZipArchive 只持有指针，不拷贝数据；
//   * 条目名统一用正斜杠，查找时忽略大小写（EPUB 里的路径大小写不统一很常见）；
//   * 解压失败一律返回 false，不抛异常（项目里没开异常）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 中央目录里的一条记录（只保留用得上的字段）
struct ZipEntry {
    std::string name;      // 条目名，例如 "word/document.xml"、"OEBPS/text/ch01.xhtml"
    uint16_t    method;    // 压缩方法：0 = 不压缩(stored)，8 = deflate
    uint32_t    compSize;  // 压缩后大小（字节）
    uint32_t    uncompSize;// 原始大小（字节）
    uint32_t    localOff;  // 本地文件头在文件中的偏移
};

class ZipArchive {
public:
    // 解析整个 zip（data 指向完整文件内容，size 是文件字节数）
    // 返回 true 表示中央目录解析成功；之后就可以 Find / ReadIndex / ReadName
    bool Load(const uint8_t* data, size_t size);

    bool   Ok() const { return m_ok; }
    size_t Count() const { return m_entries.size(); }
    const ZipEntry& At(size_t i) const { return m_entries[i]; }

    // 按名字查找条目下标，找不到返回 -1（忽略大小写）
    int Find(const std::string& name) const;

    // 按下标解压到 out（out 会被清空再赋值）
    bool ReadIndex(size_t i, std::string& out) const;

    // 按名字解压到 out
    bool ReadName(const std::string& name, std::string& out) const;

private:
    const uint8_t* m_data = nullptr;
    size_t         m_size = 0;
    bool           m_ok   = false;
    std::vector<ZipEntry> m_entries;
};

// 原始 DEFLATE 解压（不带 zlib 头，正是 zip 条目里存的那种数据）
//   src/srcLen     ：压缩数据
//   out            ：解压结果
//   expectedSize   ：期望的原始大小（来自中央目录的 uncompSize，用于预分配与校验）
//   consumed       ：可选，输出实际消耗的压缩字节数
bool InflateRaw(const uint8_t* src, size_t srcLen, std::string& out,
                size_t expectedSize, size_t* consumed = nullptr);
