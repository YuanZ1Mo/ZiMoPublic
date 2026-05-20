/**
 * @file zm_net_socket.cpp
 * @brief TCP/SSL 网络套接字封装模块实现
 */

#include "zm_net_socket.h"


#include "zm_net_dns.h"
#include "../spdlog/zm_logger.h"
#include "../util/zm_util_sys.h"

#include <openssl/include/openssl/err.h>

#include <mstcpip.h>
#include <atomic>

// ============================================================================
// Global
// ============================================================================

/** @brief Winsock 初始化引用计数，原子操作保证线程安全 */
std::atomic<int> g_winsock_ref{0};

/** @brief 全局 Winsock 辅助对象单例，由使用方在程序启动时创建 */
std::unique_ptr<ZmWinSockHelper> g_winsock_helper = nullptr;


// ============================================================================
// ZmWinSockHelper
// ============================================================================

ZmWinSockHelper::ZmWinSockHelper()
{
    g_winsock_ref++;
    if (1 == g_winsock_ref)
    {
        PUBLIC_LOG_INFO("[WinSocket] Initializing");
        WSADATA wsaData;
        if (0 != WSAStartup(MAKEWORD(2, 2), &wsaData))
        {
            PUBLIC_LOG_ERROR("[WinSocket] Initializing invoke WSAStartup() failed: {}", ZmSystem::ErrMsg(-1));
        }
    }
}

ZmWinSockHelper::~ZmWinSockHelper()
{
    g_winsock_ref--;
    if (0 == g_winsock_ref)
    {
        WSACleanup();
        //PUBLIC_LOG_INFO("[WinSocket] Cleanup");
    }
}

void ZmWinSockHelper::Init()
{
    if (!g_winsock_helper)
    {
        g_winsock_helper.reset(new ZmWinSockHelper());
    }
}

// ============================================================================
// ZmNetSocketBase — static methods
// ============================================================================

uint16_t ZmNetSocketBase::LocalPort(evutil_socket_t fd)
{
    if (Zm_IsSocketValid(fd))
    {
        // 使用 sockaddr_in6 容器同时兼容 IPv4 和 IPv6
        struct sockaddr_in6 sa6 = { 0 };
        socklen_t           sa_len = sizeof(struct sockaddr_in6);
        struct sockaddr*    sa = (struct sockaddr*)&sa6;
        if (0 == getsockname(fd, sa, &sa_len))
        {
            // sin6_port 存储为网络字节序，需用 ntohs 转为主机字节序
            return ntohs(sa6.sin6_port);
        }
    }
    return 0;
}

uint16_t ZmNetSocketBase::PeerPort(evutil_socket_t fd)
{
    if (Zm_IsSocketValid(fd))
    {
        struct sockaddr_in6 sa6 = { 0 };
        socklen_t           sa_len = sizeof(struct sockaddr_in6);
        struct sockaddr*    sa = (struct sockaddr*)&sa6;
        if (0 == getpeername(fd, sa, &sa_len))
        {
            return ntohs(sa6.sin6_port);
        }
    }
    return 0;
}

void ZmNetSocketBase::IgnoreSignalPipe(evutil_socket_t fd)
{
    if (Zm_IsSocketValid(fd))
    {
        // Windows 没有 SIGPIPE 信号，无需处理
    }
}

int ZmNetSocketBase::FindSrcAddr(evutil_socket_t fd, int sa_family,
    std::string& ip_str, std::string& mac_addr)
{
    if (!Zm_IsSocketValid(fd))
        return -1;

    // 根据地址族确定 sockaddr 的实际大小
    socklen_t len = 0;
    switch (sa_family)
    {
    case AF_INET:   len = sizeof(struct sockaddr_in);  break;
    case AF_INET6:  len = sizeof(struct sockaddr_in6); break;
    default:        return -1;
    }

    struct sockaddr_in6 src_addr6 = { 0 };
    struct sockaddr*    src_addr = (struct sockaddr*)&src_addr6;
    if (0 != getsockname(fd, src_addr, &len))
    {
        PUBLIC_LOG_ERROR("getsockname failed");
        return -1;
    }

    // 通过 ZmNetIP 将 sockaddr 转为可读的 IP 和 MAC
    ZmByteBuffer buf(2048);
    ZM_PEER_ADDR pa = { 0 };
    ZmNetIP::SockaddrToPeer(src_addr, &pa);
    char ip_str_buf[128] = { 0 };
    ZmNetIP::IPToStr(&pa.ip, ip_str_buf, 128);
    PUBLIC_LOG_INFO("FindSrcAddr ip:{}", ip_str_buf);
    ip_str = ip_str_buf;
    if (ZmNetIP::GetMacAddresses(buf.Str(), buf.Size(), &pa.ip, 0) > 0)
    {
        mac_addr = std::string(buf.Str());
        PUBLIC_LOG_INFO("FindSrcAddr mac_addr:{}", mac_addr.c_str());
    }
    return 0;
}

int ZmNetSocketBase::FindLocalAddrByDesAddress(std::string& ip_str, std::string& mac_str, std::string des_ip_str, int des_port)
{
    const int MAX_WAIT_TIME = 5000; ///< 最大等待时间（毫秒）
    int result = -1;
    struct addrinfo hints, *res = NULL;
    SOCKET sockfd = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;  ///< IPv4 和 IPv6 均可
    hints.ai_socktype = SOCK_DGRAM; ///< UDP 即可，不需要真正发包
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(des_ip_str.c_str(), std::to_string(des_port).c_str(), &hints, &res) != 0)
    {
        PUBLIC_LOG_ERROR("Error resolving server address");
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (!Zm_IsSocketValid(sockfd))
    {
        PUBLIC_LOG_ERROR("Error creating socket");
        goto cleanup;
    }
    {
        struct timeval tv;
        tv.tv_sec  = MAX_WAIT_TIME / 1000;
        tv.tv_usec = (MAX_WAIT_TIME % 1000) * 1000;
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0)
        {
            PUBLIC_LOG_ERROR("Error setting socket timeout");
            goto cleanup;
        }
    }

    // UDP connect 不会真正发包，仅让内核选择出口地址
    if (connect(sockfd, res->ai_addr, (int)res->ai_addrlen) < 0)
    {
        PUBLIC_LOG_ERROR("Error tcp connect");
        goto cleanup;
    }

    if (FindSrcAddr(sockfd, res->ai_family, ip_str, mac_str) < 0)
    {
        PUBLIC_LOG_ERROR("Error FindSrcAddr");
        goto cleanup;
    }

    result = 0;

cleanup:
    if (Zm_IsSocketValid(sockfd))
        closesocket(sockfd);
    if (res)
        freeaddrinfo(res);
    return result;
}

// ============================================================================
// ZmNetSocketBase — constructor / destructor
// ============================================================================

ZmNetSocketBase::ZmNetSocketBase()
{
}

ZmNetSocketBase::~ZmNetSocketBase()
{
}

// ============================================================================
// ZmNetSocketBase — non-virtual methods
// ============================================================================

int ZmNetSocketBase::SendAll(const void* buffer, size_t length)
{
    const unsigned char* ptr  = (const unsigned char*)buffer;
    size_t               left = length;
    while (left > 0)
    {
        int ret = Send(ptr, left);
        if (ret > 0)
        {
            ptr  += ret;
            left -= ret;
        }
        else if (ret < 0)
        {
            // 发送出错，立即返回错误码
            return ret;
        }
        else
        {
            // Send 返回 0，对端已关闭，停止发送
            break;
        }
    }
    return (int)(length - left);
}

int ZmNetSocketBase::RecvAll(void* buffer, size_t length)
{
    unsigned char* ptr  = (unsigned char*)buffer;
    size_t         left = length;
    while (left > 0)
    {
        int ret = Recv(ptr, left);
        if (ret > 0)
        {
            ptr  += ret;
            left -= ret;
        }
        else if (ret < 0)
        {
            // 接收出错，立即返回错误码
            return ret;
        }
        else
        {
            // Recv 返回 0，对端已关闭，停止接收
            break;
        }
    }
    return (int)(length - left);
}

// ============================================================================
// ZmNetSocketTCP — static methods
// ============================================================================

void ZmNetSocketTCP::CloseFD(evutil_socket_t fd, bool blocking)
{
    if (Zm_IsSocketValid(fd))
    {
        // 先优雅关闭发送/接收通道，再释放句柄
        ::shutdown(fd, SD_BOTH);
        if (blocking)
        {
            ::closesocket(fd);
        }
    }
}

void ZmNetSocketTCP::SetNonblocking(evutil_socket_t fd, bool nonBlocking)
{
    if (Zm_IsSocketValid(fd))
    {
        u_long nonBlock = nonBlocking ? 1 : 0;
        if (NO_ERROR != ioctlsocket(fd, FIONBIO, &nonBlock))
        {
            PUBLIC_LOG_ERROR("SetNonblocking(fd={}, nonBlock={}) failed: {}", fd, nonBlock, ZmSystem::ErrMsg(WSAGetLastError()));
        }
    }
}

void ZmNetSocketTCP::KeepAlive(evutil_socket_t fd, uint32_t idle, uint32_t interval)
{
    if (!Zm_IsSocketValid(fd))
    {
        PUBLIC_LOG_ERROR("Enable KeepAlive for FD:[{}] failed: Not a validate socket", fd);
        return;
    }

    // 先启用 SO_KEEPALIVE 选项，失败则直接返回
    BOOL keepalive = TRUE;
    if (SOCKET_ERROR == setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive)))
    {
        PUBLIC_LOG_ERROR("Enable KeepAlive for FD:[{}] setsockopt failed: {}", fd, ZmSystem::ErrMsg(WSAGetLastError()));
        return;
    }

    // 配置 KeepAlive 参数（毫秒单位）
    struct tcp_keepalive alive_in  = { 0 };
    struct tcp_keepalive alive_out = { 0 };

    if (idle > 0 && interval > 0)
    {
        alive_in.keepalivetime     = idle * 1000;
        alive_in.keepaliveinterval = interval * 1000;
    }
    else
    {
        alive_in.keepalivetime     = 60 * 1000; ///< 默认空闲 60 秒后开始探测
        alive_in.keepaliveinterval = 5 * 1000;  ///< 默认每 5 秒探测一次
    }

    alive_in.onoff = 1;
    unsigned long rbytes = 0;
    if (SOCKET_ERROR == WSAIoctl(fd, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in),
        &alive_out, sizeof(alive_out), &rbytes, NULL, NULL))
    {
        PUBLIC_LOG_ERROR("Enable KeepAlive for FD[{}] WSAIoctl failed: {}", fd, ZmSystem::ErrMsg(WSAGetLastError()));
    }
}

int ZmNetSocketTCP::ConnectTimeout(evutil_socket_t fd, const struct sockaddr* addr, socklen_t addrlen, float timeouts)
{
    int ret   = 0;
    int error = 0;

    // 超时值极小时走阻塞路径
    if (timeouts <= 0.01)
    {
        ret = ::connect(fd, addr, addrlen);
        if (0 == ret)
            return ZM_ERR_NOERROR;

        ret = WSAGetLastError();
        return ret ? ret : ENOTCONN;
    }

    // 超时路径：设为非阻塞 → connect → select 等待 → 恢复阻塞
    SetNonblocking(fd);
    ret = ::connect(fd, addr, addrlen);
    if (SOCKET_ERROR == ret && WSAEWOULDBLOCK != WSAGetLastError())
    {
        // 非 WSAEWOULDBLOCK 的错误，恢复阻塞后返回
        SetNonblocking(fd, false);
        return -1;
    }

    fd_set         r_set = { 0 };
    fd_set         w_set = { 0 };
    struct timeval tv    = { 0 };

    // 分离整数秒和微秒部分
    tv.tv_sec  = (long)timeouts;
    tv.tv_usec = (long)(timeouts * 1000000.0f) % 1000000L;
    FD_ZERO(&r_set);
    FD_SET(fd, &r_set);
    w_set = r_set;

    ret = ::select((int)fd + 1, &r_set, &w_set, NULL, &tv);
    if (0 == ret)
    {
        // select 超时
        WSASetLastError(WSAETIMEDOUT);
        return WSAETIMEDOUT;
    }
    else if (ret > 0)
    {
        // socket 可读或可写，需进一步检查连接是否真正成功
        ret = ZM_ERR_NOERROR;
        if (FD_ISSET(fd, &r_set) || FD_ISSET(fd, &w_set))
        {
            socklen_t len = sizeof(error);
            // getsockopt 获取 connect 的实际错误码
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0)
            {
                ret = ENOTCONN;
            }
        }
    }
    else
    {
        // select 本身出错
        ret = WSAGetLastError();
        ret = ret ? ret : -1;
    }

    if (error)
    {
        // 连接失败，设置 errno 供调用方诊断
        errno = error;
        ret = error;
    }
    else
    {
        // 成功，恢复为阻塞模式供后续 Send/Recv 使用
        SetNonblocking(fd, false);
    }

    return ret;
}

evutil_socket_t ZmNetSocketTCP::ProxyConnect(const char* dst_host, uint16_t dst_port,
    const char* proxy_host, uint16_t proxy_port, const char* proxy_ssl,
    const char* proxy_user, const char* proxy_pass)
{
    PUBLIC_LOG_INFO("Establishing the connection via proxy: dst={}:{}, proxy={}:{}@{}:{}, ssl={}",
        dst_host, dst_port, proxy_user, proxy_pass, proxy_host, proxy_port, proxy_ssl);

    evutil_socket_t fd = INVALID_SOCKET;
    // automatic=false，析构时不会自动关闭，由本函数控制生命周期
    ZmNetSocketTCP s(fd, false);
    if (s.Open(proxy_host, proxy_port))
    {
        char         userpass[128] = { 0 };
        ZmByteBuffer basic;
        char         buf[256] = { 0 };
        char         scheme[16] = { 0 };

        snprintf(userpass, sizeof(userpass), "%s:%s",
            proxy_user ? proxy_user : "", proxy_pass ? proxy_pass : "");

        if (ZmString::Equals(proxy_ssl, "ssl") || ZmString::Equals(proxy_ssl, "sslsmx"))
            snprintf(scheme, sizeof(scheme), "%s://", proxy_ssl);

        // 将 user:pass 做 Base64 编码用于 Proxy-Authorization 头
        ZmString::Base64Encode(basic, (const BYTE*)userpass, strlen(userpass));

        int len = 0;
        if (AF_INET6 == ZmNetIP::Validate(dst_host))
        {
            // IPv6 地址需用方括号包裹，如 [::1]:443
            len = snprintf(buf, sizeof(buf), "CONNECT %s[%s]:%d HTTP/1.1\r\n"
                "Proxy-Authorization: Basic %s\r\n\r\n",
                scheme, dst_host, dst_port, basic.Str());
        }
        else
        {
            len = snprintf(buf, sizeof(buf), "CONNECT %s%s:%d HTTP/1.1\r\n"
                "Proxy-Authorization: Basic %s\r\n\r\n",
                scheme, dst_host, dst_port, basic.Str());
        }

        if (s.Send(buf, len) == len)
        {
            memset(buf, 0, sizeof(buf));
            // 逐字节接收代理响应，直到遇到 \r\n\r\n 表示头部结束
            for (size_t i = 0; i < sizeof(buf); i++)
            {
                if (s.Recv(&buf[i], 1) == 1 && i > 4
                    && buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n')
                {
                    PUBLIC_LOG_INFO("Connect: {}:{}  rsp: {}", dst_host, dst_port, buf);
                    // 兼容 HTTP/1.0 和 HTTP/1.1 的 200 响应
                    if (0 == strncmp(buf, "HTTP/1.1 200 ", 13) || 0 == strncmp(buf, "HTTP/1.0 200 ", 13))
                    {
                        fd = s.GetFD();
                    }
                    break;
                }
            }
        }
    }

    if (!Zm_IsSocketValid(fd))
    {
        s.Close();
        fd = INVALID_SOCKET;
    }

    PUBLIC_LOG_INFO("Established the connection dst={}:{}, fd={}, result={}",
        dst_host, dst_port, fd, Zm_IsSocketValid(fd) ? "OK" : "Error");
    return fd;
}

// ============================================================================
// ZmNetSocketTCP — constructor / destructor
// ============================================================================

ZmNetSocketTCP::ZmNetSocketTCP(evutil_socket_t sock, bool automatic, bool keepalive)
    : ZmNetSocketBase(), m_sock(sock), m_automatic(automatic), m_keepalive(keepalive), m_port(0)
{
}

ZmNetSocketTCP::~ZmNetSocketTCP()
{
    if (m_automatic)
    {
        Close();
    }
}

// ============================================================================
// ZmNetSocketTCP — virtual overrides
// ============================================================================

bool ZmNetSocketTCP::Open(const char* host, uint16_t port, float timeouts)
{
    PUBLIC_LOG_INFO("TCP Open {}:{}, timeouts={}", host, port, timeouts);
    Close();

    struct sockaddr_in6 sa6 = { 0 };
    struct sockaddr*    sa  = (struct sockaddr*)&sa6;
    socklen_t           salen = 0;
    memset(&sa6, 0, sizeof(struct sockaddr_in6));

    // 通过 DNS 解析目标主机地址
    if ((salen = ZmNetDNS::GetAddressByName(&sa6, host, port)) < (socklen_t)sizeof(struct sockaddr_in))
    {
        PUBLIC_LOG_ERROR("Connecting to {}:{} resolve name failed", host, port);
        return false;
    }

    m_sock = ::socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (!Zm_IsSocketValid(m_sock))
    {
        PUBLIC_LOG_ERROR("Connecting to {}:{} create socket() failed", host, port);
        return false;
    }

    if (m_keepalive)
    {
        KeepAlive(m_sock);
    }

    int errcode = ConnectTimeout(m_sock, sa, salen, timeouts);
    if (ZM_ERR_NOERROR != errcode)
    {
        PUBLIC_LOG_ERROR("Connect to {}:{} failed: {}", host, port, ZmSystem::ErrMsg(errcode));
        Close();
    }

    return Zm_IsSocketValid(m_sock);
}

void ZmNetSocketTCP::Close()
{
    CloseFD(m_sock);
    m_sock = INVALID_SOCKET;
    m_port = 0;
}

int ZmNetSocketTCP::Send(const void* buffer, size_t length)
{
    int ret = ::send(m_sock, (const char*)buffer, (int)length, 0);
    if (ret < 0)
    {
        PUBLIC_LOG_ERROR("Send failed: {}", ZmSystem::ErrMsg(WSAGetLastError()));
    }
    return ret;
}

int ZmNetSocketTCP::Recv(void* buffer, size_t length)
{
    int ret = ::recv(m_sock, (char*)buffer, (int)length, 0);
    if (ret <= 0)
    {
        PUBLIC_LOG_ERROR("Recv failed: {}", ZmSystem::ErrMsg(WSAGetLastError()));
    }
    return ret;
}

bool ZmNetSocketTCP::IsConnected()
{
    if (!Zm_IsSocketValid(m_sock)) return false;

    // select 零超时轮询，不阻塞
    fd_set read_fds;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&read_fds);
    FD_SET(m_sock, &read_fds);

    int ret = ::select(0, &read_fds, NULL, NULL, &tv);
    if (ret <= 0) return true; ///< 无事件或 select 出错，保守认为仍连接

    // socket 可读，用 MSG_PEEK 偷看数据判断是对端关闭还是正常数据
    char buf;
    ret = ::recv(m_sock, &buf, 1, MSG_PEEK);
    return ret > 0;
}

// ============================================================================
// ZmNetSocketTCP — non-virtual methods
// ============================================================================

const evutil_socket_t ZmNetSocketTCP::GetFD() const
{
    return m_sock;
}

uint16_t ZmNetSocketTCP::Port()
{
    if (!m_port)
    {
        m_port = ZmNetSocketBase::LocalPort(m_sock);
    }
    return m_port;
}

// ============================================================================
// ZmNetSocketSSL — constructor / destructor
// ============================================================================

ZmNetSocketSSL::ZmNetSocketSSL(const char* servername)
    : ZmNetSocketBase(), m_bio(NULL)
{
    memset(m_servername, 0, sizeof(m_servername));
    snprintf(m_servername, sizeof(m_servername), "%s", ZmString::IsEmpty(servername) ? "" : servername);
}

ZmNetSocketSSL::~ZmNetSocketSSL()
{
    Close();
}

// ============================================================================
// ZmNetSocketSSL — virtual overrides
// ============================================================================

bool ZmNetSocketSSL::Open(const char* host, uint16_t port, float timeouts)
{
    PUBLIC_LOG_INFO("SSL Open {}:{}, timeouts={}", host, port, timeouts);

    Close();
    bool ret = false;
    evutil_socket_t raw_fd = INVALID_SOCKET; ///< IPv6 路径下跟踪原始 socket，失败时需手动清理

    do
    {
        char       hostname[64] = { 0 };
        ZM_IP_ADDR dst_ip = { 0 };
        int        protocol = ZmNetIP::Validate(host, &dst_ip);

        if (AF_INET6 == protocol)
        {
            snprintf(hostname, sizeof(hostname), "[%s]", host);

            // IPv6 路径：手动创建 socket → ConnectTimeout → 包装为 BIO
            struct sockaddr_in6 sa6 = { 0 };
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port   = htons(port);
            memcpy(sa6.sin6_addr.s6_addr, dst_ip.ipv6_u8, 16);

            raw_fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
            if (!Zm_IsSocketValid(raw_fd))
            {
                PUBLIC_LOG_ERROR("socket(AF_INET6) failed: {}", ZmSystem::ErrMsg(WSAGetLastError()));
                break;
            }
            if (0 != ZmNetSocketTCP::ConnectTimeout(raw_fd, (const struct sockaddr*)&sa6,
                (socklen_t)sizeof(struct sockaddr_in6), timeouts))
            {
                PUBLIC_LOG_ERROR("connect ipv6 failed: {}", ZmSystem::ErrMsg(WSAGetLastError()));
                break;
            }

            // 将 fd 包装进 BIO，BIO_CLOSE 使 BIO 析构时自动关闭 fd
            BIO* bcon = BIO_new_fd((int)raw_fd, BIO_CLOSE);
            if (!bcon)
            {
                unsigned long err = ERR_get_error();
                char buf[256];
                ERR_error_string_n(err, buf, sizeof(buf));
                PUBLIC_LOG_ERROR("BIO_new_fd failed: {}", buf);
                break;
            }

            if (!m_client_sslctx)
            {
                m_client_sslctx = ZmSSLContext::MakeClientCTX();
            }

            BIO* bssl = BIO_new_ssl(m_client_sslctx, 1);
            if (!bssl)
            {
                BIO_free(bcon);
                unsigned long err = ERR_get_error();
                char buf[256];
                ERR_error_string_n(err, buf, sizeof(buf));
                PUBLIC_LOG_ERROR("BIO_new_ssl failed: {}", buf);
                break;
            }

            // BIO_push 将 SSL BIO 挂在连接 BIO 之上，形成 BIO 链
            m_bio = BIO_push(bssl, bcon);
            if (!m_bio)
            {
                BIO_free(bcon);
                BIO_free(bssl);
                unsigned long err = ERR_get_error();
                char buf[256];
                ERR_error_string_n(err, buf, sizeof(buf));
                PUBLIC_LOG_ERROR("BIO_push failed: {}", buf);
                break;
            }
            // fd 所有权已移交给 BIO（BIO_CLOSE），此处不再负责关闭
            raw_fd = INVALID_SOCKET;
        }
        else
        {
            // IPv4 路径：使用 BIO_new_ssl_connect 自动处理连接
            snprintf(hostname, sizeof(hostname), "%s", host);

            if (!m_client_sslctx)
            {
                m_client_sslctx = ZmSSLContext::MakeClientCTX();
            }

            m_bio = BIO_new_ssl_connect(m_client_sslctx);
            if (!m_bio)
            {
                unsigned long err = ERR_get_error();
                char buf[256];
                ERR_error_string_n(err, buf, sizeof(buf));
                PUBLIC_LOG_ERROR("BIO_new_ssl_connect failed: {}", buf);
                break;
            }
        }

        BIO_set_conn_hostname(m_bio, hostname);

        char portstr[16] = { 0 };
        snprintf(portstr, sizeof(portstr), "%d", port);
        BIO_set_conn_port(m_bio, portstr);

        // 获取 SSL 对象用于 SNI 和后续配置
        SSL* ssl = NULL;
        BIO_get_ssl(m_bio, &ssl);
        if (!ssl)
        {
            unsigned long err = ERR_get_error();
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            PUBLIC_LOG_ERROR("BIO_get_ssl failed: {}", buf);
            break;
        }

        // 设置 SNI：优先使用构造时传入的 servername，为空则使用 host
        const char* sni = ZmString::IsEmpty(m_servername) ? host : m_servername;
        // 仅当 sni 是域名（非 IP 地址）时才设置 SNI
        if (0 == ZmNetIP::Validate(sni) && !SSL_set_tlsext_host_name(ssl, sni))
        {
            unsigned long err = ERR_get_error();
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            PUBLIC_LOG_ERROR("SSL_set_tlsext_host_name('{}') failed, error: {}", sni, buf);
            break;
        }

        // SSL_MODE_AUTO_RETRY 使 BIO_read/BIO_write 在遇到 renegotiation 时自动重试
        SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
        long errcode = BIO_do_connect(m_bio);
        if (errcode <= 0)
        {
            unsigned long err = ERR_get_error();
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            PUBLIC_LOG_ERROR("BIO_do_connect failed: {}", buf);
            break;
        }

        // 校验服务器证书指纹
        evutil_socket_t fd = (evutil_socket_t)BIO_get_fd(m_bio, NULL);
        if (!ZmSSLContext::ValidateSSLFingerprint(fd, ssl, host, port))
        {
            PUBLIC_LOG_ERROR("Check SSL fingerprint failed");
            break;
        }

        ZmNetSocketBase::IgnoreSignalPipe(fd);
        ret = true;
    } while (false);

    if (!ret)
    {
        Close();
        // IPv6 路径下 fd 所有权未移交 BIO 时需要手动清理
        if (Zm_IsSocketValid(raw_fd))
        {
            closesocket(raw_fd);
        }
    }
    return ret;
}

void ZmNetSocketSSL::Close()
{
    if (m_bio)
    {
        // BIO_free_all 释放整个 BIO 链（SSL BIO + 连接 BIO），同时关闭底层 fd
        BIO_free_all(m_bio);
    }
    m_bio = NULL;

    if (m_client_sslctx)
    {
        SSL_CTX_free(m_client_sslctx);
        CONF_modules_unload(0);
        m_client_sslctx = NULL;
    }
}

int ZmNetSocketSSL::Send(const void* buffer, size_t length)
{
    if (!m_bio) return -1;
    return BIO_write(m_bio, buffer, (int)length);
}

int ZmNetSocketSSL::Recv(void* buffer, size_t length)
{
    if (!m_bio) return -1;
    return BIO_read(m_bio, buffer, (int)length);
}

bool ZmNetSocketSSL::IsConnected()
{
    if (!m_bio) return false;

    SSL* ssl = NULL;
    BIO_get_ssl(m_bio, &ssl);
    if (!ssl) return false;

    // 检查是否收到对端的 shutdown 通知
    if (SSL_get_shutdown(ssl) & SSL_RECEIVED_SHUTDOWN) return false;

    int fd = (int)BIO_get_fd(m_bio, NULL);
    return Zm_IsSocketValid(fd);
}

// ============================================================================
// ZmNetSocketSSL — non-virtual methods
// ============================================================================

bool ZmNetSocketSSL::Handshake(evutil_socket_t fd, const char* hostname, uint16_t port)
{
    // TODO: 在已有 TCP 连接上附加 SSL 握手，参考 Open() 中的 IPv6 实现
    return false;
}
