# ZmWebSocket Client 模块分析（AstraliserPublic 版本）

## 概述

基于 **libevent bufferevent** 的标准 **WebSocket (RFC 6455)** 客户端模块。客户端通过 TCP 连接后手动发起 HTTP Upgrade 握手，验证通过后切换到 WebSocket 帧通信模式。支持 Ping/Pong 自动响应、观察者模式消息分发和可配置的自动重连。

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `zm_websocket_client.h/cpp` | ZmMessageWSClient（核心客户端）、ZmMessageClient（同步包装 + 观察者） |
| `zm_websocket_protocol.h/cpp` | WebSocket 帧编解码、握手构造与验证（SHA-1/Base64） |
| `zm_websocket_utils.h/cpp` | ZmMessageWSBase（基类）、状态管理、消息队列、JSON 转义 |
| `zm_socket_utils.h/cpp` | 套接字工具函数（跨平台 bind/create/connect，libevent 线程初始化） |

---

## 类层次

```
ZmMessageWSBase                ← 状态管理 + 消息队列 + 控制事件抽象
  └─ ZmMessageWSClient         ← 客户端核心（握手 + 帧通信）
ZmMessageClient                ← 线程安全同步包装 + 观察者模式 + 自动重连
```

---

## ZmMessageWSClient

核心客户端，实现 WebSocket 握手和帧通信。

### 内部状态机

```cpp
enum WS_CLIENT_STATE {
    WS_CLIENT_HANDSHAKE,    // TCP 已连接，等待握手完成
    WS_CLIENT_CONNECTED     // WebSocket 握手完成，帧通信模式
};
```

### 核心成员

| 成员 | 类型 | 用途 |
|------|------|------|
| `m_connectBev` | `bufferevent*` | 与服务端通信的 bufferevent |
| `m_wsState` | `WS_CLIENT_STATE` | 当前内部状态（HANDSHAKE / CONNECTED） |
| `m_handshakeBuffer` | `string` | 累积握手响应（HTTP 响应可能分多次到达） |
| `m_wsKey` | `string` | 客户端生成的 Sec-WebSocket-Key |
| `m_frameBuffer` | `vector<char>` | 累积帧数据（处理帧拆包） |
| `m_heartbeatEnabled` | `bool` | 心跳开关（默认 true） |
| `m_heartbeatIntervalMs` | `uint32_t` | 心跳间隔（默认 30000ms） |

### 连接流程

```
Connect(address, port)
 │
 ├─ hasState(RUNNING)? → 是则直接返回
 ├─ changeState(RUNNING)
 ├─ clearEvent()
 ├─ 清空 m_handshakeBuffer / m_frameBuffer
 ├─ m_wsState = WS_CLIENT_HANDSHAKE
 ├─ zm_util_eventbase_init()
 │
 ├─ do {
 │   ├─ event_base_new()
 │   ├─ zm_util_create_socket_nonblock(address, port) → 非阻塞套接字
 │   ├─ bufferevent_socket_new(evbase, fd, BEV_OPT_CLOSE_ON_FREE)
 │   ├─ bufferevent_enable(EV_READ | EV_WRITE)
 │   ├─ bufferevent_set_timeouts()
 │   ├─ bufferevent_setcb(read/write/event, this)
 │   │   └─ read 回调：从 input 读取数据 → onBufferedRead(data, len)
 │   └─ zm_util_be_socket_connect(bev, address, port) → 发起 TCP 连接
 │   } while(false)
 │
 ├─ 成功:
 │   ├─ event_new(m_ctrlEvent) → 控制事件
 │   ├─ event_add(m_ctrlEvent)
 │   └─ event_base_dispatch()  → 进入事件循环（阻塞）
 │       │
 │       └─ 事件循环内部流程:
 │           ├─ TCP 连接成功 → BEV_EVENT_CONNECTED:
 │           │   └─ 发送 HTTP Upgrade 握手请求
 │           ├─ 收到握手响应 → processHandshake():
 │           │   └─ 验证 101 + Sec-WebSocket-Accept → WS_CLIENT_CONNECTED
 │           └─ 帧通信 → processFrameData():
 │               └─ zm_ws_decode_frame → 处理 TEXT/BINARY/PING/CLOSE
 │
 └─ 退出循环后:
     ├─ m_port = 0
     ├─ closeDoneCallback()
     └─ changeState(STOPPED)
```

### 握手过程

**发送握手请求**（`onBufferedEvent` 收到 `BEV_EVENT_CONNECTED` 时）：

```
1. zm_ws_generate_key() → 生成 Sec-WebSocket-Key，保存到 m_wsKey
2. 构造 HTTP Upgrade 请求:
   GET / HTTP/1.1
   Host: 127.0.0.1:port
   Upgrade: websocket
   Connection: Upgrade
   Sec-WebSocket-Key: <key>
   Sec-WebSocket-Version: 13
3. evbuffer_add + bufferevent_flush 发送请求
```

**验证握手响应**（`processHandshake`）：

```
1. 查找 "\r\n\r\n" 确定 HTTP 响应头结束
2. zm_ws_validate_handshake_response(data, len, m_wsKey)
   ├─ 检查 HTTP 101
   ├─ 检查 Upgrade: websocket
   └─ 比对 Sec-WebSocket-Accept == zm_ws_compute_accept_key(m_wsKey)
3. 成功 → m_wsState = WS_CLIENT_CONNECTED
4. 处理可能的额外数据（响应头之后的帧数据）
5. addState(ZM_WS_STATE_CONNECTED)
6. event_active(ZM_WS_CONTROL_CONNECTED)
```

### 帧数据处理

`processFrameData(data, len)` — 在 `WS_CLIENT_CONNECTED` 状态下处理接收到的数据：

```
1. 数据追加到 m_frameBuffer
2. while (totalConsumed < len):
   ├─ zm_ws_decode_frame() → 解析一帧
   │   ├─ consumedBytes == 0 → 数据不足，等待更多
   │   └─ consumedBytes > 0 → 处理该帧
   ├─ switch (opcode):
   │   ├─ ZM_WS_OPCODE_TEXT / ZM_WS_OPCODE_BINARY:
   │   │   └─ json11 解析 → 提取 TOPIC + CONTENT → readCallback
   │   ├─ ZM_WS_OPCODE_PING:
   │   │   └─ zm_ws_encode_frame(PONG, mask=true) → 自动回复 Pong
   │   ├─ ZM_WS_OPCODE_PONG:
   │   │   └─ 更新心跳状态（预留）
   │   └─ ZM_WS_OPCODE_CLOSE:
   │       └─ event_active(ZM_WS_CONTROL_CLOSE)
   └─ free(payload)
3. m_frameBuffer.erase(已消费部分)
```

### 消息发送

`doSendAction()` — 由 `ZM_WS_CONTROL_SEND` 事件触发：

```
1. 检查 m_wsState == WS_CLIENT_CONNECTED
2. while (!m_pendingMessages.empty()):
   ├─ 取队头消息
   ├─ zm_ws_encode_frame(data, len, ZM_WS_OPCODE_TEXT, &frameLen, mask=true)
   │   └─ 客户端发送必须 mask
   ├─ evbuffer_add(output, frame, frameLen)
   ├─ bufferevent_flush()
   ├─ free(frame) + free(message_item)
   └─ 编码失败 → 消息放回队列头部，退出
3. 若队列非空 → event_active(ZM_WS_CONTROL_SEND) 继续
```

### IsConnected

```cpp
bool IsConnected() {
    return m_wsState == WS_CLIENT_CONNECTED && hasState(ZM_WS_STATE_CONNECTED);
}
```

双重条件：内部状态必须是 `WS_CLIENT_CONNECTED`，且 WSBase 状态包含 `ZM_WS_STATE_CONNECTED`。

### 停止流程

```
Stop()
 ├─ 若 RUNNING | CONNECTED:
 │   └─ event_active(ZM_WS_CONTROL_CLOSE)
 │       → onControlClose()
 │         ├─ ClearAllCachedMessages()
 │         ├─ bufferevent_free(m_connectBev)
 │         ├─ event_free(m_ctrlEvent)
 │         └─ event_base_loopbreak()
 └─ 否则:
     ├─ clearAll()
     └─ closeDoneCallback()（加锁拷贝后调用）
```

---

## ZmMessageClient

线程安全的同步包装层，增加观察者模式和自动重连。

### 核心成员

| 成员 | 用途 |
|------|------|
| `m_wsClient` | ZmMessageWSClient 实例指针 |
| `m_observerList` | 观察者集合（unordered_set，自定义哈希和相等比较） |
| `m_host` / `m_port` | 服务端地址 |
| `m_reconnectTimeout` | 重连延迟（ms），负数则不重连 |
| `m_semaphore` | 条件变量，同步 Connect/Stop |
| `m_mutex` / `m_semaphoreMutex` / `m_syncMutex` | 三把互斥锁 |

### 观察者模式

```cpp
struct ZmMessageReceiverObserver {
    const void* observer;              // 观察者对象指针（唯一标识）
    ZmReceiverMessageCallback method;    // std::function<void(const char*, const char*, void*)>
    long identifier;                   // 哈希标识 = reinterpret_cast<long>(observer)
};
```

**线程安全改进**（相比 Public 版本）：`onReadCallback` 在调用前先拷贝观察者列表到局部 vector，释放锁后再逐一调用，避免了回调中操作观察者列表导致的迭代器失效：

```
onReadCallback(topic, content)
  → lock → 拷贝 observers 到 observersCopy → unlock
  → 遍历 observersCopy → 调用 method(topic, content, observer)
```

### 连接同步

```
Connect(address, port)
 ├─ lock(m_mutex)
 ├─ new ZmMessageWSClient()（try-catch 保护）
 ├─ 配置 4 个回调 + 超时 + 缓存
 ├─ lock(m_syncMutex)
 ├─ unlock(m_mutex)
 ├─ std::thread(workThread) → client->Connect() → delete client
 ├─ thread.detach()
 └─ m_semaphore.wait_for(30秒, 直到 client->IsConnected())
     └─ 使用 wait_for 替代 wait（超时保护，默认 30 秒）
```

### 自动重连

```
reconnect()
  → lock → 拷贝 reconnectTimeout / host / port → unlock
  → 若 reconnectTimeout >= 0 && m_wsClient == nullptr && host 非空:
      sleep(reconnectTimeout ms)
      Connect(host, port)
```

- 所有参数先拷贝到局部变量，再释放锁后操作
- `reconnectTimeout < 0` 不重连
- `m_wsClient != nullptr` 时不重连（可能已被其他路径重建）

### 回调链

```
ZmMessageWSClient 事件
  → ZmMessageClient::onXxxCallback()
    ├─ lock(m_mutex)
    ├─ m_wsClient = nullptr（错误/关闭时）
    ├─ m_semaphore.notify_all()
    ├─ unlock(m_mutex)
    ├─ 释放 m_syncMutex
    ├─ lock(m_mutex) → 拷贝用户回调到局部变量 → unlock
    └─ 调用用户回调
```

所有回调先拷贝到局部变量再释放锁后调用，减少锁持有时间。

---

## 服务端 vs 客户端对比

| 维度 | Server | Client |
|------|--------|--------|
| 通信方式 | evhttp + evws（库处理帧编解码） | bufferevent + 手动帧编解码 |
| 握手 | evws_new_session 自动处理 | 手动构造/验证 HTTP Upgrade |
| 消息发送 | evws_send（库自动编码） | zm_ws_encode_frame(mask=true) + evbuffer_add |
| 消息接收 | onWsMessage 回调（库已解码） | zm_ws_decode_frame 手动解码 |
| 入站消息处理 | 仅更新统计 | JSON 解析 → 观察者分发 |
| Ping/Pong | evws 库自动处理 | 手动编码 Pong 回复 |
| 连接数限制 | 有（SetMaxConnections） | 无 |
| 统计 | 有（ZmServerStatistics） | 无 |
| 心跳 | 配置项（未实际实现定时器） | 配置项（未实际实现定时器） |

---

## 线程安全

| 组件 | 锁 | 说明 |
|------|----|------|
| ZmMessageWSBase | `recursive_mutex` | 状态管理、消息队列 |
| ZmMessageWSClient | 继承 Base 的锁 | 事件在 event_base 线程串行 |
| ZmMessageClient | `mutex` × 3 | m_mutex 保护成员；m_semaphoreMutex 条件变量；m_syncMutex 回调同步 |

### 相比 Public 版本的改进

1. **观察者遍历安全**：拷贝观察者列表后释放锁再调用，避免迭代器失效
2. **回调调用安全**：所有回调先拷贝到局部变量，释放锁后再调用
3. **连接超时**：使用 `wait_for` 替代无限 `wait`，默认 30 秒超时
4. **null 检查**：AddReceiverObserver / RemoveReceiverObserver 增加 null 参数检查
5. **内存分配保护**：new 使用 try-catch 保护
6. **reconnect 安全**：先拷贝参数到局部变量再操作
7. **Stop 安全**：先拷贝 wsClient 指针再操作，避免锁内调用 Stop

---

## 已知问题

1. **心跳未实际实现**：`m_heartbeatEnabled` / `m_heartbeatIntervalMs` 已声明但未实现 Ping 发送定时器和超时检测
2. **握手请求中 Host 硬编码**：`onBufferedEvent` 中 Host 固定为 `127.0.0.1`，未使用实际传入的 address
3. **统计中 dropped 和 ack_timeouts 未更新**：声明了字段但没有在代码中递增
4. **重连无上限**：`reconnect()` 无最大重连次数或指数退避
5. **重连 sleep 不可中断**：`std::this_thread::sleep_for` 无法被 Stop 中断
