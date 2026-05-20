#include "zm_util_file.h"

#include "zm_util_sys.h"
#include "../spdlog/zm_logger.h"

#include <openssl/evp.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

/**
 * @brief 文件分块读取时每块的大小，16 KB
 */
#define ZM_FILE_READ_BLOCK_SIZE     0x4000


// ═══════════════════════════════════════════════════════════════════════
//  read / write
// ═══════════════════════════════════════════════════════════════════════

bool ZmFile::Read(const char* filepath, ZmByteBuffer& content)
{
    size_t offset = 0;

    /**
     * @brief 记录 ReadEx 回调通知的文件大小，用于区分"空文件"和"读取失败"
     */
    size_t fsize = 0;

    auto on_data = [&](const BYTE* data, size_t dlen)
        {
            if (dlen > 0)
            {
                if (data)
                {
                    // 将当前块数据写入缓冲区的对应偏移位置
                    content.Put(data, dlen, offset);
                    offset += dlen;
                }
                else
                {
                    // data==NULL && dlen>0: ReadEx 的文件大小提示，用于预分配缓冲区
                    fsize = dlen;
                    content.Reset(dlen);
                    offset = 0;
                }
            }
            return true;
        };

    size_t total = ReadEx(filepath, on_data);

    // fsize==0 表示空文件（GetFileSizeEx 返回 0），视为读取成功
    if (0 == fsize)
    {
        return true;
    }

    // 校验实际读到的数据总量是否与文件大小一致
    return total > 0 && (offset == total);
}

bool ZmFile::Write(const char* filepath, const BYTE* data, size_t len)
{
    bool ret = false;
    HANDLE handle = INVALID_HANDLE_VALUE;

    do
    {
        // CREATE_ALWAYS: 若文件存在则截断为 0 后写入；不存在则创建
        handle = ::CreateFileA(filepath, GENERIC_WRITE, 0,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (INVALID_HANDLE_VALUE == handle)
        {
            PUBLIC_LOG_ERROR("Writing file, invoking CreateFileA('{}') failed: {}", filepath, ZmSystem::ErrMsg(-1));
            break;
        }

        // 分块写入，每块最大 1GB，避免 DWORD 截断导致大数据写入不完整
        size_t remaining = len;
        const BYTE* ptr = data;
        while (remaining > 0)
        {
            DWORD chunk = (DWORD)(ZM_MIN(remaining, 0x40000000));
            DWORD written = 0;
            if (!::WriteFile(handle, ptr, chunk, &written, NULL) || written != chunk)
            {
                PUBLIC_LOG_ERROR("Writing file, invoking WriteFile('{}') failed: {}", filepath, ZmSystem::ErrMsg(-1));
                break;
            }
            ptr += written;
            remaining -= written;
        }
        ret = (0 == remaining);
    } while (false);

    if (INVALID_HANDLE_VALUE != handle)
    {
        CloseHandle(handle);
    }
    return ret;
}

bool ZmFile::Append(const char* filepath, const BYTE* data, size_t len)
{
    bool ret = false;
    HANDLE hFile = INVALID_HANDLE_VALUE;

    do
    {
        // OPEN_ALWAYS: 若文件存在则打开；不存在则创建
        hFile = ::CreateFileA(filepath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (INVALID_HANDLE_VALUE == hFile)
        {
            PUBLIC_LOG_ERROR("Append file '{}' invoke CreateFile failed: {}", filepath, ZmSystem::ErrMsg(-1));
            break;
        }

        // 定位到文件末尾，用于追加写入
        if (INVALID_SET_FILE_POINTER == ::SetFilePointer(hFile, 0, NULL, FILE_END))
        {
            PUBLIC_LOG_ERROR("Append file '{}' invoke SetFilePointer failed: {}", filepath, ZmSystem::ErrMsg(-1));
            break;
        }

        // 分块写入，逻辑与 Write 相同
        size_t remaining = len;
        const BYTE* ptr = data;
        while (remaining > 0)
        {
            DWORD chunk = (DWORD)(ZM_MIN(remaining, 0x40000000));
            DWORD written = 0;
            if (!::WriteFile(hFile, ptr, chunk, &written, NULL) || written != chunk)
            {
                PUBLIC_LOG_ERROR("Append file '{}' invoke WriteFile failed: {}", filepath, ZmSystem::ErrMsg(-1));
                break;
            }
            ptr += written;
            remaining -= written;
        }
        ret = (0 == remaining);
    } while (false);

    if (INVALID_HANDLE_VALUE != hFile)
    {
        CloseHandle(hFile);
    }
    return ret;
}

size_t ZmFile::ReadEx(const char* filepath, std::function<bool(const BYTE* data, size_t dlen)> ondata)
{
    size_t       total = 0;
    HANDLE       handle = INVALID_HANDLE_VALUE;
    ZmByteBuffer buf(ZM_FILE_READ_BLOCK_SIZE);
    DWORD        cnt = 0;

    do
    {
        // FILE_SHARE_READ: 允许其他进程同时读取，但阻止写入
        handle = ::CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (INVALID_HANDLE_VALUE == handle)
        {
            PUBLIC_LOG_ERROR("Read file '{}' invoke CreateFileA failed: {}", filepath, ZmSystem::ErrMsg(-1));
            break;
        }

        // 使用 GetFileSizeEx 获取 64 位文件大小，支持超过 4GB 的大文件
        LARGE_INTEGER liSize;
        if (!GetFileSizeEx(handle, &liSize))
        {
            PUBLIC_LOG_ERROR("Read file '{}' invoke GetFileSizeEx failed: {}", filepath, ZmSystem::ErrMsg(-1));
            break;
        }
        // 通知调用方文件大小，调用方可据此预分配内存
        ondata(NULL, (size_t)liSize.QuadPart);

        // 循环分块读取，直到 ReadFile 返回的字节数小于缓冲区大小（表示到达文件末尾）
        while (true)
        {
            buf.Zero();
            cnt = (DWORD)buf.Size();
            if (!::ReadFile(handle, buf.Head(), (DWORD)buf.Size(), &cnt, NULL))
            {
                PUBLIC_LOG_ERROR("Read file '{}' invoke ReadFile failed: {}", filepath, ZmSystem::ErrMsg(-1));
                break;
            }
            if (cnt > 0)
            {
                total += cnt;
                // 若回调返回 true 且当前块是满块（cnt == buf.Size()），继续读取下一块
                if (ondata(buf.Head(), cnt) && cnt == buf.Size())
                {
                    continue;
                }
            }
            // cnt==0 表示文件末尾，或回调返回 false 中止读取
            break;
        }
    } while (false);

    if (INVALID_HANDLE_VALUE != handle)
    {
        CloseHandle(handle);
    }
    // 无论成功或失败，始终发送结束信号
    ondata(NULL, 0);
    return total;
}

bool ZmFile::ReadString(const char* filepath, std::string& content)
{
    ZmByteBuffer buf;
    if (!Read(filepath, buf))
    {
        return false;
    }
    size_t size = buf.Size();
    if (size > 0)
    {
        const BYTE* data = buf.Head();
        // 检测并跳过 UTF-8 BOM（EF BB BF），避免读取后字符串开头出现不可见字符
        if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        {
            data += 3;
            size -= 3;
        }
        content.assign(reinterpret_cast<const char*>(data), size);
    }
    else
    {
        content.clear();
    }
    return true;
}

bool ZmFile::WriteString(const char* filepath, const std::string& content)
{
    return Write(filepath, (const BYTE*)content.data(), content.size());
}

// ═══════════════════════════════════════════════════════════════════════
//  copy / rename / delete
// ═══════════════════════════════════════════════════════════════════════

bool ZmFile::Copy(const char* srcpath, const char* dstpath)
{
    // CopyFileA 第三个参数 FALSE 表示目标文件已存在时直接覆盖
    if (FALSE == ::CopyFileA(srcpath, dstpath, FALSE))
    {
        PUBLIC_LOG_ERROR("Copy '{}' -> '{}' failed: {}", srcpath, dstpath, ZmSystem::ErrMsg(-1));
        return false;
    }
    return true;
}

bool ZmFile::Rename(const char* oldpath, const char* newpath)
{
    // MOVEFILE_REPLACE_EXISTING: 若 newpath 已存在则替换，避免先删后移的竞争窗口
    if (FALSE == ::MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING))
    {
        PUBLIC_LOG_ERROR("Rename '{}' -> '{}' failed: {}", oldpath, newpath, ZmSystem::ErrMsg(-1));
        return false;
    }
    return true;
}

void ZmFile::Delete(const char* filepath)
{
    DeleteFileA(filepath);
}

void ZmFile::DeleteDir(const char* dirname)
{
    HANDLE          hFile;
    WIN32_FIND_DATAA fi;

    char            pattern[MAX_PATH] = { 0 };
    char            path[MAX_PATH] = { 0 };

    // 构造搜索模式 "dirname\*"，匹配目录下的所有条目
    snprintf(pattern, sizeof(pattern), "%s\\*", dirname);
    hFile = FindFirstFileA(pattern, &fi);
    if (INVALID_HANDLE_VALUE != hFile)
    {
        do
        {
            // 跳过当前目录 "." 和父目录 ".." 条目，避免无限递归
            if (0 != strcmp(fi.cFileName, ".") && 0 != strcmp(fi.cFileName, ".."))
            {
                snprintf(path, sizeof(path), "%s\\%s", dirname, fi.cFileName);
                // 先移除只读属性，否则后续删除操作会因权限不足而失败
                if (fi.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                {
                    SetFileAttributesA(path, fi.dwFileAttributes & (~FILE_ATTRIBUTE_READONLY));
                }
                if (FILE_ATTRIBUTE_DIRECTORY & fi.dwFileAttributes)
                {
                    DeleteDir(path);
                }
                else
                {
                    ::DeleteFileA(path);
                }
            }
        } while (::FindNextFileA(hFile, &fi) == TRUE);

        ::FindClose(hFile);
    }
    // 目录内容清空后，删除目录本身
    ::RemoveDirectoryA(dirname);
}

// ═══════════════════════════════════════════════════════════════════════
//  query
// ═══════════════════════════════════════════════════════════════════════

bool ZmFile::Exists(const char* filepath)
{
    return TRUE == ::PathFileExistsA(filepath);
}

bool ZmFile::IsDirectory(const char* filepath)
{
    return TRUE == ::PathIsDirectoryA(filepath);
}

size_t ZmFile::GetSize(const char* filepath)
{
    struct _stat64 buf;
    return (0 == _stat64(filepath, &buf)) ? (size_t)buf.st_size : 0;
}

size_t ZmFile::GetSize(int fd)
{
    struct _stat64 buf;
    return (0 == _fstat64(fd, &buf)) ? (size_t)buf.st_size : 0;
}

// ═══════════════════════════════════════════════════════════════════════
//  path helpers
// ═══════════════════════════════════════════════════════════════════════

const char* ZmFile::FilenameOfPath(const char* filepath)
{
    // PathFindFileNameA 内部处理了 '/' 和 '\' 两种分隔符，以及 UNC 路径等边界情况
    const char* fn = ::PathFindFileNameA(filepath);
    return fn ? fn : filepath;
}

const char* ZmFile::ExtensionOfPath(const char* filepath)
{
    const char* ext = ::PathFindExtensionA(filepath);
    // PathFindExtensionA 返回指向点号 '.' 的指针，+1 跳过点号返回纯扩展名
    // 若路径无扩展名（如 "README"），返回值指向 '\0'，此时返回 NULL
    return (ext && ext[0] != '\0') ? (ext + 1) : NULL;
}

bool ZmFile::DirNameOfPath(const char* filepath, char* buf, size_t buflen)
{
    if (!filepath || !buf || buflen == 0)
    {
        return false;
    }
    snprintf(buf, buflen, "%s", filepath);
    buf[buflen - 1] = '\0';
    // PathRemoveFileSpecA 原地移除路径中最后一个分隔符及其后的文件名部分
    return TRUE == ::PathRemoveFileSpecA(buf);
}

bool ZmFile::GetModuleDir(char* buf, size_t buflen)
{
    if (!buf || buflen == 0)
    {
        return false;
    }
    // NULL 表示当前进程的 exe 文件路径
    if (0 == ::GetModuleFileNameA(NULL, buf, (DWORD)buflen))
    {
        return false;
    }
    // 去除文件名部分，仅保留目录路径
    return TRUE == ::PathRemoveFileSpecA(buf);
}

// ═══════════════════════════════════════════════════════════════════════
//  directory
// ═══════════════════════════════════════════════════════════════════════

bool ZmFile::MakeDirs(const char* dirname)
{
    // 目标目录已存在，直接返回成功
    if (::PathIsDirectoryA(dirname))
    {
        return true;
    }
    char tmp[MAX_PATH] = { 0 };
    snprintf(tmp, sizeof(tmp), "%s", dirname);

    // 去除尾部斜杠，否则 PathRemoveFileSpecA 无法正确剥离路径组件
    size_t len = strlen(tmp);
    while (len > 0 && (tmp[len - 1] == '\\' || tmp[len - 1] == '/'))
    {
        tmp[--len] = '\0';
    }
    if (len == 0)
    {
        return false;
    }

    // 递归确保父目录存在：
    //   1. PathRemoveFileSpecA 剥离最后一个路径组件得到父路径
    //   2. tmp[0]!='\0' 防止 PathRemoveFileSpecA 产生空路径导致无限递归
    //   3. 父目录不存在时递归创建
    //   4. 父目录就绪后，CreateDirectoryA 创建当前层级
    if (::PathRemoveFileSpecA(tmp) && tmp[0] != '\0' && !::PathIsDirectoryA(tmp))
    {
        if (!MakeDirs(tmp))
        {
            return false;
        }
    }
    return TRUE == ::CreateDirectoryA(dirname, NULL);
}

// ═══════════════════════════════════════════════════════════════════════
//  hash
// ═══════════════════════════════════════════════════════════════════════

bool ZmFile::MD5HashHex(const char* filepath, char* md5)
{
    // 使用 EVP 接口（OpenSSL 3.0+ 推荐方式），替代已废弃的 MD5_Init/Update/Final
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        return false;
    }
    if (1 != EVP_DigestInit_ex(ctx, EVP_md5(), NULL))
    {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    BYTE md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;

    auto on_data = [&](const BYTE* data, size_t dlen)
        {
            if (NULL != data && dlen > 0)
            {
                EVP_DigestUpdate(ctx, data, dlen);
            }
            return true;
        };

    // 分块读取文件并逐块喂入 MD5 计算上下文，内存占用恒定（约 16KB）
    size_t total = ReadEx(filepath, on_data);

    EVP_DigestFinal_ex(ctx, md, &md_len);
    EVP_MD_CTX_free(ctx);

    if (0 == total)
    {
        return false;
    }
    // 将 16 字节二进制哈希转换为 32 字符小写十六进制字符串
    ZmString::Hex(md, md5, md_len);
    return true;
}
