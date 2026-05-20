#ifndef ZM_UTIL_FILE_H
#define ZM_UTIL_FILE_H

#include "zm_util_str.h"

/**
 * @brief 文件操作工具类，提供文件读写、复制、删除、目录管理、路径解析、哈希等静态方法
 *
 * 所有方法均为静态方法，无需实例化即可使用。
 * 底层使用 Win32 API（CreateFileA / WriteFile / ReadFile 等），仅适用于 Windows 平台。
 */
class ZmFile
{
public:
    // ── read / write ──────────────────────────────────────────────────

    /**
     * @brief 将文件全部内容读入字节缓冲区
     *
     * 内部先通过 ReadEx 获取文件大小并预分配缓冲区，再分块读取填入。
     * 对于空文件（0 字节），直接返回 true，content 保持空。
     *
     * @param filepath  待读取文件的路径
     * @param content   输出缓冲区，读取完成后其内容为文件的完整二进制数据
     * @return true 读取成功（包括空文件）；false 文件打开失败或数据不完整
     *
     * @example
     *   ZmByteBuffer buf;
     *   if (ZmFile::Read("config.bin", buf))
     *   {
     *       // buf.Head() 指向文件数据，buf.Size() 为文件大小
     *   }
     */
    static bool   Read(const char* filepath, ZmByteBuffer& content);

    /**
     * @brief 将二进制数据写入文件（覆盖写入）
     *
     * 若文件已存在则截断为 0 后写入；若不存在则创建新文件。
     * 内部按 1GB 分块写入，支持超过 4GB 的大文件。
     *
     * @param filepath  目标文件路径
     * @param data      待写入的数据指针
     * @param len       待写入的数据长度（字节）
     * @return true 写入成功（全部数据已写入）；false 创建文件或写入失败
     *
     * @example
     *   BYTE data[] = {0x01, 0x02, 0x03};
     *   ZmFile::Write("output.bin", data, sizeof(data));
     */
    static bool   Write(const char* filepath, const BYTE* data, size_t len);

    /**
     * @brief 将二进制数据追加到文件末尾
     *
     * 若文件不存在则自动创建。内部使用 SetFilePointer 定位到文件末尾后写入。
     * 多线程/多进程并发追加同一文件时，数据可能交叉，不保证原子性。
     *
     * @param filepath  目标文件路径
     * @param data      待追加的数据指针
     * @param len       待追加的数据长度（字节）
     * @return true 追加成功；false 文件操作失败
     *
     * @example
     *   const char* log = "2026-01-01 event\n";
     *   ZmFile::Append("app.log", (const BYTE*)log, strlen(log));
     */
    static bool   Append(const char* filepath, const BYTE* data, size_t len);

    /**
     * @brief 分块读取文件，通过回调逐块传递数据
     *
     * 回调调用顺序：
     *   1. ondata(NULL, filesize)  — 预告知文件大小，调用方可用于预分配内存
     *   2. ondata(data, chunkSize) — 每次传递一个块的数据，可能被调用多次
     *   3. ondata(NULL, 0)         — 读取结束（无论成功或失败）
     *
     * @param filepath  待读取文件的路径
     * @param ondata    数据回调函数
     *   - data=NULL, dlen>0 : 文件大小提示，可用于预分配缓冲区
     *   - data!=NULL, dlen>0 : 一块实际数据，dlen 为本块字节长度
     *   - data=NULL, dlen=0  : 读取结束信号
     *   - 返回 true 继续读取，返回 false 中止读取
     * @return 实际读取的总字节数；文件打开失败时返回 0
     *
     * @example
     *   size_t total = ZmFile::ReadEx("data.bin",
     *       [](const BYTE* data, size_t len) -> bool {
     *           if (data) { process(data, len); }
     *           return true;  // 返回 false 可提前中止
     *       });
     */
    static size_t ReadEx(const char* filepath, std::function<bool(const BYTE* data, size_t dlen)> ondata);

    /**
     * @brief 将文件全部内容读取为文本字符串
     *
     * 内部调用 Read 读取二进制数据后转换为 std::string。
     * 自动检测并跳过 UTF-8 BOM（EF BB BF）前缀。
     *
     * @param filepath  待读取文件的路径
     * @param content   输出字符串，读取成功后包含文件文本内容
     * @return true 读取成功；false 文件打开失败
     *
     * @example
     *   std::string text;
     *   if (ZmFile::ReadString("config.json", text))
     *   {
     *       // text 包含文件全部文本（已去除 UTF-8 BOM）
     *   }
     */
    static bool   ReadString(const char* filepath, std::string& content);

    /**
     * @brief 将字符串写入文件（覆盖写入）
     *
     * 等价于 Write(filepath, content.data(), content.size())。
     *
     * @param filepath  目标文件路径
     * @param content   待写入的字符串
     * @return true 写入成功；false 写入失败
     *
     * @example
     *   ZmFile::WriteString("notes.txt", "Hello, World!");
     */
    static bool   WriteString(const char* filepath, const std::string& content);

    // ── copy / rename / delete ────────────────────────────────────────

    /**
     * @brief 复制文件
     *
     * 底层调用 Win32 CopyFileA，由操作系统完成复制，支持任意大小文件。
     * 若目标文件已存在则直接覆盖。
     *
     * @param srcpath  源文件路径
     * @param dstpath  目标文件路径
     * @return true 复制成功；false 复制失败
     *
     * @example
     *   ZmFile::Copy("a.txt", "backup/a.txt");
     */
    static bool   Copy(const char* srcpath, const char* dstpath);

    /**
     * @brief 重命名或移动文件
     *
     * 底层使用 MoveFileExA + MOVEFILE_REPLACE_EXISTING：
     *   - 若 newpath 已存在则直接替换
     *   - 同盘符下为瞬间重命名；跨盘符时由系统执行复制+删除
     *
     * @param oldpath  原文件路径
     * @param newpath  新文件路径
     * @return true 重命名成功；false 重命名失败
     *
     * @example
     *   ZmFile::Rename("temp.dat", "data/final.dat");
     */
    static bool   Rename(const char* oldpath, const char* newpath);

    /**
     * @brief 删除指定文件
     *
     * 底层调用 DeleteFileA。若文件不存在或被占用，操作静默失败（无返回值）。
     *
     * @param filepath  待删除文件的路径
     */
    static void   Delete(const char* filepath);

    /**
     * @brief 递归删除目录及其所有内容
     *
     * 遍历目录下的所有文件和子目录，先移除只读属性再逐一删除，最后删除目录本身。
     * 对于符号链接目录，仅删除链接本身，不递归进入。
     *
     * @param dirname  待删除目录的路径
     */
    static void   DeleteDir(const char* dirname);

    // ── query ─────────────────────────────────────────────────────────

    /**
     * @brief 判断文件或目录是否存在
     *
     * @param filepath  待检测路径
     * @return true 路径存在（可以是文件或目录）；false 路径不存在
     *
     * @example
     *   if (ZmFile::Exists("config.ini")) { ... }
     */
    static bool   Exists(const char* filepath);

    /**
     * @brief 判断指定路径是否为目录
     *
     * @param filepath  待检测路径
     * @return true 是目录；false 不是目录或路径不存在
     *
     * @example
     *   if (ZmFile::IsDirectory("C:\\logs")) { ... }
     */
    static bool   IsDirectory(const char* filepath);

    /**
     * @brief 获取文件大小
     *
     * 使用 _stat64 获取文件大小，支持超过 4GB 的大文件。
     *
     * @param filepath  文件路径
     * @return 文件大小（字节）；文件不存在或获取失败时返回 0
     *
     * @example
     *   size_t sz = ZmFile::GetSize("video.mp4");
     */
    static size_t GetSize(const char* filepath);

    /**
     * @brief 通过文件描述符获取文件大小
     *
     * 使用 _fstat64 获取文件大小，支持超过 4GB 的大文件。
     *
     * @param fd  已打开的文件描述符（fileno() 返回值或 open() 返回值）
     * @return 文件大小（字节）；获取失败时返回 0
     *
     * @example
     *   FILE* fp = fopen("data.bin", "rb");
     *   size_t sz = ZmFile::GetSize(fileno(fp));
     */
    static size_t GetSize(int fd);

    // ── path helpers ──────────────────────────────────────────────────

    /**
     * @brief 从完整路径中提取文件名部分
     *
     * @param filepath  文件路径，如 "C:\\Users\\doc.txt"
     * @return 指向 filepath 内部文件名起始位置的指针，如 "doc.txt"；
     *         filepath 本身不含路径分隔符时返回 filepath 原指针
     *
     * @example
     *   const char* name = ZmFile::FilenameOfPath("C:\\A\\B\\file.txt");
     *   // name == "file.txt"
     */
    static const char* FilenameOfPath(const char* filepath);

    /**
     * @brief 从文件路径中提取扩展名（不含点号）
     *
     * @param filepath  文件路径，如 "report.pdf"
     * @return 扩展名字符串指针（指向 filepath 内部），如 "pdf"；
     *         无扩展名时返回 NULL
     *
     * @example
     *   const char* ext = ZmFile::ExtensionOfPath("archive.tar.gz");
     *   // ext == "gz"（返回最后一个点号之后的部分）
     */
    static const char* ExtensionOfPath(const char* filepath);

    /**
     * @brief 从文件路径中提取目录部分
     *
     * @param filepath  文件路径，如 "C:\\A\\B\\file.txt"
     * @param buf       输出缓冲区，用于接收目录部分，如 "C:\\A\\B"
     * @param buflen    输出缓冲区大小（字节）
     * @return true 成功提取目录部分；false 参数无效或路径无目录部分
     *
     * @example
     *   char dir[MAX_PATH];
     *   ZmFile::DirNameOfPath("C:\\A\\B\\file.txt", dir, sizeof(dir));
     *   // dir == "C:\\A\\B"
     */
    static bool        DirNameOfPath(const char* filepath, char* buf, size_t buflen);

    /**
     * @brief 获取当前进程 exe 文件所在目录
     *
     * @param buf     输出缓冲区，接收目录路径（如 "C:\\Program Files\\MyApp"）
     * @param buflen  输出缓冲区大小（字节），建议至少 MAX_PATH
     * @return true 获取成功；false 参数无效或获取失败
     *
     * @example
     *   char dir[MAX_PATH];
     *   ZmFile::GetModuleDir(dir, sizeof(dir));
     *   // dir == "C:\\Program Files\\MyApp"
     */
    static bool        GetModuleDir(char* buf, size_t buflen);

    // ── directory ─────────────────────────────────────────────────────

    /**
     * @brief 递归创建多级目录
     *
     * 若目标目录已存在则直接返回 true。
     * 会自动去除路径尾部的 '/' 或 '\'，然后从父目录开始逐级向下创建。
     *
     * @param dirname  目标目录路径，如 "C:\\A\\B\\C"
     * @return true 目录已存在或创建成功；false 创建失败
     *
     * @example
     *   ZmFile::MakeDirs("C:\\A\\B\\C");
     *   // 将依次创建 C:\A、C:\A\B、C:\A\B\C（不存在的层级）
     */
    static bool MakeDirs(const char* dirname);

    // ── hash ──────────────────────────────────────────────────────────

    /**
     * @brief 计算文件的 MD5 哈希值并以十六进制字符串输出
     *
     * 使用 OpenSSL EVP 接口计算，分块读取文件，内存占用恒定。
     * 输出为 32 字符的小写十六进制字符串（不含结尾 '\0' 需要至少 33 字节）。
     *
     * @note MD5 不适用于安全敏感场景，仅用于文件校验、去重等非安全用途。
     *
     * @param filepath  文件路径
     * @param md5       输出缓冲区，需至少 33 字节（32 字符 + '\0'）
     * @return true 计算成功；false 文件打开失败或读取为空
     *
     * @example
     *   char hash[33];
     *   if (ZmFile::MD5HashHex("data.bin", hash))
     *   {
     *       // hash == "d41d8cd98f00b204e9800998ecf8427e"
     *   }
     */
    static bool MD5HashHex(const char* filepath, char* md5);
};

#endif /* ZM_UTIL_FILE_H */
