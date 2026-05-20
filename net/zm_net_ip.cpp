#include "zm_net_ip.h"

#include "../util/zm_util_str.h"
#include "../spdlog/zm_logger.h"

#include <Iphlpapi.h>
#pragma comment(lib, "Iphlpapi")

/** IPv4 点分十进制格式化字符串模板，配合 printf/snprintf 使用 */
#define ZM_IPV4_CANON_FORMAT            "%d.%d.%d.%d"
/** 将主机序（小端）的 32 位 IPv4 地址展开为 printf 可变参数列表，与 ZM_IPV4_CANON_FORMAT 配合使用 */
#define ZM_IPV4_CANON_ARGS_LE(ipv4)     ((ipv4)>>24)&0xFF, ((ipv4)>>16)&0xFF, ((ipv4)>>8)&0xFF, (ipv4)&0xFF

/** in6_addr 结构体的全零初始化器，用于定义时初始化：struct in6_addr addr = ZM_IN6_ADDR_INIT() */
#define ZM_IN6_ADDR_INIT()  { {{0}} }

/** ZM_IP_CIDR6 结构体的全零初始化器，用于定义时初始化：ZM_IP_CIDR6 cidr = ZM_IP_CIDR6_INIT() */
#define ZM_IP_CIDR6_INIT()  {{{0}}, {{0}}, {0}, 0}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmNetIP

/**
 * @brief 将 sockaddr 结构转换为 ZM_PEER_ADDR（主机序 IP + 端口）
 *
 * 先清零输出结构，再将网络序的端口和 IP 地址转为主机序存入。
 */
void ZmNetIP::SockaddrToPeer(const struct sockaddr* sa, ZM_PEER_ADDR* addr)
{
    memset(addr, 0, sizeof(ZM_PEER_ADDR));
    addr->ip.family = sa->sa_family;
    if (sa->sa_family == AF_INET6)
    {
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)sa;
        addr->port = ntohs(in6->sin6_port);
        memcpy(addr->ip.ipv6_u8, &in6->sin6_addr, 16);
    }
    else
    {
        const struct sockaddr_in* in4 = (const struct sockaddr_in*)sa;
        addr->port = ntohs(in4->sin_port);
        // IPv4 网络序转主机序存储
        addr->ip.ipv4 = ntohl(in4->sin_addr.s_addr);
    }
}

/**
 * @brief 将 ZM_PEER_ADDR（主机序）转换为 sockaddr_in6 结构
 *
 * IPv4 时虽然参数类型是 sockaddr_in6，但实际写入 sockaddr_in（复用同一块内存，
 * 因为 sockaddr_in 大小不超过 sockaddr_in6）。
 */
void ZmNetIP::SockaddrFromPeer(const ZM_PEER_ADDR* addr, struct sockaddr_in6* sa)
{
    memset(sa, 0, sizeof(struct sockaddr_in6));
    if (addr->ip.family == AF_INET6)
    {
        sa->sin6_family = AF_INET6;
        memcpy(sa->sin6_addr.s6_addr, addr->ip.ipv6_u8, 16);
        sa->sin6_port = htons(addr->port);
    }
    else
    {
        struct sockaddr_in* sa4 = (struct sockaddr_in*)sa;
        sa4->sin_family = AF_INET;
        // 主机序 IPv4 转回网络序
        sa4->sin_addr.s_addr = htonl(addr->ip.ipv4);
        sa4->sin_port = htons(addr->port);
    }
}

/**
 * @brief 将 sockaddr 结构中的 IP 地址转换为可读字符串
 *
 * 对于 IPv6 地址，当 v4map=true 且地址为 IPv4-mapped 格式（::ffff:x.x.x.x）
 * 或 IPv4-compatible 格式（::x.x.x.x）时，提取后 4 字节输出为 IPv4 字符串。
 */
char* ZmNetIP::SockaddrToStr(const struct sockaddr* sa, char* ipstr, size_t capacity, bool v4map)
{
    if (AF_INET6 == sa->sa_family)
    {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)sa;
        uint32_t* u6_addr32 = (uint32_t*)sa6->sin6_addr.s6_addr;
        // 检查前 64 位是否为 0，且中间 16 位为 0xFFFF 或 0（IPv4-mapped / IPv4-compatible）
        if (v4map && (0 == u6_addr32[0] && 0 == u6_addr32[1]
            && (u6_addr32[2] == htonl(0x0FFFFU) || 0 == u6_addr32[2])))
        {
            // 取最后 32 位作为 IPv4 地址转换
            const uint32_t* u6_addr32 = (const uint32_t*)sa6->sin6_addr.s6_addr;
            evutil_inet_ntop(AF_INET, (const void*)&u6_addr32[3], ipstr, (socklen_t)capacity);
        }
        else
        {
            evutil_inet_ntop(AF_INET6, (const void*)&sa6->sin6_addr, ipstr, (socklen_t)capacity);
        }
    }
    else
    {
        struct sockaddr_in* sa4 = (struct sockaddr_in*)sa;
        evutil_inet_ntop(AF_INET, (const void*)&sa4->sin_addr, ipstr, (socklen_t)capacity);
    }
    return ipstr;
}

/**
 * @brief 将 IP 地址字符串解析为 sockaddr_in6 结构
 *
 * IPv4 时实际写入 sockaddr_in（复用参数内存），IPv6 时写入完整的 sockaddr_in6。
 */
int ZmNetIP::SockaddrFromStr(const char* ipstr, struct sockaddr_in6* sa)
{
    struct in6_addr in6 = ZM_IN6_ADDR_INIT();
    if (1 == evutil_inet_pton(AF_INET, ipstr, &in6))
    {
        if (sa)
        {
            struct in_addr* in4 = (struct in_addr*)&in6;
            struct sockaddr_in* sa4 = (struct sockaddr_in*)sa;
            memset(sa4, 0, sizeof(struct sockaddr_in));
            sa4->sin_family = AF_INET;
            sa4->sin_addr.s_addr = in4->s_addr;
        }
        return AF_INET;
    }
    else if (1 == evutil_inet_pton(AF_INET6, ipstr, &in6))
    {
        if (sa)
        {
            memset(sa, 0, sizeof(struct sockaddr_in6));
            sa->sin6_family = AF_INET6;
            memcpy(&sa->sin6_addr, &in6, sizeof(struct in6_addr));
        }
        return AF_INET6;
    }
    return 0;
}

/**
 * @brief 从字符串解析 IP 地址到 ZM_IP_ADDR 结构，等价于 Validate()
 */
int ZmNetIP::IPFromStr(const char* ipstr, ZM_IP_ADDR* ip)
{
    return Validate(ipstr, ip);
}

/**
 * @brief 将 ZM_IP_ADDR 转换为可读 IP 地址字符串，自动判断 IPv4/IPv6
 */
const char* ZmNetIP::IPToStr(const ZM_IP_ADDR* addr, char* ipstr, size_t capacity)
{
    return (addr->family == AF_INET6) ? IPv6ToStr((const struct in6_addr*)addr->ipv6_u8, ipstr, capacity)
        : IPv4ToStr(addr->ipv4, ipstr, false);
}

/**
 * @brief 将 32 位整数形式的 IPv4 地址转换为点分十进制字符串
 */
char* ZmNetIP::IPv4ToStr(uint32_t ipv4, char* str, bool bigendian)
{
    if (bigendian)
    {
        snprintf(str, 16, "%d.%d.%d.%d", ipv4 & 0xFF, (ipv4 >> 8) & 0xFF, (ipv4 >> 16) & 0xFF, (ipv4 >> 24) & 0xFF);
    }
    else
    {
        snprintf(str, 16, "%d.%d.%d.%d", (ipv4 >> 24) & 0xFF, (ipv4 >> 16) & 0xFF, (ipv4 >> 8) & 0xFF, ipv4 & 0xFF);
    }
    return str;
}

/**
 * @brief 将 in6_addr 结构转换为 IPv6 可读字符串
 */
const char* ZmNetIP::IPv6ToStr(const struct in6_addr* in6, char* ipstr, size_t capacity)
{
    return evutil_inet_ntop(AF_INET6, (const void*)in6, ipstr, (socklen_t)capacity);
}

/**
 * @brief 验证字符串是否为合法的 IPv4 或 IPv6 地址，并可选地解析到 ZM_IP_ADDR
 *
 * 先尝试 IPv4 解析，失败再尝试 IPv6。IPv4 结果转为主机序存储。
 */
int ZmNetIP::Validate(const char* ipstr, ZM_IP_ADDR* ip)
{
    struct in6_addr dst6 = ZM_IN6_ADDR_INIT();
    // Check valid IPv4.
    if (evutil_inet_pton(AF_INET, ipstr, &dst6) == 1)
    {
        if (ip)
        {
            struct in_addr* dst4 = (struct in_addr*)&dst6;
            ip->family = AF_INET;
            // pton 输出为网络序，转为主机序存储
            ip->ipv4 = ntohl(dst4->s_addr);
        }
        return AF_INET;
    }
    // Check valid IPv6.
    else if (evutil_inet_pton(AF_INET6, ipstr, &dst6) == 1)
    {
        if (ip)
        {
            ip->family = AF_INET6;
            memcpy(ip->ipv6_u8, dst6.s6_addr, 16);
        }
        return AF_INET6;
    }
    return 0;
}

/**
 * @brief 比较两个 ZM_IP_ADDR 地址的大小
 *
 * 同 family 时逐字节（IPv6）或直接比较 uint32（IPv4）；
 * 不同 family 时 IPv6（AF_INET6=10）大于 IPv4（AF_INET=2）。
 */
int ZmNetIP::IPCompare(const ZM_IP_ADDR* ip1, const ZM_IP_ADDR* ip2)
{
    if (ip1->family == ip2->family)
    {
        if (ip1->family == AF_INET6)
        {
            return ZmNetIPv6::Compare(ip1->ipv6_u8, ip2->ipv6_u8);
        }
        else
        {
            return ip1->ipv4 == ip2->ipv4 ? 0 : (ip1->ipv4 > ip2->ipv4 ? 1 : -1);
        }
    }
    else
    {
        return ip1->family > ip2->family ? 1 : -1;
    }
}

/**
 * @brief 比较主机序 ZM_IP_ADDR 与网络序 sockaddr 中的 IP 地址
 *
 * 对 IPv4 先将 sockaddr 中的网络序地址通过 ntohl 转为主机序再比较。
 * 对 IPv6 直接逐字节比较（IPv6 存储本身就是网络字节序的字节数组）。
 */
int ZmNetIP::IPCompareHostNet(const ZM_IP_ADDR* ip1, const struct sockaddr* ip2)
{
    if (ip1->family == ip2->sa_family)
    {
        if (ip1->family == AF_INET6)
        {
            const struct sockaddr_in6* sa6 = (const struct sockaddr_in6*)ip2;
            return ZmNetIPv6::Compare(ip1->ipv6_u8, sa6->sin6_addr.s6_addr);
        }
        else
        {
            const struct sockaddr_in* sa4 = (const struct sockaddr_in*)ip2;
            // 将 sockaddr 中的网络序 IPv4 转为主机序后比较
            uint32_t ipv4 = ntohl(sa4->sin_addr.s_addr);
            return ip1->ipv4 == ipv4 ? 0 : (ip1->ipv4 > ipv4 ? 1 : -1);
        }
    }
    else
    {
        return ip1->family > ip2->sa_family ? 1 : -1;
    }
}

/**
 * @brief 判断 IP 地址是否为回环地址
 *
 * IPv4: 127.0.0.0/8（最高字节为 127）
 * IPv6: ::1（前 15 字节全零，最后一字节为 1）
 */
bool ZmNetIP::IsLoopback(const ZM_IP_ADDR* ip)
{
    if (ip->family == AF_INET6)
    {
        for (int i = 0; i < 15; i++)
        {
            if (ip->ipv6_u8[i] != 0) { return false; }
        }
        return ip->ipv6_u8[15] == 1;
    }
    return (ip->ipv4 >> 24) == 127;
}

/**
 * @brief 判断 IP 地址是否为私有/内网地址
 *
 * IPv4:
 *   - 10.0.0.0/8      (A类私有)
 *   - 172.16.0.0/12    (B类私有)
 *   - 192.168.0.0/16   (C类私有)
 * IPv6:
 *   - fc00::/7         (唯一本地地址，含 fc00:: 和 fd00::)
 */
bool ZmNetIP::IsPrivate(const ZM_IP_ADDR* ip)
{
    if (ip->family == AF_INET6)
    {
        // fc00::/7: 最高字节的最高 7 位为 1111110，即 (byte & 0xFE) == 0xFC
        return (ip->ipv6_u8[0] & 0xFE) == 0xFC;
    }
    // 10.0.0.0/8
    if ((ip->ipv4 >> 24) == 10) { return true; }
    // 172.16.0.0/12: 高 12 位为 0xAC1
    if ((ip->ipv4 >> 20) == 0xAC1) { return true; }
    // 192.168.0.0/16
    if ((ip->ipv4 >> 16) == 0xC0A8) { return true; }
    return false;
}

/**
 * @brief 判断 IP 地址是否为链路本地地址
 *
 * IPv4: 169.254.0.0/16
 * IPv6: fe80::/10
 */
bool ZmNetIP::IsLinkLocal(const ZM_IP_ADDR* ip)
{
    if (ip->family == AF_INET6)
    {
        // fe80::/10: 最高字节为 0xFE，次字节高 2 位为 10
        return ip->ipv6_u8[0] == 0xFE && (ip->ipv6_u8[1] & 0xC0) == 0x80;
    }
    // 169.254.0.0/16: 高 16 位为 0xA9FE
    return (ip->ipv4 >> 16) == 0xA9FE;
}

/**
 * @brief 判断 IP 地址是否为组播地址
 *
 * IPv4: 224.0.0.0/4（最高 4 位为 1110，即 224~239）
 * IPv6: ff00::/8（最高字节为 0xFF）
 */
bool ZmNetIP::IsMulticast(const ZM_IP_ADDR* ip)
{
    if (ip->family == AF_INET6)
    {
        return ip->ipv6_u8[0] == 0xFF;
    }
    // 224.0.0.0/4: 高 4 位为 0xE (1110)
    return (ip->ipv4 >> 28) == 0xE;
}

/**
 * @brief 判断 IPv6 地址是否为 IPv4-mapped IPv6 地址（::ffff:x.x.x.x）
 *
 * IPv4-mapped IPv6 格式: 前 80 位为 0，中间 16 位为 0xFFFF，最后 32 位为 IPv4 地址。
 * 对应的 u32 数组为 [0x00000000, 0x00000000, 0x0000FFFF, ipv4]
 */
bool ZmNetIP::IsIPv4MappedIPv6(const uint32_t* ipv6_u32)
{
    return ipv6_u32[0] == 0 && ipv6_u32[1] == 0 && ipv6_u32[2] == htonl(0x0FFFFU);
}

/**
 * @brief 通过前缀位数生成 IPv4 子网掩码
 *
 * bits=0 返回 0，bits=32 返回 0xFFFFFFFF。
 * bits < 32 时，高位 bits 个 1，其余为 0。
 */
uint32_t ZmNetIP::IPv4MaskFromBits(uint16_t bits)
{
    if (bits >= 32) { return 0xFFFFFFFF; }
    if (bits == 0) { return 0; }
    return ~((1U << (32 - bits)) - 1);
}

/**
 * @brief 计算 IPv4 子网掩码中从最高位开始连续 1 的位数
 *
 * 从最高位向低位逐位检查，遇到第一个 0 停止。
 */
uint16_t ZmNetIP::IPv4SubnetBits(uint32_t mask)
{
    uint16_t bits = 0;
    for (int i = 31; i >= 0; i--)
    {
        if (mask & (1U << i))
        {
            bits++;
        }
        else
        {
            break;
        }
    }
    return bits;
}

/**
 * @brief 计算 IPv4 子网的广播地址
 *
 * 广播地址 = 网络地址 | (~子网掩码)
 */
uint32_t ZmNetIP::IPv4SubnetBroadcast(uint32_t subnet, uint16_t bits)
{
    uint32_t mask = IPv4MaskFromBits(bits);
    return subnet | (~mask);
}

/**
 * @brief 获取系统网卡的 MAC 地址列表，或查询指定 IP 对应网卡的 MAC 地址
 *
 * 使用 Windows IP Helper API (GetAdaptersAddresses) 枚举所有网卡。
 * 当指定 specip 时，遍历网卡的单播地址寻找匹配项，找到后立即返回。
 * 当不指定 specip 时，遍历所有有效以太网网卡，将 MAC 地址格式化后依次写入缓冲区。
 */
size_t ZmNetIP::GetMacAddresses(char* hwaddr, size_t capacity, const ZM_IP_ADDR* specip, int nMode)
{
#define _SP_FORMAT_HWADDR(str, space, addr) snprintf((str), (space), \
                                                "%02x-%02x-%02x-%02x-%02x-%02x", \
                                                (addr)[0], (addr)[1], (addr)[2], \
                                                (addr)[3], (addr)[4], (addr)[5])

#define _SP_FORMAT_HWADDRB(str, space, addr) snprintf((str), (space), \
                                                "%02x:%02x:%02x:%02x:%02x:%02x", \
                                                (addr)[0], (addr)[1], (addr)[2], \
                                                (addr)[3], (addr)[4], (addr)[5])
    if (!hwaddr || !capacity)
    {
        return 0;
    }
    size_t count = 0;
    size_t offset = 0;
    memset(hwaddr, 0, capacity);

    // 第一次调用获取所需缓冲区大小
    ULONG size = 0;
    if (ERROR_BUFFER_OVERFLOW != GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size))
    {
        return 0;
    }
    ZmByteBuffer buf(size);
    IP_ADAPTER_ADDRESSES* adapters = (IP_ADAPTER_ADDRESSES*)buf.Head();
    // 第二次调用获取实际适配器信息
    if (NO_ERROR != GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters, &size))
    {
        return 0;
    }

    for (IP_ADAPTER_ADDRESSES* aa = adapters; aa != NULL; aa = aa->Next)
    {
        if (specip)
        {
            // 指定了 IP 时，遍历该网卡的所有单播地址寻找匹配
            for (IP_ADAPTER_UNICAST_ADDRESS* ua = aa->FirstUnicastAddress; ua != NULL; ua = ua->Next)
            {
                if (0 == ZmNetIP::IPCompareHostNet(specip, ua->Address.lpSockaddr))
                {
                    if (nMode == 1)
                    {
                        _SP_FORMAT_HWADDRB(hwaddr, capacity, aa->PhysicalAddress);
                    }
                    else
                    {
                        _SP_FORMAT_HWADDR(hwaddr, capacity, aa->PhysicalAddress);
                    }

                    return 1;
                }
            }
        }
        else if (offset < capacity && IsEthernetAddress(aa->PhysicalAddress))
        {
            // 未指定 IP 时，格式化所有有效以太网网卡的 MAC 地址
            if (nMode == 1)
            {
                offset += _SP_FORMAT_HWADDRB(hwaddr + offset, capacity - offset, aa->PhysicalAddress);
            }
            else
            {
                offset += _SP_FORMAT_HWADDR(hwaddr + offset, capacity - offset, aa->PhysicalAddress);
            }

            offset++;
            count++;
        }
    }

    return count;
}

/**
 * @brief 通过已连接的 socket 文件描述符获取本地网卡 MAC 地址
 *
 * 流程: getsockname 获取本地绑定的 IP -> SockaddrToPeer 转为主机序 ->
 *       GetMacAddresses 查找该 IP 对应网卡的 MAC 地址。
 */
std::string ZmNetIP::GetMacAddressOfFD(evutil_socket_t fd)
{
    if (fd > 0 && INVALID_SOCKET != fd)
    {
        struct sockaddr_in6 sa6 = { 0 };
        socklen_t           sa_len = sizeof(struct sockaddr_in6);
        struct sockaddr* sa = (struct sockaddr*)&sa6;
        if (0 == getsockname(fd, sa, &sa_len))
        {
            ZmByteBuffer buf(2048);
            ZM_PEER_ADDR pa = { 0 };
            ZmNetIP::SockaddrToPeer(sa, &pa);
            if (GetMacAddresses(buf.Str(), buf.Size(), &pa.ip) > 0)
            {
                return std::string(buf.Str());
            }
        }
    }
    return "";
}

/**
 * @brief 将 sockaddr 地址信息打印输出用于调试
 *
 * 根据 sa_family 分支处理，提取 IP 地址和端口号。
 */
void ZmNetIP::DumpAddress(const struct sockaddr* address, const char* title)
{
    char               ipstr[128] = { 0 };
    uint16_t port = 0;
    struct sockaddr* addr = const_cast<struct sockaddr*>(address);
    if (addr->sa_family == AF_UNIX)
    {

    }
    else if (addr->sa_family == AF_INET6)
    {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)addr;
        port = ntohs(sa6->sin6_port);
        evutil_inet_ntop(AF_INET6, (void*)&sa6->sin6_addr, ipstr, sizeof(ipstr));
    }
    else if (addr->sa_family == AF_INET)
    {
        struct sockaddr_in* sa4 = (struct sockaddr_in*)addr;
        port = ntohs(sa4->sin_port);
        evutil_inet_ntop(AF_INET, (void*)&sa4->sin_addr, ipstr, sizeof(ipstr));
    }
    PUBLIC_LOG_INFO("Dumping the {} address: {}:{}, sa_family={}", title, ipstr, port, addr->sa_family);
}

/**
 * @brief 判断 MAC 地址是否为有效的以太网地址（非全零）
 *
 * 仅检查前 5 个字节是否全为零，全零视为无效。
 */
bool ZmNetIP::IsEthernetAddress(const BYTE* addr)
{
    // Empty
    if (addr[0] == 0x00 && addr[1] == 0x00 && addr[2] == 0x00 && addr[3] == 0x00 && addr[4] == 0x00)
    {
        return false;
    }

    return true;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmNetIPv6

/**
 * @brief 将 IPv6 地址打印输出用于调试
 *
 * 使用 evutil_inet_ntop 转换为可读字符串后输出日志。
 */
void ZmNetIPv6::Dump(const char* label, const void* ipv6)
{
    char ipstr[64] = { 0 };
    evutil_inet_ntop(AF_INET6, ipv6, ipstr, (socklen_t)sizeof(ipstr));
    PUBLIC_LOG_INFO("{} {}", label, ipstr);
}

/**
 * @brief 按字典序比较两个 IPv6 地址
 */
int ZmNetIPv6::Compare(const uint8_t* ip1, const uint8_t* ip2)
{
    for (int i = 0; i < 16; i++)
    {
        if (ip1[i] == ip2[i]) { continue; }
        else { return ip1[i] > ip2[i] ? 1 : -1; }
    }
    return 0;
}

/**
 * @brief 判断 IPv6 地址是否为全零地址（::）
 */
bool ZmNetIPv6::IsZero(const uint8_t* ip)
{
    for (int i = 0; i < 16; i++)
    {
        if (0 != ip[i]) { return false; }
    }
    return true;
}

/**
 * @brief 取两个 IPv6 地址中的较大值
 */
void ZmNetIPv6::Max(uint8_t* max, const uint8_t* ip1, const uint8_t* ip2)
{
    if (Compare(ip1, ip2) > 0) { if (max != ip1) { memcpy(max, ip1, 16); } }
    else { if (max != ip2) { memcpy(max, ip2, 16); } }
}

/**
 * @brief 取两个 IPv6 地址中的较小值
 */
void ZmNetIPv6::Min(uint8_t* max, const uint8_t* ip1, const uint8_t* ip2)
{
    if (Compare(ip1, ip2) < 0) { if (max != ip1) { memcpy(max, ip1, 16); } }
    else { if (max != ip2) { memcpy(max, ip2, 16); } }
}

/**
 * @brief 判断 IPv6 地址是否在指定闭区间内
 */
bool ZmNetIPv6::Between(const uint8_t* ip, const uint8_t* left, const uint8_t* right)
{
    return (Compare(ip, left) >= 0 && Compare(ip, right) <= 0);
}

/**
 * @brief 交换两个 IPv6 地址的值
 */
void ZmNetIPv6::Swap(uint8_t* ip1, uint8_t* ip2)
{
    uint8_t temp[16] = { 0 };
    memcpy(temp, ip1, 16);
    memcpy(ip1, ip2, 16);
    memcpy(ip2, temp, 16);
}

/**
 * @brief 计算 IPv6 地址的下一个地址（+1）
 *
 * 从最低字节开始加 1，若溢出（值为 255）则进位到更高字节。
 * 例如 ::FFFF -> ::1:0, ::FFFF:FFFF -> ::1:0:0
 * 注意: 全 FF 地址（FFFF:...:FFFF）溢出时会回绕为全零。
 */
uint8_t* ZmNetIPv6::Next(uint8_t* ip, uint8_t* output)
{
    uint8_t* next = output ? output : ip;
    for (int i = 15; i >= 0; i--)
    {
        if (ip[i] == 255)
        {
            next[i] = '\0';
        }
        else
        {
            next[i] = ip[i] + 0x01;
            return next;
        }
    }
    return next;
}

/**
 * @brief 计算 IPv6 地址的上一个地址（-1）
 *
 * 从最低字节开始减 1，若下溢（值为 0）则向更高字节借位。
 * 注意: 全零地址（::）下溢时会回绕为全 FF。
 */
uint8_t* ZmNetIPv6::Prev(uint8_t* ip, uint8_t* output)
{
    uint8_t* prev = output ? output : ip;
    for (int i = 15; i >= 0; i--)
    {
        if (0 == ip[i])
        {
            prev[i] = '\xFF';
        }
        else
        {
            prev[i] = ip[i] - 0x01;
            return prev;
        }
    }
    return prev;
}

/**
 * @brief 将 128 位 IPv6 地址整体左移 1 位
 *
 * 从最高字节开始左移，高位丢弃，低位移入低位。
 * 相邻字节间通过检查 bit7 实现进位传递。
 */
void ZmNetIPv6::BitShiftLeft(uint8_t* ip)
{
    for (int i = 0; i < 16; i++)
    {
        ip[i] = ip[i] << 1;
        if (i < 15 && (ip[i + 1] & 0x80)) { ip[i] = ip[i] | 0x01; }
        else { ip[i] = ip[i] & 0xFE; }
    }
}

/**
 * @brief 将 128 位 IPv6 地址整体右移 1 位
 *
 * 从最低字节开始右移，低位丢弃，高位移入高位。
 * 相邻字节间通过检查 bit0 实现进位传递。
 */
void ZmNetIPv6::BitShiftRight(uint8_t* ip)
{
    for (int i = 15; i >= 0; i--)
    {
        ip[i] = ip[i] >> 1;
        if (i > 0 && (ip[i - 1] & 0x01)) { ip[i] = ip[i] | 0x80; }
        else { ip[i] = ip[i] & 0x7F; }
    }
}

/**
 * @brief 初始化 CIDR 结构体，可选地填充各字段
 */
void ZmNetIPv6::CIDRSet(ZM_IP_CIDR6* cidr, const uint8_t* network,
    const uint8_t* broadcast, const uint8_t* mask, uint16_t bits)
{
    memset(cidr, 0, sizeof(ZM_IP_CIDR6));
    if (network) { memcpy(cidr->network, network, 16); }
    if (broadcast) { memcpy(cidr->broadcast, broadcast, 16); }
    if (mask) { memcpy(cidr->mask, mask, 16); }
    cidr->bits = bits;
}

/**
 * @brief 计算子网掩码中从最高位开始连续 1 的位数
 *
 * 算法: 从最高字节（i=0）的最高位开始，逐位向低位扫描，
 * 遇到第一个 0 时停止，返回已数过的连续 1 的个数。
 */
uint16_t ZmNetIPv6::SubnetBits(const uint8_t* subnet)
{
    uint16_t bits = 0;
    for (int i = 0; i < 16; i++)
    {
        // 检查该字节是否全部为 1
        if (subnet[i] == 0xFF)
        {
            bits += 8;
            continue;
        }
        // 从最高位开始逐位计数连续的 1
        int b = subnet[i];
        for (int j = 7; j >= 0; j--)
        {
            if (b & (1 << j))
            {
                bits++;
            }
            else
            {
                // 遇到第一个 0，连续 1 结束
                return bits;
            }
        }
    }
    return bits;
}

/**
 * @brief 通过前缀位数生成 16 字节子网掩码
 *
 * 当 bits > 0 时先将全部 16 字节置 0xFF，再将超出前缀的部分清零，
 * 最后对跨越字节边界的位逐位置 1。
 * 当 bits == 0 时全部清零（无网络位）。
 */
void ZmNetIPv6::MaskFromBits(uint8_t* mask, uint16_t bits)
{
    if (bits > 0)
    {
        // 先全部置 0xFF
        for (int i = 0; i < 16; i++)
        {
            mask[i] = '\xFF';
        }
        if (bits < 128)
        {
            // byte_index 为前缀完整覆盖的字节之后的那个字节索引
            uint16_t byte_index = bits / 8;
            // byte_bits 为该字节内需要保留的前缀位数
            uint16_t byte_bits = bits % 8;
            // 清除 byte_index 及之后的所有字节
            memset(&mask[byte_index], 0, 16 - byte_index);
            // 在 byte_index 字节中，从高位开始逐位置 1
            for (uint16_t i = 0; i < byte_bits; i++)
            {
                mask[byte_index] = (mask[byte_index] >> 1) | 0x80;
            }
        }
    }
    else
    {
        memset(mask, 0, 16);
    }
}

/**
 * @brief 计算子网广播地址
 *
 * 广播地址 = 网络地址 | (~子网掩码)，即主机位全部置 1。
 */
void ZmNetIPv6::SubnetBroadcast(uint8_t* broadcast, const uint8_t* subnet, uint16_t bits)
{
    uint8_t mask[16] = { 0 };
    MaskFromBits(mask, bits);
    for (int i = 0; i < 16; i++)
    {
        // subnet | (~mask) 将主机位全部置 1
        broadcast[i] = subnet[i] | (~mask[i]);
    }
}

/**
 * @brief 判断 IP 地址是否属于指定的 CIDR 子网
 *
 * 判断逻辑: (ip & mask) == network
 */
bool ZmNetIPv6::CIDRBlockContaints(const uint8_t* network, const uint8_t* mask, const uint8_t* ip)
{
    for (int i = 0; i < 16; i++)
    {
        if ((ip[i] & mask[i]) != network[i])
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 判断 IPv6 地址是否落在任一地址范围内
 */
bool ZmNetIPv6::RangeContaints(const std::vector<ZM_IP_CIDR6>& ranges6, const uint8_t* ipv6)
{
    for (auto it = ranges6.begin(); it != ranges6.end(); it++)
    {
        if (Compare(ipv6, it->start) >= 0 && Compare(ipv6, it->end) <= 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 将 IPv6 地址范围拆分为一组 CIDR 子网块
 *
 * 算法（贪心）:
 * 1. 从起始地址开始，找到其最小掩码位数（最低非零 bit 的位置）
 * 2. 从该掩码位数开始逐步增大，找到使广播地址不超过结束地址的最大 CIDR 块
 * 3. 将该块加入结果列表，将当前地址前进到广播地址的下一个地址
 * 4. 重复直到当前地址超过结束地址
 */
void ZmNetIPv6::CIDRFromRange(std::vector<ZM_IP_CIDR6>& cidrs, const struct in6_addr* from, const struct in6_addr* to)
{
    struct in6_addr subnet = ZM_IN6_ADDR_INIT();
    memcpy(subnet.s6_addr, from, 16);
    /** loops 防止代码出错时死循环 */
    int loops = 0;
    while (Compare(subnet.s6_addr, to->s6_addr) <= 0 && loops++ < 512)
    {
        /** 找到最接近的 prefix mask bits */
        ZM_IP_CIDR6 cidr = ZM_IP_CIDR6_INIT();
        uint16_t bits = SubnetBits(subnet.s6_addr);
        memcpy(cidr.network, subnet.s6_addr, 16);
        while (bits <= 128)
        {
            MaskFromBits(cidr.mask, bits);
            SubnetBroadcast(cidr.broadcast, cidr.network, bits);

            if (Compare(cidr.broadcast, to->s6_addr) <= 0)
            {
                // 广播地址不超过结束地址，当前 bits 可用
                cidr.bits = bits;
                cidrs.push_back(cidr);
                // 前进到广播地址的下一个地址
                memcpy(subnet.s6_addr, cidr.broadcast, 16);
                Next(subnet.s6_addr);
                break;
            }
            // 广播地址超出范围，缩小子网（增加掩码位数）
            bits++;
        }
    }
}

/**
 * @brief CIDRFromRange 的字符串版本
 *
 * 先将字符串解析为 ZM_IP_ADDR，验证均为 IPv6 后委托给 CIDRFromRange。
 */
void ZmNetIPv6::CIDRFromRangeEx(std::vector<ZM_IP_CIDR6>& cidrs, const char* from, const char* to)
{
    ZM_IP_ADDR ip_from = { 0 };
    ZM_IP_ADDR ip_to = { 0 };
    if (AF_INET6 == ZmNetIP::IPFromStr(from, &ip_from)
        && AF_INET6 == ZmNetIP::IPFromStr(to, &ip_to))
    {
        return CIDRFromRange(cidrs, (const struct in6_addr*)ip_from.ipv6_u8, (const struct in6_addr*)ip_to.ipv6_u8);
    }
}

/**
 * @brief 解析 IP 范围字符串为 ZM_IP_RANGE6 结构
 *
 * 按优先级依次尝试三种格式:
 * 1. 范围格式 "ip1-ip2": 解析两端 IP 作为 start/end
 * 2. CIDR 格式 "ip/bits": 解析 IP 和掩码位数，通过 SubnetBroadcast 计算 end
 * 3. 单个 IP: start 和 end 设为相同地址
 */
bool ZmNetIPv6::ParseRange(ZM_IP_RANGE6* range, const char* ipstr)
{
    struct in6_addr ipaddr = ZM_IN6_ADDR_INIT();

    ZmByteBuffer buf(strlen(ipstr), ipstr);
    char* sub = strchr(buf.Str(), '-');
    memset(range, 0, sizeof(ZM_IP_RANGE6));
    if (NULL != sub)
    {
        // 格式: ip1-ip2
        struct in6_addr ip2 = ZM_IN6_ADDR_INIT();
        *sub = '\0';
        sub++;
        if (1 == evutil_inet_pton(AF_INET6, buf.Str(), &ipaddr)
            && 1 == evutil_inet_pton(AF_INET6, sub, &ip2))
        {
            ZmNetIPv6::CIDRSet(range, ipaddr.s6_addr, ip2.s6_addr);
            return true;
        }
    }
    else if (NULL != (sub = strchr(buf.Str(), '/')))
    {
        // 格式: ip/bits
        *sub = '\0';
        sub++;
        if (1 == evutil_inet_pton(AF_INET6, buf.Str(), &ipaddr))
        {
            int bits = atoi(sub);
            if (ZmString::IsNumeric(sub) && bits >= 0 && bits <= 128)
            {
                ZmNetIPv6::CIDRSet(range, ipaddr.s6_addr);
                // 通过掩码位数计算广播地址作为范围终点
                ZmNetIPv6::SubnetBroadcast(range->end, ipaddr.s6_addr, bits);
                return true;
            }
        }
    }
    else if (1 == evutil_inet_pton(AF_INET6, buf.Str(), &ipaddr))
    {
        // 格式: 单个 IP
        ZmNetIPv6::CIDRSet(range, ipaddr.s6_addr, ipaddr.s6_addr);
        return true;
    }
    return false;
}
