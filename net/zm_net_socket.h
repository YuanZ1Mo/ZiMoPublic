/**
 * @file zm_net_socket.h
 * @brief TCP/SSL 网络套接字封装模块
 *
 * 提供阻塞式 TCP 和 SSL 客户端连接能力，包括：
 * - Winsock 生命周期管理 (ZmWinSockHelper)
 * - TCP 连接，支持超时控制、KeepAlive、代理隧道 (ZmNetSocketTCP)
 * - SSL/TLS 连接，支持 SNI 和证书指纹校验 (ZmNetSocketSSL)
 */

#ifndef ZM_NET_SOCKET_H
#define ZM_NET_SOCKET_H


#include "../ssl/zm_ssl_ctx.h"
#include <event2/util.h>

#include <memory>

//#define ZM_SOCKET_SEND_ERROR                4000001
//#define ZM_SOCKET_RECEIVE_ERROR             4000002

/**
 * @brief 判断 socket 句柄是否有效
 *
 * 在 Windows 上，有效句柄满足 fd > 0 且不等于 INVALID_SOCKET。
 * 注意：fd == 0 在 Windows 上被视为无效（与 POSIX 不同）。
 *
 * @param fd  待检测的 socket 句柄
 * @return    非零表示有效，零表示无效
 */
#define Zm_IsSocketValid(fd)  ( fd > 0 && INVALID_SOCKET != fd )

/**
 * @brief Winsock 初始化/清理的 RAII 辅助类
 *
 * 构造时通过引用计数调用 WSAStartup()，析构时引用计数归零调用 WSACleanup()。
 * 支持多个实例共存，仅在首个实例创建和末个实例销毁时执行实际操作。
 *
 * @example 通过 Init 初始化全局单例
 * @code
 *   ZmWinSockHelper::Init();
 *   // ... 程序退出时自动 WSACleanup
 * @endcode
 */
class ZmWinSockHelper
{
public:
    /** @brief 初始化 Winsock 环境，内部通过原子引用计数保证只 WSAStartup 一次 */
    ZmWinSockHelper();

    /** @brief 引用计数归零时自动调用 WSACleanup() */
    ~ZmWinSockHelper();

    /**
    * @brief 初始化全局 Winsock 辅助对象单例
    *
    * 若 g_winsock_helper 尚未创建，则创建一个新实例并赋值给 g_winsock_helper。
    * 多次调用是安全的，仅在首次调用时执行实际创建。
    *
    * @example
    * @code
    *   int main() {
    *       ZmWinSockHelper::Init();
    *       // ... 所有网络操作均可正常使用
    *       return 0;
    *   }
    * @endcode
    */
    static void Init();
};

/**
 * @brief 网络套接字抽象基类
 *
 * 定义了 Open / Close / Send / Recv / IsConnected 五个核心操作的统一接口，
 * 并提供 SendAll / RecvAll 等循环收发工具方法。
 *
 * 子类必须实现全部纯虚函数。
 */
class ZmNetSocketBase
{
public:
    /**
     * @brief 获取 socket 绑定的本地端口
     * @param fd  已绑定或已连接的 socket 句柄
     * @return    本地端口号（主机字节序），失败返回 0
     */
    static uint16_t LocalPort(evutil_socket_t fd);

    /**
     * @brief 获取 socket 对端的远程端口
     * @param fd  已连接的 socket 句柄
     * @return    对端端口号（主机字节序），失败返回 0
     */
    static uint16_t PeerPort(evutil_socket_t fd);

    /**
     * @brief 屏蔽 SIGPIPE 信号（跨平台占位）
     *
     * Windows 没有 SIGPIPE，此函数为空实现，保留用于 macOS/iOS 等平台的兼容。
     *
     * @param fd  socket 句柄
     */
    static void IgnoreSignalPipe(evutil_socket_t fd);

    /**
     * @brief 根据 socket 的本地地址解析本机 IP 和 MAC
     *
     * 通过 getsockname() 获取 socket 绑定的本地地址，再查询对应的 MAC 地址。
     * sa_family 决定按 IPv4 还是 IPv6 大小解析 sockaddr。
     *
     * @param fd        已连接的 socket 句柄
     * @param sa_family 地址族，AF_INET 或 AF_INET6
     * @param ip_str    [out] 输出本机 IP 字符串
     * @param mac_addr  [out] 输出本机 MAC 地址字符串
     * @return          0 成功，-1 失败
     */
    static int FindSrcAddr(evutil_socket_t fd, int sa_family,
        std::string& ip_str, std::string& mac_addr);

    /**
     * @brief 通过目标地址探测本机出口 IP 和 MAC
     *
     * 创建一个 UDP socket 并 connect 到目标地址（不会真正发送数据），
     * 再通过 getsockname() 获取内核选择的出口地址。
     *
     * @param ip_str      [out] 输出本机出口 IP 字符串
     * @param mac_str     [out] 输出本机出口 MAC 地址字符串
     * @param des_ip_str  目标 IP 或域名
     * @param des_port    目标端口
     * @return            0 成功，-1 失败
     *
     * @example
     *   std::string ip, mac;
     *   ZmNetSocketBase::FindLocalAddrByDesAddress(ip, mac, "8.8.8.8", 53);
     *   // ip = "192.168.1.100", mac = "AA:BB:CC:DD:EE:FF"
     */
    static int FindLocalAddrByDesAddress(std::string& ip_str, std::string& mac_str,
        std::string des_ip_str, int des_port);

    ZmNetSocketBase();
    virtual ~ZmNetSocketBase();

    /**
     * @brief 建立连接
     * @param host      目标主机地址（IP 或域名）
     * @param port      目标端口
     * @param timeouts  连接超时秒数，0 表示阻塞等待
     * @return          true 成功，false 失败
     */
    virtual bool Open(const char* host = NULL, uint16_t port = 0, float timeouts = 0) = 0;

    /** @brief 关闭连接并释放资源 */
    virtual void Close() = 0;

    /**
     * @brief 发送数据
     * @param buffer  待发送数据缓冲区
     * @param length  数据长度
     * @return        实际发送字节数，错误返回 -1
     */
    virtual int  Send(const void* buffer, size_t length) = 0;

    /**
     * @brief 接收数据
     * @param buffer  接收缓冲区
     * @param length  缓冲区大小
     * @return        实际接收字节数，连接关闭返回 0，错误返回 -1
     */
    virtual int  Recv(void* buffer, size_t length) = 0;

    /**
     * @brief 检测连接是否仍然存活
     * @return  true 连接正常，false 已断开或未连接
     */
    virtual bool IsConnected() = 0;

    /**
     * @brief 循环发送直到全部数据写完或出错
     *
     * 内部循环调用 Send()，每次发送部分数据后推进指针，直到 length 字节全部发完。
     * Send() 返回 0 时停止（对端关闭），返回负值时立即返回错误码。
     *
     * @param buffer  待发送数据缓冲区
     * @param length  数据总长度
     * @return        实际发送字节数（>=0），出错时返回负值
     *
     * @example
     *   const char data[] = "Hello";
     *   int sent = sock.SendAll(data, sizeof(data));
     *   // sent == sizeof(data) 表示全部发完
     */
    int SendAll(const void* buffer, size_t length);

    /**
     * @brief 循环接收直到填满指定长度或出错/对端关闭
     *
     * 行为与 SendAll 对称：循环调用 Recv() 直到收满 length 字节。
     * Recv() 返回 0（对端关闭）或负值（错误）时停止。
     *
     * @param buffer  接收缓冲区
     * @param length  期望接收的总字节数
     * @return        实际接收字节数（>=0），出错时返回负值
     */
    int RecvAll(void* buffer, size_t length);
};

/**
 * @brief 阻塞式 TCP 套接字
 *
 * 封装了 TCP 连接的完整生命周期，支持：
 * - 带超时的连接建立（ConnectTimeout）
 * - TCP KeepAlive 心跳保活
 * - HTTP CONNECT 代理隧道（ProxyConnect）
 * - 非阻塞模式切换
 */
class ZmNetSocketTCP : public ZmNetSocketBase
{
public:
    /**
     * @brief 安全关闭 socket 句柄
     *
     * 先调用 shutdown(SD_BOTH) 优雅断开，blocking 为 true 时再调用 closesocket()。
     * Windows 下异步 socket 不应直接 closesocket()，可通过 blocking=false 仅做 shutdown。
     *
     * @param fd        待关闭的 socket 句柄
     * @param blocking  true 则关闭后释放句柄，false 仅 shutdown 不 closesocket
     */
    static void CloseFD(evutil_socket_t fd, bool blocking = true);

    /**
     * @brief 设置 socket 的阻塞/非阻塞模式
     * @param fd           socket 句柄
     * @param nonBlocking  true 设为非阻塞，false 设为阻塞
     */
    static void SetNonblocking(evutil_socket_t fd, bool nonBlocking = true);

    /**
     * @brief 启用 TCP KeepAlive 心跳探测
     *
     * 两个参数均为 0 或省略时使用默认值：首次探测空闲 60 秒，探测间隔 5 秒。
     * 自定义时 idle 和 interval 单位均为秒，内部会转换为毫秒。
     *
     * @param fd        socket 句柄
     * @param idle      首次 KeepAlive 探测前的空闲时间（秒），默认 60
     * @param interval  两次 KeepAlive 探测之间的间隔（秒），默认 5
     */
    static void KeepAlive(evutil_socket_t fd, uint32_t idle = 0, uint32_t interval = 0);

    /**
     * @brief 带超时的 TCP 连接
     *
     * timeouts <= 0.01 时直接阻塞式 connect()；否则将 socket 设为非阻塞，
     * 发起 connect 后用 select() 等待连接完成或超时。
     * 无论成功或失败，最终都会将 socket 恢复为阻塞模式。
     *
     * @param fd        已创建的 socket 句柄
     * @param addr      目标地址
     * @param addrlen   地址长度
     * @param timeouts  超时秒数，<= 0.01 表示阻塞
     * @return          ZM_ERR_NOERROR(0) 成功，其他值为错误码（WSAETIMEDOUT 等）
     */
    static int ConnectTimeout(evutil_socket_t fd, const struct sockaddr* addr,
        socklen_t addrlen, float timeouts);

    /**
     * @brief 通过 HTTP CONNECT 代理建立 TCP 隧道
     *
     * 连接到代理服务器后发送 CONNECT 请求，代理返回 200 即建立隧道，
     * 返回的 fd 可用于后续的 TCP 通信。
     *
     * @param dst_host    目标主机
     * @param dst_port    目标端口
     * @param proxy_host  代理服务器地址
     * @param proxy_port  代理服务器端口
     * @param proxy_ssl   代理协议前缀（"ssl" 或 "sslsmx"），一般传 NULL
     * @param proxy_user  代理认证用户名，可为 NULL
     * @param proxy_pass  代理认证密码，可为 NULL
     * @return            隧道 socket 句柄，失败返回 INVALID_SOCKET
     *
     * @example
     *   evutil_socket_t fd = ZmNetSocketTCP::ProxyConnect(
     *       "example.com", 443,
     *       "proxy.local", 8080, NULL, "user", "pass");
     *   if (Zm_IsSocketValid(fd)) { // 隧道已建立，可进行后续通信 }
     */
    static evutil_socket_t ProxyConnect(const char* dst_host, uint16_t dst_port,
        const char* proxy_host, uint16_t proxy_port, const char* proxy_ssl = NULL,
        const char* proxy_user = NULL, const char* proxy_pass = NULL);

    /**
     * @brief 构造 TCP 套接字对象
     * @param sock        外部传入的 socket 句柄，默认 INVALID_SOCKET 表示稍后 Open
     * @param automatic   true 则析构时自动 Close()
     * @param keepalive   true 则 Open 成功后自动启用 KeepAlive
     */
    ZmNetSocketTCP(evutil_socket_t sock = INVALID_SOCKET, bool automatic = true, bool keepalive = false);

    /** @brief 若 m_automatic 为 true 则自动关闭连接 */
    virtual ~ZmNetSocketTCP();

    /**
     * @brief 解析主机名并建立 TCP 连接
     *
     * 内部调用 ZmNetDNS::GetAddressByName 解析域名，再通过 ConnectTimeout 发起连接。
     * 重复调用会先 Close 上一次连接。
     *
     * @param host      目标主机（IP 或域名）
     * @param port      目标端口
     * @param timeouts  连接超时秒数，0 表示阻塞
     * @return          true 连接成功，false 失败
     *
     * @example
     *   ZmNetSocketTCP sock;
     *   if (sock.Open("example.com", 80, 5.0f)) {
     *       sock.Send("GET / HTTP/1.1\r\n\r\n", 18);
     *   }
     */
    virtual bool Open(const char* host, uint16_t port, float timeouts = 0);

    /** @brief 关闭连接，释放 socket 句柄并重置端口缓存 */
    virtual void Close();

    virtual int  Send(const void* buffer, size_t length);
    virtual int  Recv(void* buffer, size_t length);

    /**
     * @brief 检测 TCP 连接是否存活
     *
     * 通过 select() 零超时轮询 socket 可读状态：
     * - 无事件 → 仍存活
     * - 可读 → 用 MSG_PEEK 偷看 1 字节判断是对端关闭还是正常数据
     *
     * @return  true 连接正常，false 已断开或句柄无效
     */
    virtual bool IsConnected();

    /** @brief 获取内部 socket 句柄 */
    const evutil_socket_t GetFD() const;

    /**
     * @brief 获取本地端口号
     * @return  本地端口号（首次调用后缓存）
     */
    uint16_t Port();


    //int GetErrorCode() { return m_error_code; }

    //void SetErrorCode(int errorCode) { m_error_code = errorCode; }

private:
    evutil_socket_t m_sock;        ///< socket 句柄
    bool            m_automatic;   ///< 析构时是否自动 Close
    bool            m_keepalive;   ///< Open 时是否启用 KeepAlive
    uint16_t        m_port;        ///< 本地端口缓存，0 表示未查询
    //int             m_error_code = 0;

};

/**
 * @brief SSL/TLS 套接字
 *
 * 基于 OpenSSL BIO 链实现 SSL 客户端连接，支持 IPv4/IPv6、SNI 和证书指纹校验。
 * IPv4 路径使用 BIO_new_ssl_connect 自动建立连接，IPv6 路径手动创建 socket 后包装为 BIO。
 */
class ZmNetSocketSSL : public ZmNetSocketBase
{
public:
    /**
     * @brief 构造 SSL 套接字
     * @param servername  TLS SNI 服务器名称，为 NULL 时使用 Open() 中的 host 参数
     */
    ZmNetSocketSSL(const char* servername = NULL);

    virtual ~ZmNetSocketSSL();

    /**
     * @brief 建立 SSL/TLS 连接
     *
     * 流程：DNS 解析 → TCP 连接（IPv6 使用 ConnectTimeout 支持）→ 创建 BIO →
     * SNI 设置 → SSL 握手 → 证书指纹校验。任一步骤失败则中断并清理资源。
     *
     * @param host      目标主机（IP 或域名）
     * @param port      目标端口
     * @param timeouts  连接超时秒数（仅 IPv6 路径生效）
     * @return          true 连接成功，false 失败
     *
     * @example
     *   ZmNetSocketSSL ssl;
     *   if (ssl.Open("example.com", 443, 10.0f)) {
     *       ssl.Send(request, len);
     *   }
     */
    virtual bool Open(const char* host = NULL, uint16_t port = 0, float timeouts = 0.0f);

    /** @brief 释放 SSL BIO 链及关联资源 */
    virtual void Close();

    /**
     * @brief 通过 SSL 发送数据
     * @return  实际发送字节数，未连接时返回 -1
     */
    virtual int  Send(const void* buffer, size_t length);

    /**
     * @brief 通过 SSL 接收数据
     * @return  实际接收字节数，未连接时返回 -1
     */
    virtual int  Recv(void* buffer, size_t length);

    /**
     * @brief 检测 SSL 连接是否存活
     *
     * 依次检查：BIO 是否存在 → SSL 对象是否有效 → 对端是否已发送 shutdown → 底层 fd 是否有效。
     *
     * @return  true 连接正常，false 已断开
     */
    virtual bool IsConnected();

    /**
     * @brief 在已有 TCP 连接上附加 SSL 握手（未实现）
     * @param fd        已连接的 TCP socket
     * @param hostname  SNI 主机名
     * @param port      远程端口
     * @return          当前固定返回 false
     */
    bool Handshake(evutil_socket_t fd, const char* hostname, uint16_t port);

private:
    BIO*  m_bio;              ///< OpenSSL BIO 链（包含 SSL 和连接 BIO）
    char  m_servername[128];  ///< TLS SNI 服务器名称
    SSL_CTX* m_client_sslctx;
};

#endif // ZM_NET_SOCKET_H
