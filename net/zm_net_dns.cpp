#include "zm_net_dns.h"

//#include "../log/y_log.h"

#include "../util/zm_util_thread.h"
#include "../util/zm_util_str.h"
#include "../util/zm_util_sys.h"

#include <set>
#include <Ws2tcpip.h>
#include <Iphlpapi.h>
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ws2_32.lib")

/**
 * @brief DNS 缓存条目结构体
 *
 * 存储主机名到地址的映射关系及过期时间。
 * expirems 为 0 表示永不过期（永久缓存）。
 */
typedef struct
{
    uint64_t            expirems;
    char                hostname[128];
    struct sockaddr_in6 address;
}ZM_DNS_ITEM;

static std::vector<std::string> s_well_known_hosts;
static std::mutex                s_well_known_mutex;

/**
 * @brief DNS 缓存管理类，内部使用固定大小的数组存储缓存条目
 *
 * 提供 Lookup（查找）、Put（添加/更新）、ResetExpireMS（更新过期时间）、Clear（清空）操作。
 * 所有操作均通过 mutex 保证线程安全。当缓存满时，采用 LRU 近似策略
 * （替换过期时间最小的条目）进行淘汰。
 */
class ZmDNSCache
{
public:
    ZmDNSCache() : _items(512)
    {
        _items.Clear();
    }

    ~ZmDNSCache() {}

    /**
     * @brief 在缓存中查找指定主机名的地址
     *
     * 遍历缓存数组，匹配主机名后检查是否过期。若未过期则拷贝地址到输出参数。
     *
     * @param sa       输出的地址结构体（需由调用方分配内存）
     * @param hostname 要查找的主机名
     * @param now      当前时间戳（毫秒），用于判断是否过期
     * @return true 找到且未过期，false 未找到或已过期
     */
    inline bool Lookup(struct sockaddr* sa, const char* hostname, uint64_t now)
    {
        std::unique_lock<std::mutex> lock(_mutex);

        for (size_t i = 0; i < _items.Count(); i++)
        {
            ZM_DNS_ITEM* tmp = _items.At(i);
            if (0 == strcmp(hostname, tmp->hostname))
            {
                // expirems == 0 表示永久有效，永远不过期
                if (now < tmp->expirems || 0L == tmp->expirems)
                {
                    memcpy(sa, &tmp->address,
                        (tmp->address.sin6_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
                    return true;
                }
                break;
            }
        }
        return false;
    }

    /**
     * @brief 将主机名和地址添加到缓存，或更新已有条目
     *
     * 若主机名已存在，直接更新地址和过期时间；若不存在，优先追加新条目，
     * 缓存已满时替换过期时间最小（最早过期）的条目。
     *
     * @param hostname 主机名（空字符串或已经是 IP 地址的不会缓存）
     * @param sa       地址信息
     * @param now      当前时间戳（毫秒），用于计算过期时间
     */
    inline void Put(const char* hostname, const struct sockaddr* sa, uint64_t now)
    {
        // 跳过空主机名和已经是IP地址的主机名（无需缓存）
        if (ZmString::IsEmpty(hostname) || ZmNetIP::Validate(hostname))
        {
            return;
        }

        std::unique_lock<std::mutex> lock(_mutex);
        ZM_DNS_ITEM* item = NULL;
        uint64_t     expirems = now + ZM_DNS_TTL_MS;
        uint64_t     xmin = expirems + 1;
        for (size_t i = 0; i < _items.Count(); i++)
        {
            ZM_DNS_ITEM* tmp = _items.At(i);
            if (0 == strcmp(hostname, tmp->hostname))
            {
                // 主机名已存在，更新地址
                memcpy(&tmp->address, sa, (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
                if (0 != tmp->expirems)
                {
                    // 非永久缓存才更新过期时间，永久缓存保持 expirems=0 不变
                    tmp->expirems = expirems;
                }
                return;
            }
            else if (0 != tmp->expirems && tmp->expirems < xmin)
            {
                /** 找到时间最小的替换掉 */
                item = tmp;
            }
        }

        if (!_items.IsFull() || NULL == item)
        {
            item = _items.Add();
        }
        if (item)
        {
            snprintf(item->hostname, sizeof(item->hostname), "%s", hostname);
            memcpy(&item->address, sa, (sa->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
            item->expirems = expirems;
        }
    }

    /**
     * @brief 重置指定主机名的过期时间
     *
     * @param hostname 主机名
     * @param expirems 新的过期时间戳（毫秒），传 0 表示永久有效
     * @return true 找到并更新成功，false 缓存中无此主机名
     */
    inline bool ResetExpireMS(const char* hostname, uint64_t expirems)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        for (size_t i = 0; i < _items.Count(); i++)
        {
            ZM_DNS_ITEM* tmp = _items.At(i);
            if (0 == strcmp(hostname, tmp->hostname))
            {
                tmp->expirems = expirems;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 清空所有缓存条目
     */
    inline void Clear()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _items.Clear();
    }

private:
    ZmArrayList<ZM_DNS_ITEM>    _items;
    std::mutex                 _mutex;
};
ZmDNSCache  g_zm_dns_cache;

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmNetDNS
//

/**
 * @brief 更新缓存中指定主机名的 TTL（存活时间）
 *
 * 查找缓存中已有的条目并更新其过期时间。仅对非 IP 地址的主机名生效。
 *
 * @param hostname 主机名
 * @param ttl      存活偏移时间（毫秒）。传 0 表示设置为永久有效（不过期），
 *                 默认使用 ZM_DNS_TTL_MS（300秒）
 */
void ZmNetDNS::CacheUpdateTTL(const char* hostname, int64_t ttl)
{
    if (!ZmNetIP::Validate(hostname))
    {
        bool contains = g_zm_dns_cache.ResetExpireMS(hostname, (0L != ttl) ? (ZmSystem::CurrentTimeMills() + ttl) : 0L);
        //Y_LOGI("[net][dns] Updated cache item: hostname='%s', ttl=%" PRId64 ", result=%sFUND", hostname, ttl, contains ? "" : "NOT ");
    }
}

/**
 * @brief 将主机名和对应的地址存入 DNS 缓存
 *
 * @param hostname 主机名
 * @param sa       地址信息（支持 sockaddr_in 和 sockaddr_in6）
 */
void ZmNetDNS::CachePut(const char* hostname, const struct sockaddr* sa)
{
    g_zm_dns_cache.Put(hostname, sa, ZmSystem::CurrentTimeMills());
}

/**
 * @brief 从 DNS 缓存中查找主机名对应的地址并设置端口号
 *
 * @param hostname 主机名
 * @param port     目标端口号（网络字节序写入 sa 中）
 * @param sa       输出的地址结构体指针
 * @param now      当前时间戳（毫秒），用于判断缓存是否过期
 * @return 地址结构体长度（sizeof(sockaddr_in) 或 sizeof(sockaddr_in6)），
 *         返回 0 表示缓存未命中或已过期
 */
socklen_t ZmNetDNS::CacheGet(const char* hostname, uint16_t port, struct sockaddr* sa, uint64_t now)
{
    if (!ZmNetIP::Validate(hostname) && g_zm_dns_cache.Lookup(sa, hostname, now))
    {
        if (sa->sa_family == AF_INET6)
        {
            ((struct sockaddr_in6*)sa)->sin6_port = htons(port);

            return sizeof(struct sockaddr_in6);
        }
        else
        {
            ((struct sockaddr_in*)sa)->sin_port = htons(port);

            return sizeof(struct sockaddr_in);
        }
    }

    return 0;
}

/**
 * @brief 清空所有 DNS 缓存条目
 */
void ZmNetDNS::ClearCache()
{
    g_zm_dns_cache.Clear();
}

/**
 * @brief 添加一个知名主机名到列表中
 *
 * @param hostname 知名主机名
 */
void ZmNetDNS::AddWellKnownHost(const char* hostname)
{
    std::lock_guard<std::mutex> lock(s_well_known_mutex);
    s_well_known_hosts.emplace_back(hostname);
}

/**
 * @brief 从知名主机列表中删除指定的主机名（精确匹配）
 *
 * @param hostname 要删除的主机名
 */
void ZmNetDNS::DelWellKnownHost(const char* hostname)
{
    std::lock_guard<std::mutex> lock(s_well_known_mutex);
    for (auto itor = s_well_known_hosts.begin(); itor != s_well_known_hosts.end(); itor++)
    {
        if (*itor == hostname)
        {
            s_well_known_hosts.erase(itor);
            break;
        }
    }
}

/**
 * @brief 清空所有知名主机名
 */
void ZmNetDNS::DelWellKnownHost()
{
    std::lock_guard<std::mutex> lock(s_well_known_mutex);
    s_well_known_hosts.clear();
}

/**
 * @brief 判断主机名是否以某个知名主机名结尾（后缀匹配）
 *
 * @param hostname 待判断的主机名
 * @return true 表示该主机名属于知名主机
 */
bool ZmNetDNS::IsWellKnownHost(const char* hostname)
{
    std::lock_guard<std::mutex> lock(s_well_known_mutex);
    for (const auto& host : s_well_known_hosts)
    {
        if (ZmString::EndsWith(hostname, host.c_str()))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 获取系统配置的 DNS 解析服务器地址（仅 IPv4）
 *
 * 通过 Windows API GetNetworkParams 获取系统 DNS 配置。
 * 多个地址以逗号分隔返回。
 *
 * @return 逗号分隔的 DNS 服务器 IPv4 地址字符串，如 "8.8.8.8,8.8.4.4"
 *
 * @note 不支持获取本机 IPv6 的 DNS 服务器地址
 *
 * @example
 * @code
 *   std::string dns = ZmNetDNS::GetResolves();
 *   // dns 可能是 "192.168.1.1,8.8.8.8"
 * @endcode
 */
std::string ZmNetDNS::GetResolves()
{
    std::string resolves;

#   define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#   define FREE(x)   HeapFree(GetProcessHeap(), 0, (x))

    // 首次分配 FIXED_INFO 大小的缓冲区，若不够则重新分配
    FIXED_INFO* pFixedInfo = (FIXED_INFO*)MALLOC(sizeof(FIXED_INFO));
    ULONG       ulOutBufLen = sizeof(FIXED_INFO);
    DWORD       dwRetVal = 0;
    if (pFixedInfo && GetNetworkParams(pFixedInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW)
    {
        FREE(pFixedInfo);
        pFixedInfo = (FIXED_INFO*)MALLOC(ulOutBufLen);
    }
    if (pFixedInfo && (dwRetVal = GetNetworkParams(pFixedInfo, &ulOutBufLen)) == NO_ERROR)
    {
        // 第一个 DNS 服务器地址
        resolves.append(pFixedInfo->DnsServerList.IpAddress.String);
        // 遍历链表获取其余 DNS 服务器地址
        IP_ADDR_STRING* pIPAddr = pFixedInfo->DnsServerList.Next;
        while (pIPAddr)
        {
            resolves.append(",").append(pIPAddr->IpAddress.String);
            pIPAddr = pIPAddr->Next;
        }
    }
    if (pFixedInfo)
    {
        FREE(pFixedInfo);
    }
#   undef MALLOC
#   undef FREE


    return resolves;
}

/**
 * @brief 获取系统配置的 DNS 服务器地址（同时支持 IPv4 和 IPv6）
 *
 * 通过 Windows API GetAdaptersAddresses 枚举所有网卡的 DNS 服务器配置，
 * 自动过滤掉站点本地 IPv6 地址（fec0::/10），结果去重后以逗号分隔。
 *
 * @return 逗号分隔的 DNS 服务器地址字符串，包含 IPv4 和 IPv6
 *
 * @example
 * @code
 *   std::string dns = ZmNetDNS::GetDNSAddresses();
 *   // dns 可能是 "192.168.1.1,2001:4860:4860::8888,8.8.8.8"
 * @endcode
 */
std::string ZmNetDNS::GetDNSAddresses()
{
    std::set<std::string> uniqueAddresses;
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);

    // 首次分配 15000 字节，若不够则按返回值重新分配
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == ERROR_BUFFER_OVERFLOW)
    {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    }

    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR)
    {
        // 遍历所有网卡
        for (PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses; pCurrAddresses != NULL; pCurrAddresses = pCurrAddresses->Next)
        {
            // 遍历当前网卡的 DNS 服务器地址链表
            for (IP_ADAPTER_DNS_SERVER_ADDRESS* pDnServer = pCurrAddresses->FirstDnsServerAddress; pDnServer != NULL; pDnServer = pDnServer->Next)
            {
                char addressBuffer[INET6_ADDRSTRLEN] = { 0 };
                if (pDnServer->Address.lpSockaddr->sa_family == AF_INET)
                {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)pDnServer->Address.lpSockaddr)->sin_addr, addressBuffer, sizeof(addressBuffer));
                    uniqueAddresses.insert(addressBuffer);
                }
                else if (pDnServer->Address.lpSockaddr->sa_family == AF_INET6)
                {
                    const struct sockaddr_in6* sockaddr = (struct sockaddr_in6*)pDnServer->Address.lpSockaddr;
                    // 过滤掉站点本地地址 fec0::/10，这些地址通常不是正常的 DNS 服务器
                    if (!IsSiteLocalAddress(sockaddr))
                    {
                        inet_ntop(AF_INET6, &sockaddr->sin6_addr, addressBuffer, sizeof(addressBuffer));
                        uniqueAddresses.insert(addressBuffer);
                    }
                }
            }
        }
    }

    if (pAddresses)
    {
        free(pAddresses);
    }

    // 使用 set 去重后拼接为逗号分隔的字符串
    std::string resolves;
    for (const auto& address : uniqueAddresses)
    {
        if (!resolves.empty())
        {
            resolves.append(",");
        }
        resolves.append(address);
    }

    return resolves;
}

/**
 * @brief 通过主机名解析 IP 地址，返回主机序的 ZM_IP_ADDR
 *
 * 内部调用 GetAddressByName 进行实际解析，根据返回的地址族
 * 自动填充 ZM_IP_ADDR 的 ipv4 或 ipv6_u8 字段。
 *
 * @param ip        输出的 IP 地址结构体。IPv4 存入 ip->ipv4（主机序），
 *                  IPv6 存入 ip->ipv6_u8（网络序原始字节）
 * @param hostname  主机名
 * @param port      目标端口号
 * @param ipaddress 可选输出缓冲区，接收 IP 地址可读字符串。传 NULL 不输出
 * @param cnt       ipaddress 缓冲区大小。传 0 不输出字符串
 * @param family    地址族过滤：AF_UNSPEC（默认）、AF_INET、AF_INET6
 * @return true 解析成功，false 解析失败
 *
 * @example
 * @code
 *   ZM_IP_ADDR ip;
 *   char ipstr[64] = {0};
 *   bool ok = ZmNetDNS::GetHostIPByName(&ip, "www.example.com", 443, ipstr, sizeof(ipstr));
 *   if (ok) printf("IP: %s, family: %d\n", ipstr, ip.family);
 * @endcode
 */
bool ZmNetDNS::GetHostIPByName(ZM_IP_ADDR* ip, const char* hostname, uint16_t port,
    char* ipaddress, int cnt, uint16_t family)
{
    struct sockaddr_in6 sa6;
    memset(ip, 0, sizeof(ZM_IP_ADDR));
    if (GetAddressByName(&sa6, hostname, port, family) > 0)
    {
        if (sa6.sin6_family == AF_INET6)
        {
            memcpy(ip->ipv6_u8, &sa6.sin6_addr, 16);
            ip->family = AF_INET6;
            if (NULL != ipaddress && cnt > 0)
            {
                evutil_inet_ntop(AF_INET6, (void*)&sa6.sin6_addr, ipaddress, cnt);
            }
        }
        else
        {
            struct sockaddr_in* sa4 = (struct sockaddr_in*)&sa6;
            ip->ipv4 = ntohl(sa4->sin_addr.s_addr);  // 网络序转主机序
            ip->family = AF_INET;
            if (NULL != ipaddress && cnt > 0)
            {
                evutil_inet_ntop(AF_INET, (void*)&sa4->sin_addr, ipaddress, cnt);
            }
        }
        return true;
    }
    return false;
}

/**
 * http://beej.us/guide/bgnet/output/html/multipage/getaddrinfoman.html
 * http://www.logix.cz/michal/devel/various/getaddrinfo.c.xp
 * https://paulschreiber.com/blog/2005/10/28/simple-getaddrinfo-example/
 * One of the differences between getaddrinfo() and gethostbyname() is that the former supports both IPv4 and IPv6,
 *  while the latter only supports IPv4. So when you call getaddrinfo() with ai_family set to 0 (AF_UNSPEC),
 *  it won't return until it gets a response (or hits a timeout) for both A and AAAA queries for the domain name provided.
 *  gethostbyname() only queries for an A record.
 */

/**
 * @brief 通过主机名获取 sockaddr 地址结构，支持 IPv4 和 IPv6
 *
 * 解析流程：
 * 1. 若 hostname 本身就是 IPv6 地址字符串，直接转换返回
 * 2. 查找 DNS 缓存，命中则直接返回
 * 3. 通过 GetAddrInfoExW 在独立线程中进行异步解析（超时3秒）
 * 4. 解析成功则加入缓存；失败则尝试返回已过期的缓存（降级策略）
 *
 * @param sa6      输出的地址结构体。IPv4 时实际写入 sockaddr_in（复用同一块内存）
 * @param hostname 主机名
 * @param port     目标端口号
 * @param type     socket 类型，SOCK_STREAM（TCP）或 SOCK_DGRAM（UDP）
 * @param family   地址族过滤：AF_UNSPEC、AF_INET、AF_INET6
 * @return 地址结构体长度，0 表示解析失败
 */
socklen_t ZmNetDNS::GetAddressByName(struct sockaddr_in6* sa6, const char* hostname, uint16_t port,
    int type, uint16_t family)
{
    // 2019.06.20 return ipv6 address directly
    // 如果 hostname 本身就是 IPv6 地址，直接填充 sockaddr_in6 返回
    ZM_IP_ADDR xip = { 0 };
    if (AF_INET6 == ZmNetIP::Validate(hostname, &xip))
    {
        memset(sa6, 0, sizeof(struct sockaddr_in6));
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(port);
        memcpy(&sa6->sin6_addr.s6_addr, xip.ipv6_u8, 16);
        return sizeof(struct sockaddr_in6);
    }

    // 空主机名不能解析
    if (ZmString::IsEmpty(hostname))
    {
        return 0;
    }

    // 如果 hostname 是 IPv4 地址，直接转换返回（避免走到 GetAddrInfoEx+NS_DNS 报 WSAEINVAL）
    if (AF_INET == ZmNetIP::Validate(hostname, &xip))
    {
        struct sockaddr_in* sa4 = (struct sockaddr_in*)sa6;
        memset(sa6, 0, sizeof(struct sockaddr_in6));
        sa4->sin_family = AF_INET;
        sa4->sin_port = htons(port);
        sa4->sin_addr.s_addr = htonl(xip.ipv4);
        return sizeof(struct sockaddr_in);
    }

    // 查找 DNS 缓存
    uint64_t now = ZmSystem::CurrentTimeMills();
    socklen_t salen = ZmNetDNS::CacheGet(hostname, port, (struct sockaddr*)sa6, now);
    if (salen > 0)
    {
        return salen;
    }

    char strport[16];
    snprintf(strport, sizeof(strport), "%d", port);
    sa6->sin6_family = 0;

    // 转换为宽字符供 GetAddrInfoExW 使用
    wchar_t whostname[256] = { 0 };
    wchar_t wservice[16] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, hostname, -1, whostname, 256);
    MultiByteToWideChar(CP_ACP, 0, strport, -1, wservice, 16);

    ADDRINFOEXW hints = { 0 };
    hints.ai_family = (PF_INET6 == family) ? PF_INET6 : PF_UNSPEC;
    hints.ai_socktype = type;

    PADDRINFOEXW result = NULL;
    HANDLE cancelHandle = NULL;
    struct timeval tv = { 1, 0 };  // GetAddrInfoExW 自身 1 秒超时

    // 在独立线程中调用 GetAddrInfoExW，利用 API 自带的超时参数保证线程不会永久阻塞
    std::packaged_task<int()> task([&]() -> int {
        return GetAddrInfoEx(whostname, wservice, NS_DNS, NULL,
            &hints, &result, &tv, NULL, NULL, &cancelHandle);
    });

    std::future<int> futureResult = task.get_future();
    std::thread t(std::move(task));

    int err = WSAETIMEDOUT;

    try {
        // 等待 3 秒，远长于 API 自身的 1 秒超时，留出足够余量
        std::future_status status = futureResult.wait_for(std::chrono::milliseconds(3000));
        if (status == std::future_status::ready) {
            err = futureResult.get();
        }
        else {
            // 超时则取消 GetAddrInfoExW 调用
            if (cancelHandle) {
                GetAddrInfoExCancel(&cancelHandle);
            }
        }
    }
    catch (const std::system_error& e) {
        ZM_UNUSED(e);
    }
    catch (const std::runtime_error& e) {
        ZM_UNUSED(e);
    }

    t.join();

    if (err == 0)
    {
        char ipaddr[64];
        memset(ipaddr, 0, sizeof(ipaddr));
        for (PADDRINFOEXW p = result; p != NULL; p = p->ai_next)
        {
            /** 如果指定了 family 则需返回匹配的 */
            if ((family != AF_INET && family != AF_INET6) || family == p->ai_addr->sa_family)
            {
                salen = p->ai_addr->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
                memcpy(sa6, p->ai_addr, salen);
                ((struct sockaddr_in*)sa6)->sin_port = htons(port);

                // 如果 family 匹配或结果为 IPv4，优先使用
                if (family == p->ai_addr->sa_family || p->ai_addr->sa_family == AF_INET)
                    break;
            }
        }

        g_zm_dns_cache.Put(hostname, (struct sockaddr*)sa6, now);
    }
    else
    {
        // 解析失败时，尝试返回已过期的缓存（降级策略：过期数据总比没有好）
        salen = CacheGet(hostname, port, (struct sockaddr*)sa6, 0);
    }

    if (result) {
        FreeAddrInfoExW(result);
    }

    return salen;
}

/**
 * @brief 从 evutil_addrinfo 链表中提取第一个 IPv4 或 IPv6 地址
 *
 * 遍历链表，优先返回 IPv4 地址（遇到 AF_INET 即 break），
 * 其次返回 IPv6 地址。同时可选地将 IP 地址转换为字符串形式。
 *
 * @param sa6       输出的地址结构体。可为 NULL（不输出地址）
 * @param addr      evutil_addrinfo 链表头指针
 * @param ipstr     可选的输出缓冲区，接收 IP 地址字符串。传 NULL 不输出
 * @param capacity  ipstr 缓冲区大小
 * @return 地址结构体长度（sizeof(sockaddr_in) 或 sizeof(sockaddr_in6)），0 表示无有效地址
 */
socklen_t ZmNetDNS::ExtractEventAddrInfo(struct sockaddr_in6* sa6, struct evutil_addrinfo* addr,
    char* ipstr, size_t capacity)
{
    socklen_t salen = 0;
    for (struct evutil_addrinfo* ai = addr; ai; ai = ai->ai_next)
    {
        if (ai->ai_family == AF_INET)
        {
            salen = sizeof(struct sockaddr_in);
            if (sa6)
            {
                // IPv4 地址存储在 sockaddr_in6 的前 sizeof(sockaddr_in) 字节中
                struct sockaddr_in* sa4 = (struct sockaddr_in*)sa6;
                memset(sa6, 0, sizeof(struct sockaddr_in6));
                memcpy(sa6, ai->ai_addr, salen);
                sa4->sin_port = htons(((sockaddr_in*)ai->ai_addr)->sin_port);
            }
            if (ipstr && capacity > 0)
            {
                evutil_inet_ntop(AF_INET, &(((struct sockaddr_in*)ai->ai_addr)->sin_addr), ipstr, capacity);
            }
            break;
        }
        else if (ai->ai_family == AF_INET6)
        {
            salen = sizeof(struct sockaddr_in6);
            if (sa6)
            {
                memcpy(sa6, ai->ai_addr, salen);
                sa6->sin6_port = htons(((sockaddr_in6*)ai->ai_addr)->sin6_port);
            }
            if (ipstr && capacity > 0)
            {
                evutil_inet_ntop(AF_INET6, &(((struct sockaddr_in6*)ai->ai_addr)->sin6_addr), ipstr, capacity);
            }
            break;
        }
    }
    return salen;
}

/**
 * @brief 判断 IPv6 地址是否为站点本地地址（fec0::/10）
 *
 * 站点本地地址范围 fec0::/10 已被 RFC 4291 废弃，
 * 但某些旧系统仍可能配置此类地址，应过滤掉。
 *
 * @param sockaddr IPv6 地址结构体指针
 * @return true 表示是站点本地地址
 *
 * @example
 * @code
 *   // fec0::1 的第0字节=0xfe, 第1字节=0xc0
 *   // (0xc0 & 0xc0) == 0xc0, 返回 true
 *   // 2001:db8::1 的第0字节=0x20, 不满足条件, 返回 false
 * @endcode
 */
bool ZmNetDNS::IsSiteLocalAddress(const struct sockaddr_in6* sockaddr)
{
    // Check if the address is in the site-local range (fec0::/10)
    // 第0字节必须为 0xfe，第1字节高2位必须为 11（即 & 0xc0 == 0xc0）
    return (sockaddr->sin6_addr.s6_addr[0] == 0xfe) && ((sockaddr->sin6_addr.s6_addr[1] & 0xc0) == 0xc0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * +------------------ UDP Header Format ------------------------------+
 * | Offsets Octet |            0            |            1            |
 * |  Octet  Bit   | 00 01 02 03 04 05 06 07 | 08 09 10 11 12 13 14 15 |
 * +---------------+-------------------------+-------------------------+
 * |    0      0   |                    Source Port                    |
 * +---------------+-------------------------+-------------------------+
 * |    2     16   |                  Destination Port                 |
 * +---------------+-------------------------+-------------------------+
 * |    4     32   |                  UDP Length                       |
 * +---------------+-------------------------+-------------------------+
 * |    6     48   |                  Check Code                       |
 * +---------------+-------------------------+-------------------------+
 */
 /**
  * +------------------------- DNS Packet ------------------------------+
  * | Offsets Octet |            0            |            1            |
  * |  Octet  Bit   | 00 01 02 03 04 05 06 07 | 08 09 10 11 12 13 14 15 |
  * +---------------+-------------------------+-------------------------+
  * |    0      0   |                        ID                         |
  * +---------------+---*-----------*--*--*---+--*---------*------------+
  * |    2     16   | QR|  opcode   |AA|TC|RD |RA|  (zero) |  rcode     |
  * +---------------+-------------------------+-------------------------+
  * |    4     32   |                      QDCOUNT                      |
  * +---------------+-------------------------+-------------------------+
  * |    6     48   |                      ANCOUNT                      |
  * +---------------+-------------------------+-------------------------+
  * |    8     64   |                      NSCOUNT                      |
  * +---------------+-------------------------+-------------------------+
  * |   10     80   |                      ARCOUNT                      |
  * +---------------+-------------------------+-------------------------+
  */

/**
 * @brief 递归解析 DNS 报文中的域名标签（Label）
 *
 * DNS 域名使用标签序列编码：
 * - 普通标签：1字节长度 + 对应字节数的数据，如 [3]www[7]example[3]com[0]
 * - 压缩标签：高2位为 11（即 & 0xC0 == 0xC0），后14位指向报文内另一位置的偏移
 *
 * 解析结果以 '.' 前缀拼接各标签，如 ".www.example.com"。
 *
 * @param label  输出缓冲区，接收解析后的域名
 * @param space  输出缓冲区剩余大小
 * @param dnsp   DNS 报文数据
 * @param offset 当前解析位置
 * @param limit  报文数据总长度（防止越界）
 * @param depth  递归深度计数器（防止压缩指针死循环，最大16层）
 * @return 解析完成后新的偏移位置
 *
 * @example
 * @code
 *   // 假设 dnsp 中偏移 12 处存放了 DNS 标签编码的域名
 *   char name[128];
 *   size_t next_offset = ZmNetDNS::ParseLabel(name, sizeof(name), dnsp, 12, dlen);
 *   // name 可能是 ".www.example.com"，next_offset 指向标签之后的下一字段
 * @endcode
 */
size_t ZmNetDNS::ParseLabel(char* label, size_t space, const BYTE* dnsp, size_t offset, size_t limit, int depth)
{
    // 越界保护或递归过深，终止解析
    if (offset >= limit || depth > 16) { *label = '\0'; return offset; }

    uint8_t len = dnsp[offset];
    if (0 == len)
    {
        // 长度为 0 表示域名结束
        *label = '\0';
        return offset + 1;
    }
    else if (0xC0 == (dnsp[offset] & 0xC0))
    {
        /** 压缩格式：高2位为11，后14位为报文内偏移指针 */
        // 读取 2 字节，掩码取低 14 位得到偏移值，递归解析
        ParseLabel(label, space, dnsp, ntohs(*((uint16_t*)(dnsp + offset))) & 0x3FFF, limit, depth + 1);
        return offset + 2;  // 压缩指针固定占 2 字节
    }
    else
    {
        // 普通标签：len 为当前标签的数据长度
        if (space < 2) { *label = '\0'; return offset + len + 1; }
        *label++ = '.';
        space--;
        size_t copylen = ZM_MIN(space, (size_t)len);
        memcpy(label, dnsp + offset + 1, copylen);
        label += copylen;
        space -= copylen;
        // 递归解析下一个标签
        return ParseLabel(label, space, dnsp, offset + len + 1, limit, depth + 1);
    }
}

/**
 * @brief 将主机名编码为 DNS 标签格式写入报文缓冲区
 *
 * 将点分域名（如 "www.example.com"）转换为 DNS 协议要求的标签编码格式
 * （如 [3]www[7]example[3]com[0]）。
 *
 * 算法：先将整个主机名拷贝到缓冲区（跳过第一个字节），然后用 '.' 作为分隔符，
 * 每个 '.' 前面的字节计数累加到对应位置（替换掉 '.'），最终末尾补 '\0'。
 *
 * @param dnsp     DNS 报文缓冲区
 * @param offset   写入起始偏移
 * @param hostanme 待编码的主机名（点分格式）
 * @return 写入的总字节数（含结尾 '\0'）
 *
 * @example
 * @code
 *   BYTE buf[512];
 *   size_t len = ZmNetDNS::LabelPut(buf, 0, "www.example.com");
 *   // buf 内容: [3]w w w [7]e x a m p l e [3]c o m [0]
 *   // len == 17
 * @endcode
 */
size_t ZmNetDNS::LabelPut(BYTE* dnsp, size_t offset, const char* hostanme)
{
    BYTE* label = dnsp + offset;
    size_t cpos = 0;
    size_t nlen = strlen(hostanme);
    // 先将主机名拷贝到 label+1 的位置（跳过第一个字节，留给第一个标签的长度）
    memcpy(label + 1, hostanme, nlen);
    nlen++;                /** 因为跳过了第一个字符，因此长度增加 1 */
    label[0] = '.';    /** 设置第一个为 '.' 作为长度计数器的占位 */

    // 遍历所有字符，遇到 '.' 时重置长度计数器，否则累加计数器
    for (size_t i = 0; i < nlen; i++)
    {
        if (label[i] == '.')
        {
            // 遇到分隔符，记录位置并重置计数为 0
            cpos = i;
            label[cpos] = 0;
        }
        else
        {
            // 非分隔符，对应位置的长度计数 +1
            label[cpos] = label[cpos] + 1;
            // HPLogt("label[%02d]=%02x", cpos, label[cpos]);
        }
    }
    label[nlen] = '\0';     /** Label 结束必须是 '\0' */
    return nlen + 1;
}

/**
 * @brief 向 DNS 报文缓冲区写入一条资源记录（Resource Record）
 *
 * 写入格式：域名标签 + 记录头（type/class/ttl/rdlen）+ 资源数据。
 * TTL 硬编码为 60 秒。
 *
 * @param dnsp   DNS 报文缓冲区
 * @param offset 写入起始偏移
 * @param rname  资源记录的域名
 * @param rtype  记录类型（1=A, 5=CNAME, 28=AAAA 等）
 * @param rclass 记录类别（通常为 1=IN）
 * @param rdlen  资源数据长度（字节）
 * @param rdata  资源数据指针（可为 NULL）
 * @return 写入的总字节数
 */
size_t ZmNetDNS::FieldRecordPut(BYTE* dnsp, size_t offset, const char* rname,
    uint16_t rtype, uint16_t rclass, uint16_t rdlen, const BYTE* rdata)
{
    /** TODO: 支持报文压缩 */
    size_t         rnlen = LabelPut(dnsp, offset, rname);
    ZM_DNS_RECORD* record = (ZM_DNS_RECORD*)(dnsp + offset + rnlen);

    memset(record, 0, sizeof(ZM_DNS_RECORD));
    record->rtype = htons(rtype);
    record->rclass = htons(rclass);
    record->ttl = htonl(60);      // TTL 固定为 60 秒
    record->rdlen = htons(rdlen);
    if (rdata && rdlen > 0)
    {
        memcpy(&record->rdata, rdata, rdlen);
    }
    return (rnlen + sizeof(ZM_DNS_RECORD) + rdlen);
}

/**
 * @brief 向 DNS 报文缓冲区写入查询问题字段（Question Field）
 *
 * 写入格式：域名标签 + qtype(2字节) + qclass(2字节)。
 *
 * @param dnsp   DNS 报文缓冲区
 * @param offset 写入起始偏移
 * @param quest  查询问题结构体
 * @return 写入的总字节数（标签长度 + 4 字节 type/class）
 */
size_t ZmNetDNS::FieldQuestPut(BYTE* dnsp, size_t offset, const ZM_NET_DNS_QUESTION* quest)
{
    // [*]QName
    size_t         nlen = LabelPut(dnsp, offset, quest->qname);
    // 复用 ZM_DNS_RECORD 结构体的前 4 字节（rtype + rclass）来写入 qtype + qclass
    ZM_DNS_RECORD* record = (ZM_DNS_RECORD*)(dnsp + offset + nlen);
    record->rtype = htons(quest->qtype);
    record->rclass = htons(quest->qclass);
    return (nlen + 4);
}

/**
 * @brief 根据地址族自动选择 A 或 AAAA 记录类型，写入 DNS 资源记录
 *
 * IPv4 地址生成 A 记录（type=1，rdlen=4），
 * IPv6 地址生成 AAAA 记录（type=28，rdlen=16）。
 *
 * @param dnsp     DNS 报文缓冲区
 * @param offset   写入起始偏移
 * @param hostname 资源记录的域名
 * @param addr     地址信息（通过 sa_family 判断 IPv4/IPv6）
 * @return 写入的总字节数
 *
 * @example
 * @code
 *   struct sockaddr_in sa4;
 *   sa4.sin_family = AF_INET;
 *   sa4.sin_addr.s_addr = inet_addr("192.168.1.1");
 *   BYTE buf[512];
 *   size_t off = 12;  // 跳过 DNS Header
 *   off += ZmNetDNS::FieldRecordPutARPA(buf, off, "mydevice.local", (sockaddr*)&sa4);
 *   // 写入 A 记录：mydevice.local -> 192.168.1.1, rdlen=4
 * @endcode
 */
size_t ZmNetDNS::FieldRecordPutARPA(BYTE* dnsp, size_t offset, const char* hostname, const struct sockaddr* addr)
{
    uint16_t rtype = 0;
    uint16_t rclass = 0x01;  // IN (Internet)
    uint16_t rdlen = 0;
    BYTE* rdata = NULL;

    if (addr->sa_family == AF_INET6)
    {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)addr;
        rtype = 28;  // AAAA 记录
        rdlen = (uint16_t)sizeof(struct in6_addr);  // 16 字节
        rdata = (BYTE*)&sa6->sin6_addr;
    }
    else
    {
        struct sockaddr_in* sa4 = (struct sockaddr_in*)addr;
        rtype = 1;  // A 记录
        rdlen = (uint16_t)sizeof(struct in_addr);  // 4 字节
        rdata = (BYTE*)&sa4->sin_addr;
    }
    return FieldRecordPut(dnsp, offset, hostname, rtype, rclass, rdlen, rdata);
}

/**
 * @brief 从 DNS 报文中提取查询问题区域（Question Section）
 *
 * 解析指定数量的查询记录，仅将第一条记录写入输出参数。
 * 每条记录包含域名标签（可变长）+ qtype(2字节) + qclass(2字节)。
 *
 * @param quest   输出结构体，仅第一条记录会被写入（可为 NULL）
 * @param dns     DNS 报文数据
 * @param qdcount Question 区域的记录数
 * @param offset  Question 区域在报文中的起始偏移（通常为 12，即 Header 之后）
 * @param limit   报文数据总长度
 * @return 解析完成后新的偏移位置
 */
size_t ZmNetDNS::ExtractQuest(ZM_NET_DNS_QUESTION* quest, const BYTE* dns, size_t qdcount, size_t offset, size_t limit)
{
    if (quest)
    {
        if (qdcount > 0 && dns)
        {
            ZM_NET_DNS_QUESTION q = { {0}, 0, 0 };
            for (size_t i = 0; i < qdcount; i++)
            {
                memset(&q, 0, sizeof(ZM_NET_DNS_QUESTION));
                // 解析域名标签
                offset = ParseLabel(q.qname, sizeof(q.qname), dns, offset, limit);
                // ParseLabel 会在标签前加 '.'，需要去掉前导 '.'
                if (q.qname[0] == '.')
                {
                    memmove(&q.qname[0], &q.qname[1], sizeof(q.qname) - 1);
                    q.qname[sizeof(q.qname) - 1] = '\0';
                }
                // 读取 qtype 和 qclass（各 2 字节网络序）
                q.qtype = ntohs(*((uint16_t*)(dns + offset)));
                q.qclass = ntohs(*((uint16_t*)(dns + offset + 2)));
                // //Y_LOGI("%s [%ld]%s,%d,%d", i, __Y_FUNC__, query.qname, query.qtype, query.qclass);
                offset += 4;
                // 仅保存第一条查询记录到输出
                if (0 == i && quest)
                {
                    memcpy(quest, &q, sizeof(ZM_NET_DNS_QUESTION));
                }
            }
        }
        else
        {
            memset(quest, 0, sizeof(ZM_NET_DNS_QUESTION));
        }
    }
    return offset;
}

/**
 * @brief 从 DNS 报文中提取应答/授权/附加区域记录（Answer/Authority/Additional）
 *
 * 解析指定数量的资源记录，仅将第一条记录写入输出参数。
 * 每条记录包含：域名标签 + type(2) + class(2) + ttl(4) + rdlength(2) + rdata(rdlength)。
 *
 * @param aaa    输出结构体，仅第一条记录会被写入（可为 NULL）
 * @param dns    DNS 报文数据
 * @param count  该区域的记录数
 * @param offset 该区域在报文中的起始偏移
 * @param limit  报文数据总长度
 * @return 解析完成后新的偏移位置
 */
size_t ZmNetDNS::ExtractAAA(ZM_NET_DNS_AAA* aaa, const BYTE* dns, size_t count, size_t offset, size_t limit)
{
    if (count > 0)
    {
        ZM_NET_DNS_AAA atmp = { {0}, 0, 0, 0, 0, {0} };
        for (size_t i = 0; i < count; i++)
        {
            memset(&atmp, 0, sizeof(ZM_NET_DNS_AAA));
            // 解析域名标签
            offset = ParseLabel(atmp.name, sizeof(atmp.name), dns, offset, limit);
            // 安全检查：确保后续固定字段（type+class+ttl+rdlength = 10字节）不会越界
            if (offset + 10 > limit) break;
            // 去掉 ParseLabel 产生的前导 '.'
            if (atmp.name[0] == '.')
            {
                memmove(&atmp.name[0], &atmp.name[1], sizeof(atmp.name) - 1);
                atmp.name[sizeof(atmp.name) - 1] = '\0';
            }
            // 读取固定头部字段：type(2) + class(2) + ttl(4) + rdlength(2) = 10 字节
            atmp.type = ntohs(*((uint16_t*)(dns + offset)));
            atmp.rclass = ntohs(*((uint16_t*)(dns + offset + 2)));
            atmp.ttl = ntohl(*((uint32_t*)(dns + offset + 4)));
            atmp.rdlength = ntohs(*((uint16_t*)(dns + offset + 8)));
            // 读取资源数据（rdata），限制不超过缓冲区大小
            if (atmp.rdlength > 0 && offset + 10 + atmp.rdlength <= limit)
            {
                memcpy(atmp.rdata, dns + offset + 10, ZM_MIN(atmp.rdlength, sizeof(atmp.rdata)));
            }
            // //Y_LOGI("YNetDNS::ExtractAAA [%ld]%s,%d,%d,%d", i, atmp.name, atmp.type, atmp.rclass, atmp.ttl);
            offset = offset + 10 + atmp.rdlength;

            // 仅保存第一条记录到输出
            if (i == 0 && aaa)
            {
                memcpy(aaa, &atmp, sizeof(ZM_NET_DNS_AAA));
            }
        }
    }
    else if (aaa)
    {
        memset(aaa, 0, sizeof(ZM_NET_DNS_AAA));
    }

    return offset;
}

/**
 * @brief 从 DNS 请求（Query）UDP 报文中解析出查询的 hostname
 *
 * 仅处理请求报文（QR=0），且问题数不超过 8 条的情况。
 *
 * @param quest   输出的查询问题结构体
 * @param udpdata DNS 请求 UDP 报文数据
 * @param dlen    报文数据长度
 *
 * @example
 * @code
 *   ZM_NET_DNS_QUESTION quest;
 *   ZmNetDNS::ParseQueryUDP(&quest, udp_buffer, recv_len);
 *   printf("Query: %s, type: %d\n", quest.qname, quest.qtype);
 * @endcode
 */
void ZmNetDNS::ParseQueryUDP(ZM_NET_DNS_QUESTION* quest, const BYTE* udpdata, size_t dlen)
{
    ZM_NET_DNS_HEAD* dnshead = (ZM_NET_DNS_HEAD*)udpdata;
    // 检查：QR 位为 0（请求报文）且问题数 < 8
    if (0 == (dnshead->flags_int16 & htons(0x8000)) && ntohs(dnshead->qdcount) < 8)
    {
        // DNS Header 固定 12 字节，Question 区域从偏移 12 开始
        ExtractQuest(quest, udpdata, ntohs(dnshead->qdcount), 12, dlen);
    }
}

/**
 * @brief 从 DNS 应答（Reply）UDP 报文中解析出查询的 hostname
 *
 * 仅处理应答报文（QR=1），从 Question 区域提取第一条查询记录。
 *
 * @param quest   输出的查询问题结构体
 * @param dnsdata DNS 应答 UDP 报文数据
 * @param dlen    报文数据长度
 *
 * @example
 * @code
 *   ZM_NET_DNS_QUESTION quest;
 *   ZmNetDNS::ParseReplyUDP(&quest, reply_buffer, recv_len);
 *   if (quest.qname[0] != '\0') {
 *       printf("Reply for: %s\n", quest.qname);
 *   }
 * @endcode
 */
void ZmNetDNS::ParseReplyUDP(ZM_NET_DNS_QUESTION* quest, const BYTE* dnsdata, size_t dlen)
{
    if (dnsdata && dlen > 14)
    {
        ZM_NET_DNS_HEAD* dnshead = (ZM_NET_DNS_HEAD*)dnsdata;
        // 检查 QR 位为 1（应答报文）
        if (dnshead && (dnshead->flags_int16 & htons(0x8000)))
        {
            /* size_t offset = */
            ExtractQuest(quest, dnsdata, ntohs(dnshead->qdcount), 12, dlen);
        }
    }
}

// https://stackoverflow.com/a/39375234/1928946

/**
 * @brief 构建 DNS 查询报文（Query）
 *
 * 生成完整的 DNS 查询报文：Header(12字节) + Question(变长)。
 * 事务 ID 基于当前毫秒时间戳生成，标志位设置 RD=1（期望递归查询）。
 *
 * @param udpdata  输出缓冲区，接收 DNS 报文数据
 * @param hostname 要查询的域名
 * @return 报文总长度（字节）
 *
 * @example
 * @code
 *   BYTE buf[512];
 *   size_t len = ZmNetDNS::BuildQuery(buf, "www.example.com");
 *   // buf 现在包含完整的 DNS 查询报文
 *   // len 为报文长度（Header 12 字节 + Question 区域）
 *   sendto(sock, (char*)buf, len, 0, (sockaddr*)&dns_addr, sizeof(dns_addr));
 * @endcode
 */
size_t ZmNetDNS::BuildQuery(BYTE* udpdata, const char* hostname)
{
    ZM_NET_DNS_HEAD* dnsh = (ZM_NET_DNS_HEAD*)udpdata;
    memset(dnsh, 0, sizeof(ZM_NET_DNS_HEAD));
    // 使用当前时间戳的低 16 位作为事务 ID
    dnsh->trans_id = htons((uint16_t)(ZmSystem::CurrentTimeMills() & 0xFFFF));
    dnsh->flags.rd = 1;        // 设置期望递归（Recursion Desired）
    dnsh->qdcount = htons(1);  // 1 个查询问题

    ZM_NET_DNS_QUESTION quest;
    QuestInit(&quest, hostname);

    // DNS Header 固定 12 字节，Question 区域从偏移 12 开始
    return 12 + FieldQuestPut(udpdata, 12, &quest);
}

/**
 * @brief 构建 DNS 应答报文（Reply）
 *
 * 根据查询问题和解析到的地址，生成完整的 DNS 应答报文：
 * Header(12字节) + Question(变长) + Answer(变长)。
 * 标志位设置 QR=1（应答）、AA=1（权威应答）。
 *
 * @param dnsp     输出缓冲区，接收 DNS 报文数据
 * @param trans_id 事务ID，需与对应请求报文的 trans_id 一致
 * @param quest    查询问题结构体（从请求报文中解析得到）
 * @param addr     解析到的地址。传 NULL 则应答中不包含 Answer 区域（域名不存在）
 * @return 报文总长度（字节）
 *
 * @example
 * @code
 *   // 收到 DNS 查询后构建应答
 *   ZM_NET_DNS_QUESTION quest;
 *   ZmNetDNS::ParseQueryUDP(&quest, query_buf, query_len);
 *
 *   struct sockaddr_in addr;
 *   addr.sin_family = AF_INET;
 *   addr.sin_addr.s_addr = inet_addr("192.168.1.100");
 *
 *   BYTE reply[512];
 *   ZM_NET_DNS_HEAD* qhead = (ZM_NET_DNS_HEAD*)query_buf;
 *   size_t reply_len = ZmNetDNS::BuildReply(reply, ntohs(qhead->trans_id), &quest, (sockaddr*)&addr);
 *   sendto(sock, (char*)reply, reply_len, 0, client_addr, addr_len);
 * @endcode
 */
size_t ZmNetDNS::BuildReply(BYTE* dnsp, uint16_t trans_id, const ZM_NET_DNS_QUESTION* quest,
    const struct sockaddr* addr)
{
    size_t offset = 0;

    // 构建 DNS Header
    ZM_NET_DNS_HEAD* dnsh = (ZM_NET_DNS_HEAD*)dnsp;
    memset(dnsh, 0, sizeof(ZM_NET_DNS_HEAD));
    dnsh->trans_id = htons(trans_id);
    dnsh->flags.qr = 1;   // 应答报文
    dnsh->flags.aa = 1;   // 权威应答
    // dnsh->flags.rcode = (NULL!=addr)? 0x00 : 0x03;
    offset += 12;

    // Question fields: 回显查询问题
    dnsh->qdcount = htons(1);
    offset += FieldQuestPut(dnsp, offset, quest);

    // Answer fields: 根据地址类型写入 A 或 AAAA 记录
    uint16_t ancount = 0;
    // TODO: supports multiple addresses
    if (addr)
    {
        ancount++;
        offset += FieldRecordPutARPA(dnsp, offset, quest->qname, addr);
    }
    dnsh->ancount = htons(ancount);

    // Authority/Additional fields
    // skips

    return offset;
}