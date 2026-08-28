#include "zm_util_libevent.h"

#include "zm_util_logger.h"

#include <../libevent/include/event2/event.h>
#include <../libevent/include/event2/thread.h>
#include <../libevent/include/event2/bufferevent_ssl.h>


void OnLibEventFatalCB(int err)
{
    PUBLIC_LOG_ERROR("Received LibEvent fatal: code={}", err);
}

void OnLibEventLogCB(int severity, const char* msg)
{
    PUBLIC_LOG_INFO("Received LibEvent log: [{}]{}", severity, msg);
}

event_fatal_cb g_event_fatal_cb = nullptr;
event_log_cb g_event_log_cb = nullptr;

/**
 * @brief 初始化 libevent 线程支持和信号处理
 *
 * 必须在 event_base_new() 之前调用，全局调用一次。
 * - Windows: 调用 evthread_use_windows_threads() 启用 Windows 线程支持
 * - Linux: 忽略 SIGPIPE 信号 + 调用 evthread_use_pthreads() 启用 pthread 线程支持
 */
void zm_util_eventbase_init()
{
#ifdef _WIN32
    evthread_use_windows_threads();
#else
    signal(SIGPIPE, SIG_IGN);

    // signal() 在某些系统上会被自动恢复默认处理，sigaction() 更可靠，
    // 两者配合确保 SIGPIPE 始终被忽略
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    action.sa_flags   = 0;
    if (sigaction(SIGPIPE, (const struct sigaction*)&action, NULL) < 0)
    {
    }
    evthread_use_pthreads();
#endif

    if (!g_event_fatal_cb)
    {
        g_event_fatal_cb = OnLibEventFatalCB;
        event_set_fatal_callback(g_event_fatal_cb);
    }

    if (!g_event_log_cb)
    {
        g_event_log_cb = OnLibEventLogCB;
        event_set_log_callback(g_event_log_cb);
    }
}

void zm_util_bufferevent_free(struct bufferevent* bev)
{
    if (!bev) {
        return;
    }

    // 获取 socket 描述符
    evutil_socket_t fd = bufferevent_getfd(bev);
    if (fd != -1) {
#ifdef _WIN32
        ::shutdown(fd, SD_BOTH);
#else
        ::shutdown(fd, SHUT_RDWR);
#endif
    }

    // 直接释放，不再手动干预 SSL 对象
    bufferevent_free(bev);
}

size_t zm_util_bufferevent_output_len(struct bufferevent* bev)
{
    //可能对于sslevent,underlying不可读，bufferevent_get_output 返回 NULL
    evbuffer* buffer = bufferevent_get_output(bev);
    return buffer ? evbuffer_get_length(buffer) : 0;
}

size_t zm_util_bufferevent_input_len(struct bufferevent* bev)
{
    //可能对于sslevent,underlying不可读，bufferevent_get_intput 返回 NULL
    evbuffer* buffer = bufferevent_get_input(bev);
    return buffer ? evbuffer_get_length(buffer) : 0;
}

evutil_socket_t zm_util_create_socket_nonblock(const char* address, uint16_t port,
    ZmSocketReuseType reuse_type)
{
    if (address == nullptr || address[0] == '\0')
    {
        return -1;
    }

    (void)(port);

    /* 根据地址字符串判断地址族（简单判断：含冒号为 IPv6，否则 IPv4） */
    int family = AF_INET;
    if (strchr(address, ':'))
    {
        family = AF_INET6;
    }

    evutil_socket_t fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    /* 设置地址复用 */
    int optval = 1;
    if (reuse_type == ZmSocketReuseType::ZM_SO_REUSEADDR)
    {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&optval), sizeof(optval));
    }

    /* 设置为非阻塞模式 */
    if (evutil_make_socket_nonblocking(fd) < 0)
    {
        evutil_closesocket(fd);
        return -1;
    }

    return fd;
}

int zm_util_bufferevent_socket_connect(struct bufferevent* bev, const char* address, uint16_t port)
{
    if (bev == nullptr || address == nullptr || address[0] == '\0')
    {
        return -1;
    }

    /* 构造 sockaddr */
    struct sockaddr_storage ss = {};
    int ss_len = sizeof(ss);

    if (evutil_parse_sockaddr_port(address, reinterpret_cast<struct sockaddr*>(&ss), &ss_len) < 0)
    {
        /* 尝试作为域名解析（本次同步解析，实际异步应使用 evdns） */
        struct evutil_addrinfo hints;
        struct evutil_addrinfo* ai = nullptr;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);

        if (evutil_getaddrinfo(address, port_str, &hints, &ai) != 0)
        {
            return -1;
        }

        if (ai == nullptr)
        {
            return -1;
        }

        memcpy(&ss, ai->ai_addr, ai->ai_addrlen);
        ss_len = static_cast<int>(ai->ai_addrlen);
        evutil_freeaddrinfo(ai);
    }
    else
    {
        /* evutil_parse_sockaddr_port 成功，手动设置端口 */
        if (ss.ss_family == AF_INET)
        {
            reinterpret_cast<struct sockaddr_in*>(&ss)->sin_port = htons(port);
        }
        else if (ss.ss_family == AF_INET6)
        {
            reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_port = htons(port);
        }
    }

    /* 发起异步连接 */
    if (bufferevent_socket_connect(bev, reinterpret_cast<struct sockaddr*>(&ss), ss_len) < 0)
    {
        return -1;
    }

    return 0;
}

// ZmEventBuffer

/**
 * @brief 构造函数，创建一个新的 evbuffer
 */
ZmEventBuffer::ZmEventBuffer()
{
    m_buf = evbuffer_new();
}

/**
 * @brief 析构函数，释放 evbuffer 资源
 */
ZmEventBuffer::~ZmEventBuffer()
{
    evbuffer_free(m_buf);
}

/**
 * @brief 隐式转换为 evbuffer 指针
 *
 * @return 内部 evbuffer 指针
 */
ZmEventBuffer::operator struct evbuffer* ()
{
    return m_buf;
}

/**
 * @brief 获取内部 evbuffer 指针
 *
 * @return 内部 evbuffer 指针
 */
struct evbuffer* ZmEventBuffer::Buffer()
{
    return m_buf;
}

/**
 * @brief 获取 evbuffer 中已缓存数据的总长度
 *
 * @return 数据长度（字节数）
 */
size_t ZmEventBuffer::Length()
{
    return evbuffer_get_length(m_buf);
}

/**
 * @brief 将分散的链式缓冲区合并为连续内存，返回数据指针
 *
 * @return 指向连续数据的指针
 */
unsigned char* ZmEventBuffer::PullUp()
{
    return evbuffer_pullup(m_buf, Length());
}

// ZmEventLine

/**
 * @brief 从 evbuffer 中读取一行
 *
 * @param buf 目标 evbuffer 指针
 */
ZmEventLine::ZmEventLine(struct evbuffer* buf)
{
    m_line = evbuffer_readln(buf, &m_length, EVBUFFER_EOL_CRLF);
}

/**
 * @brief 析构函数，释放行数据内存
 */
ZmEventLine::~ZmEventLine()
{
    if (m_line)
    {
        free(m_line);
    }
    m_line = NULL;
}

/**
 * @brief 获取读取到的行内容
 *
 * @return 行内容字符串指针；若无完整行则返回 NULL
 */
const char* ZmEventLine::Line()
{
    return m_line;
}

/**
 * @brief 获取行内容的字节长度
 *
 * @return 行长度（字节数）
 */
size_t ZmEventLine::Length()
{
    return m_length;
}
