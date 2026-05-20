#ifndef ZM_SSL_FINGERPRINT_H
#define ZM_SSL_FINGERPRINT_H


#include "../net/zm_net_ip.h"
#include "../util/zm_util_container.h"

#include <mutex>

#include <openssl/include/openssl/ssl.h>

/**
 * @brief 每个主机允许存储的最大证书指纹数量
 *
 * 同一主机可能存在多张合法证书（如轮换过渡期），此常量限制每条记录中最多保存的指纹数
 */
#define ZM_FP_MAX_CNT   8

/**
 * @brief SSL 证书指纹记录
 *
 * 按 hostname + port 维度存储一组预置的 SHA1 指纹，用于白名单校验：
 * 连接时将服务器实际证书指纹与本记录中的指纹逐条比对，匹配则放行，全部不匹配则拒绝
 */
typedef struct
{
    char            hostname[128];              /**< 主机名，如 "www.example.com" */
    ZM_PEER_ADDR    addr;                       /**< 解析后的对端地址（主机序） */
    uint8_t         fpcnt;                      /**< 当前已存储的指纹数量，范围 [0, ZM_FP_MAX_CNT] */
    unsigned char   fingerprint[ZM_FP_MAX_CNT][20]; /**< SHA1 指纹数组，每条 20 字节 */
} ZM_SSL_FINGERPRINT;

/**
 * @brief SSL 证书指纹白名单管理器（单例）
 *
 * 提供证书指纹的注册与校验功能，用于在 SSL/TLS 连接中验证服务器证书合法性。
 *
 * 工作流程：
 * 1. 启动阶段通过 Put() 预置信任的服务器证书指纹
 * 2. 连接阶段通过 Validate() 比对实际证书指纹与预置指纹
 * 3. 指纹库为空时 Validate() 直接放行；指纹库非空但目标主机无记录时拒绝连接
 *
 * @example
 * @code
 * // 注册信任指纹
 * ZmSSLFingerprint::instance()->Put("www.example.com", 443,
 *     "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4");
 *
 * // 连接后校验
 * bool ok = ZmSSLFingerprint::instance()->Validate(fd, ssl, "www.example.com", 443);
 * @endcode
 */
class ZmSSLFingerprint
{
public:
    // === Static ===

    /**
     * @brief 获取单例实例（线程安全）
     * @return 指向全局唯一实例的指针
     */
    static ZmSSLFingerprint* instance();

    /**
     * @brief 从 SSL 连接中提取对端证书的 SHA1 摘要
     * @param ssl  已建立的 SSL 连接对象
     * @param md   输出缓冲区，至少 SHA_DIGEST_LENGTH (20) 字节
     * @return true 提取成功，md 中为证书 SHA1 摘要；false 对端未提供证书或摘要计算失败
     * @note 内部调用 X509_free 释放临时证书对象，调用者无需处理
     */
    static bool GetSSLCertSHA1(SSL* ssl, unsigned char* md);

    // === Constructor / Destructor ===

    /**
     * @brief 构造函数，初始化指纹列表（初始容量 8）
     */
    ZmSSLFingerprint();

    /**
     * @brief 析构函数
     */
    ~ZmSSLFingerprint();

    // === Public ===

    /**
     * @brief 注册一条信任的服务器证书指纹
     *
     * 从指纹字符串中过滤非十六进制字符后提取前 40 个十六进制位（20 字节 SHA1），
     * 存入以 host:port 为键的记录中。若 host 已有记录，追加到其指纹数组（自动去重）；
     * 若无记录，先通过 DNS 解析 host 获取 IP 地址再新建记录。
     *
     * @param host        主机名，如 "www.example.com"；传入空字符串则清空整个指纹库
     * @param port        端口号
     * @param fingerprint 证书指纹字符串，支持含冒号分隔的格式，如 "A1:B2:C3:..."
     * @param digest      摘要算法名称，目前仅支持 "sha1"，其他值会打印警告（默认 "sha1"）
     * @return 非空指针 指向该记录的地址信息；NULL 参数无效、指纹长度不足、DNS 解析失败或指纹数已满
     *
     * @example
     * @code
     * // 含冒号的指纹格式会被自动过滤
     * ZmSSLFingerprint::instance()->Put("www.example.com", 443,
     *     "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4");
     *
     * // 连续调用追加同一主机的不同指纹（如证书轮换期）
     * ZmSSLFingerprint::instance()->Put("api.example.com", 443,
     *     "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44");
     * ZmSSLFingerprint::instance()->Put("api.example.com", 443,
     *     "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD");
     *
     * // 清空指纹库
     * ZmSSLFingerprint::instance()->Put("", 0, "");
     * @endcode
     */
    ZM_PEER_ADDR* Put(const char* host, uint16_t port, const char* fingerprint, const char* digest = "sha1");

    /**
     * @brief 通过 socket fd 和 SSL 连接校验服务器证书指纹（白名单模式）
     *
     * 查找策略（按优先级依次尝试）：
     * 1. 按 host + port 查找预置指纹
     * 2. 从 SSL 的 SNI 扩展中取出 servername 再查找
     * 3. 通过 getpeername() 获取对端 IP 地址查找
     *
     * @param fd    已连接的 socket 文件描述符，用于在 SNI 不可用时获取对端地址
     * @param ssl   已建立的 SSL 连接对象
     * @param host  目标主机名
     * @param port  目标端口号
     * @return true 指纹匹配通过，或指纹库为空时直接放行；
     *         false 指纹库非空但目标主机无记录，或有记录但指纹全部不匹配
     */
    bool Validate(evutil_socket_t fd, SSL* ssl, const char* host, uint16_t port);

    /**
     * @brief 直接比对 SSL 连接的证书指纹与指定指纹字符串
     *
     * @param ssl         已建立的 SSL 连接对象
     * @param fingerprint 待比对的指纹字符串，支持含冒号分隔的格式，如 "A1:B2:C3:..."
     * @return true 指纹匹配；false ssl 为空、fingerprint 为空、指纹长度不足或不匹配
     *
     * @example
     * @code
     * bool ok = ZmSSLFingerprint::instance()->Validate(ssl, "A1:B2:C3:D4:...");
     * @endcode
     */
    bool Validate(SSL* ssl, const char* fingerprint);

private:
    // === Private ===

    /**
     * @brief 从 PEM 格式缓冲区中解析并打印证书指纹（调试用）
     * @param pembuf PEM 编码的证书数据
     * @param pemlen PEM 数据长度
     */
    void                DumpCertBuffer(const char* pembuf, size_t pemlen);

    /**
     * @brief 打印 X509 证书的 SHA1 指纹到日志（调试用）
     * @param cert X509 证书对象，允许为 NULL（内部做空指针保护）
     */
    void                DumpCert(X509* cert);

    /**
     * @brief 按 servername + port 查找指纹记录
     * @param servername 服务器主机名
     * @param port       端口号
     * @return 找到则返回记录指针；未找到返回 NULL
     */
    ZM_SSL_FINGERPRINT*  QueryByHostame(const char* servername, uint16_t port);

    /**
     * @brief 按对端 IP 地址 + port 查找指纹记录
     * @param addr 对端地址（主机序）
     * @return 找到则返回记录指针；未找到返回 NULL
     */
    ZM_SSL_FINGERPRINT*  QueryByAddress(const ZM_PEER_ADDR* addr);

private:
    ZmArrayList<ZM_SSL_FINGERPRINT> m_fingerprints;  /**< 指纹白名单列表 */
    std::mutex                     m_mutex;          /**< 操作 m_fingerprints 时的互斥锁 */
};

#endif /* ZM_SSL_FINGERPRINT_H */
