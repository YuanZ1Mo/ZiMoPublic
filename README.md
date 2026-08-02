# ZiMoPublic

ZiMo 生态的 C++ 公共基础库，为 ZiMoService 及其他上层项目提供网络通信、Windows 服务框架、SSL/TLS 安全、日志、JSON 处理、线程工具等通用能力。

## 模块总览

```
ZiMoPublic/
├── define/          # 通用宏定义、版本号
├── json/            # nlohmann/json 封装（类型安全的读写辅助）
├── libevent/        # 预编译 libevent 头文件及静态库（事件驱动网络库）
├── net/             # 网络通信模块（TCP/HTTP/RESTful/DNS/ZmReqLoop 请求调度/路由中间件/消息广播/SSE）
├── openssl/         # 预编译 OpenSSL 头文件及静态库
├── service/         # Windows 服务基类（SCM 集成、安装/卸载）
├── spdlog/          # 定制版 spdlog 日志库 + zm_logger 封装
├── ssl/             # SSL/TLS 上下文管理、证书指纹校验
├── util/            # 通用工具（线程、字符串、文件、容器、加密、系统）
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
| `ZmHttpdTask` | HTTP 请求上下文：读取 URI/方法/请求头，写入响应状态码/头/体，支持流式分块回复 |
| `ZmHttpHead` | HTTP 头部封装：解析、构建、键值查询 |
| `ZmHttpServer` | 多线程 HTTP 服务器：每个请求分配独立 Worker 线程处理，不阻塞事件循环 |
| `ZmJsonRpcServer` | JSON-RPC 2.0 服务器：协议解析与分发，支持 GET（Base64）/POST、JSONP 回调 |
| `ZmRESTfulServer` | ★ RESTful HTTP 服务器：继承 ZmHttpServer，支持异步回调 + `TriggerReply()` 直通响应 |

关键设计：
- **线程模型**：事件循环线程接收请求 → Worker 线程处理 → event_active 通知事件循环线程发送响应
- **CORS 支持**：自动添加跨域响应头
- **即时释放**：Worker 资源即时释放（池化复用），减少内存占用
- **SSE 推送**：支持 `StartStreamReply`/`SendReplyChunk`/`EndStreamReply` 流式分块响应（text/event-stream）
- **请求头透传**：Worker 线程可通过 `ZmHttpdTask` 读写请求头/响应头，支持业务侧自定义头部

#### zm_net_http_router — HTTP 路由中间件

| 类 | 说明 |
|----|------|
| `ZmHttpRouter` | Express 风格的路由器：`Get`/`Post`/`Put`/`Delete`/`Any` 注册，前缀树匹配 |
| `ZmHttpMiddlewareLogging` | 日志中间件：记录请求方法、路径、耗时 |
| `ZmHttpMiddlewareRecovery` | 异常恢复中间件：捕获 handler 异常返回 500 |

路由模式：`(task, next)` 函数管道，支持 `*` 通配符兜底路由。

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
| `ZmSSLContext` | SSL 上下文管理器：证书/密钥加载、PKCS12 解析、X509 解析、客户端 SSL_CTX 创建 |
| `ZmMemoryBIO` | OpenSSL 内存 BIO 的 RAII 封装 |

支持：
- PEM / PKCS12（PFX）格式证书加载
- 加密私钥加载（密码回调）
- 客户端双认证证书
- 国密 SM2 证书兼容
- 证书信息提取与日志输出
- SSL 指纹校验

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
| `zm_util_str.h` | 字符串工具：Unicode/ANSI 适配（`String` typedef）、编码转换（UTF8_To_Unicode/Unicode_To_UTF8 含 std::string/std::wstring 重载）、URL 编解码（URLDecode 含 std::string 重载）、`zm_strndup`、`zm_strsep` |
| `zm_util_container.h` | 容器工具：动态数组（`ZmArrayList`）、字节缓冲区（`ZmByteBuffer`）、字符串列表（`ZmStringList`） |
| `zm_util_crypto.h` | 加密工具 |
| `zm_util_file.h` | 文件 I/O 工具：`Read`/`Write`/`ReadString`/`ReadEx`、`Copy`/`Rename`/`Delete`/`DeleteDir`、`Exists`/`IsDirectory`/`GetSize`、`MakeDirs`、`MD5HashHex` |
| `zm_util_libevent.h` | libevent 辅助函数 |
| `zm_util_sys.h` | 系统工具 |
| `zm_util_win_os.h` | Windows 操作系统相关工具 |

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

### libevent / openssl — 第三方预编译库

- **libevent**：事件驱动网络库，提供 `event_base`、`evhttp`、`evdns`、`bufferevent` 等
- **openssl**：SSL/TLS 加密库，提供 `SSL_CTX`、`BIO`、`X509` 等

两个库均以预编译头文件 + 静态库形式引入，无需单独编译。

## 依赖关系

```
net ────────→ ssl ──→ util
net ────────→ json ──→ util
net ────────→ libevent
ssl ────────→ openssl
service ────→ util
define ───── (无依赖)
```

net 模块内部依赖：
```
zm_net_req_loop          → zm_net_runloop + util（ZmThread）
zm_net_req_loop_protocol → zm_net_req_loop + json
zm_net_http         → zm_net_runloop + zm_net_http_router

## 构建与集成

ZiMoPublic 作为源码级公共库，由上层项目（如 ZiMoService）直接引用其头文件并链接预编译的 libevent/openssl 静态库。

上层项目的 vcxproj 中应配置：
- **附加包含目录**：`$(ProjectDir)..\ZiMoPublic\` 及其子目录
- **库目录**：`libevent` 和 `openssl` 的预编译 `.lib` 路径
- **强制包含**：`stdafx.h`（或对应的预编译头）

## 设计原则

- **声明与定义分离**：头文件只放声明，实现放在对应的 `.cpp` 文件中
- **成员变量命名**：`m_` 前缀（结构体除外），全局变量 `g_` 前缀
- **注释规范**：按 `@brief @param @return @example` 格式，中文注释，UTF-8 编码，LF 换行
- **代码组织**：public → protected → private，函数与成员变量分开
- **RAII 资源管理**：如 `ZmMemoryBIO`、`ZmWinSockHelper`、`RotatingLoggerBase`
- **单例模式**：全局服务（如 `ZmSSLFingerprint::instance()`、`DefaultLogger`）
- **回调模式**：`std::function` + 模板成员函数绑定（如 `ZmMessageServer::SetBindDoneCallback`）

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