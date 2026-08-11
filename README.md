# ZiMoPublic

ZiMo 生态的 C++ 公共基础库，为 ZiMoService 及其他上层项目提供网络通信、Windows 服务框架、SSL/TLS 安全、日志、JSON 处理、SQLite、线程工具等通用能力。

## 模块总览

```
ZiMoPublic/
├── define/          # 通用宏定义、版本号
├── json/            # nlohmann/json 封装（类型安全的读写辅助）
├── libevent/        # 预编译 libevent 头文件及静态库（事件驱动网络库）
├── libopus/         # 预编译 Opus 头文件及静态库（音频编码）
├── net/             # 网络通信模块（TCP/HTTP 服务端与客户端/RESTful/DNS/事件循环线程/请求调度/路由中间件/消息广播/SSE）
├── openssl/         # 预编译 OpenSSL 头文件及静态库
├── service/         # Windows 服务基类（SCM 集成、安装/卸载）
├── spdlog/          # 定制版 spdlog 日志库 + zm_logger 封装
├── sqllite/         # SQLite amalgamation 源码（sqlite3.c/h，编译时引入）
├── ssl/             # SSL/TLS 上下文管理、证书指纹校验、session ticket
├── util/            # 通用工具（线程、字符串、文件、容器、SQLite、zip、系统）
├── zlib/            # zlib 1.3.1 头文件及静态库（gzip/deflate 解压、zip 写入）
```

## 各模块详解

### define — 通用定义

| 文件 | 说明 |
|------|------|
| `zm_simple_define.h` | 通用宏：`ZM_UNUSED`、`ZM_MAX`/`ZM_MIN`、`ZM_BETWEEN`、`ZmSleepMS`/`ZmSleepUS`、`zm_memcpy`/`zm_memset`（安全零长）、`Zm_IsValidHandle` |
| `zm_version_define.h` | 版本宏：`ZIMO_SERVER_VERSION "1.0.0.0"` |

### json — JSON 处理

基于 nlohmann::ordered_json 的类型安全读写辅助，全部 inline 实现。

| 函数 | 说明 |
|------|------|
| `zm_json_get_int` | 读取 int（支持 0x 十六进制字符串、布尔转换） |
| `zm_json_get_str` | 读取 string（数值/对象/数组自动序列化） |
| `zm_json_get_float` | 读取 double |
| `zm_json_get_bool` | 读取 bool（支持 "true"/"false"、0/非0 转换） |
| `zm_json_get_path` | 按 `"a.b.c"` 点路径从嵌套 JSON 中取值 |
| `zm_json_get_array` | 读取 JSON 数组为 `std::vector<T>` |
| `zm_json_has` | 安全检查字段是否存在 |
| `zm_json_size` | 获取数组/对象元素个数 |
| `zm_json_set` | 写入键值对 |
| `zm_json_erase` | 安全删除字段 |
| `zm_json_dump` | 序列化为字符串 |
| `zm_json_merge` | 浅合并两个 JSON 对象 |
| `zm_json_parse` | 解析 JSON 字符串（带异常捕获） |

类型别名：`using ZMJSON = nlohmann::ordered_json;`

### net — 网络通信

#### zm_net_runloop — 事件循环线程

| 类 | 说明 |
|----|------|
| `ZmEvBaseRunLoop` | 自带 libevent 事件循环的线程（`ZmThread` 子类）：`Loop()` 启动并等待就绪、`Control()` 投递控制事件（含 EXIT）、`GetEventBase()`/`GetEventDnsBase()`；内置 60 秒心跳定时器 |

为广播服务端、HTTP 客户端等自持事件循环的模块提供基础线程骨架。

#### zm_net_ip — IP 地址与协议头

| 结构体/类 | 说明 |
|-----------|------|
| `ZM_IP_ADDR` | IPv4/IPv6 地址联合体（主机字节序） |
| `ZM_PEER_ADDR` | IP + 端口对端地址 |
| `ZM_IP_CIDR6` / `ZM_IP_RANGE6` | IPv6 CIDR 子网 / 地址范围 |
| `ZM_TCP_HEAD` / `ZM_UDP_HEAD` / `ZM_ICMP_HEAD` | 传输层协议头结构体 |
| `ZmNetIP` | IPv4/IPv6 地址转换、比较、验证、分类（回环/私有/链路本地/组播）、MAC 地址查询 |
| `ZmNetIPv6` | IPv6 地址比较、算术运算、CIDR 子网计算、范围解析 |

#### zm_net_dns — DNS 解析

| 结构体/类 | 说明 |
|-----------|------|
| `ZM_NET_DNS_HEAD` | DNS 报文头部结构体（含位域 Flags） |
| `ZM_NET_DNS_QUESTION` | DNS 查询问题结构体 |
| `ZM_NET_DNS_AAA` | DNS 应答/授权/附加记录结构体 |
| `ZM_DNS_RECORD` | DNS 资源记录头部（含柔性数组 rdata） |
| `ZmNetDNS` | DNS 工具类：缓存管理、域名解析、知名主机管理、系统 DNS 获取、DNS 报文构建/解析 |

特性：
- DNS 缓存（TTL 300 秒可配）
- 知名主机后缀匹配
- 同时获取系统 IPv4 + IPv6 DNS 服务器地址
- DNS 报文构建（Query/Reply）与解析（UDP 请求/应答）

#### zm_net_socket — TCP/SSL 套接字

| 类 | 说明 |
|----|------|
| `ZmWinSockHelper` | Winsock 初始化的 RAII 辅助类（引用计数） |
| `ZmNetSocketBase` | 网络套接字抽象基类：`Open`/`Close`/`Send`/`Recv`/`IsConnected` |
| `ZmNetSocketTCP` | 阻塞式 TCP 客户端：超时连接、KeepAlive、HTTP CONNECT 代理隧道、非阻塞模式切换 |
| `ZmNetSocketSSL` | SSL/TLS 客户端：基于 OpenSSL BIO 链，支持 SNI、IPv4/IPv6、证书指纹校验 |

#### zm_net_http — HTTP / JSON-RPC / RESTful 服务端

| 类 | 说明 |
|----|------|
| `ZmHttpUtil` | HTTP 工具：动词解析、请求解析、URI 解析、Query 参数提取 |
| `ZmHttpdTask` | HTTP 请求上下文：读取 URI/方法/请求头，写入响应状态码/头/体，支持流式分块回复；`IsHttps()` 判断连接是否 TLS；`SetRateLimit`/`JoinRateLimitGroup` 单连接/分组限速；连接关闭通知 |
| `ZmHttpHead` | HTTP 头部封装：解析、构建、键值查询 |
| `ZmHttpServer` | HTTP/HTTPS 服务器：构造传入证书即启用 TLS（可选 HTTP→HTTPS 301 重定向端口、TLS 会话缓存）；多线程 Worker + doer 池化；证书热加载、HSTS、session ticket 密钥注入/轮换；可选集成 `ZmReqLoopPool`（`EnableLoopPool`） |
| `ZmJsonRpcServer` | JSON-RPC 2.0 服务器：协议解析与分发，支持 GET（Base64）/POST、JSONP 回调 |
| `ZmRESTfulServer` | ★ RESTful HTTP 服务器：继承 ZmHttpServer，支持异步回调 + `TriggerReply()` 直通响应 |

关键设计：
- **线程模型**：事件循环线程接收请求 → Worker 线程处理 → event_active 通知事件循环线程发送响应
- **HTTPS/TLS**：构造参数 `certFile`/`keyFile` 非空即启用；`redirect_from_port` 自动把 HTTP 请求 301 到 HTTPS；`sessionCacheSize`/`sessionContext` 启用 TLS 会话缓存；HTTPS 模式自动发送 HSTS 头
- **证书热加载**：`ReloadCertificate()` 原子替换 SSL_CTX（事件循环线程内操作），旧 ctx 延时 5 分钟释放，现有 TLS 连接不受影响；热加载后自动补设 session cache 与 ticket 密钥
- **Session Ticket**：`SetTicketKeys()`/`PostSetTicketKeys()`（80 字节，投递到事件循环线程执行，避免与并发握手竞争）
- **限速**：`ZmHttpdTask::SetRateLimit()` 单连接独立限速（收发独立 bps，动态可调）；`JoinRateLimitGroup()` 多连接共享带宽池，可与单连接限速叠加取最小值
- **连接关闭通知**：closecb 广播到该连接所有在飞 doer（流式断连检测）
- **CORS 支持**：自动添加跨域响应头；浏览器带 `Origin` 时回显精确值并允许凭据
- **即时释放**：Worker 资源即时释放（doer 池化复用），减少内存占用
- **SSE 推送**：支持 `StartStreamReply`/`SendReplyChunk`/`EndStreamReply` 流式分块响应（text/event-stream）
- **请求头透传**：Worker 线程可通过 `ZmHttpdTask` 读写请求头/响应头，支持业务侧自定义头部
- **业务事件循环池**：`EnableLoopPool()` 启用 `ZmReqLoopPool`（预创建/扩容/预算 deadline/断连放弃），`SetLoopPoolFactory()` 供派生类注入 per-request 状态（JRPC→`ZmReqLoopJrpc`）；`BeginClose()`/`DrainWorkers()` 消除关闭期 doer 与池销毁竞态

#### zm_net_http_router — HTTP 路由中间件

| 类 | 说明 |
|----|------|
| `ZmHttpRouter` | Express 风格的路由器：`Get`/`Post`/`Put`/`Delete`/`Any` 注册，前缀树匹配 |
| `ZmHttpMiddlewareLogging` | 日志中间件：记录请求方法、路径、耗时 |
| `ZmHttpMiddlewareRecovery` | 异常恢复中间件：捕获 handler 异常返回 500 |

路由模式：`(task, next)` 函数管道，支持 `*` 通配符兜底路由。

#### zm_net_http_client — HTTP/S 客户端（自带事件循环线程）

基于 libevent evhttp + OpenSSL + zlib 的完整 HTTP/HTTPS 客户端。每个 `ZmHttpClient` 实例持有独立事件循环线程（event_base + evdns_base，由 `ZmHttpClientLoop` 承载），任意线程可安全调用；单实例并发请求由连接池承载，`ZmHttpClientPool` 提供实例级池化（预创建 + 扩容 + 排队）。

| 类 | 说明 |
|----|------|
| `ZmHttpClientRequest` | 请求构建器（链式）：方法/URL/请求头、四种请求体（内存/JSON/表单/文件或流式 chunked）、超时（连接/读写/总）、重定向、代理（含认证）、Basic/Bearer 认证、cookie jar、gzip、重试、流式回调（SSE/数据块/进度）、落盘、Range、响应体上限、请求级 TLS 覆盖 |
| `ZmHttpClientResponse` | 响应：状态码、头（大小写不敏感查询）、体、Content-Length |
| `ZmHttpClientResult` | 请求结果：传输错误码 + 错误文本 + 响应。`Ok()` 只判传输层——404/500 等仍是"成功到达的响应"（与 curl/Go 同口径） |
| `ZmHttpClient` | 客户端：`Send`（同步，返回 `std::unique_ptr<ZmHttpClientResult>`，自动释放）/ `SendAsync`（异步，回调在循环线程执行且恰好触发一次）/ `Cancel(id)` / `CancelAll` / `CloseAll` |
| `ZmHttpClientPool` | 客户端实例池：预创建 + 扩容（上限）+ 排队（50ms 轮询，带中止标志）；回池须无在飞请求（CancelAll）+ 全部连接已销毁（CloseAll） |

特性：

- **连接池** — 按 `scheme:host:port` 空闲 keep-alive 复用（每主机上限 4，复用前校验 bev 有效性，失效自动丢弃）
- **重定向** — 301/302/303/307/308 语义正确（301 POST、302/303 转 GET 清体，307/308 保留方法/体）；跨主机剥离 `Authorization`/`Cookie`/`Referer`/`Origin`（防凭据与内部路径泄漏）；跳转预算与重试预算分离；总超时覆盖整条链
- **cookie jar** — RFC 6265 子集：域后缀 + 路径前缀匹配、`Secure`、`Max-Age`/`Expires`（Max-Age 优先）、仅主机域 cookie、IP 字面量忽略 Domain 属性、跨域 Domain 拒绝存储（防投毒）；进程内不持久化
- **gzip/deflate 自动解压** — 请求侧自动补 `Accept-Encoding: gzip, deflate`；响应侧经 zlib 边解压边分发，64KB 有界输出缓冲防解压炸弹；`Content-Encoding` 为 br 等其他编码原样透传
- **重试** — 仅幂等方法（GET/HEAD/PUT/DELETE）+ 连接类错误，指数退避（封顶），chunked 上传不重试；退避定时器可取消（收口/取消立即终止）
- **代理** — HTTP absolute-form + HTTPS CONNECT 隧道（含 `SetProxyAuth` 代理认证，逐跳头仅发代理）；代理连接不回池（防 key 错配）
- **TLS** — SNI（IP 字面量不发送）、mTLS 客户端证书（客户端级 / 请求级覆盖）、自定义 CA、verify 开关；SSL_CTX 懒构建 + 配置变更脏重建（旧身份空闲连接自动作废）
- **流式** — SSE 逐事件（`data:` 行解析，帧上限 1MB 防膨胀）、`OnDataChunk` 增量回调、`OutputFile` 边收边写落盘、chunked 流式上传泵（输出缓冲回调驱动，背压有界 ≤ 2 块）、进度回调（`sent/received, total`）
- **响应体上限** — `SetMaxBodySize`（解压后字节口径）：流式增量分发时中止（取消流）、全量收口时报错，超限 → `ZM_HTTPC_ERR_RESPONSE_TOO_LARGE`
- **超时** — 连接 / 读写（`SetReadWriteTimeout`，默认跟随连接超时）/ 总超时（deadline 覆盖整条链含重试，流式 2xx 后自动取消由读写超时接管）

错误码：`ZM_HTTPC_ERR_CONNECT` / `CONNECT_TIMEOUT` / `TIMEOUT` / `CANCELLED` / `PARSE` / `FILE_IO` / `STREAM_BROKEN` / `SSL` / `PROXY` / `REDIRECT` / `RESPONSE_TOO_LARGE` / `UNSUPPORTED`。

**线程模型**：单循环线程内零锁；任意线程经 `event_base_once` 投递；回调（SSE/数据块/进度/异步完成）全部在循环线程执行，回调内勿做重活、勿同步重入本客户端。

#### zm_net_http_client_loop — 客户端事件循环线程

| 类 | 说明 |
|----|------|
| `ZmHttpClientLoop` | `ZmHttpClient` 的自带事件循环线程（仿 `ZmEvBaseRunLoop`，定制为无心跳定时器）：`Start()` 启动并等待 event_base 就绪、`Stop()` 退出循环（join），event_base/evdns_base 线程内创建与释放 |

#### zm_net_req_loop — per-request 事件循环线程 + ZmReqLoopPool 池（事件驱动，池随基类同文件）

| 类 | 说明 |
|----|------|
| `ZmReqLoop` | per-request 事件循环线程：业务 = 入口回调 + 续体回调，全部在本线程（event_base）上执行 |
| `ZmReqLoopPool` | 请求池：预创建 + 扩容（上限）+ 排队（doer 线程等待，不阻塞 HTTP 事件循环）；每台 HTTP 服务器各自持有一个实例 |

关键设计：
- **事件驱动**：close/超时/外部返回均以事件（`PostToLoop`）送达本线程，per-request 状态仅本线程触碰，无跨线程竞争
- **生命周期**：池 Acquire → PostToLoop(START) → ProcessStart（Bind + onStart）→ 业务分段推进 → 回复 helper（TryReply + task 直通 + Release）→ 回池
- **deadline 兜底**：绝对截止时间 = 请求到达 + 业务预算；到期缺省 504（可覆写 onTimeout），流式开始自动取消 deadline
- **回收纪律**：Release() 后不得再碰 task；epoch++ 使队列中陈旧事件失效（防池复用后跨请求污染）

#### zm_net_req_loop_protocol — JRPC/RESTful 回复 helper（task 直通，子类独立文件）

| 类 | 说明 |
|----|------|
| `ZmReqLoopJrpc` | JRPC 回复子类：持 per-request 回复函数（服务器侧 replyCB 包装），`Response()` 内部走 TryReply 门 + task 直通 |
| `ZmReqLoopRest` | RESTful 回复子类：`ResponseJson`/`ResponseError`/`ResponseEmpty`/`ResponseRaw`/`ResponseRedirect`/`ResponseFile`、流式 `ResponseStreamStart`/`ResponseStreamChunk`/`ResponseStreamEnd`、SSE `ResponseSSEStart`/`ResponseSSEEvent`/`ResponseSSEEnd`；流式 helper 自动取消 deadline |

回调类型（数据仅回调期间有效）：
- `ZmReqLoopJrpcRequestCB = std::function<void(ZmReqLoop*, const char*)>` — JRPC 业务回调（reqData 为请求 JSON 字符串）
- `ZmReqLoopRestfulRequestCB = std::function<void(ZmReqLoop*, const BYTE*, size_t)>` — RESTful 业务回调（body 指向请求 evbuffer，回复前有效，勿拷贝后使用）

#### zm_net_broadcast — TCP 消息广播

基于 libevent + ZmEvBaseRunLoop 的 TCP 一对多消息推送模块，包含服务端与客户端。

| 文件 | 说明 |
|------|------|
| `zm_net_broadcast_base.h/.cpp` | 公共定义：状态枚举 `ZM_BROADCAST_STATE`、消息结构 `BcMessage`、客户端信息 `BcClientInfo`、帧协议编解码（4 字节大端长度 + JSON body）、UUID 生成 |
| `zm_net_broadcast_server.h/.cpp` | `ZmBroadcastServer`：TCP 广播服务端，支持监听、握手、心跳、Tag 过滤、立即/延时/定时发送、客户端管理 |
| `zm_net_broadcast_client.h/.cpp` | `ZmBroadcastClient`：TCP 广播客户端，支持连接/重连、握手、心跳响应、Tag 订阅、业务消息回调 |

**服务端特性：**
- 端口绑定失败无限重试、6 状态流转（IDLE→STARTING→LISTENING→STOPPING→STOPPED + ERROR）
- 同步停止 `Stop()`（投递停止任务并等待事件循环完成，1s 超时兜底）/ 异步停止 `AsyncStop()`（完成标志 = `GetState() == STOPPED`）
- 客户端握手：settings → confirm_settings → 分配 client_id（握手超时可配）
- 双向活动检测心跳（服务端主导 ping/pong，活跃通信时零心跳开销）
- Tag 过滤订阅/取消（subscribe/unsubscribe），仅推送给匹配客户端
- 消息格式：`{"id":"...","timestamp":"...","topic":"...","content":...}`，线程安全发送
- 每客户端独立消息队列（溢出丢弃最旧），连接数限制，按 client_id 踢出

**客户端特性：**
- 连接失败自动重试（1 秒间隔，无限次）
- 握手自动回执 + 初始 Tag 自动订阅
- 心跳自动响应（收到 ping 回 pong）
- 业务消息回调通过 ZmThreadPool 投递到业务线程
- 断线重连后自动恢复 Tag 订阅列表

**通信协议：**
```
握手:     服务端 → settings →  客户端
          客户端 → confirm_settings → 服务端
心跳:     服务端 → ping → 客户端
          客户端 → pong → 服务端
订阅:     客户端 → subscribe/unsubscribe → 服务端
业务:     服务端 → {id,topic,content} → 客户端
帧格式:   [4字节大端长度][JSON body]
```

**上层集成（ZiMoService）：**
- `BroadcastManager`：包装 `ZmBroadcastServer`，自管理事件循环线程
- `NetDock`：通过 `OpenBroadcastServer()`/`CloseBroadcastServer()` 管理生命周期
- `ServicePortal`：暴露 `BroadcastMessage()` 供 JRPC `broadcast` 方法和 RESTful `POST /broadcast` 共用
- 前端控制面板显示 Broadcast 运行状态（状态/端口/连接数/发送数）

### service — Windows 服务框架

| 类 | 说明 |
|----|------|
| `ZmServiceBase` | Windows 服务基类：SCM 注册/调度、状态报告、事件日志、会话/电源/关机事件回调 |
| `ZMServiceManager` | 服务管理工具：`Install`/`Uninstall`/`Start`/`Stop` |

特性：
- 支持 `Run()` 注册为 Windows 服务或 `RunDebugMode()` 前台调试
- 自动注册电源通知（`RegisterPowerSettingNotification`）
- 会话变更：登录/登出/锁屏/解锁/远程桌面连接
- 电源事件：睡眠/恢复/AC 切换/电池电量/电源设置变更
- 单例模式，静态回调自动转发到实例

### ssl — SSL/TLS 安全

#### zm_ssl_ctx — SSL 上下文管理

| 结构体/类 | 说明 |
|-----------|------|
| `ZM_X509_INFO` | X509 证书信息结构体（版本/有效期/序列号/颁发者/主题） |
| `ZmSSLContext` | SSL 上下文管理器：证书/密钥加载、PKCS12 解析、X509 解析、客户端 SSL_CTX 创建、ticket appdata 回调注册（预留能力） |
| `ZmMemoryBIO` | OpenSSL 内存 BIO 的 RAII 封装 |
| `ZmSessionTicketManager` | Session Ticket 密钥管理：加载/生成密钥文件、`Key()` 供服务器注入 |
| `ZmTicketKeyRotator` | Ticket 密钥定时轮换器：内部持有独立 `ZmEvBaseRunLoop` 线程，按间隔自动轮换（如 12 小时）并回调通知；多个 HTTPS 服务器可共享同一实例统一密钥 |

支持：
- PEM / PKCS12（PFX）格式证书加载
- 加密私钥加载（密码回调）
- 客户端双认证证书
- 国密 SM2 证书兼容
- 证书信息提取与日志输出
- SSL 指纹校验
- Session ticket 密钥注入/轮换（`ZmTicketKeyRotator::Init(ticketFile)` + `Start(interval, onRotate)`，旋转后调用 `ZmHttpServer::SetTicketKeys`）

#### zm_ssl_fingerprint — 证书指纹白名单

| 结构体/类 | 说明 |
|-----------|------|
| `ZM_SSL_FINGERPRINT` | 指纹记录：hostname + port + 最多 8 条 SHA1 指纹 |
| `ZmSSLFingerprint` | 指纹白名单管理器（单例）：注册/校验，支持 SNI、peer address 多种查找方式 |

### spdlog — 日志系统

#### zm_logger — 日志封装

| 类 | 说明 |
|----|------|
| `RotatingLoggerBase` | 滚动文件日志基类：可配置的文件大小、文件数、格式模式 |
| `DefaultLogger` | 默认日志管理器（logger_name = "DEFAULT"） |
| `PublicLogger` | 公共库日志管理器（logger_name = "PUBLIC"） |

日志宏（自动懒初始化）：

```cpp
// 默认日志
DEFAULT_LOG_TRACE(...)   DEFAULT_LOG_DEBUG(...)
DEFAULT_LOG_INFO(...)    DEFAULT_LOG_WARN(...)
DEFAULT_LOG_ERROR(...)   DEFAULT_LOG_CRITICAL(...)

// 公共库日志
PUBLIC_LOG_TRACE(...)    PUBLIC_LOG_DEBUG(...)
PUBLIC_LOG_INFO(...)     PUBLIC_LOG_WARN(...)
PUBLIC_LOG_ERROR(...)    PUBLIC_LOG_CRITICAL(...)
```

特性：
- 滚动文件输出（默认 10MB × 10 文件）
- 日志路径 `%ProgramData%\ZiMo\logs\<exe_name>.log`
- 支持宽字符串转 UTF-8
- 自定义格式：`[时间] [logger名] [进程ID] [线程ID] [级别] [源文件] [函数] [行号] 内容`

### util — 通用工具

| 文件 | 说明 |
|------|------|
| `zm_util_thread.h` | `ZmThread` — 基于 C++20 `std::jthread` 的线程封装（同步启停、状态管理、协作停止、AutoDelete）；`ZmThreadPool` — 线程池（立即/延迟执行、自动增长、任务取消） |
| `zm_util_str.h` | 字符串工具：`String` typedef（Unicode/ANSI 适配）、`ZmString` 工具类（编码转换、格式化、查找替换、Base64/Base32、URL 编解码、Hex 等）、`ZmStringList` 字符串列表、`zm_strndup`、`zm_strsep` |
| `zm_util_container.h` | 容器工具：动态数组（`ZmArrayList`）、字节缓冲区（`ZmByteBuffer`）、对象池（`ZmObjectPool`）、二进制表（`ZmBinaryTable`） |
| `zm_util_file.h` | `ZmFile` 文件 I/O 静态工具：读取（全量/分块回调/文本）、写入（覆盖/追加）、复制/重命名/删除/递归删目录、存在/目录判断/大小、路径解析（文件名/扩展名/目录/exe 目录）、`MD5HashHex` |
| `zm_util_libevent.h` | libevent 辅助：`ZmEventBuffer`/`ZmEventLine`（事件缓冲区读写）、`ZmSocketReuseType` 等 |
| `zm_util_sqlite.h` | SQLite 轻量封装：`ZmSqliteConn`（RAII 连接/事务辅助/错误日志）、`ZmSqliteStmt`（RAII 预处理语句）、`BindText`/`BindInt` bind 便捷 |
| `zm_util_sys.h` | 系统工具：`ZmSystem`（时间/错误信息/系统信息/路径/环境变量/进程）、`ZmSystemLoad`、`ZmSingleInstance`（命名互斥体单实例守卫）、DLL 动态加载 |
| `zm_util_win_os.h` | Windows 系统工具：`OS_VERSION`/`OSVersion()` 版本查询与系统版本判断、`POWER_OPTIONS` 电源操作 |
| `zm_util_zlib.h` | `ZipWriter` — 流式 zip 写入器（仅写入不解压）：raw deflate + CRC-32，按条目生成、`Drain()` 取走字节（内存 O(单条目)），`Finish()` 写 central directory + EOCD |

#### zm_util_sqlite — SQLite 轻量封装（zm 命名空间）

| 类 | 说明 |
|----|------|
| `ZmSqliteConn` | RAII 连接包装：`Open(path, logTag)`（READWRITE \| CREATE \| FULLMUTEX）、`Close`（幂等）、`Exec`（失败记日志）、`Begin`/`Commit`/`Rollback` 事务辅助（BEGIN IMMEDIATE）、`LastInsertRowId`/`Changes`/`ExtendedErrorCode`、`Raw()`、`Mutex()` |
| `ZmSqliteStmt` | RAII 预处理语句：`prepare_v2`，失败时 `p == nullptr` 由调用方判空 |
| `BindText`/`BindInt` | bind 便捷（SQLITE_TRANSIENT / sqlite3_bind_int64） |

线程安全契约（重要）：
- 每连接一个 `std::mutex`，经 `Mutex()` 暴露给调用方
- 类内方法一律不加锁，由调用方在多语句序列外持锁（`std::lock_guard lk(conn.Mutex())`）；MSVC 对非递归互斥量重复加锁抛异常，禁止类内加锁后调用方再加锁
- 不做行/列读取包装：业务代码直接经 `ZmSqliteStmt::p` 调用 `sqlite3_step`/`sqlite3_column_*`/`sqlite3_bind_*`，与裸 SQLite 等价
- 保持 SQLite 默认行为（不做 journal/WAL/busy_timeout 设置）

#### ZmThread 状态流转

```
STOPPED →(Start) STARTING → RUNNING →(Stop) STOPPING → STOPPED
```

两种使用模式：
- **callable 模式**：构造时传入函数对象
- **继承模式**：子类 override `Run()` / `Run(stop_token)`，重写 `OnStopping()` 打断阻塞调用

#### ZmThreadPool

- N 个 Worker 线程 + 1 个 Timer 线程
- 支持立即执行、延迟执行、`std::future` 异步获取结果
- `InvokeLater` / `InvokeCancel` 全局接口
- Worker 自动增长（上限 128）

#### ZipWriter — 流式 zip 写入

```cpp
ZipWriter w;
w.BeginEntry("a.txt", false);
w.Write(data, len); ...        // 可分块调用，内部边压边积
w.EndEntry();
w.BeginEntry("empty/", true);  // 空目录条目
w.EndEntry();
w.Finish();                    // 写 central directory + EOCD
// Data() / Drain(out) 取走字节（流式发送，内存 O(单条目)）
```

格式按 APPNOTE（Local File Header + raw deflate + Central Directory + EOCD），压缩用 zlib raw deflate（deflateInit2 windowBits=-15）+ crc32()，零新增第三方库。

### sqllite — SQLite 数据库

SQLite amalgamation 单文件源码（sqlite3.c / sqlite3.h / sqlite3ext.h / shell.c），由上层项目直接编译进工程，无需单独链接外部库。

使用方式：
- 编译时引入 `sqlite3.c`（可定义 `SQLITE_THREADSAFE=1` 等编译宏）
- 业务侧经 `util/zm_util_sqlite.h` 的 `ZmSqliteConn`/`ZmSqliteStmt` 封装使用
- ZiMoService 中的 `DbInitializer` 以声明式建表/补列，模块注入连接，见 Service 仓库

### libevent / openssl / zlib / libopus — 第三方预编译库

- **libevent**：事件驱动网络库，提供 `event_base`、`evhttp`、`evdns`、`bufferevent` 等
- **openssl**：SSL/TLS 加密库，提供 `SSL_CTX`、`BIO`、`X509` 等
- **zlib**：gzip/deflate 数据压缩库（zlib 1.3.1，`zlibstatic.lib`），HTTP 客户端响应自动解压、`ZipWriter` 压缩用
- **libopus**：Opus 音频编解码库，`opus.lib` 静态库，音频模块（如 ZiMoService 语音通话）使用

各库均以预编译头文件 + 静态库形式引入，无需单独编译。目录布局一致：

```
<lib>/
├── include/          # 头文件
└── lib/VC/x64/MT/    # 静态库（libopus 为 opus.lib，zlib 为 zlibstatic.lib）
```

注意：`zconf.h` 由 CMake 构建时生成（不在 zlib 源码包内），重新编译 zlib 后需同步复制 `zconf.h` 与 `Release\zlibstatic.lib` 到本目录。

## 依赖关系

```
net ────────→ ssl ──→ util
net ────────→ json ──→ util
net ────────→ libevent
net ────────→ zlib          (HTTP 客户端 gzip/deflate 自动解压)
ssl ────────→ openssl
service ────→ util
util ────────→ sqllite      (zm_util_sqlite → sqlite3.c)
util ────────→ zlib         (ZipWriter → raw deflate)
define ───── (无依赖)
libopus ───── (预编译库，无源码依赖)
```

net 模块内部依赖：
```
zm_net_runloop           → util（ZmThread）
zm_net_req_loop          → util（ZmThread），事件循环结构类似 ZmEvBaseRunLoop 但独立实现
zm_net_req_loop_protocol → zm_net_req_loop + json
zm_net_http              → zm_net_http_router + ssl（ticket 密钥）
zm_net_http_client       → zm_net_http_client_loop + ssl(ZmSSLContext) + zlib + openssl + libevent
zm_net_broadcast         → zm_net_runloop
```

## 构建与集成

ZiMoPublic 作为源码级公共库，由上层项目（如 ZiMoService）直接引用其头文件并链接预编译的 libevent/openssl/zlib/libopus 静态库。

上层项目的 vcxproj 中应配置：
- **附加包含目录**：`$(ProjectDir)..\ZiMoPublic\` 及其子目录
- **库目录**：`libevent`、`openssl`、`zlib`、`libopus` 的预编译 `.lib` 路径
- **强制包含**：`stdafx.h`（或对应的预编译头）
- **SQLite**：将 `sqllite\sqlite3.c` 加入编译（或预编译后链接 `sqlite3.lib`）

OpenSSL 以静态库（`libcrypto_static.lib` / `libssl_static.lib`）链接时：
- 需定义 `OPENSSL_STATIC` 预处理器宏（头文件声明改为非 dllimport）
- 额外链接 `crypt32.lib`（静态 OpenSSL 的 Windows 证书存储 winstore 依赖）

## 设计原则

- **声明与定义分离**：头文件只放声明，实现放在对应的 `.cpp` 文件中
- **成员变量命名**：`m_` 前缀（结构体除外），全局变量 `g_` 前缀
- **注释规范**：按 `@brief @param @return @example` 格式，中文注释，UTF-8 编码，LF 换行
- **代码组织**：public → protected → private，函数与成员变量分开
- **RAII 资源管理**：如 `ZmMemoryBIO`、`ZmWinSockHelper`、`ZmSqliteConn`、`RotatingLoggerBase`
- **单例模式**：全局服务（如 `ZmSSLFingerprint::instance()`、`DefaultLogger`）
- **回调模式**：`std::function` + 事件循环线程投递（如 `ZmReqLoop` 入口/续体回调、`ZmBroadcastClient` 消息回调、`ZmHttpServer` 的 `OnHttpdRequestCB`）
- **单事件循环线程内零锁**：跨线程操作一律经 `event_active`/`event_base_once` 投递到循环线程执行（如 `ZmHttpServer` 的 reply 控制事件、`PostSetTicketKeys`、`ZmReqLoopPool` 的 `PostToLoop`）

## 提交规范

```
feat: 新功能（feature）
用于提交新功能。
例如：feat: 增加用户注册功能

fix: 修复 bug
用于提交 bug 修复。
例如：fix: 修复登录页面崩溃的问题

docs: 文档变更
用于提交仅文档相关的修改。
例如：docs: 更新README文件

style: 代码风格变动（不影响代码逻辑）
用于提交仅格式化、标点符号、空白等不影响代码运行的变更。
例如：style: 删除多余的空行

refactor: 代码重构（既不是新增功能也不是修复bug的代码更改）
用于提交代码重构。
例如：refactor: 重构用户验证逻辑

perf: 性能优化
用于提交提升性能的代码修改。
例如：perf: 优化图片加载速度

test: 添加或修改测试
用于提交测试相关的内容。
例如：test: 增加用户模块的单元测试

chore: 杂项（构建过程或辅助工具的变动）
用于提交构建过程、辅助工具等相关的内容修改。
例如：chore: 更新依赖库

build: 构建系统或外部依赖项的变更
用于提交影响构建系统的更改。
例如：build: 升级webpack到版本5

ci: 持续集成配置的变更
用于提交CI配置文件和脚本的修改。
例如：ci: 修改GitHub Actions配置文件

revert: 回滚
用于提交回滚之前的提交。
例如：revert: 回滚feat: 增加用户注册功能
```

##包含规范

```
优先级由上到下
1. 尽量使用前向声明
2. 对应的头文件（foo.cpp → foo.h）
3. 本项目其他头文件
4. 第三方库头文件
5. 标准库头文件
```
