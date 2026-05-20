#ifndef ZM_NET_DNS_H
#define ZM_NET_DNS_H

#include "zm_net_ip.h"

#define ZM_DNS_TTL_MS    300000L     /** 300 秒内不做重复解析 */

/**
 * https://jocent.me/2017/06/18/dns-protocol-principle.html
 * 1. 会话标识（2字节）：是DNS报文的ID标识，对于请求报文和其对应的应答报文，这个字段是相同的，
 *                     通过它可以区分DNS应答报文是哪个请求的响应
 * 2. 标志（2字节）：
 *      +--------------------------------------------------+
 *      | QR | opcode | AA | TC | RD | RA | (zero) | rcode |
 *      +--------------------------------------------------+
 *      |  1 |    4   |  1 |  1 |  1 |  1 |    3   |   4   |
 *      +--------------------------------------------------+
 *      QR（1bit）	查询/响应标志，0为查询，1为响应
 *      opcode（4bit）	0表示标准查询，1表示反向查询，2表示服务器状态请求
 *      AA（1bit）	表示授权回答
 *      TC（1bit）	表示可截断的
 *      RD（1bit）	表示期望递归
 *      RA（1bit）	表示可用递归
 *  rcode（4bit）	表示返回码，0表示没有差错，3表示名字差错，2表示服务器错误（Server Failure）
 * 3. 数量字段（总共8字节）：Questions、Answer RRs、Authority RRs、Additional RRs 各自表示后面的四个区域的数目。
 *                         Questions表示查询问题区域节的数量，
 *                         Answers表示回答区域的数量，
 *                         Authoritative namesversers表示授权区域的数量，
 *                         Additional recoreds表示附加区域的数量
 */
 // https://www2.cs.duke.edu/courses/fall16/compsci356/DNS/DNS-primer.pdf
 // http://www.firewall.cx/networking-topics/protocols/domain-name-system-dns/160-protocols-dns-query.html
 // http://www.firewall.cx/networking-topics/protocols/domain-name-system-dns/161-protocols-dns-response.html
 /**
  * Flags Parameter Fields
  *  Bit No.     Meaning when unset/set
  *    1         0 = Query
  *              1 = Response
  *   2-5        0000 = Standard Query
  *              0100 = Inverse (in-addr.arpa)
  *              0010 & 0001 not used
  *    6         0 = non-authoritative DNS answer
  *              1 = authoritative DNS answer
  *    7         0 = Message not truncated
  *              1 = Message truncated
  *    8         0 = Non-recursive query
  *              1 = Recursive query
  *    9         0 = Recursion not available
  *              1 = Recursion available
  *  10 & 12     Reserved
  *    11        0 = Answer/authority protion was not authenticated by the server
  *              1 = Answer/authority portion was authenticated by the server
  *   13-16      0000 = No error
  *              0100 = Format error in query
  *              0010 = Server failure
  *              0001 = Name does not exist
  */

/**
 * @brief DNS 报文标志位结构体（2字节），对应 DNS Header 中的 Flags 字段
 *
 * 按网络字节序位域定义，描述了 DNS 报文的查询/响应方向、操作类型、
 * 是否递归、返回码等信息。
 *
 * @note 位域排列与 DNS 协议规范一致，第一个字节包含 rd/tc/aa/opcode/qr，
 *       第二个字节包含 rcode/_resv/ra。
 */
typedef struct
{
    uint8_t     rd : 1;    /** [7] Recursion desired 表示期望递归*/
    uint8_t     tc : 1;    /** [6] Truncated*/
    uint8_t     aa : 1;    /** [5] Authoritative 是否是权威应答 */
    uint8_t     opcode : 4;    /** [1] opcode */
    uint8_t     qr : 1;    /** [0] 0:Query, 1:Reply */

    uint8_t     rcode : 4;    /** [C] reply code 表示返回码，0表示没有差错，3表示名字差错，2表示服务器错误（Server Failure）*/
    uint8_t     _resv : 3;    /** [9] - */
    uint8_t     ra : 1;    /** [8] Recursion available 表示可用递归 */

}ZM_NET_DNS_FLAGS;



/**
 * @brief DNS 报文头部结构体（12字节），对应 DNS 协议报文的前 12 字节
 *
 * 包含事务ID、标志位、以及四个计数字段（问题/回答/授权/附加区域的记录数）。
 *
 * @example
 * @code
 *   ZM_NET_DNS_HEAD head;
 *   memset(&head, 0, sizeof(head));
 *   head.trans_id = htons(0x1234);  // 设置事务ID
 *   head.flags.rd = 1;              // 期望递归查询
 *   head.qdcount = htons(1);        // 1个查询问题
 * @endcode
 */
typedef struct
{
    uint16_t    trans_id;
    union
    {
        ZM_NET_DNS_FLAGS flags;      /** 位域形式访问各标志位 */
        uint16_t         flags_int16; /** 整体以 uint16_t 形式访问（用于判断 QR 位等） */
    };
    uint16_t    qdcount;    // question
    uint16_t    ancount;    // answer
    uint16_t    nscount;    // authority
    uint16_t    arcount;    // additional
}ZM_NET_DNS_HEAD;

/**
 * @brief DNS 资源记录（Resource Record）的固定头部结构体
 *
 * 对应 DNS 协议中 Answer/Authority/Additional 区域内每条记录的头部。
 * rdata 为柔性数组（长度0），实际数据紧跟在 rdlen 指定的长度之后。
 *
 * @note 结构体使用 1 字节对齐（pack），总大小固定为 10 字节（不含 rdata）
 */
#if defined(_MSC_VER)
#pragma warning(disable:4200)
#pragma pack(push, 1)
#endif
typedef struct
{
    uint16_t    rtype;
    uint16_t    rclass;
    uint32_t    ttl;
    uint16_t    rdlen;
    BYTE        rdata[0]; //触发warning:4200
}ZM_DNS_RECORD;
#if defined(_MSC_VER)
#   pragma pack(pop)
#   pragma warning(default:4200)
#endif

/**
 * https://asia.cloudns.net/wiki/article/9/
 * qtype 查询类型定义表
 * 类型   助记符     说明
 *  1     A         IPv4地址。
 *  2     NS        名字服务器。
 *  5     CNAME     规范名称，定义主机的正式名字的别名。
 *  6     SOA       开始授权，标记一个区的开始。
 *  11    WKS       熟知服务，定义主机提供的网络服务。
 *  12    PTR       指针，把IP地址转化为域名。
 *  13    HINFO     主机信息，给出主机使用的硬件和操作系统的表述。
 *  15    MX        邮件交换，把邮件改变路由送到邮件服务器。
 *  28    AAAA      IPv6地址
 *  252   AXFR      传送整个区的请求。
 *  255   ANY       对所有记录的请求。
 *  qclass, A two octet code that specifies the class of the query.
 *          You should always use 0x0001 for this project, representing Internet addresses.
 */

/**
 * @brief DNS 查询问题（Question）结构体
 *
 * 对应 DNS 报文中 Question 区域的一条查询记录，
 * 包含查询的域名、类型（A/AAAA/NS 等）和类别（通常为 IN=1）。
 *
 * @example
 * @code
 *   ZM_NET_DNS_QUESTION quest;
 *   ZmNetDNS::QuestInit(&quest, "www.example.com");        // 默认查询 IPv4 A 记录
 *   ZmNetDNS::QuestInit(&quest, "www.example.com", AF_INET6); // 查询 IPv6 AAAA 记录
 * @endcode
 */
typedef struct
{
    char        qname[128];
    uint16_t    qtype;
    uint16_t    qclass;
}ZM_NET_DNS_QUESTION;


/**
 * @brief DNS 应答/授权/附加区域记录结构体（Answer/Authority/Additional）
 *
 * 解析 DNS 报文中 Answer、Authority、Additional 三个区域的记录时使用，
 * 将域名、类型、类别、TTL、数据长度和数据内容统一存储。
 *
 * @note rdata 缓冲区固定为 128 字节，足以容纳 IPv6 地址（16字节）等常见记录
 */
typedef struct
{
    char        name[128];
    uint16_t    type;       // 1:A:Host Address, 5:CNAME:Canonical Name (Alias), 28:AAAA:IPv6
    uint16_t    rclass;
    uint32_t    ttl;
    uint16_t    rdlength;
    BYTE        rdata[128];
}ZM_NET_DNS_AAA;    // Answer/Authority/Additional


/**
 * @brief DNS 解析工具类，提供域名解析、DNS缓存、DNS报文构建与解析等功能
 *
 * 所有方法均为静态方法，无需实例化。该类封装了以下核心能力：
 * - DNS 缓存管理（CachePut/CacheGet/ClearCache）
 * - 域名解析（GetHostIPByName/GetAddressByName）
 * - 知名主机管理（AddWellKnownHost/DelWellKnownHost/IsWellKnownHost）
 * - 系统 DNS 配置获取（GetResolves/GetDNSAddresses）
 * - DNS 报文构建与解析（BuildQuery/BuildReply/ParseQueryUDP/ParseReplyUDP）
 *
 * @example 域名解析
 * @code
 *   ZM_IP_ADDR ip;
 *   char ipstr[64] = {0};
 *   bool ok = ZmNetDNS::GetHostIPByName(&ip, "www.example.com", 80, ipstr, sizeof(ipstr));
 *   if (ok) { printf("resolved: %s\n", ipstr); }
 * @endcode
 *
 * @example 构建DNS查询报文
 * @code
 *   BYTE buf[512];
 *   size_t len = ZmNetDNS::BuildQuery(buf, "www.example.com");
 *   // 发送 buf[0..len-1] 到 DNS 服务器
 * @endcode
 */
class ZmNetDNS
{
private:
    ZmNetDNS() {};

public:
    ~ZmNetDNS() {};

    /**
     * @brief 初始化 DNS 查询问题结构体
     *
     * 根据 sin_family 自动设置 qtype：AF_INET6 对应 AAAA(28)，
     * AF_INET 对应 A(1)。
     *
     * @param quest      输出的查询问题结构体指针
     * @param qname      查询的域名，如 "www.example.com"，可为 NULL（置空）
     * @param sin_family 地址族，AF_INET（默认，查IPv4）或 AF_INET6（查IPv6）
     * @param qclass     查询类别，默认 1 表示 Internet（IN）
     *
     * @example
     * @code
     *   ZM_NET_DNS_QUESTION quest;
     *   ZmNetDNS::QuestInit(&quest, "example.com");           // IPv4 A 记录查询
     *   ZmNetDNS::QuestInit(&quest, "example.com", AF_INET6); // IPv6 AAAA 记录查询
     * @endcode
     */
    static inline void QuestInit(ZM_NET_DNS_QUESTION* quest,
        const char* qname, uint16_t sin_family = AF_INET, uint16_t qclass = 1)
    {
        memset(quest, 0, sizeof(ZM_NET_DNS_QUESTION));
        snprintf(quest->qname, sizeof(quest->qname), "%s", qname ? qname : "");
        quest->qtype = (AF_INET6 == sin_family) ? 28 : 1;
        quest->qclass = qclass;
    }

    /**
     * @brief 更新缓存中指定主机名的 TTL（存活时间）
     *
     * @param hostname 主机名
     * @param ttl      存活时间（毫秒）。传 0 表示永久有效（不过期），
     *                 默认使用 ZM_DNS_TTL_MS（300秒）
     */
    static void        CacheUpdateTTL(const char* hostname, int64_t ttl = ZM_DNS_TTL_MS);

    /**
     * @brief 将主机名和对应的地址存入 DNS 缓存
     *
     * @param hostname 主机名
     * @param sa       地址信息（支持 sockaddr_in 和 sockaddr_in6）
     */
    static void        CachePut(const char* hostname, const struct sockaddr* sa);

    /**
     * @brief 从 DNS 缓存中查找主机名对应的地址
     *
     * @param hostname 主机名
     * @param port     目标端口号（会写入输出的 sa 中）
     * @param sa       输出的地址结构体指针（需由调用方分配内存）
     * @param now      当前时间戳（毫秒），用于判断缓存是否过期
     * @return 地址结构体长度（sizeof(sockaddr_in) 或 sizeof(sockaddr_in6)），
     *         返回 0 表示缓存未命中或已过期
     */
    static socklen_t   CacheGet(const char* hostname, uint16_t port, struct sockaddr* sa, uint64_t now);

    /**
     * @brief 清空所有 DNS 缓存条目
     */
    static void        ClearCache();

    /**
     * @brief 添加一个知名主机名到列表中
     *
     * @param hostname 知名主机名，如 "google.com"
     */
    static void        AddWellKnownHost(const char* hostname);

    /**
     * @brief 从知名主机列表中删除指定的主机名
     *
     * @param hostname 要删除的主机名
     */
    static void        DelWellKnownHost(const char* hostname);

    /**
     * @brief 清空所有知名主机名
     */
    static void        DelWellKnownHost();

    /**
     * @brief 判断主机名是否以某个知名主机名结尾（后缀匹配）
     *
     * 例如知名主机名为 "google.com"，则 "mail.google.com" 会返回 true。
     *
     * @param hostname 待判断的主机名
     * @return true 表示该主机名属于知名主机
     */
    static bool        IsWellKnownHost(const char* hostname);

    /**
     * @brief 获取系统配置的 DNS 解析服务器地址（仅 IPv4）
     *
     * 通过 Windows API GetNetworkParams 获取，多个地址以逗号分隔。
     * 不支持获取 IPv6 DNS 服务器地址。
     *
     * @return 逗号分隔的 DNS 服务器 IPv4 地址字符串，如 "8.8.8.8,8.8.4.4"
     *
     * @example
     * @code
     *   std::string dns = ZmNetDNS::GetResolves();
     *   // dns 可能是 "192.168.1.1,8.8.8.8"
     * @endcode
     */
    static std::string GetResolves();

    /**
     * @brief 获取系统配置的 DNS 服务器地址（同时支持 IPv4 和 IPv6）
     *
     * 通过 Windows API GetAdaptersAddresses 枚举所有网卡的 DNS 配置，
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
    static std::string GetDNSAddresses();

    /**
     * @brief 通过主机名解析 IP 地址，返回主机序的 ZM_IP_ADDR
     *
     * 内部调用 GetAddressByName 进行实际解析，支持 IPv4 和 IPv6。
     *
     * @param ip        输出的 IP 地址结构体，ipv4 为主机序
     * @param hostname  主机名，如 "www.example.com"
     * @param port      目标端口号
     * @param ipaddress 可选的输出缓冲区，接收 IP 地址字符串形式。传 NULL 则不输出
     * @param cnt       ipaddress 缓冲区大小（字节）。传 0 则不输出字符串
     * @param family    地址族过滤：AF_UNSPEC（默认，优先IPv4）、AF_INET、AF_INET6
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
    static bool      GetHostIPByName(ZM_IP_ADDR* ip, const char* hostname, uint16_t port,
        char* ipaddress = NULL, int cnt = 0, uint16_t family = AF_UNSPEC);

    /**
     * @brief 通过主机名获取 sockaddr 地址结构，支持 IPv4 和 IPv6
     *
     * 优先检查 hostname 是否为 IP 地址字符串（直接转换），
     * 然后查找 DNS 缓存，缓存未命中时通过 GetAddrInfoExW 异步解析。
     * 解析结果会自动加入缓存。
     *
     * @param sa6      输出的地址结构体。IPv4 时实际写入 sockaddr_in（复用同一块内存）
     * @param hostname 主机名
     * @param port     目标端口号
     * @param type     socket 类型，SOCK_STREAM（默认，TCP）或 SOCK_DGRAM（UDP）
     * @param family   地址族过滤：AF_UNSPEC（默认）、AF_INET、AF_INET6
     * @return 地址结构体长度（sizeof(sockaddr_in) 或 sizeof(sockaddr_in6)），
     *         返回 0 表示解析失败
     */
    static socklen_t GetAddressByName(struct sockaddr_in6* sa6, const char* hostname, uint16_t port,
        int type = SOCK_STREAM, uint16_t family = AF_UNSPEC);

    /**
     * @brief 从 evutil_addrinfo 链表中提取第一个 IPv4/IPv6 地址
     *
     * 遍历 evutil_addrinfo 链表，优先返回 IPv4 地址。
     * 可选地将 IP 地址转换为字符串形式。
     *
     * @param sa6       输出的地址结构体，可为 NULL
     * @param addr      evutil_addrinfo 链表头指针
     * @param port      目标端口号（写入输出地址中）
     * @param ipstr     可选的输出缓冲区，接收 IP 地址字符串。传 NULL 则不输出
     * @param capacity  ipstr 缓冲区大小
     * @return 地址结构体长度，0 表示链表中无有效地址
     */
    static socklen_t ExtractEventAddrInfo(struct sockaddr_in6* sa6, struct evutil_addrinfo* addr,
        char* ipstr = NULL, size_t capacity = 0);

    /**
     * @brief 从 DNS 请求（Query）UDP 报文中解析出查询的 hostname
     *
     * @param quest   输出的查询问题结构体，接收解析结果
     * @param udpdata DNS 请求 UDP 报文数据（包含 DNS Header + Question 区域）
     * @param dlen    报文数据长度（字节）
     */
    static void   ParseQueryUDP(ZM_NET_DNS_QUESTION* quest, const BYTE* udpdata, size_t dlen);

    /**
     * @brief 从 DNS 应答（Reply）UDP 报文中解析出查询的 hostname
     *
     * @param quest   输出的查询问题结构体，接收解析结果
     * @param dnsdata DNS 应答 UDP 报文数据
     * @param dlen    报文数据长度（字节）
     */
    static void   ParseReplyUDP(ZM_NET_DNS_QUESTION* quest, const BYTE* dnsdata, size_t dlen);

    /**
     * @brief 构建 DNS 查询报文（Query）
     *
     * 生成完整的 DNS 查询报文，包含 Header 和 Question 区域。
     * 事务ID 基于当前时间戳生成，标志位设置 RD=1（期望递归）。
     *
     * @param udpdata  输出缓冲区，接收 DNS 报文数据
     * @param hostname 要查询的域名
     * @return 报文总长度（字节）
     *
     * @example
     * @code
     *   BYTE buf[512];
     *   size_t len = ZmNetDNS::BuildQuery(buf, "www.example.com");
     *   sendto(sock, (char*)buf, len, 0, (sockaddr*)&dns_server, sizeof(dns_server));
     * @endcode
     */
    static size_t BuildQuery(BYTE* udpdata, const char* hostname);

    /**
     * @brief 构建 DNS 应答报文（Reply）
     *
     * 根据查询问题和解析到的地址，生成 DNS 应答报文，
     * 包含 Header + Question + Answer 区域。
     *
     * @param dnsp     输出缓冲区，接收 DNS 报文数据
     * @param trans_id 事务ID，需与对应请求的 trans_id 一致
     * @param quest    查询问题结构体
     * @param addr     解析到的地址，传 NULL 则应答中不包含 Answer 区域
     * @return 报文总长度（字节）
     *
     * @example
     * @code
     *   ZM_NET_DNS_QUESTION quest;
     *   ZmNetDNS::QuestInit(&quest, "mydevice.local");
     *   struct sockaddr_in addr;
     *   addr.sin_family = AF_INET;
     *   addr.sin_addr.s_addr = inet_addr("192.168.1.100");
     *   BYTE reply[512];
     *   size_t len = ZmNetDNS::BuildReply(reply, 0x1234, &quest, (sockaddr*)&addr);
     * @endcode
     */
    static size_t BuildReply(BYTE* dnsp, uint16_t trans_id, const ZM_NET_DNS_QUESTION* quest, const struct sockaddr* addr);

private:
    /**
     * @brief 判断 IPv6 地址是否为站点本地地址（fec0::/10）
     *
     * 站点本地地址通常不应作为正常的 DNS 服务器地址，用于过滤。
     *
     * @param sockaddr IPv6 地址结构体指针
     * @return true 表示是站点本地地址
     */
    static bool IsSiteLocalAddress(const struct sockaddr_in6* sockaddr);

    /**
     * @brief 递归解析 DNS 报文中的域名标签（Label）
     *
     * DNS 域名使用标签编码格式：每个标签前有一个长度字节，以 0 长度结尾。
     * 支持压缩格式（高2位为 11 时，后续14位为偏移指针）。
     *
     * @param label  输出缓冲区，接收解析后的域名（以 '.' 分隔）
     * @param space  输出缓冲区大小
     * @param dnsp   DNS 报文数据
     * @param offset 当前解析位置在报文中的偏移
     * @param limit  报文数据总长度（防止越界）
     * @param depth  递归深度（防止死循环，最大16层）
     * @return 解析完成后新的偏移位置
     */
    static size_t ParseLabel(char* label, size_t space, const BYTE* dnsp, size_t offset, size_t limit, int depth = 0);

    /**
     * @brief 从 DNS 报文中提取查询问题区域（Question Section）
     *
     * @param quest   输出数组，仅第一条记录会被写入
     * @param dns     DNS 报文数据
     * @param qdcount Question 区域的记录数
     * @param offset  Question 区域在报文中的起始偏移
     * @param limit   报文数据总长度
     * @return 解析完成后新的偏移位置
     */
    static size_t ExtractQuest(ZM_NET_DNS_QUESTION* quest, const BYTE* dns, size_t qdcount, size_t offset, size_t limit);

    /**
     * @brief 从 DNS 报文中提取应答/授权/附加区域记录（Answer/Authority/Additional）
     *
     * @param aaa    输出结构体，仅第一条记录会被写入
     * @param dns    DNS 报文数据
     * @param count  该区域的记录数
     * @param offset 该区域在报文中的起始偏移
     * @param limit  报文数据总长度
     * @return 解析完成后新的偏移位置
     */
    static size_t ExtractAAA(ZM_NET_DNS_AAA* aaa, const BYTE* dns, size_t count, size_t offset, size_t limit);

    /**
     * @brief 将主机名编码为 DNS 标签格式写入报文缓冲区
     *
     * 将点分域名（如 "www.example.com"）转换为 DNS 标签格式
     * （如 [3]www[7]example[3]com[0]），写入 dnsp+offset 位置。
     *
     * @param dnsp     DNS 报文缓冲区
     * @param offset   写入起始偏移
     * @param hostanme 待编码的主机名
     * @return 写入的总字节数（含结尾的 '\0'）
     */
    static size_t LabelPut(BYTE* dnsp, size_t offset, const char* hostanme);

    /**
     * @brief 向 DNS 报文缓冲区写入一条资源记录（Resource Record）
     *
     * @param dnsp   DNS 报文缓冲区
     * @param offset 写入起始偏移
     * @param rname  资源记录的域名
     * @param rtype  记录类型（1=A, 28=AAAA 等）
     * @param rclass 记录类别（通常为 1=IN）
     * @param rdlen  资源数据长度
     * @param rdata  资源数据（如 IPv4 的 4 字节地址、IPv6 的 16 字节地址）
     * @return 写入的总字节数
     */
    static size_t FieldRecordPut(BYTE* dnsp, size_t offset, const char* rname,
        uint16_t rtype, uint16_t rclass, uint16_t rdlen, const BYTE* rdata);

    /**
     * @brief 向 DNS 报文缓冲区写入查询问题字段（Question Field）
     *
     * @param dnsp   DNS 报文缓冲区
     * @param offset 写入起始偏移
     * @param quest  查询问题结构体
     * @return 写入的总字节数（标签 + qtype + qclass = 4 + 标签长度）
     */
    static size_t FieldQuestPut(BYTE* dnsp, size_t offset, const ZM_NET_DNS_QUESTION* quest);

    /**
     * @brief 根据地址族自动选择 A 或 AAAA 类型，写入 DNS 资源记录
     *
     * IPv4 地址使用 A 记录（type=1，rdlen=4），
     * IPv6 地址使用 AAAA 记录（type=28，rdlen=16）。
     *
     * @param dnsp     DNS 报文缓冲区
     * @param offset   写入起始偏移
     * @param hostname 资源记录的域名
     * @param addr     地址信息（通过 sa_family 判断 IPv4/IPv6）
     * @return 写入的总字节数
     */
    static size_t FieldRecordPutARPA(BYTE* dnsp, size_t offset, const char* hostname, const struct sockaddr* addr);
};

#endif /* ZM_NET_DNS_H */