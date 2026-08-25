#ifndef ZM_UTIL_ZIPFILE_H
#define ZM_UTIL_ZIPFILE_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

/**
 * @brief 流式 zip 文件写入器(输出到 fd;后台预打包与分享打包统一组件)
 *
 * 与 ZipWriter(内存版)→ 本组件取代之,唯一 zip 写入器:
 * - 直接 `_write` 到文件描述符:内存 O(单个 deflate 块 64KB),条目/包大小无内存约束;
 * - ZIP64 自适应(APPNOTE 6.3.10):local header 无条件 bit-3(0x0008)数据流模式
 *   + ZIP64 extra(usize/csize 占位),条目结束写 24B ZIP64 数据描述符;
 *   中央目录字段溢出时 32 位槽 = 0xFFFFFFFF + ZIP64 extra(仅溢出字段);
 *   条目数 >65535 或 cd 大小/偏移 >4GB → ZIP64 EOCD + locator + 哨兵常规 EOCD。
 *   >4GB 单文件/整包不再损坏;现代解压器(资源管理器/7-Zip/WinRAR/zipfile/bsdtar)均可读。
 *
 * 用法(单线程顺序):
 *   ZipFileWriter w(fd);
 *   w.BeginEntry("a.txt", false);
 *   w.Write(data, len); ...            // 可分块;64KB 块为爽点
 *   w.EndEntry();
 *   w.BeginEntry("empty/", true);      // 空目录条目
 *   w.EndEntry();
 *   w.Finish();                        // 必须:中央目录 + EOCD;此后不得再 BeginEntry
 *
 * 失败语义:写入失败快速失败布尔返回 + Failed() 坠死(不保证恢复);
 * 取消:SetCancelHook 返回 false 时当前 deflate 中止(不置 Failed,调用方决定处置)。
 * fd 归调用方(打开/关闭由调用方;writer 只写不关)。
 */
class ZipFileWriter
{
public:
    explicit ZipFileWriter(int fd);
    ~ZipFileWriter();

    ZipFileWriter(const ZipFileWriter&) = delete;
    ZipFileWriter& operator=(const ZipFileWriter&) = delete;

    /** @brief 开始一个条目;isDir=true 时调用方不应 Write */
    bool BeginEntry(const std::string& name, bool isDir);

    /** @brief 写入一块数据(累计 CRC-32 + raw deflate 增量写 fd) */
    bool Write(const unsigned char* data, size_t len);

    /** @brief 结束当前条目(flush deflate + 写 ZIP64 数据描述符 + 登记中央目录) */
    bool EndEntry();

    /** @brief 完成整个 zip(中央目录 + ZIP64/常规 EOCD);此后不得再 BeginEntry */
    bool Finish();

    /** @brief 已写入字节数(含 header/描述符;Finish 后含中央目录+EOCD)——绝对偏移基准 */
    uint64_t Written() const { return m_wrote; }

    /** @brief 是否已处于失败状态(任一次磁盘写入失败) */
    bool Failed() const { return m_failed; }

    /** @brief 取消检查钩子(每个 deflate 块处理前调用);false = 立即中止当前条目 */
    void SetCancelHook(std::function<bool()> f) { m_cancel = std::move(f); }

private:
    struct EntryInfo
    {
        std::string name;
        uint32_t    crc = 0;
        uint64_t    compressed   = 0;
        uint64_t    uncompressed = 0;
        bool        isDir = false;
        uint64_t    headerPos = 0;   // local header 绝对偏移
    };

    bool WriteTo(const void* data, size_t len);   // 全量写 fd(循环处理短写);失败置 Failed
    bool DeflateChunk(const unsigned char* data, size_t len, bool final);

    int  m_fd = -1;
    bool m_failed = false;
    bool m_aborted = false;           // 取消钩子触发:文件已半成品,此后所有操作拒绝
    bool m_finished = false;
    bool m_curActive = false;
    std::function<bool()> m_cancel;

    uint64_t m_wrote = 0;                 // 已写字节数(恒等于绝对偏移)
    std::string m_curName;
    bool   m_curDir = false;
    uint32_t m_curCrc = 0;
    uint64_t m_curCompressed = 0;
    uint64_t m_curUncompressed = 0;
    uint64_t m_curHeaderPos = 0;

    void* m_strm = nullptr;               // z_stream*(仅活动文件条目期间)
    std::vector<EntryInfo> m_entries;
};

#endif // ZM_UTIL_ZIPFILE_H
