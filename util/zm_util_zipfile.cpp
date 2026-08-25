#include "zm_util_zipfile.h"

#include <zlib.h>

#include <io.h>
#include <cstdio>
#include <cstring>

namespace
{
// ZIP 常量(APPNOTE 6.3.10)
constexpr uint32_t kLocalHeaderSig  = 0x04034B50;
constexpr uint32_t kCentralSig      = 0x02014B50;
constexpr uint32_t kEocdSig         = 0x06054B50;
constexpr uint32_t kEocd64Sig       = 0x06064B50;
constexpr uint32_t kEocd64LocSig    = 0x07064B50;
constexpr uint16_t kDeflateMethod   = 8;
constexpr uint16_t kStoreMethod     = 0;
constexpr uint16_t kVersion20       = 20;
constexpr uint16_t kVersion45       = 45;   // ZIP64 版本号
constexpr uint16_t kZip64ExtraTag   = 0x0001;
constexpr uint64_t kMaxU32          = 0xFFFFFFFFull;

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

void PutU64(std::vector<unsigned char>& v, uint64_t x)
{
    PutU32(v, (uint32_t)(x & 0xFFFFFFFFull));
    PutU32(v, (uint32_t)(x >> 32));
}
} // namespace

ZipFileWriter::ZipFileWriter(int fd)
    : m_fd(fd)
{
}

ZipFileWriter::~ZipFileWriter()
{
    if (m_strm)
        deflateEnd(static_cast<z_stream*>(m_strm));
}

bool ZipFileWriter::WriteTo(const void* data, size_t len)
{
    if (m_failed)
        return false;
    if (m_fd < 0)
    {
        m_failed = true;   // 无效 fd = 硬错误
        return false;
    }
    const char* p = static_cast<const char*>(data);
    size_t left = len;
    while (left > 0)
    {
        int n = _write(m_fd, p, (unsigned int)left);
        if (n <= 0)
        {
            m_failed = true;
            return false;
        }
        p += n;
        left -= (size_t)n;
        m_wrote += (uint64_t)n;
    }
    return true;
}

bool ZipFileWriter::BeginEntry(const std::string& name, bool isDir)
{
    if (m_finished || m_curActive || m_aborted || name.empty() || name.size() > 0xFFFF)
        return false;

    // local file header(30B)+ 文件名(可选 ZIP64 extra)
    // 文件条目:bit3(0x0008)= CRC/大小在尾部数据描述符;32 位大小槽一律 0xFFFFFFFF + ZIP64
    // extra 占位(usize/csize)——流式写入时大小未知,描述符模式的标准写法
    m_curHeaderPos = m_wrote;
    m_curName = name;
    m_curDir = isDir;
    m_curCrc = 0;
    m_curCompressed = 0;
    m_curUncompressed = 0;
    m_curActive = true;

    std::vector<unsigned char> h;
    PutU32(h, kLocalHeaderSig);
    PutU16(h, kVersion45);                          // ZIP64 版本(V2.0 也可;45 更严谨)
    PutU16(h, isDir ? 0x0800 : 0x0808);             // UTF-8 |(文件)数据描述符
    PutU16(h, isDir ? kStoreMethod : kDeflateMethod);
    PutU16(h, 0);                                   // mod time
    PutU16(h, 0);                                   // mod date
    PutU32(h, 0);                                   // crc(文件条目在描述符)
    if (isDir)
    {
        PutU32(h, 0);                               // csize
        PutU32(h, 0);                               // usize
        PutU16(h, (uint16_t)name.size());
        PutU16(h, 0);                               // extra len
    }
    else
    {
        PutU32(h, (uint32_t)kMaxU32);               // csize:ZIP64 哨兵
        PutU32(h, (uint32_t)kMaxU32);               // usize:ZIP64 哨兵
        PutU16(h, (uint16_t)name.size());
        PutU16(h, 20);                              // extra len(2 tag + 2 len + 16)
    }
    // ★ 结构顺序 = header + 文件名 + extra(extra 在前会导致解析器将 extra 当作文件名读取)
    h.insert(h.end(), name.begin(), name.end());
    if (!isDir)
    {
        PutU16(h, kZip64ExtraTag);
        PutU16(h, 16);
        PutU64(h, 0);                               // usize 占位
        PutU64(h, 0);                               // csize 占位(显式 0:描述符为准)
    }

    if (!WriteTo(h.data(), h.size()))
        return false;

    if (isDir)
        return true;

    auto* zs = new z_stream();
    std::memset(zs, 0, sizeof(z_stream));
    // 压缩等级 1(Z_BEST_SPEED):打包内容以已压缩格式为主(zip/exe/iso),level 6 收益趋零,
    // 反而单线程打满 4-5 倍 CPU 时间;level 1 对不可压/已压内容提速 2-4x,体积几乎无损
    if (deflateInit2(zs, Z_BEST_SPEED, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
    {
        delete zs;
        m_curActive = false;
        return false;
    }
    m_strm = zs;
    return true;
}

bool ZipFileWriter::DeflateChunk(const unsigned char* data, size_t len, bool final)
{
    auto* zs = static_cast<z_stream*>(m_strm);
    if (!zs || m_failed || m_aborted)
        return false;
    if (m_cancel && !m_cancel())
    {
        m_aborted = true;   // 取消 = 文件已半成品,此后一切操作拒绝(防 Finish 产出残缺包)
        return false;
    }

    zs->next_in = const_cast<Bytef*>(data);
    zs->avail_in = (uInt)len;

    std::vector<unsigned char> buf(64 * 1024);
    do
    {
        // ★ 每 deflate 块前检查(大单次 Write 可能输出多块)
        if (m_aborted)
            return false;
        if (m_cancel && !m_cancel())
        {
            m_aborted = true;
            return false;
        }
        zs->next_out = buf.data();
        zs->avail_out = (uInt)buf.size();
        int rc = deflate(zs, final ? Z_FINISH : Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
            return false;
        size_t produced = buf.size() - zs->avail_out;
        if (produced > 0)
        {
            m_curCompressed += produced;
            if (!WriteTo(buf.data(), produced))
                return false;
        }
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

bool ZipFileWriter::Write(const unsigned char* data, size_t len)
{
    if (!m_curActive || m_curDir || m_aborted || !data || len == 0)
        return false;
    m_curCrc = (uint32_t)crc32(m_curCrc, data, (uInt)len);
    m_curUncompressed += len;
    return DeflateChunk(data, len, false);
}

bool ZipFileWriter::EndEntry()
{
    if (!m_curActive || m_aborted)
        return false;

    if (!m_curDir)
    {
        // 结束 deflate 流(flush 残留)
        if (!DeflateChunk(nullptr, 0, true))
        {
            m_curActive = false;
            return false;
        }
        // 24B ZIP64 数据描述符:crc(4)+ csize(8)+ usize(8)
        std::vector<unsigned char> d;
        PutU32(d, m_curCrc);
        PutU64(d, m_curCompressed);
        PutU64(d, m_curUncompressed);
        if (!WriteTo(d.data(), d.size()))
        {
            m_curActive = false;
            return false;
        }
    }

    m_entries.push_back({m_curName, m_curCrc, m_curCompressed, m_curUncompressed,
                         m_curDir, m_curHeaderPos});
    m_curActive = false;
    return true;
}

bool ZipFileWriter::Finish()
{
    if (m_finished || m_curActive || m_failed || m_aborted)
        return false;
    m_finished = true;

    // ── 中央目录 ────────────────────────────────────────────────────────────
    uint64_t cdStart = m_wrote;
    for (const auto& e : m_entries)
    {
        bool zip64 = (e.compressed > kMaxU32) || (e.uncompressed > kMaxU32) ||
                     (e.headerPos > kMaxU32);
        uint16_t extraLen = 0;
        uint16_t verNeed = zip64 ? kVersion45 : kVersion20;

        std::vector<unsigned char> c;
        PutU32(c, kCentralSig);
        PutU16(c, zip64 ? kVersion45 : kVersion20);   // version made by
        PutU16(c, verNeed);
        PutU16(c, e.isDir ? 0x0800 : 0x0808);         // 同 local:UTF-8 | 描述符
        PutU16(c, e.isDir ? kStoreMethod : kDeflateMethod);
        PutU16(c, 0);                                  // time
        PutU16(c, 0);                                  // date
        PutU32(c, e.crc);
        PutU32(c, (uint32_t)(zip64 ? kMaxU32 : e.compressed));
        PutU32(c, (uint32_t)(zip64 ? kMaxU32 : e.uncompressed));
        PutU16(c, (uint16_t)e.name.size());
        PutU16(c, 0);                                  // extra len(回填)
        PutU16(c, 0);                                  // comment
        PutU16(c, 0);                                  // disk
        PutU16(c, 0);                                  // internal attr
        PutU32(c, e.isDir ? 0x10 : 0);                 // external attr
        PutU32(c, (uint32_t)(zip64 ? kMaxU32 : e.headerPos));

        if (zip64)
        {
            // ZIP64 extra:仅溢出字段,顺序 usize/csize/offset(APPNOTE 6.3.10)
            std::vector<unsigned char> ex;
            PutU16(ex, kZip64ExtraTag);
            PutU16(ex, 0);                             // data len(回填)
            if (e.uncompressed > kMaxU32) PutU64(ex, e.uncompressed);
            if (e.compressed   > kMaxU32) PutU64(ex, e.compressed);
            if (e.headerPos    > kMaxU32) PutU64(ex, e.headerPos);
            size_t dataLen = ex.size() - 4;
            ex[2] = (unsigned char)(dataLen & 0xFF);
            ex[3] = (unsigned char)((dataLen >> 8) & 0xFF);
            // 46B 固定头:extraLen 字段位于 [30]/[31](前面插的占位 0 在此回填)
            c[30] = (unsigned char)(ex.size() & 0xFF);
            c[31] = (unsigned char)((ex.size() >> 8) & 0xFF);
            c.insert(c.end(), ex.begin(), ex.end());
        }
        c.insert(c.end(), e.name.begin(), e.name.end());

        if (!WriteTo(c.data(), c.size()))
            return false;
    }
    uint64_t cdSize = m_wrote - cdStart;

    // ── EOCD(22B;必要时 ZIP64 EOCD + locator 前置)──────────────────────────
    bool needZip64Eocd = (m_entries.size() > 0xFFFFull) || (cdSize > kMaxU32) ||
                         (cdStart > kMaxU32);
    if (needZip64Eocd)
    {
        std::vector<unsigned char> e64;
        PutU32(e64, kEocd64Sig);
        PutU64(e64, 44);                                 // EOCD64 记录大小(除 12B 头)
        PutU16(e64, kVersion45);
        PutU16(e64, kVersion45);
        PutU32(e64, 0);                                  // this disk
        PutU32(e64, 0);                                  // cd start disk
        PutU64(e64, (uint64_t)m_entries.size());
        PutU64(e64, (uint64_t)m_entries.size());
        PutU64(e64, cdSize);
        PutU64(e64, cdStart);
        if (!WriteTo(e64.data(), e64.size()))
            return false;

        std::vector<unsigned char> l;
        PutU32(l, kEocd64LocSig);
        PutU32(l, 0);                                    // disk with EOCD64
        PutU64(l, m_wrote - e64.size());                 // ZIP64 EOCD 起始绝对偏移
        PutU32(l, 1);                                    // total disks
        if (!WriteTo(l.data(), l.size()))
            return false;
    }

    std::vector<unsigned char> e;
    PutU32(e, kEocdSig);
    PutU16(e, 0);                                        // this disk
    PutU16(e, 0);                                        // cd start disk
    if (needZip64Eocd)
    {
        PutU16(e, 0xFFFF);                               // 条目数哨兵
        PutU16(e, 0xFFFF);
        PutU32(e, (uint32_t)kMaxU32);                    // cd size 哨兵
        PutU32(e, (uint32_t)kMaxU32);                    // cd offset 哨兵
    }
    else
    {
        PutU16(e, (uint16_t)m_entries.size());
        PutU16(e, (uint16_t)m_entries.size());
        PutU32(e, (uint32_t)cdSize);
        PutU32(e, (uint32_t)cdStart);
    }
    PutU16(e, 0);                                        // comment len
    return WriteTo(e.data(), e.size());
}
