#include "zm_ssl_fingerprint.h"
#include <memory>
#include "../net/zm_net_dns.h"
#include "../util/zm_util_str.h"
#include "../spdlog/zm_logger.h"

/**
 * @page ssl_fingerprint_guide 证书指纹获取方法
 *
 * 分两步（因为 www.so.com 和 so.com 是同一个服务器且默认是 so.com 的证书，
 * 因此需要通过 -servername 指定 SNI 为 www.so.com）：
 *
 * @code
 * openssl s_client -showcerts -servername www.so.com -connect www.so.com:443 </dev/null 2>/dev/null | openssl x509 -outform PEM > so.pem
 * openssl x509 -in so.pem -sha1 -noout -fingerprint
 * @endcode
 *
 * 一步到位：
 * @code
 * openssl s_client -servername www.so.com -connect www.so.com:443 < /dev/null 2>/dev/null | openssl x509 -fingerprint -noout -in /dev/stdin
 * openssl s_client -connect 172.16.1.232:443 < /dev/null 2>/dev/null | openssl x509 -fingerprint -noout -in /dev/stdin
 * @endcode
 */

// === Global ===

/** @brief 全局单例实例 */
static std::unique_ptr<ZmSSLFingerprint> g_zm_SSLFingerprint;

/** @brief 保证 instance() 只初始化一次的同步标志 */
static std::once_flag g_Y_SSL_FINGERPRINT_once;

// === Static ===

/**
 * @brief 获取单例实例（线程安全）
 * @return 指向全局唯一实例的指针
 */
ZmSSLFingerprint* ZmSSLFingerprint::instance()
{
    std::call_once(g_Y_SSL_FINGERPRINT_once, []() {
        g_zm_SSLFingerprint.reset(new ZmSSLFingerprint());
    });
    return g_zm_SSLFingerprint.get();
}

/**
 * @brief 从 SSL 连接中提取对端证书的 SHA1 摘要
 * @param ssl  已建立的 SSL 连接对象
 * @param md   输出缓冲区，至少 SHA_DIGEST_LENGTH (20) 字节
 * @return true 提取成功；false 对端未提供证书或摘要计算失败
 */
bool ZmSSLFingerprint::GetSSLCertSHA1(SSL* ssl, unsigned char* md)
{
    memset(md, 0, SHA_DIGEST_LENGTH);
    X509* cert = SSL_get_peer_certificate(ssl);
    if (!cert)
        return false;
    bool ret = (X509_digest(cert, EVP_sha1(), md, NULL) != 0);
    X509_free(cert);
    return ret;
}

// === Constructor / Destructor ===

ZmSSLFingerprint::ZmSSLFingerprint() : m_fingerprints(8)
{}

ZmSSLFingerprint::~ZmSSLFingerprint()
{}

// === Public ===

/**
 * @brief 注册一条信任的服务器证书指纹
 *
 * 从指纹字符串中过滤非十六进制字符后提取前 40 个十六进制位（20 字节 SHA1），
 * 存入以 host:port 为键的记录中。若 host 已有记录，追加到其指纹数组（自动去重）；
 * 若无记录，先通过 DNS 解析 host 获取 IP 地址再新建记录。
 *
 * @param host        主机名；传入空字符串则清空整个指纹库
 * @param port        端口号
 * @param fingerprint 证书指纹字符串，支持含冒号分隔的格式
 * @param digest      摘要算法名称，目前仅支持 "sha1"（默认 "sha1"）
 * @return 非空指针 指向该记录的地址信息；NULL 参数无效、指纹长度不足、DNS 解析失败或指纹数已满
 */
ZM_PEER_ADDR* ZmSSLFingerprint::Put(const char* host, uint16_t port, const char* fingerprint, const char* digest)
{
    PUBLIC_LOG_INFO("Put a remote server certificate fingerprint: addr={}:{}, fingerprint={}", host, port, fingerprint);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!ZmString::IsEmpty(host))
    {
        if (ZmString::IsEmpty(digest) || 0 != strcmp(digest, "sha1"))
        {
            PUBLIC_LOG_WARN("Put fingerprint: only sha1 digest is supported, ignored digest={}", digest ? digest : "(null)");
        }

        // 从 fingerprint 字符串中提取前 40 个十六进制字符（过滤冒号等非 HEX 字符）
        ZmByteBuffer str(64);
        size_t sha1len = SHA_DIGEST_LENGTH * 2;
        size_t i = 0;
        char* ptr = const_cast<char*>(fingerprint);
        while (i < sha1len && *ptr)
        {
            if (zm_is_hex_char(*ptr))
            {
                str[i++] = *ptr;
            }
            ptr++;
        }

        if (strlen(str.Str()) >= sha1len)
        {
            ZM_SSL_FINGERPRINT* fp = QueryByHostame(host, port);
            ZM_IP_ADDR          ip = { 0 };
            // 短路求值：若 hostname 已有记录则跳过 DNS 解析，否则尝试 DNS 解析获取 IP
            if (fp || ZmNetDNS::GetHostIPByName(&ip, host, port))
            {
                if (!fp)
                {
                    fp = m_fingerprints.Add();
                    snprintf(fp->hostname, sizeof(fp->hostname), "%s", host);
                    memcpy(&fp->addr.ip, &ip, sizeof(ZM_IP_ADDR));
                    fp->addr.port = port;
                }
                if (fp->fpcnt < ZM_FP_MAX_CNT)
                {
                    // 解析十六进制字符串为二进制指纹
                    unsigned char newfp[20];
                    ZmString::FromHex(str.Str(), newfp, sha1len);
                    // 去重：若指纹已存在则跳过
                    for (size_t j = 0; j < fp->fpcnt; j++)
                    {
                        if (0 == memcmp(fp->fingerprint[j], newfp, sizeof(newfp)))
                        {
                            return &fp->addr;
                        }
                    }
                    memcpy(fp->fingerprint[fp->fpcnt], newfp, sizeof(newfp));
                    fp->fpcnt++;
                    return &fp->addr;
                }
            }
        }
    }
    else
    {
        // host 为空时清空指纹库
        m_fingerprints.Clear();
    }
    return NULL;
}

/**
 * @brief 通过 socket fd 和 SSL 连接校验服务器证书指纹（白名单模式）
 *
 * 查找策略（按优先级依次尝试）：
 * 1. 按 host + port 查找预置指纹
 * 2. 从 SSL 的 SNI 扩展中取出 servername 再查找
 * 3. 通过 getpeername() 获取对端 IP 地址查找
 *
 * @param fd    已连接的 socket 文件描述符
 * @param ssl   已建立的 SSL 连接对象
 * @param host  目标主机名
 * @param port  目标端口号
 * @return true 指纹匹配通过，或指纹库为空时直接放行；
 *         false 指纹库非空但目标主机无记录，或有记录但指纹全部不匹配
 */
bool ZmSSLFingerprint::Validate(evutil_socket_t fd, SSL* ssl, const char* host, uint16_t port)
{
    // 指纹库为空时直接放行，不做任何校验
    if (m_fingerprints.Count() > 0)
    {
        X509* cert = SSL_get_peer_certificate(ssl);
        DumpCert(cert);

        // 第一步：按 host + port 查找
        ZM_SSL_FINGERPRINT* fp = QueryByHostame(host, port);
        if (!fp)
        {
            PUBLIC_LOG_ERROR("QueryByHostame do not fund {}:{}", host, port);
            // 第二步：通过 SNI 扩展中的 servername 查找
            const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
            if (!ZmString::IsEmpty(servername))
            {
                fp = QueryByHostame(servername, port);
            }
        }
        if (!fp && fd > 0)
        {
            PUBLIC_LOG_ERROR("SSL_get_servername do not fund {}:{}", host, port);
            // 第三步：通过对端 IP 地址查找
            ZM_PEER_ADDR        addr = { 0 };
            struct sockaddr_in6 sa = { 0 };
            socklen_t           sa_len = sizeof(struct sockaddr_in6);
            if (0 == getpeername(fd, (struct sockaddr*)&sa, &sa_len))
            {
                ZmNetIP::SockaddrToPeer((struct sockaddr*)&sa, &addr);
                fp = QueryByAddress(&addr);
            }
        }

        if (fp && fp->fpcnt > 0)
        {
            // 将实际证书 SHA1 与预置指纹逐一比对
            unsigned char md[24];
            if (GetSSLCertSHA1(ssl, md))
            {
                for (size_t i = 0; i < fp->fpcnt; i++)
                {
                    if (0 == memcmp(md, fp->fingerprint[i], SHA_DIGEST_LENGTH))
                    {
                        X509_free(cert);
                        return true;
                    }
                }
            }
            X509_free(cert);
            return false;
        }
        else
        {
            PUBLIC_LOG_ERROR("no fingerprint for {}:{}", host, port);
        }
        X509_free(cert);
        return false;
    }
    return true;
}

/**
 * @brief 直接比对 SSL 连接的证书指纹与指定指纹字符串
 * @param ssl         已建立的 SSL 连接对象
 * @param fingerprint 待比对的指纹字符串，支持含冒号分隔的格式
 * @return true 指纹匹配；false ssl 为空、fingerprint 为空、指纹长度不足或不匹配
 */
bool ZmSSLFingerprint::Validate(SSL* ssl, const char* fingerprint)
{
    if (ssl && NULL != fingerprint)
    {
        // 从 fingerprint 字符串中提取前 40 个十六进制字符（过滤冒号等非 HEX 字符）
        ZmByteBuffer hex(64);
        size_t sha1len = SHA_DIGEST_LENGTH * 2;
        size_t i = 0;
        char* ptr = const_cast<char*>(fingerprint);
        while (i < sha1len && *ptr)
        {
            if (zm_is_hex_char(*ptr))
            {
                hex[i++] = *ptr;
            }
            ptr++;
        }
        if (strlen(hex.Str()) >= sha1len)
        {
            unsigned char fp[24];
            unsigned char md[24];
            ZmString::FromHex(hex.Str(), fp, sha1len);
            return GetSSLCertSHA1(ssl, md) && (0 == memcmp(md, fp, SHA_DIGEST_LENGTH));
        }
    }
    return false;
}

// === Private ===

/**
 * @brief 从 PEM 格式缓冲区中解析并打印证书指纹（调试用）
 * @param pembuf PEM 编码的证书数据
 * @param pemlen PEM 数据长度
 */
void ZmSSLFingerprint::DumpCertBuffer(const char* pembuf, size_t pemlen)
{
    BIO* bio = BIO_new_mem_buf((void*)pembuf, (int)pemlen);
    X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    DumpCert(cert);
    if (cert)
    {
        X509_free(cert);
    }
    if (bio)
    {
        if (BIO_set_close(bio, BIO_CLOSE)) {}
        BIO_free(bio);
    }
}

/**
 * @brief 打印 X509 证书的 SHA1 指纹到日志（调试用）
 * @param cert X509 证书对象，允许为 NULL
 */
void ZmSSLFingerprint::DumpCert(X509* cert)
{
    if (!cert)
        return;
    ZmByteBuffer md(64);
    ZmByteBuffer hex(64);
    unsigned int flen;
    md.Zero();
    if (!X509_digest(cert, EVP_sha1(), md.Head(), &flen))
    {
        PUBLIC_LOG_ERROR("Dump X509 certificate, invoking X509_digest() failed");
    }
    else
    {
        ZmString::Hex(md.Head(), hex.Str(), flen, false);
        PUBLIC_LOG_INFO("Dump X509 certificate: {}", hex.Str());
    }
}

/**
 * @brief 按 servername + port 查找指纹记录
 * @param servername 服务器主机名
 * @param port       端口号
 * @return 找到则返回记录指针；未找到返回 NULL
 */
ZM_SSL_FINGERPRINT* ZmSSLFingerprint::QueryByHostame(const char* servername, uint16_t port)
{
    for (size_t i = 0; i < m_fingerprints.Count(); i++)
    {
        ZM_SSL_FINGERPRINT* fp = m_fingerprints.At(i);
        if (0 == strcmp(fp->hostname, servername) && fp->addr.port == port)
        {
            return fp;
        }
    }
    return NULL;
}

/**
 * @brief 按对端 IP 地址 + port 查找指纹记录
 * @param addr 对端地址（主机序）
 * @return 找到则返回记录指针；未找到返回 NULL
 */
ZM_SSL_FINGERPRINT* ZmSSLFingerprint::QueryByAddress(const ZM_PEER_ADDR* addr)
{
    for (size_t i = 0; i < m_fingerprints.Count(); i++)
    {
        ZM_SSL_FINGERPRINT* fp = m_fingerprints.At(i);
        if (0 == ZmNetIP::IPCompare(&fp->addr.ip, &addr->ip) && fp->addr.port == addr->port)
        {
            return fp;
        }
    }
    return NULL;
}
