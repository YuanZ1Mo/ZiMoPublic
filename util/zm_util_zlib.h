#ifndef ZIP_WRITER_H
#define ZIP_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 流式 zip 写入器(仅写入,不解压)
 *
 * 格式按 APPNOTE:Local File Header + raw deflate 数据 + Central Directory + EOCD。
 * 压缩用 zlib raw deflate(deflateInit2 windowBits=-15)+ crc32(),零新增第三方库。
 *
 * 用法(单线程顺序):
 *   ZipWriter w;
 *   w.BeginEntry("a.txt", false);
 *   w.Write(data, len); ...        // 可分块调用,内部边压边积
 *   w.EndEntry();
 *   w.BeginEntry("empty/", true);  // 空目录条目:无数据
 *   w.EndEntry();
 *   w.Finish();                    // 完成;此后 Data() 为完整 zip 字节
 */
class ZipWriter
{
public:
    ZipWriter();
    ~ZipWriter();

    /** @brief 开始一个条目;isDir=true 时调用方不应 Write(数据长度 0) */
    bool BeginEntry(const std::string& name, bool isDir);

    /** @brief 写入一块数据(累计 CRC-32 + deflate) */
    bool Write(const unsigned char* data, size_t len);

    /** @brief 结束当前条目(回填 CRC/压缩大小到 local header,登记 central directory) */
    bool EndEntry();

    /** @brief 完成整个 zip(写 central directory + EOCD);此后不得再 BeginEntry */
    bool Finish();

    /** @brief 已生成的 zip 字节;Finish 后完整,调用方逐块取走发送 */
    const std::vector<unsigned char>& Data() const { return m_out; }

    /** @brief 取走并清空已生成字节(流式发送:逐条目生成后取走,内存 O(单条目));
     *         累加已 Drain 字节数,保证 Finish 的 central directory 偏移正确 */
    void Drain(std::vector<unsigned char>& out)
    {
        out.swap(m_out);
        m_drained += out.size();
        m_out.clear();
    }

private:
    struct EntryInfo
    {
        std::string name;
        uint32_t    crc = 0;
        uint64_t    compressed   = 0;
        uint64_t    uncompressed = 0;
        bool        isDir = false;
        uint64_t    headerPos = 0;   // m_out 中 local header 起点(回填用)
    };

    bool DeflateChunk(const unsigned char* data, size_t len, bool final);

    std::vector<unsigned char> m_out;
    std::vector<EntryInfo>     m_entries;

    // 当前条目 deflate 状态
    void*       m_strm = nullptr;     // z_stream*
    std::string m_curName;
    bool        m_curDir = false;
    bool        m_curActive = false;  // BeginEntry 后、EndEntry 前
    uint32_t    m_curCrc = 0;
    uint64_t    m_curCompressed = 0;
    uint64_t    m_curUncompressed = 0;
    uint64_t    m_curHeaderPos = 0;
    size_t      m_drained = 0;      ///< 已 Drain 字节数(central offset 计算)
    bool        m_finished = false;
};

#endif // ZIP_WRITER_H
