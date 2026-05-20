/**
 * @file zm_ssl_ctx.h
 * @brief SSL/TLS 上下文管理模块
 *
 * 封装 OpenSSL，提供客户端/服务端 SSL 上下文的创建、证书加载、密钥配置、
 * 证书解析及 SSL 指纹校验等功能，同时支持国密 SM 系列算法。
 */

#ifndef ZM_SSL_CTX_H
#define ZM_SSL_CTX_H
#include "../util/zm_util_str.h"

#include <openssl/include/openssl/ssl.h>
#include <event2/util.h>

/**
 * @brief X509 证书信息结构体
 *
 * 存储 PEM 格式证书解析后的关键信息，用于展示或校验证书属性。
 */
typedef struct
{
    long        version;        /**< 证书版本: 0=V1, 1=V2, 2=V3 */
    uint64_t    not_before;     /**< 证书生效时间 (time_t) */
    uint64_t    not_after;      /**< 证书过期时间 (time_t) */
    char        serial[64];     /**< 证书序列号 (十进制或冒号分隔的十六进制) */
    char        issuer[256];    /**< 颁发者 (Issuer DN) */
    char        subject[1024];  /**< 主题 (Subject DN) */
} ZM_X509_INFO;

/**
 * @brief SSL 上下文管理器
 *
 * 提供全局 SSL_CTX 的懒加载、证书/密钥加载、X509 解析、
 * SSL 指纹校验等功能。所有方法均为静态方法，无需实例化。
 */
class ZmSSLContext
{
public:

    /* ========== Certificate & Key ========== */

    /**
     * @brief 从 PKCS12 文件加载客户端双认证证书
     *
     * 读取 PFX 文件内容后委托给 SetPfxCertBufferPass() 完成。
     *
     * @param sslctx   目标 SSL 上下文
     * @param pfxfile  PKCS12 (PFX) 文件路径
     * @param pass     PFX 文件保护密码
     * @return 0 表示成功，其他值含义同 SetPfxCertBufferPass()
     */
    static int  SetPfxCertFilePass(SSL_CTX* sslctx, const char* pfxfile, const char* pass);

    /**
     * @brief 从内存缓冲区加载 PKCS12 双认证证书
     *
     * 解析 PKCS12 格式数据，提取证书和私钥并加载到 SSL_CTX 中，
     * 同时校验证书与私钥是否匹配。
     *
     * @param sslctx  目标 SSL 上下文
     * @param pfxbuf  PKCS12 二进制数据指针
     * @param len     数据长度
     * @param pass    PKCS12 保护密码
     * @return 0: 成功
     *         2:  解析 PKCS12 数据失败 (d2i_PKCS12_bio)
     *         3:  PKCS12 内部解析失败 (PKCS12_parse)
     *         11: 加载证书到 SSL_CTX 失败
     *         12: 加载私钥到 SSL_CTX 失败
     *         13: 证书与私钥不匹配
     *
     * @note 当 PKCS12 MAC 校验失败时，会调用 PKCS12_set_mac() 重新设置 MAC，
     *       以兼容某些非标准 PFX 文件。
     */
    static int  SetPfxCertBufferPass(SSL_CTX* sslctx, const unsigned char* pfxbuf, size_t len, const char* pass);

    /**
     * @brief 从 PEM 内存缓冲区加载服务端证书链
     *
     * 参考 SSL_CTX_use_certificate_chain_file() 源码实现。
     * 加载第一个证书作为主体证书，后续证书作为额外证书链加入 SSL_CTX。
     *
     * @param sslctx  目标 SSL 上下文
     * @param buffer  PEM 格式证书数据（可包含多个证书）
     * @param len     数据长度
     * @return 1 表示成功，0 表示失败
     *
     * @note 加载成功后会清除旧的证书链 (SSL_CTX_clear_chain_certs)。
     */
    static int  UseCertBuffer(SSL_CTX* sslctx, const unsigned char* buffer, size_t len);

    /**
     * @brief 从 PEM 文件加载加密私钥
     *
     * @param sslctx    目标 SSL 上下文
     * @param file      PEM 格式私钥文件路径
     * @param pass      私钥加密密码
     * @return 1 表示成功，-1 表示失败
     */
    static int  UsePrivateKeyFilePass(SSL_CTX* sslctx, const char* file, const char* pass);

    /**
     * @brief 从 PEM 内存缓冲区加载加密私钥
     *
     * @param sslctx  目标 SSL 上下文
     * @param buffer  PEM 格式私钥数据
     * @param len     数据长度
     * @param pass    私钥加密密码
     * @return 1 表示成功，-1 表示失败
     */
    static int  UsePrivateKeyBufferPass(SSL_CTX* sslctx, const unsigned char* buffer, size_t len, const char* pass);

    /* ========== Certificate parsing ========== */

    /**
     * @brief 将 X509 证书信息输出到日志
     *
     * @param x  X509 证书对象指针
     */
    static void DumpX509(const X509* x);

    /**
     * @brief DER 格式证书转 PEM 格式
     *
     * @param[out] pem   输出的 PEM 数据缓冲区
     * @param      der   DER 格式证书数据
     * @param      dlen  DER 数据长度
     */
    static void DER2PEM(ZmByteBuffer& pem, BYTE* der, size_t dlen);

    /**
     * @brief 解析 PEM 格式 X509 证书，提取关键信息
     *
     * @param[out] xinfo  接收解析结果的结构体指针
     * @param      pem    PEM 格式证书数据
     * @param      plen   数据长度
     * @return true 解析成功，false 解析失败
     *
     * @note 时间字段 (not_before/not_after) 未做时区转换，基于本地时区解释。
     *       序列号短整数以十进制输出，长序列号以冒号分隔的十六进制输出。
     */
    static bool ParseX509(ZM_X509_INFO* xinfo, const char* pem, size_t plen);

    /**
     * @brief 从 PKCS12 数据中提取证书和私钥为 PEM 格式
     *
     * 解析 PKCS12 数据，将私钥、主体证书、CA 证书链依次以 PEM 格式添加到 pems 列表中。
     * 支持标准 OpenSSL 和修改版 OpenSSL (SM2) 两种解析路径。
     *
     * @param[out] pems      接收 PEM 数据的字符串列表
     * @param      data      PKCS12 二进制数据
     * @param      dlen      数据长度
     * @param      password  PKCS12 保护密码
     *
     * @note 当标准 PKCS12_parse 失败且错误码为 0x100D5010 (SM2 证书私钥解析失败) 时，
     *       会尝试仅解析证书和 CA 链（跳过私钥），以兼容 SM2 证书。
     */
    static void ExtractCert(ZmStringList& pems, const BYTE* data, size_t dlen, const char* password);

    /**
    * @brief 创建客户端 SSL 上下文
    *
    * @param smx_version  SMX 版本号，0 表示标准 TLS
    * @return 新创建的 SSL_CTX 指针，失败返回 NULL
    */
    static SSL_CTX* MakeClientCTX();

    static void     Release();

    static bool          ValidateSSLFingerprint(evutil_socket_t fd, SSL* ssl, const char* host, uint16_t port);
    static bool          ValidateSSLFingerprint(SSL* ssl, const char* fingerprint);

private:
    /**
     * @brief 从 BIO 加载加密私钥到 SSL_CTX
     *
     * @param sslctx  目标 SSL 上下文
     * @param bio     已打开的 BIO（包含 PEM 私钥数据）
     * @param pass    私钥加密密码，通过 PEM_read_bio_PrivateKey 的回调参数传入
     * @return 1 表示成功，-1 表示失败
     */
    static int      UsePrivateKeyBIOPass(SSL_CTX* sslctx, void* bio, const char* pass);

    ~ZmSSLContext();
};

/**
 * @brief OpenSSL 内存 BIO 的 RAII 封装
 *
 * 简化 OpenSSL BIO 的内存操作，构造时创建 BIO，析构时自动释放。
 * 支持从已有数据创建只读 BIO，或创建空的可写 BIO。
 */
class ZmMemoryBIO
{
public:
    /**
     * @brief 构造内存 BIO
     *
     * @param data  已有数据指针，非 NULL 时创建只读内存 BIO；NULL 时创建可写空 BIO
     * @param len   数据长度
     */
    ZmMemoryBIO(const BYTE* data = NULL, size_t len = 0);

    /** @brief 析构并释放 BIO 资源 */
    ~ZmMemoryBIO();

    /** @return 底层 BIO 指针 */
    BIO*        Bio();

    /**
     * @brief 获取底层 BUF_MEM 指针
     * @note 每次调用都重新从 BIO 获取，确保数据最新
     * @return BUF_MEM 指针，失败返回 NULL
     */
    BUF_MEM*    Buf();

    /** @return BIO 中的数据指针，BIO 为空时返回 NULL */
    const char* BufData();

    /** @return BIO 中的数据长度 */
    size_t      BufLen();

private:
    BIO*      m_bio;   /**< OpenSSL BIO 对象 */
    BUF_MEM*  m_buf;   /**< BUF_MEM 指针，由 Buf() 填充 */
};

#endif /* ZM_SSL_CTX_H */
