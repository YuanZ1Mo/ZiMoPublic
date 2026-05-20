/**
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * !!!!!!!! 在各平台 AF_INET6 定义值不一致，因此在网络传输数据中指明 IPv4/IPv6 时不能使用 AF_INET6，应该自己定义常量 !!!!!!!!
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 */

#ifndef ZM_NET_IP_H
#define ZM_NET_IP_H

#include <event2/util.h>
#include <string>
#include <vector>

/**
 * @brief 存放 IP 地址的数据结构，可同时存放 IPv4 和 IPv6 地址
 *
 * IPv4 地址使用主机字节序（little-endian on x86）存储在 ipv4 字段中，
 * 高位字节在高位比特。IPv6 地址以网络字节序的原始字节数组形式存储。
 * 内部使用 union 共享 16 字节空间，IPv4 仅占用前 4 字节。
 */
typedef struct
{
    uint16_t     family;  // AF_INET(2), AF_INET6(10)
    union
    {
        struct
        {
            uint32_t ipv4;
            uint8_t  __ipv4_pad[12];
        };
        uint8_t      ipv6_u8[16];
        // uint16_t     ipv6_u16[8];
        uint32_t     ipv6_u32[4];
    };
}ZM_IP_ADDR;

/**
 * @brief 封装 IP 地址和端口号的对端地址结构
 *
 * 端口号使用主机字节序，IP 地址内部也是主机字节序。
 * 用于在应用层统一表示一个网络对端（IP + Port）。
 */
typedef struct
{
    uint16_t   port;
    ZM_IP_ADDR ip;
}ZM_PEER_ADDR;


/**
 * @brief IPv6 CIDR 子网信息或 IP 地址范围
 *
 * start/end 用于表示范围，network/broadcast 用于表示 CIDR 子网，
 * 两者通过 union 共享存储空间。mask 为子网掩码，bits 为前缀位数。
 */
typedef struct
{
    union
    {
        uint8_t     network[16];      // ip_min
        uint8_t     start[16];
    };
    union
    {
        uint8_t     broadcast[16];   // ip_max
        uint8_t     end[16];
    };
    uint8_t     mask[16];
    uint16_t    bits;
}ZM_IP_CIDR6, ZM_IP_RANGE6;



/**
 * @brief TCP 协议头部结构体（20 字节固定部分 + 12 字节选项/填充）
 */
typedef struct
{
    uint16_t    src_port;
    uint16_t    dst_port;
    uint32_t    seq_num;
    uint32_t    ack_num;

    uint8_t     reserved : 4;
    uint8_t     offset : 4;

    uint8_t     flags;
    uint16_t    window;

    uint16_t    checksum;
    uint16_t    urg_ptr;
    union
    {
        BYTE    padding[12];
        BYTE    options[12];
    };

}ZM_TCP_HEAD;

/**
 * @brief UDP 协议头部结构体（8 字节）
 */
typedef struct
{
    uint16_t    src_port;
    uint16_t    dst_port;
    uint16_t    udp_len;    /** 包括UDP数据报头部和"数据"部分 */
    uint16_t    checksum;
}ZM_UDP_HEAD;

/**
 * @brief ICMP 协议头部结构体（4 字节）
 */
typedef struct
{
    uint8_t     type;
    uint8_t     code;
    uint16_t    checksum;
}ZM_ICMP_HEAD;

typedef struct
{
    uint64_t    read_bytes;
    uint64_t    write_bytes;
}ZM_NET_FLOW;

/**
 * @brief IP 地址工具类，提供 IPv4/IPv6 地址的转换、比较、验证等功能
 *
 * 所有方法均为静态方法，无需实例化。该类封装了 sockaddr 与内部地址结构之间的
 * 转换、字符串形式的 IP 地址解析、MAC 地址查询等常用操作。
 *
 * @example IP 地址验证与转换
 * @code
 *   ZM_IP_ADDR addr;
 *   int family = ZmNetIP::Validate("192.168.1.1", &addr);  // 返回 AF_INET
 *   char str[64];
 *   ZmNetIP::IPToStr(&addr, str, sizeof(str));  // str = "192.168.1.1"
 * @endcode
 */
class ZmNetIP
{
private:
    ZmNetIP();

public:
    ~ZmNetIP();

    // === Address conversion ===

    /**
     * @brief 将 sockaddr 结构转换为 ZM_PEER_ADDR（主机序 IP + 端口）
     * @param sa   源 sockaddr 指针，支持 AF_INET / AF_INET6
     * @param addr 输出的对端地址结构，端口号和 IP 均转换为主机字节序
     */
    static void     SockaddrToPeer(const struct sockaddr* sa, ZM_PEER_ADDR* addr);

    /**
     * @brief 将 ZM_PEER_ADDR（主机序）转换为 sockaddr_in6 结构
     * @param addr 源对端地址结构
     * @param sa   输出的 sockaddr_in6 指针。
     *             若内部为 IPv4，则实际写入的是 sockaddr_in（复用同一块内存）
     */
    static void     SockaddrFromPeer(const ZM_PEER_ADDR* addr, struct sockaddr_in6* sa);

    /**
     * @brief 将 sockaddr 结构中的 IP 地址转换为可读字符串
     * @param sa       源 sockaddr 指针
     * @param ipstr    输出缓冲区，接收 IP 地址字符串
     * @param capacity 缓冲区大小（字节），建议至少 46 字节（IPv6 最长表示）
     * @param v4map    为 true 时，若为 IPv4-mapped IPv6 地址（::ffff:x.x.x.x），
     *                 则输出为 IPv4 格式字符串。默认 false
     * @return 指向 ipstr 的指针
     */
    static char*    SockaddrToStr(const struct sockaddr* sa, char* ipstr, size_t capacity, bool v4map = false);

    /**
     * @brief 将 IP 地址字符串解析为 sockaddr_in6 结构
     * @param ipstr IP 地址字符串，支持 IPv4 和 IPv6 格式
     * @param sa    输出的 sockaddr_in6 指针。若为 IPv4 则实际写入 sockaddr_in。
     *              可为 NULL（仅验证格式合法性）
     * @return AF_INET 表示 IPv4，AF_INET6 表示 IPv6，0 表示非法
     */
    static int      SockaddrFromStr(const char* ipstr, struct sockaddr_in6* sa);

    // === IP string conversion ===

    /**
     * @brief 从字符串解析 IP 地址到 ZM_IP_ADDR 结构，等价于 Validate()
     * @param ipstr IP 地址字符串
     * @param ip    输出的 IP 地址结构
     * @return AF_INET / AF_INET6 / 0
     */
    static int      IPFromStr(const char* ipstr, ZM_IP_ADDR* ip);

    /**
     * @brief 将 ZM_IP_ADDR 转换为可读 IP 地址字符串，自动判断 IPv4/IPv6
     * @param addr     IP 地址结构
     * @param ipstr    输出缓冲区
     * @param capacity 缓冲区大小
     * @return 指向 ipstr 的指针
     */
    static const char* IPToStr(const ZM_IP_ADDR* addr, char* ipstr, size_t capacity);

    /**
     * @brief 将 32 位整数形式的 IPv4 地址转换为点分十进制字符串
     *
     * 内部存储使用小端序（主机序），高位字节在高地址。
     * 例如 ipv4 = 0xC0A80101 表示 192.168.1.1。
     *
     * @param ipv4      32 位 IPv4 地址
     * @param str       输出缓冲区，至少 16 字节
     * @param bigendian 为 true 时 ipv4 按网络字节序（大端）解读。默认 false
     * @return 指向 str 的指针
     *
     * @example
     * @code
     *   char buf[16];
     *   // 主机序（小端）: 高位字节在高地址，0xC0A80101 -> "192.168.1.1"
     *   ZmNetIP::IPv4ToStr(0xC0A80101, buf, false);
     *   // 网络序（大端）: 高位字节在低地址，0x0101A8C0 -> "192.168.1.1"
     *   ZmNetIP::IPv4ToStr(0x0101A8C0, buf, true);
     * @endcode
     */
    static char*    IPv4ToStr(uint32_t ipv4, char* str, bool bigendian = false);

    /**
     * @brief 将 in6_addr 结构转换为 IPv6 可读字符串
     * @param in6      IPv6 地址结构指针
     * @param ipstr    输出缓冲区
     * @param capacity 缓冲区大小
     * @return 指向 ipstr 的指针，失败返回 NULL
     */
    static const char* IPv6ToStr(const struct in6_addr* in6, char* ipstr, size_t capacity);

    // === Validation & comparison ===

    /**
     * @brief 验证字符串是否为合法的 IPv4 或 IPv6 地址，并可选地解析到 ZM_IP_ADDR
     * @param ipstr 待验证的 IP 地址字符串，如 "192.168.1.1" 或 "::1"
     * @param ip    输出参数，接收解析后的 IP 地址，可为 NULL（仅验证不解析）
     * @return AF_INET 表示合法 IPv4，AF_INET6 表示合法 IPv6，0 表示非法地址
     *
     * @example
     * @code
     *   ZM_IP_ADDR addr;
     *   int ret = ZmNetIP::Validate("10.0.0.1", &addr);    // ret == AF_INET
     *   ret = ZmNetIP::Validate("fe80::1", &addr);          // ret == AF_INET6
     *   ret = ZmNetIP::Validate("not_an_ip", nullptr);      // ret == 0
     * @endcode
     */
    static int      Validate(const char* ipstr, ZM_IP_ADDR* ip = NULL);

    /**
     * @brief 比较两个 ZM_IP_ADDR 地址的大小
     * @param ip1 第一个 IP 地址
     * @param ip2 第二个 IP 地址
     * @return 0 表示相等，1 表示 ip1 > ip2，-1 表示 ip1 < ip2。
     *         不同 family 时，IPv6（AF_INET6=10）大于 IPv4（AF_INET=2）
     */
    static int      IPCompare(const ZM_IP_ADDR* ip1, const ZM_IP_ADDR* ip2);

    /**
     * @brief 比较主机序 ZM_IP_ADDR 与网络序 sockaddr 中的 IP 地址
     * @param ip1 主机字节序的 IP 地址
     * @param ip2 网络字节序的 sockaddr 地址（IPv4 取 sin_addr，IPv6 取 sin6_addr）
     * @return 0 表示相等，1 表示 ip1 > ip2，-1 表示 ip1 < ip2
     */
    static int      IPCompareHostNet(const ZM_IP_ADDR* ip1, const struct sockaddr* ip2);

    // === IP classification ===

    /**
     * @brief 判断 IP 地址是否为回环地址
     * @param ip 待判断的 IP 地址
     * @return true 表示是回环地址（IPv4: 127.0.0.0/8，IPv6: ::1）
     */
    static bool     IsLoopback(const ZM_IP_ADDR* ip);

    /**
     * @brief 判断 IP 地址是否为私有/内网地址
     * @param ip 待判断的 IP 地址
     * @return true 表示是私有地址（IPv4: 10/8, 172.16/12, 192.168/16；IPv6: fc00::/7）
     */
    static bool     IsPrivate(const ZM_IP_ADDR* ip);

    /**
     * @brief 判断 IP 地址是否为链路本地地址
     * @param ip 待判断的 IP 地址
     * @return true 表示是链路本地地址（IPv4: 169.254/16，IPv6: fe80::/10）
     */
    static bool     IsLinkLocal(const ZM_IP_ADDR* ip);

    /**
     * @brief 判断 IP 地址是否为组播地址
     * @param ip 待判断的 IP 地址
     * @return true 表示是组播地址（IPv4: 224/4，IPv6: ff00::/8）
     */
    static bool     IsMulticast(const ZM_IP_ADDR* ip);

    /**
     * @brief 判断 IPv6 地址是否为 IPv4-mapped IPv6 地址（::ffff:x.x.x.x）
     * @param ipv6_u32 指向 16 字节 IPv6 地址的 uint32_t 数组视图
     * @return true 表示是 IPv4-mapped IPv6 地址
     *
     * IPv4-mapped IPv6 格式: 前 80 位为 0，中间 16 位为 0xFFFF，最后 32 位为 IPv4 地址。
     * 对应的 u32 数组为 [0x00000000, 0x00000000, 0x0000FFFF, ipv4]
     */
    static bool     IsIPv4MappedIPv6(const uint32_t* ipv6_u32);

    // === IPv4 subnet ===

    /**
     * @brief 通过前缀位数生成 IPv4 子网掩码
     * @param bits 前缀位数（0~32）
     * @return 32 位主机序子网掩码
     *
     * @example
     * @code
     *   uint32_t mask = ZmNetIP::IPv4MaskFromBits(24);  // 0xFFFFFF00 -> 255.255.255.0
     *   mask = ZmNetIP::IPv4MaskFromBits(16);            // 0xFFFF0000 -> 255.255.0.0
     * @endcode
     */
    static uint32_t IPv4MaskFromBits(uint16_t bits);

    /**
     * @brief 计算 IPv4 子网掩码中从最高位开始连续 1 的位数
     * @param mask 32 位主机序子网掩码
     * @return 前缀位数（0~32）
     */
    static uint16_t IPv4SubnetBits(uint32_t mask);

    /**
     * @brief 计算 IPv4 子网的广播地址（主机位全部置 1）
     * @param subnet 网络地址（主机序）
     * @param bits   前缀位数（0~32）
     * @return 广播地址（主机序）
     *
     * @example
     * @code
     *   uint32_t subnet = 0xC0A80100;  // 192.168.1.0
     *   uint32_t bcast = ZmNetIP::IPv4SubnetBroadcast(subnet, 24);  // 0xC0A801FF -> 192.168.1.255
     * @endcode
     */
    static uint32_t IPv4SubnetBroadcast(uint32_t subnet, uint16_t bits);

    // === MAC address ===

    /**
     * @brief 获取系统网卡的 MAC 地址列表，或查询指定 IP 对应网卡的 MAC 地址
     * @param hwaddr   输出缓冲区，接收格式化后的 MAC 地址字符串（多个以 '\0' 分隔）
     * @param capacity 缓冲区大小（字节）
     * @param specip   指定 IP 地址，若非 NULL 则只返回该 IP 所在网卡的 MAC。默认 NULL
     * @param nMode    MAC 地址格式：0 为横线分隔（xx-xx-xx-xx-xx-xx），
     *                 1 为冒号分隔（xx:xx:xx:xx:xx:xx）。默认 0
     * @return 查询指定 IP 时返回 1（找到）或 0（未找到）；
     *         未指定 IP 时返回找到的以太网网卡 MAC 地址数量
     *
     * @example
     * @code
     *   char buf[256];
     *   // 获取所有网卡 MAC 地址（横线格式）
     *   size_t count = ZmNetIP::GetMacAddresses(buf, sizeof(buf));
     *   // 获取指定 IP 对应网卡的 MAC 地址（冒号格式）
     *   ZM_IP_ADDR ip;
     *   ZmNetIP::Validate("192.168.1.100", &ip);
     *   ZmNetIP::GetMacAddresses(buf, sizeof(buf), &ip, 1);
     * @endcode
     */
    static size_t   GetMacAddresses(char* hwaddr, size_t capacity, const ZM_IP_ADDR* specip = NULL, int nMode = 0);

    /**
     * @brief 通过已连接的 socket 文件描述符获取本地网卡 MAC 地址
     * @param fd 已建立连接的 socket 描述符
     * @return MAC 地址字符串（横线格式），失败返回空字符串
     */
    static std::string GetMacAddressOfFD(evutil_socket_t fd);

    // === Debug ===

    /**
     * @brief 将 sockaddr 地址信息打印输出用于调试
     * @param address 指向 sockaddr 结构体的指针，支持 AF_INET / AF_INET6
     * @param title   输出标题标签，默认为空字符串
     */
    static void     DumpAddress(const struct sockaddr* address, const char* title = "");

private:
    /**
     * @brief 判断 MAC 地址是否为有效的以太网地址（非全零）
     * @param addr 6 字节的 MAC 地址
     * @return true 表示有效
     */
    static bool     IsEthernetAddress(const BYTE* addr);
};



/**
 * @brief IPv6 地址工具类，提供 IPv6 地址的比较、运算、CIDR 子网计算等功能
 *
 * 所有方法均为静态方法。IPv6 地址以 16 字节的 uint8_t 数组表示（网络字节序）。
 *
 * @example IPv6 地址比较
 * @code
 *   uint8_t ip1[16] = {0x20,0x01,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
 *   uint8_t ip2[16] = {0x20,0x01,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,2};
 *   int cmp = ZmNetIPv6::Compare(ip1, ip2);  // cmp == -1 (ip1 < ip2)
 * @endcode
 */
class ZmNetIPv6
{
private:
    ZmNetIPv6();
    ~ZmNetIPv6();

public:
    // === Debug ===

    /**
     * @brief 将 IPv6 地址打印输出用于调试
     * @param label 标签前缀
     * @param ipv6  指向 16 字节 IPv6 地址的指针
     */
    static void     Dump(const char* label, const void* ipv6);

    // === Comparison & predicates ===

    /**
     * @brief 按字典序比较两个 IPv6 地址
     * @param ip1 第一个 IPv6 地址（16 字节）
     * @param ip2 第二个 IPv6 地址（16 字节）
     * @return 0 相等，1 表示 ip1 > ip2，-1 表示 ip1 < ip2
     */
    static int      Compare(const uint8_t* ip1, const uint8_t* ip2);

    /**
     * @brief 判断 IPv6 地址是否为全零地址（::）
     * @param ip 16 字节 IPv6 地址
     * @return true 表示全零
     */
    static bool     IsZero(const uint8_t* ip);

    /**
     * @brief 取两个 IPv6 地址中的较大值
     * @param max  输出结果，可以为 ip1 或 ip2 自身（原地操作）
     * @param ip1  第一个 IPv6 地址
     * @param ip2  第二个 IPv6 地址
     */
    static void     Max(uint8_t* max, const uint8_t* ip1, const uint8_t* ip2);

    /**
     * @brief 取两个 IPv6 地址中的较小值
     * @param max  输出结果，可以为 ip1 或 ip2 自身（原地操作）
     * @param ip1  第一个 IPv6 地址
     * @param ip2  第二个 IPv6 地址
     */
    static void     Min(uint8_t* max, const uint8_t* ip1, const uint8_t* ip2);

    /**
     * @brief 判断 IPv6 地址是否在指定闭区间内
     * @param ip    待判断的 IPv6 地址
     * @param left  区间左端点（包含）
     * @param right 区间右端点（包含）
     * @return true 表示 ip 在 [left, right] 范围内
     */
    static bool     Between(const uint8_t* ip, const uint8_t* left, const uint8_t* right);

    // === Arithmetic ===

    /**
     * @brief 交换两个 IPv6 地址的值
     * @param ip1 第一个 IPv6 地址
     * @param ip2 第二个 IPv6 地址
     */
    static void     Swap(uint8_t* ip1, uint8_t* ip2);

    /**
     * @brief 计算 IPv6 地址的下一个地址（+1），类似 IPv4 中 ip+1
     * @param ip     输入的 IPv6 地址（16 字节）
     * @param output 输出缓冲区，可为 NULL（此时结果写回 ip 本身）
     * @return 指向结果的指针（output 或 ip）
     *
     * 从最低字节开始加 1，若溢出（值为 255）则进位到更高字节。
     * 例如 ::FFFF -> ::1:0, ::FFFF:FFFF -> ::1:0:0
     * 注意: 全 FF 地址（FFFF:...:FFFF）溢出时会回绕为全零。
     */
    static uint8_t* Next(uint8_t* ip, uint8_t* output = NULL);

    /**
     * @brief 计算 IPv6 地址的上一个地址（-1），类似 IPv4 中 ip-1
     * @param ip     输入的 IPv6 地址（16 字节）
     * @param output 输出缓冲区，可为 NULL（此时结果写回 ip 本身）
     * @return 指向结果的指针（output 或 ip）
     *
     * 从最低字节开始减 1，若下溢（值为 0）则向更高字节借位。
     * 注意: 全零地址（::）下溢时会回绕为全 FF。
     */
    static uint8_t* Prev(uint8_t* ip, uint8_t* output = NULL);

    // === Bit operations ===

    /**
     * @brief 将 128 位 IPv6 地址整体左移 1 位
     * @param ip 16 字节 IPv6 地址，原地修改
     *
     * 从最高字节开始左移，高位丢弃，低位移入低位。
     * 相邻字节间通过检查 bit7 实现进位传递。
     */
    static void     BitShiftLeft(uint8_t* ip);

    /**
     * @brief 将 128 位 IPv6 地址整体右移 1 位
     * @param ip 16 字节 IPv6 地址，原地修改
     *
     * 从最低字节开始右移，低位丢弃，高位移入高位。
     * 相邻字节间通过检查 bit0 实现进位传递。
     */
    static void     BitShiftRight(uint8_t* ip);

    // === CIDR & subnet ===

    /**
     * @brief 初始化 CIDR 结构体，可选地填充各字段
     * @param cidr      待初始化的 CIDR 结构
     * @param network   网络地址（16 字节），NULL 则置零
     * @param broadcast 广播地址（16 字节），NULL 则置零
     * @param mask      子网掩码（16 字节），NULL 则置零
     * @param bits      前缀位数
     */
    static void     CIDRSet(ZM_IP_CIDR6* cidr, const uint8_t* network = NULL,
                            const uint8_t* broadcast = NULL, const uint8_t* mask = NULL, uint16_t bits = 0);

    /**
     * @brief 计算子网掩码中连续 1 的位数（从高位开始）
     * @param subnet 16 字节子网掩码
     * @return 前缀位数（0~128）
     *
     * 从掩码最低字节开始，逐位检查最低位是否为 1。
     * 遇到第一个 1 时停止，用总位数 128 减去已检查的位数即为前缀长度。
     */
    static uint16_t SubnetBits(const uint8_t* subnet);

    /**
     * @brief 通过前缀位数生成 16 字节子网掩码
     * @param mask 输出的 16 字节掩码
     * @param bits 前缀位数（0~128）
     *
     * 先全部置 0xFF，再根据 bits 清除后半部分，
     * 最后对跨越字节边界的部分逐位置位。
     */
    static void     MaskFromBits(uint8_t* mask, uint16_t bits);

    /**
     * @brief 计算子网广播地址（网络地址 | 取反掩码）
     * @param broadcast 输出的广播地址（16 字节）
     * @param subnet    网络地址（16 字节）
     * @param bits      前缀位数
     */
    static void     SubnetBroadcast(uint8_t* broadcast, const uint8_t* subnet, uint16_t bits);

    /**
     * @brief 判断 IP 地址是否属于指定的 CIDR 子网
     * @param network 网络地址（16 字节）
     * @param mask    子网掩码（16 字节）
     * @param ip      待判断的 IP 地址（16 字节）
     * @return true 表示 ip 在子网内
     *
     * 判断逻辑: (ip & mask) == network
     */
    static bool     CIDRBlockContaints(const uint8_t* network, const uint8_t* mask, const uint8_t* ip);

    /**
     * @brief 判断 IPv6 地址是否落在任一地址范围内
     * @param ranges6 地址范围列表
     * @param ipv6    待判断的 IPv6 地址（16 字节）
     * @return true 表示 ipv6 在某个范围内
     */
    static bool     RangeContaints(const std::vector<ZM_IP_CIDR6>& ranges6, const uint8_t* ipv6);

    /**
     * @brief 将 IPv6 地址范围拆分为一组 CIDR 子网块
     * @param cidrs 输出的 CIDR 列表
     * @param from  起始 IPv6 地址
     * @param to    结束 IPv6 地址
     *
     * 算法从起始地址开始，贪心地找到最大的 CIDR 块，然后前进到下一个地址继续，
     * 直到覆盖整个范围。
     *
     * @example
     * @code
     *   // 2001::1234 ~ 2001::5678 会被拆分为多个 CIDR 块
     *   std::vector<ZM_IP_CIDR6> cidrs;
     *   struct in6_addr from, to;
     *   evutil_inet_pton(AF_INET6, "2001::1234", &from);
     *   evutil_inet_pton(AF_INET6, "2001::5678", &to);
     *   ZmNetIPv6::CIDRFromRange(cidrs, &from, &to);
     * @endcode
     */
    static void     CIDRFromRange(std::vector<ZM_IP_CIDR6>& cidrs, const struct in6_addr* from, const struct in6_addr* to);

    /**
     * @brief CIDRFromRange 的字符串版本，自动解析 IP 字符串
     * @param cidrs 输出的 CIDR 列表
     * @param from  起始 IPv6 地址字符串
     * @param to    结束 IPv6 地址字符串
     */
    static void     CIDRFromRangeEx(std::vector<ZM_IP_CIDR6>& cidrs, const char* from, const char* to);

    /**
     * @brief 解析 IP 范围字符串为 ZM_IP_RANGE6 结构
     * @param range 输出的 IP 范围结构
     * @param ipstr 范围字符串，支持以下格式：
     *              - 单个 IP: "2001::1"
     *              - IP 范围: "2001::1-2001::100"
     *              - CIDR:    "2001::/64" 或 "2001::1/128"
     * @return true 解析成功
     *
     * @example
     * @code
     *   ZM_IP_RANGE6 range;
     *   ZmNetIPv6::ParseRange(&range, "2001::1");           // 单个 IP
     *   ZmNetIPv6::ParseRange(&range, "2001::1-2001::100"); // 范围
     *   ZmNetIPv6::ParseRange(&range, "2001::/64");         // CIDR
     * @endcode
     */
    static bool     ParseRange(ZM_IP_RANGE6* range, const char* ipstr);
};
#endif /* ZM_NET_IP_H */
