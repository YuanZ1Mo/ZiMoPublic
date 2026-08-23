#include "zm_util_zlib.h"

#include <zlib.h>

#include <cstring>

namespace
{
// ZIP 常量(APPNOTE 6.3.10)
constexpr uint32_t kLocalHeaderSig  = 0x04034B50;
constexpr uint32_t kCentralSig      = 0x02014B50;
constexpr uint32_t kEocdSig         = 0x06054B50;
constexpr uint16_t kDeflateMethod   = 8;
constexpr uint16_t kStoreMethod     = 0;
constexpr uint16_t kVersionNeeded   = 20;
constexpr uint16_t kVersionMadeBy   = 20;   // 20 = 2.0;OS 0(FAT,目录属性友好)

void PutU16(std::vector<unsigned char>& v, uint16_t x)
{
    v.push_back((unsigned char)(x & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
}

void PutU32(std::vector<unsigned char>& v, uint32_t x)
{
    v.push_back((unsigned char)(x & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
    v.push_back((unsigned char)((x >> 16) & 0xFF));
    v.push_back((unsigned char)((x >> 24) & 0xFF));
}

// 覆盖 m_out[pos..] 处的 4 字节(回填 CRC/大小)
void PatchU32(std::vector<unsigned char>& v, size_t pos, uint32_t x)
{
    v[pos + 0] = (unsigned char)(x & 0xFF);
    v[pos + 1] = (unsigned char)((x >> 8) & 0xFF);
    v[pos + 2] = (unsigned char)((x >> 16) & 0xFF);
    v[pos + 3] = (unsigned char)((x >> 24) & 0xFF);
}
} // namespace

ZipWriter::ZipWriter() = default;

ZipWriter::~ZipWriter()
{
    if (m_strm)
        deflateEnd(static_cast<z_stream*>(m_strm));
}

bool ZipWriter::BeginEntry(const std::string& name, bool isDir)
{
    if (m_finished || m_curActive || name.empty())
        return false;

    // local file header(30B)+ 文件名
    m_curHeaderPos = m_out.size();
    PutU32(m_out, kLocalHeaderSig);
    PutU16(m_out, kVersionNeeded);
    PutU16(m_out, 0x0800);                  // flags: UTF-8 文件名(否则解压器按 CP437 解读中文乱码)
    PutU16(m_out, isDir ? kStoreMethod : kDeflateMethod);
    PutU16(m_out, 0);                       // mod time(0)
    PutU16(m_out, 0);                       // mod date(0)
    PutU32(m_out, 0);                       // crc(占位,EndEntry 回填)
    PutU32(m_out, 0);                       // compressed size(占位)
    PutU32(m_out, 0);                       // uncompressed size(占位)
    PutU16(m_out, (uint16_t)name.size());
    PutU16(m_out, 0);                       // extra len
    m_out.insert(m_out.end(), name.begin(), name.end());

    m_curName = name;
    m_curDir = isDir;
    m_curCrc = 0;
    m_curCompressed = 0;
    m_curUncompressed = 0;
    m_curActive = true;

    if (isDir)
        return true;

    // 初始化 deflate 流(raw,windowBits=-15)
    auto* zs = new z_stream();
    std::memset(zs, 0, sizeof(z_stream));
    if (deflateInit2(zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
    {
        delete zs;
        m_curActive = false;
        return false;
    }
    m_strm = zs;
    return true;
}

bool ZipWriter::Write(const unsigned char* data, size_t len)
{
    if (!m_curActive || m_curDir || !data || len == 0)
        return false;

    m_curCrc = (uint32_t)crc32(m_curCrc, data, (uInt)len);
    m_curUncompressed += len;
    return DeflateChunk(data, len, false);
}

bool ZipWriter::DeflateChunk(const unsigned char* data, size_t len, bool final)
{
    auto* zs = static_cast<z_stream*>(m_strm);
    if (!zs)
        return false;

    zs->next_in = const_cast<Bytef*>(data);
    zs->avail_in = (uInt)len;

    std::vector<unsigned char> buf(64 * 1024);   // 堆分配:防调用线程栈溢出
    do
    {
        zs->next_out = buf.data();
        zs->avail_out = (uInt)buf.size();
        int rc = deflate(zs, final ? Z_FINISH : Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
            return false;
        size_t produced = buf.size() - zs->avail_out;
        m_curCompressed += produced;
        m_out.insert(m_out.end(), buf.begin(), buf.begin() + produced);
        if (rc == Z_STREAM_END)
        {
            if (final)
            {
                deflateEnd(zs);
                delete zs;
                m_strm = nullptr;
            }
            break;
        }
    } while (zs->avail_out == 0);

    return true;
}

bool ZipWriter::EndEntry()
{
    if (!m_curActive)
        return false;

    if (!m_curDir)
    {
        // 结束 deflate 流(flush 残留)
        if (!DeflateChunk(nullptr, 0, true))
        {
            m_curActive = false;
            return false;
        }
    }

    // 回填 local header:CRC(14)/csize(18)/usize(22)
    // 注意:m_curHeaderPos 是当前 m_out 段内偏移(Patch 目标),条目跨段时 seg rel 不变;
    // EndEntry 与 BeginEntry 之间不会发生 Drain,此时 m_drained 即本条目的绝对分段基准
    size_t p = m_curHeaderPos;
    PatchU32(m_out, p + 14, m_curCrc);
    PatchU32(m_out, p + 18, (uint32_t)m_curCompressed);
    PatchU32(m_out, p + 22, (uint32_t)m_curUncompressed);

    // ★ 中央目录需要绝对偏移:已 Drain 字节 + 段内起点。
    //   只记段内偏移会把第二个条目起全部指回错误位置(内容重复/CRC 损坏)
    m_entries.push_back({m_curName, m_curCrc, m_curCompressed, m_curUncompressed,
                         m_curDir, m_drained + m_curHeaderPos});
    m_curActive = false;
    return true;
}

bool ZipWriter::Finish()
{
    if (m_finished || m_curActive)
        return false;
    m_finished = true;

    // central directory
    size_t cdStart = m_out.size();
    for (const auto& e : m_entries)
    {
        PutU32(m_out, kCentralSig);
        PutU16(m_out, kVersionMadeBy);
        PutU16(m_out, kVersionNeeded);
        PutU16(m_out, 0x0800);                  // flags: UTF-8 文件名
        PutU16(m_out, e.isDir ? kStoreMethod : kDeflateMethod);
        PutU16(m_out, 0);                       // time/date
        PutU16(m_out, 0);
        PutU32(m_out, e.crc);
        PutU32(m_out, (uint32_t)e.compressed);
        PutU32(m_out, (uint32_t)e.uncompressed);
        PutU16(m_out, (uint16_t)e.name.size());
        PutU16(m_out, 0);                       // extra
        PutU16(m_out, 0);                       // comment
        PutU16(m_out, 0);                       // disk
        PutU16(m_out, 0);                       // internal attr
        PutU32(m_out, e.isDir ? 0x10 : 0);      // external attr(目录位)
        PutU32(m_out, (uint32_t)e.headerPos);   // local header 偏移(EndEntry 已换算为绝对)
        m_out.insert(m_out.end(), e.name.begin(), e.name.end());
    }
    size_t cdSize = m_out.size() - cdStart;

    // EOCD(22B)
    PutU32(m_out, kEocdSig);
    PutU16(m_out, 0);                       // disk number
    PutU16(m_out, 0);                       // cd start disk
    PutU16(m_out, (uint16_t)m_entries.size());
    PutU16(m_out, (uint16_t)m_entries.size());
    PutU32(m_out, (uint32_t)cdSize);
    PutU32(m_out, (uint32_t)(m_drained + cdStart));   // 含已 Drain 的 local 数据偏移
    PutU16(m_out, 0);                       // comment len
    return true;
}
