#ifndef ZM_UTIL_LIBEVENT_H
#define ZM_UTIL_LIBEVENT_H

#include <event2/buffer.h>

/**
 * @brief socket 地址复用类型枚举
 */
enum class ZmSocketReuseType
{
    ZM_SO_REUSEADDR,           ///< 使用 SO_REUSEADDR 允许端口复用
    ZM_SO_REUSEADDR_EXCLUSIVE  ///< 使用 SO_EXCLUSIVEADDRUSE 独占端口
};

/**
 * @brief 初始化 libevent 线程支持和信号处理
 *
 * 必须在 event_base_new() 之前调用，全局调用一次即可。
 * - Windows: 调用 evthread_use_windows_threads() 启用线程安全锁
 * - Linux: 忽略 SIGPIPE 信号 + 调用 evthread_use_pthreads() 启用线程安全锁
 *
 * @example
 *   zm_util_eventbase_init();
 *   struct event_base* base = event_base_new();
 */
void zm_util_eventbase_init();

void zm_util_bufferevent_free(struct bufferevent* bev);

size_t zm_util_bufferevent_output_len(struct bufferevent* bev);

size_t zm_util_bufferevent_input_len(struct bufferevent* bev);

/**
 * @brief 创建非阻塞 TCP socket
 *
 * 根据目标地址解析地址族（IPv4/IPv6），创建对应协议族 socket，
 * 设置 SO_REUSEADDR 和非阻塞模式。
 *
 * @param address     目标主机地址（IP 或域名，仅用于判断地址族）
 * @param port        目标端口（仅用于 hints，不影响 socket 创建）
 * @param reuse_type  地址复用策略
 * @return            成功返回 socket 句柄，失败返回 -1
 *
 * @example
 *   evutil_socket_t fd = zm_util_create_socket_nonblock("127.0.0.1", 37310,
 *       ZmSocketReuseType::ZM_SO_REUSEADDR);
 *   if (fd >= 0) {
 *       struct bufferevent* bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
 *       // ...
 *   }
 */
evutil_socket_t zm_util_create_socket_nonblock(const char* address, uint16_t port,
    ZmSocketReuseType reuse_type);

/**
 * @brief 对 bufferevent 发起异步 TCP 连接
 *
 * 解析目标地址后调用 bufferevent_socket_connect() 发起异步连接。
 * 连接结果通过 bufferevent 的事件回调（BEV_EVENT_CONNECTED）通知。
 *
 * @param bev     已关联 socket 的 bufferevent 对象
 * @param address 目标主机地址（IP 或域名）
 * @param port    目标端口
 * @return        0 成功发起连接，-1 失败
 *
 * @example
 *   struct bufferevent* bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
 *   if (zm_util_bufferevent_socket_connect(bev, "127.0.0.1", 37310) == 0) {
 *       // 等待 BEV_EVENT_CONNECTED 回调
 *   }
 */
int zm_util_bufferevent_socket_connect(struct bufferevent* bev, const char* address, uint16_t port);


/**
 * @brief evbuffer 的 RAII 包装类，自动管理 evbuffer 的生命周期
 *
 * 构造时创建 evbuffer，析构时自动释放，提供常用数据访问接口。
 *
 * @example
 *   ZmEventBuffer buf;
 *   evbuffer_add_printf(buf.Buffer(), "hello");
 *   unsigned char* data = buf.PullUp();
 *   size_t len = buf.Length(); // 5
 */
class ZmEventBuffer
{
public:

    /**
     * @brief 构造函数，创建一个新的 evbuffer
     */
    ZmEventBuffer();

    /**
     * @brief 析构函数，释放 evbuffer 资源
     */
    ~ZmEventBuffer();

    /**
     * @brief 隐式转换为 evbuffer 指针，可直接传入需要 evbuffer* 的 libevent API
     *
     * @return 内部 evbuffer 指针
     */
    operator struct evbuffer* ();

    /**
     * @brief 获取内部 evbuffer 指针
     *
     * @return 内部 evbuffer 指针
     */
    struct evbuffer* Buffer();

    /**
     * @brief 获取 evbuffer 中已缓存数据的总长度
     *
     * @return 数据长度（字节数）
     */
    size_t           Length();

    /**
     * @brief 将分散的链式缓冲区合并为连续内存，返回指向数据首字节的指针
     *
     * 内部调用 evbuffer_pullup 将所有数据拷贝到一段连续内存中，
     * 便于按偏移直接读取（如解析协议头）。
     *
     * @return 指向连续数据的指针，数据长度为 Length()
     */
    unsigned char*   PullUp();

private:
    struct evbuffer* m_buf; ///< 内部 evbuffer 指针，由 libevent 管理
};

/**
 * @brief evbuffer 按行读取的 RAII 包装类，自动管理行数据的生命周期
 *
 * 从 evbuffer 中提取一行（以 CRLF 为分隔符），析构时自动释放行数据内存。
 * 读取后会从 evbuffer 中消费掉该行内容（含分隔符）。
 *
 * @example
 *   ZmEventBuffer buf;
 *   evbuffer_add_printf(buf.Buffer(), "hello\r\nworld\r\n");
 *   ZmEventLine line1(buf.Buffer()); // line1.Line() == "hello"
 *   ZmEventLine line2(buf.Buffer()); // line2.Line() == "world"
 */
class ZmEventLine
{
public:

    /**
     * @brief 从 evbuffer 中读取一行（以 CRLF 为分隔符）
     *
     * 若 evbuffer 中没有完整的 CRLF 结尾行，则 Line() 返回 NULL。
     *
     * @param buf 目标 evbuffer 指针
     */
    ZmEventLine(struct evbuffer* buf);

    /**
     * @brief 析构函数，释放行数据内存
     */
    ~ZmEventLine();

    /**
     * @brief 获取读取到的行内容（不含 CRLF 分隔符）
     *
     * @return 行内容字符串指针；若无完整行则返回 NULL
     */
    const char* Line();

    /**
     * @brief 获取行内容的字节长度（不含 CRLF 分隔符和末尾 '\0'）
     *
     * @return 行长度（字节数）
     */
    size_t      Length();

private:
    char*  m_line;   ///< 行内容指针，由 evbuffer_readln 分配，需 free 释放
    size_t m_length; ///< 行内容字节长度（不含分隔符）
};

#endif /* ZM_UTIL_LIBEVENT_H */
