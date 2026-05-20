# ZmWebSocket Server 模块分析（AstraliserPublic 版本）

## 概述

基于 **libevent evhttp + evws** 的 **标准 WebSocket (RFC 6455)** 服务器模块。服务端通过 `evhttp` 接收 HTTP 请求，由 `evws_new_session` 自动完成 WebSocket Upgrade 握手，之后通过 `evws_connection` 进行全双工帧通信。支持连接数限制、心跳配置和运行统计。

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `zm_websocket_server.h/cpp` | ZmMessageWSServer（核心服务端）、ZmMessageServer（同步包装） |
| `zm_websocket_client.h/cpp` | ZmMessageWSClient（核心客户端）、ZmMessageClient（同步包装 + 观察者） |
| `zm_websocket_protocol.h/cpp` | WebSocket 帧编解码、握手请求构造与验证、SHA-1/Base64 |
| `zm_websocket_utils.h/cpp` | ZmMessageWSBase（基类）、状态管理、消息队列 |
| `zm_socket_utils.h/cpp` | 套接字工具函数（bind、create、connect 等） |

---

## 通信协议

使用 **标准 WebSocket 协议 (RFC 6455)**：

### 握手阶段

```
Client → Server:
  GET / HTTP/1.1
  Host: 127.0.0.1:port
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Key: <base64(16 random bytes)>
  Sec-WebSocket-Version: 13

Server → Client:
  HTTP/1.1 101 Switching Protocols
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Accept: <base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))>
```

### 数据帧格式 (RFC 6455 Section 5)

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-------+-+-------------+-------------------------------+
 |F|R|R|R| opcode|M| Payload len |    Extended payload length     |
 |I|S|S|S| (4)   |A|     (7)     |            (16/64)             |
 |N|V|V|V|       |S|             |                               |
 +-+-+-+-+-------+-+-------------+-------------------------------+
 |Extended payload length continued, if payload len == 127       |
 +-------------------------------+-------------------------------+
 |                               |Masking-key, if MASK set to 1  |
 +-------------------------------+-------------------------------+
 | Masking-key (continued)       |          Payload Data         |
 +-------------------------------+-------------------------------+
```

### 业务 Payload 格式

帧 payload 为 JSON，topic 经过 JSON 转义：

```json
{ "TOPIC" : "消息主题", "CONTENT" : { ... } }
```

---

## 类层次

```
ZmMessageWSBase              ← 状态管理 + 消息队列 + 控制事件抽象
  └─ ZmMessageWSServer       ← 服务端核心（evhttp + evws）
ZmMessageServer              ← 线程安全的同步包装层
```

---

## zm_socket_utils — 套接字工具层

定义于 `zm_socket_utils.h/cpp`，封装跨平台套接字操作。

### 函数清单

| 函数 | 说明 |
|------|------|
| `zm_util_fast_socket_closeonexec(fd)` | 设置 FD_CLOEXEC（Windows 空操作） |
| `zm_util_fast_socket_nonblocking(fd)` | 设置非阻塞模式 |
| `zm_util_socket(domain, type, protocol)` | 创建套接字，优先内核原子设置 NONBLOCK+CLOEXEC |
| `zm_util_create_bind_socket_nonblock(ai, reuse)` | 创建非阻塞套接字并绑定（服务端用） |
| `zm_util_create_socket_nonblock(addr/ai, port, reuse)` | 创建非阻塞套接字（客户端用，不 bind） |
| `zm_util_bind_socket(addr/sockaddr_in, port, reuse)` | 创建并绑定（地址复用策略可选） |
| `zm_util_make_addrinfo(address, port)` | 解析地址+端口为 evutil_addrinfo |
| `zm_util_be_socket_connect(be, address, port)` | 通过 bufferevent 发起 TCP 连接 |
| `zm_util_eventbase_init()` | 初始化 libevent 线程支持（Windows/pthreads） |
| `zm_util_get_socket_port(fd)` | 从已绑定 fd 获取实际端口号 |

### 地址复用策略

| 枚举值 | 含义 |
|--------|------|
| `ZM_SO_NOREUSE` | 不设置复用（使用 libevent 默认值） |
| `ZM_SO_REUSEADDR` | SO_REUSEADDR — 允许重用 TIME_WAIT 状态的地址 |
| `ZM_SO_REUSEPORT` | SO_REUSEPORT — 允许多进程同时绑定并监听（Linux 3.9+） |

---

## zm_websocket_protocol — 协议层

定义于 `zm_websocket_protocol.h/cpp`，实现 RFC 6455 核心协议逻辑。依赖 **OpenSSL**（SHA-1、RAND_bytes）。

**仅客户端使用**：服务端通过 evws 模块处理协议，不需要手动编解码帧。

### 帧编解码

| 函数 | 说明 |
|------|------|
| `zm_ws_encode_frame(payload, len, opcode, outLen, mask)` | 编码 WebSocket 帧。`mask=true` 时客户端发送需 mask；返回 malloc 分配的帧缓冲区 |
| `zm_ws_decode_frame(data, len, &opcode, &payloadLen, &consumed)` | 解码一帧。返回 malloc 分配的 payload；数据不足时返回 nullptr 且 consumed=0 |

Opcode 枚举：

| 值 | 含义 |
|----|------|
| `ZM_WS_OPCODE_TEXT (0x1)` | 文本帧 |
| `ZM_WS_OPCODE_BINARY (0x2)` | 二进制帧 |
| `ZM_WS_OPCODE_CLOSE (0x8)` | 关闭帧 |
| `ZM_WS_OPCODE_PING (0x9)` | Ping |
| `ZM_WS_OPCODE_PONG (0xA)` | Pong |

### 握手

| 函数 | 说明 |
|------|------|
| `zm_ws_generate_key()` | 生成 16 字节随机 key，返回 base64 编码 |
| `zm_ws_compute_accept_key(key)` | 按 RFC 6455 计算 `base64(sha1(key + magic))` |
| `zm_ws_build_handshake_request(host, port, path)` | 构造 HTTP Upgrade 请求字符串 |
| `zm_ws_validate_handshake_response(resp, len, expectedKey)` | 验证 101 响应 + Sec-WebSocket-Accept |

### 辅助函数

- `zm_base64_encode()` / `zm_base64_decode()` — 内部 Base64 实现
- `zm_sha1_hash()` — OpenSSL SHA-1 包装

---

## ZmMessageWSBase

所有通信实体的基类，定义于 `zm_websocket_utils.h/cpp`。

### 状态管理

使用位掩码叠加状态，线程安全（`std::atomic<WS_STATE>` + `recursive_mutex`）：

| 状态 | 值 | 含义 |
|------|----|------|
| STOPPED | 0 | 未运行 |
| RUNNING | 1 | 事件循环运行中 |
| CONNECTED | 2 | 有对端连接 |
| BOUND | 4 | 端口绑定成功 |

操作方法均加锁：

- `addState(s)` — 若尚未包含则叠加
- `removeState(s)` — 移除某个状态位
- `hasState(s)` — 检查是否包含
- `changeState(s)` — 直接设置目标状态

### 消息格式化

```
PostNotificationWithTopic(topic, content)
  → jsonEscapeString(topic)  ← JSON 转义：" → \"，\ → \\，控制字符 → \uXXXX
  → 拼接 JSON: { "TOPIC" : "<escaped>", "CONTENT" : <content> }
  → PostNotificationWithData(json, len)
    → calloc ZmMessageItem_t → malloc + memcpy payload → push 到 m_pendingMessages
    → 若 RUNNING | CONNECTED:
        event_active(m_ctrlEvent, ZM_WS_CONTROL_SEND)
```

- topic 经过 JSON 转义（`jsonEscapeString`），保证生成的 JSON 合法
- content 作为 JSON value 原样拼入，调用方负责提供合法 JSON
- 最大缓存 `m_maxPendingMessages`（默认 1000），超出时丢弃队头最旧消息
- `ClearAllCachedMessages()` 清空并释放所有 item

### 统计信息

```cpp
struct ZmServerStatistics {
    size_t total_connections;      // 历史总连接数
    size_t current_connections;    // 当前连接数
    size_t messages_sent;          // 发送消息数
    size_t messages_received;      // 接收消息数
    size_t messages_dropped;       // 丢弃消息数（字段已声明，代码中未递增）
    size_t ack_timeouts;           // ACK 超时数（字段已声明，代码中未递增）
    size_t heartbeat_timeouts;     // 心跳超时数（字段已声明，代码中未递增）
};
```

---

## ZmMessageWSServer

核心服务端，基于 `evhttp` + `evws`。

### 核心成员

| 成员 | 类型 | 用途 |
|------|------|------|
| `m_httpServer` | `evhttp*` | HTTP 服务器（处理 WebSocket Upgrade） |
| `m_clients` | `vector<ZmMessageWsClient_t*>` | 已连接客户端列表 |
| `m_maxConnections` | `size_t` | 最大连接数（0=不限制） |
| `m_nextClientId` | `atomic<uint32_t>` | 客户端 ID 递增计数器 |
| `m_heartbeatEnabled` | `bool` | 心跳开关（默认 false，定时器未实现） |
| `m_heartbeatIntervalMs` | `uint32_t` | 心跳间隔（默认 30000ms） |
| `m_heartbeatTimeoutMs` | `uint32_t` | 心跳超时（默认 10000ms） |
| `m_statistics` | `ZmServerStatistics_t` | 运行统计 |

客户端结构体 `ZmMessageWsClient_t`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `ws_conn` | `evws_connection*` | WebSocket 连接 |
| `server` | `void*` | 关联的服务端指针 |
| `client_id` | `uint32_t` | 唯一标识符 |
| `last_activity` | `time_t` | 最后活动时间 |

### 启动流程

```
Start()
 │
 ├─ hasState(RUNNING)? → 是则直接返回
 ├─ changeState(RUNNING)
 ├─ clearAll()
 ├─ zm_util_eventbase_init()
 │
 ├─ do {
 │   ├─ event_base_new()                            → 创建事件基
 │   ├─ evhttp_new(m_evbase)                        → 创建 HTTP 服务器
 │   ├─ evhttp_bind_socket_with_handle("0.0.0.0", port) → 绑定端口
 │   ├─ 若 port=0 → zm_util_get_socket_port 获取实际端口
 │   ├─ evhttp_set_gencb(onHttpRequest, this)       → 设置 HTTP 请求回调
 │   └─ event_new(m_ctrlEvent, fd=-1, EV_PERSIST|EV_READ) → 控制事件（无 fd，手动触发）
 │   } while(false)
 │
 ├─ 成功:
 │   ├─ event_active(ZM_WS_CONTROL_BOUND) → 通知绑定完成
 │   └─ event_base_dispatch()             → 进入事件循环（阻塞）
 │       └─ 退出后:
 │           ├─ evhttp_free(m_httpServer) → 同时释放关联的 event_base
 │           ├─ m_port = 0
 │           ├─ closeDoneCallback()
 │           └─ changeState(STOPPED)
 │
 └─ 失败:
     ├─ clearAll()
     └─ bindErrorCallback()
```

### 接受客户端连接

`onHttpRequest(req, arg)` — 静态方法，由 evhttp 通用回调触发：

```
1. 检查连接数限制 (m_maxConnections > 0 && m_clients.size() >= m_maxConnections)
2. evws_new_session(req, onWsMessage, self, 0) → 自动完成 Upgrade 握手
3. evws_connection_set_closecb(wsConn, onWsClose, self) → 设置关闭回调
4. calloc ZmMessageWsClient_t → 设置 client_id / last_activity
5. push 到 m_clients
6. m_statistics.total_connections++
7. addState(CONNECTED)
8. event_active(ZM_WS_CONTROL_SEND) → 触发缓存消息发送
```

### WebSocket 消息处理

`onWsMessage(wsConn, type, data, len, arg)` — 静态方法：

```
1. findClientByConn(wsConn) → 更新 last_activity
2. 若 TEXT/BINARY 帧 → m_statistics.messages_received++
3. Ping/Pong 由 evws 库自动处理（不会到达此回调）
```

当前服务端不解析客户端消息的业务内容。

### 消息发送

`onControlSend()` — 由 `ZM_WS_CONTROL_SEND` 事件触发：

```
1. 检查 CONNECTED 状态
2. while (!m_pendingMessages.empty()):
   ├─ 取队头消息
   ├─ 遍历 m_clients:
   │   ├─ 有效连接 → evws_send_binary(client->ws_conn, data, len)
   │   └─ 无效连接 → evws_connection_free + free + 从列表移除
   ├─ 释放消息内存
   └─ m_statistics.messages_sent++
3. 若队列还有消息 → event_active(ZM_WS_CONTROL_SEND) 继续发送
```

### 客户端断开

`onWsClose(wsConn, arg)` — 静态方法：

```
1. 在 m_clients 中查找匹配的 client（通过 ws_conn 指针）
2. 从 m_clients 移除
3. evws_connection_free(client->ws_conn)
4. free(client)
5. 若 m_clients 为空 → removeState(CONNECTED)
```

### 停止流程

```
Stop()
 ├─ 若 RUNNING | BOUND:
 │   └─ event_active(ZM_WS_CONTROL_CLOSE)
 │       → onControlClose()
 │         ├─ clearAllClients()
 │         ├─ ClearAllCachedMessages()
 │         ├─ event_free(m_ctrlEvent)
 │         └─ event_base_loopbreak()
 └─ 否则:
     ├─ clearAll()
     └─ closeDoneCallback()
```

### 配置接口

| 方法 | 说明 |
|------|------|
| `SetMaxConnections(n)` | 最大连接数，0 不限制 |
| `SetHeartbeatEnabled(bool)` | 启停心跳（定时器未实现） |
| `SetHeartbeatInterval(ms)` | 心跳间隔，默认 30000ms |
| `SetHeartbeatTimeout(ms)` | 心跳超时，默认 10000ms |
| `GetStatistics()` | 获取统计数据快照 |
| `ResetStatistics()` | 重置统计 |

---

## ZmMessageServer

线程安全的同步包装层，将阻塞的 `ZmMessageWSServer` 封装为可同步调用的接口。

### 核心设计

- `ZmMessageWSServer` 运行在独立线程（`std::thread` + `detach`）
- 通过 `condition_variable` 实现 Start/Stop 同步等待
- 回调中先拷贝 callback 再释放锁后调用，减少锁持有时间

### 三把锁的协作

| 锁 | 用途 |
|----|------|
| `m_mutex` | 保护 `m_wsServer` 指针、回调指针、配置参数 |
| `m_semaphoreMutex` | `m_semaphore` 的配套 mutex |
| `m_syncMutex` | 防止 Start 返回后回调仍在执行（回调中先 notify 再 lock m_syncMutex） |

### 回调链

```
ZmMessageWSServer 事件
  → ZmMessageServer::onXxxCallback()
    ├─ m_wsServer = nullptr（错误/关闭时）
    ├─ m_semaphore.notify_all()
    ├─ 释放 m_syncMutex
    ├─ 拷贝用户回调到局部变量
    └─ 释放 m_mutex 后调用用户回调
```

所有代理方法（`PostNotificationWithTopic`、`SetXxx` 等）均加锁保护，并检查 `m_wsServer != nullptr`。

---

## 线程安全

| 组件 | 锁 | 说明 |
|------|----|------|
| ZmMessageWSBase | `recursive_mutex` | 状态管理和消息队列 |
| ZmMessageWSServer | 继承 Base 的锁 | 所有回调在 evhttp 线程中串行 |
| ZmMessageServer | `mutex` × 3 | m_mutex 保护成员；m_semaphoreMutex 条件变量；m_syncMutex 回调同步 |

---

## 已修复的问题

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 1 | `zm_websocket_utils.cpp` | `PostNotificationWithTopic` 中 topic 未转义，特殊字符破坏 JSON | 新增 `jsonEscapeString` 函数，topic 经过 JSON 转义后拼入 |
| 2 | `zm_socket_utils.cpp` | `zm_util_be_socket_connect` 成功时未释放 `aitop`，内存泄漏 | 改为无条件释放 `aitop` |
| 3 | `zm_socket_utils.cpp` | `zm_util_eventbase_init` 中 `evthread_use_pthreads` 被错误放在 `#ifdef _WIN32` 内，Linux 下不调用 | 移除多余的 `#ifdef _WIN32` 嵌套 |
| 4 | `zm_socket_utils.cpp` | `zm_util_bind_socket(const sockaddr_in*)` 中 `address_4`/`address_6` 为 if 块内局部变量，出作用域后 `address` 为悬空指针 | 统一使用外层 `address_buf[INET6_ADDRSTRLEN]` |
| 5 | `zm_socket_utils.h` | MinGW 条件分支内有多余字符 `1` | 已删除 |

---

## 已知问题

1. **心跳未实际实现**：`m_heartbeatEnabled` / `m_heartbeatIntervalMs` / `m_heartbeatTimeoutMs` 成员已声明但服务端未使用定时器检测超时
2. **服务端不处理客户端消息**：`onWsMessage` 仅更新统计，不解析业务内容
3. **统计中 dropped / ack_timeouts / heartbeat_timeouts 未更新**：声明了字段但没有在代码中递增
4. **evhttp_free 与 event_base 生命周期**：`evhttp_free` 会释放关联的 `event_base`，析构函数中需注意不再重复释放
