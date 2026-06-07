# ZiMoPublic

ZiMo 生态的 C++ 公共基础库，为 ZiMoService 及其他上层项目提供网络通信、Windows 服务框架、SSL/TLS 安全、WebSocket、日志、JSON 处理、线程工具等通用能力。

## 模块总览

```
ZiMoPublic/
├── define/          # 通用宏定义、版本号
├── json/            # nlohmann/json 封装（类型安全的读写辅助）
├── libevent/        # 预编译 libevent 头文件及静态库（事件驱动网络库）
├── net/             # 网络通信模块（TCP/HTTP/DNS/TAP 代理链）
├── openssl/         # 预编译 OpenSSL 头文件及静态库
├── service/         # Windows 服务基类（SCM 集成、安装/卸载）
├── spdlog/          # 定制版 spdlog 日志库 + zm_logger 封装
├── ssl/             # SSL/TLS 上下文管理、证书指纹校验
├── util/            # 通用工具（线程、字符串、文件、容器、加密、系统）
└── websocket/       # WebSocket 服务端/客户端实现
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

#### zm_net_http — HTTP / JSON-RPC 服务端

| 类 | 说明 |
|----|------|
| `ZmHttpUtil` | HTTP 工具：动词解析、请求解析、URI 解析、Query 参数提取 |
| `ZmHttpdTask` | HTTP 请求上下文：读取 URI/方法/请求头，写入响应状态码/头/体 |
| `ZmHttpHead` | HTTP 头部封装：解析、构建、键值查询 |
| `ZmHttpServer` | 多线程 HTTP 服务器：每个请求分配独立 Worker 线程处理，不阻塞事件循环 |
| `ZmJsonRpcServer` | JSON-RPC 2.0 服务器：协议解析与分发，支持 GET（Base64）/POST、JSONP 回调 |

关键设计：
- **线程模型**：事件循环线程接收请求 → Worker 线程处理 → event_active 通知事件循环线程发送响应
- **CORS 支持**：自动添加跨域响应头
- **延迟释放**：响应发送后 1 秒定时器释放 Worker 资源

#### zm_net_tap — TAP 代理框架

| 结构体/类 | 说明 |
|-----------|------|
| `ZM_TAP_CTX` | TAP 上下文结构体，代表一个网络连接会话的完整状态（含缓冲区、DNS、超时、回传链） |
| `ZM_TAP_SLOT` | TAP 池槽位，含回指指针保证扩容时引用稳定 |
| `ZmTapContext` | TAP 对象池：O(1) 获取/回收、自动扩容、原子序号生成 |
| `ZmTapDelegate` | TAP 协议委托基类：定义读写/连接/错误/DNS/回传等虚函数回调 |
| `ZmTapContextEventHandler` | 静态回调分发器，将 libevent C 回调桥接到 C++ 虚函数 |

关键设计：
- **对象池**：空闲栈复用，O(1) Get/Drop
- **代理链**：最多 4 层的回传代理链（LIFO），响应依次经过各 delegate 处理
- **协议探测**：首包到达时识别协议魔数，动态切换到对应 delegate
- **柔性数组 TLV**：`ZM_EXT_TLV_HEAD` 使用 MSVC 柔性数组扩展

#### zm_net_tap_hub — Hub 代理路由

| 类 | 说明 |
|----|------|
| `ZmTapHubBase` | Hub 基类：提供 IPv4/IPv6 双栈 evconnlistener 监听管理 |
| `ZmTapHubProxy` | Hub 代理：协议探测 + delegate 动态切换，支持多协议前端共享路由 |

工作流程：`Accept → 首包协议探测 → 匹配 delegate → 切换回调 → 后续数据由 delegate 处理`

#### zm_net_tap_jrpc — JRPC 协议委托

| 类 | 说明 |
|----|------|
| `ZmTapDelegateJRPC` | JRPC 协议委托：解析长度前缀帧格式（4 字节大端长度 + JSON 体），通过回调通知上层 |

回调类型：`TapDelegateJrpcRequestReadCB = std::function<void(ZM_TAP_CTX*, const char*)>`

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

### websocket — WebSocket 实现

#### zm_websocket_utils — 公共基类

| 类型 | 说明 |
|------|------|
| `ZmMessageItem` | 消息载荷（堆分配内存 + 长度） |
| `ZM_WS_CONTROL` | 位掩码控制事件：SEND / BOUND / CONNECTED / CLOSE |
| `ZmMessageWSBase` | WebSocket 通信基类：状态管理 + 消息队列 + 控制事件抽象 |

#### zm_websocket_server — 服务端

| 类 | 说明 |
|----|------|
| `ZmMessageWSServer` | WebSocket 核心服务端：基于 libevent evhttp + evws，运行在独立事件循环线程 |
| `ZmMessageServer` | 线程安全的同步包装层：`Start`/`Stop` 阻塞等待，`PostNotificationWithTopic` 广播 |

特性：
- 自动 WebSocket Upgrade 握手（evws_new_session）
- 连接数限制、心跳配置（预留）、统计信息
- 消息队列缓存 + 批量广播
- 双回调注册方式（函数指针 / 成员函数模板）

#### zm_websocket_client / zm_websocket_protocol

WebSocket 客户端实现及底层协议帧编解码。

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
| `zm_util_str.h` | 字符串工具：Unicode/ANSI 适配（`String` typedef）、`zm_strndup`、`zm_strsep`、十六进制字符判断 |
| `zm_util_container.h` | 容器工具：动态数组（`ZmArrayList`）、字节缓冲区（`ZmByteBuffer`）、字符串列表（`ZmStringList`） |
| `zm_util_crypto.h` | 加密工具 |
| `zm_util_file.h` | 文件 I/O 工具 |
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

- **libevent**：事件驱动网络库，提供 `event_base`、`evhttp`、`evdns`、`evws`（WebSocket）、`bufferevent` 等
- **openssl**：SSL/TLS 加密库，提供 `SSL_CTX`、`BIO`、`X509` 等

两个库均以预编译头文件 + 静态库形式引入，无需单独编译。

## 依赖关系

```
websocket ──→ util ──→ spdlog
net ────────→ ssl ──→ util
net ────────→ json ──→ util
net ────────→ libevent
ssl ────────→ openssl
service ────→ util
define ───── (无依赖)
```

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
