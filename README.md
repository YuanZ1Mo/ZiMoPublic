# ZiMoPublic

ZiMo 生态的 C++ 公共基础库，为 ZiMoService 及其他上层项目提供 HTTP 服务器三面(前端/JRPC/RESTful)、HTTP 客户端与流式下载、TCP 广播、Windows 服务框架、日志、JSON、SQLite 连接与事务、线程与事件循环等通用能力。

## 模块总览

| 目录          | 模块            | 说明                                                             |
| ----------- | ------------- | -------------------------------------------------------------- |
| `define/`   | 基础定义          | 通用宏(取最值/睡眠/内存拷贝)、版本定义                                          |
| `util/`     | 通用工具          | 线程/线程池、libevent 事件循环、日志、JSON、文件、字符串、容器、系统/OS、SQLite 连接与事务         |
| `net/`      | 网络层           | HTTP 服务器(基类 + 前端/JRPC/RESTful 三面)、HTTP 客户端(含流式下载)、TCP 广播、IP 工具 |
| `service/`  | Windows 服务框架  | 服务注册/卸载/调试运行,SCM 回调驱动生命周期                                      |
| `json/`     | nlohmann JSON | 单头 JSON 库(3.12.0)                                              |
| `spdlog/`   | spdlog 日志库    | header-only 日志库(1.17.0)                                        |
| `libevent/` | libevent 事件库  | 事件驱动网络库(2.2.1)                                                 |
| `libopus/`  | Opus 音频编解码    | 语音编码静态库                                                        |
| `drogon/`   | drogon Web 框架 | drogon 1.9.13 + trantor + openssl/sqlite3/hiredis 等预编译静态库      |
| `minizip-ng/` | zip 打包库     | minizip-ng 静态库(含 bz2/lzma/zstd 后端),供上层做 zip 打包               |

## 各模块详解

**define/ — 基础宏与版本**

| 头文件                   | 说明                                                                           |
| --------------------- | ---------------------------------------------------------------------------- |
| `zm_simple_define.h`  | 通用宏:`ZM_MAX/ZM_MIN/ZM_BETWEEN`、`ZmSleepMS/ZmSleepUS`、`zm_memcpy/zm_memset` 等 |
| `zm_version_define.h` | 版本定义(`ZIMO_SERVER_VERSION`)                                                  |

**util/ — 通用工具(声明与实现分离,头文件即接口)**

| 文件                         | 说明                                                                                                     |
| -------------------------- | ------------------------------------------------------------------------------------------------------ |
| `zm_util_thread.h`         | `ZmThread`(C++20 jthread 封装,状态位掩码)、`ZmThreadPool` 线程池(含 `WaitIdle` 等待在途任务排空)                     |
| `zm_util_evbase_runloop.h` | `ZmEvBaseRunLoop` — libevent 事件循环线程(多定时器增删/改间隔/一次性、跨线程投递,回调异常不外溢)                        |
| `zm_util_libevent.h`       | libevent 初始化(`zm_util_eventbase_init` 线程支持)、`ZmEventBuffer` 事件缓冲与非阻塞 socket 工具                        |
| `zm_util_logger.h`         | spdlog 封装:`RotatingLoggerBase` 轮转日志基类、`g_default_logger`/`g_public_logger`、`EnableConsoleSink` 控制台彩色输出 |
| `zm_util_json.h`           | nlohmann 封装:`ZMJSON`(ordered\_json)、类型安全读取、嵌套路径、序列化/解析、合并                                              |
| `zm_util_file.h`           | `ZmFile` 文件工具:读写/复制/删除/目录/路径解析/哈希(Win32)                                                               |
| `zm_util_str.h`            | 字符串工具与 `String` 类型(Unicode 适配)                                                                         |
| `zm_util_container.h`      | 容器:`ZmByteBuffer` 动态字节缓冲区、`ZmArrayList`、`ZmObjectPool`、`ZmBinaryTable`                                |
| `zm_util_sys.h`            | 系统工具:DLL 加载/符号查找宏等                                                                                     |
| `zm_util_win_os.h`         | Windows OS 工具:版本号、电源操作等                                                                                |
| `zm_util_sqlite.h`         | `ZmSqliteDb` — SQLite 连接与并发模型:写连接单条(可重入互斥)+ 读连接池、WAL + busy_timeout;协程接口经工作池离核,`WithTx` 提供事务,库表结构由各子类 DbModule 承担 |

**net/ — 网络层**

| 文件                                   | 说明                                                                                                  |
| ------------------------------------ | --------------------------------------------------------------------------------------------------- |
| `zm_net_http_server.h/.cpp`          | Drogon 1.9.13 HTTP 服务器基类:进程级静态生命周期(Init/Open/Close)、三面公共底座、路由归属校验与门禁、协程路由(含路径参数守卫)、文件传输(Range/304、If-Range、ETag 前缀)、整读/流式/原始字节流式发送(软件水位兜内存)、流式落盘、限流、请求 ID/访问日志、`RunOnPool` |
| `zm_net_http_frontend_server.h`      | 前端服务器面(80/443):docroot 静态文件 + 自定义 404 + per-port 门禁 + 80→443 重定向(页面/SPA 由业务层 advice 承载)            |
| `zm_net_http_jsonrpc_server.h`       | JSON-RPC 2.0 服务器面(39440):协议校验与信封(HTTP 恒 200,错误见信封 `error.code`)                                     |
| `zm_net_http_restful_server.h`       | 业务 API 服务器面(39441 /zimo/api)                                                                        |
| `zm_net_http_client.h/.cpp`          | 出站 HTTP/HTTPS 客户端门面(进程级静态):协程/异步回调/同步三形态、per-target 连接池、重试与重定向、JSON/表单/multipart 上传、TLS、出站日志   |
| `zm_net_http_client_download.h/.cpp` | `ZmHttpDownloadChannel` 流式下载通道:每会话写线程直写盘、Range 续传(If-Range)、3xx 跟随、队列背压与停滞看护、TLS 校验                       |
| `zm_net_broadcast_base.h`            | 广播模块公共定义:状态枚举、消息/客户端结构体、帧协议编解码                                                                      |
| `zm_net_broadcast_server.h`          | TCP 广播服务端:握手/心跳/Tag 订阅/每客户端队列/踢出                                                                    |
| `zm_net_broadcast_client.h`          | TCP 广播客户端:自动握手/心跳/订阅/消息回调                                                                           |
| `zm_net_ip.h`                        | IP 地址结构 `ZM_IP_ADDR`(同时支持 IPv4/IPv6)                                                                |

**service/ — Windows 服务框架**

| 文件                       | 说明                                                                            |
| ------------------------ | ----------------------------------------------------------------------------- |
| `zm_service_base.h/.cpp` | `ZmServiceBase` — 子类实现 `OnStart` 等虚函数,由 SCM 回调驱动生命周期;`Run()`/`RunDebugMode()` |

**第三方依赖(随库内置,见"依赖关系")**:`json/`、`spdlog/`、`libevent/`、`libopus/`、`drogon/`、`minizip-ng/`。

## 依赖关系

第三方依赖全部**静态链接**(MT 运行时),随库内置,无需额外部署:

| 依赖               | 版本     | 位置                            | 用途                                                                                |
| ---------------- | ------ | ----------------------------- | --------------------------------------------------------------------------------- |
| drogon / trantor | 1.9.13 | `drogon/include`、`drogon/lib/VC/x64/MT` | Web 框架;预编译静态库含 openssl/sqlite3/hiredis/mariadb/libpq/lz4/brotli/zlib/ecpg/cares 等 |
| libevent         | 2.2.1  | `libevent/`                   | 事件驱动网络库(TCP 广播、事件循环线程)                                                            |
| spdlog           | 1.17.0 | `spdlog/`                     | 日志库(header-only)                                                                  |
| nlohmann json    | 3.12.0 | `json/`                       | JSON 解析(单头)                                                                       |
| libopus          | —      | `libopus/`                    | Opus 音频编解码                                                                        |
| minizip-ng       | —      | `minizip-ng/`                 | zip 打包;静态库 `minizip-ng.lib` + `bz2/lzma/zstd`                                    |

> `drogon.lib` 约 267 MB,超过 GitHub 单文件 100 MB 限制,以分卷入库(`drogon.lib.part01~03`),克隆后需执行 `drogon/lib/VC/x64/MT/drogon_lib_merge.ps1` 合并还原(详见 `drogon/lib/VC/x64/MT/README.md`)。

## 构建与集成

```bash
msbuild LibZiMoPublic.sln /p:Configuration=Release /p:Platform=x64
```

- 产物:静态库 `Release_LibZiMoPublic\lib\VC\x64\MT\LibZiMoPublic.lib`,中间文件到 `Release_LibZiMoPublic\temp\`

- 需要 VS 2022(v143)+ Windows SDK 10.0.26100.0,C++20、Unicode 字符集、`/utf-8`

- 第三方依赖全部**静态链接**(MT 运行时),上层项目(ZiMoService)以同级目录 `..\ZiMoPublic\` 引用头文件与库即可,免 DLL 部署

- 头文件包含目录(见 `LibZiMoPublic.vcxproj`):`libevent/include`、`define`、`json`、`net`、`service`、`spdlog`、`util`、`libopus/include`、`drogon/include`、`minizip-ng/include`

- 库目录:`libevent/lib/VC/x64/MT`、`libopus/lib/VC/x64/MT`、`drogon/lib/VC/x64/MT`、`minizip-ng/lib/VC/x64/MT`

- 上层工程需在自己的链接清单里列出所用静态库(本库不代为链接),`ZiMoService.vcxproj` 的 `AdditionalDependencies` 即这份清单:trantor/event/event_core/event_extra、opus、drogon、jsoncpp、cares、lz4、sqlite3、libssl/libcrypto、brotli*、mariadbclient、pq/hiredis/pg*、zs、minizip-ng/bz2/lzma/zstd 及系统库

## 设计原则

- **静态进程级生命周期状态机**：HTTP 层为进程级静态单例生命周期。
  `ZmHttpServer` 状态机 `Uninit → Initialized → Opened → Closed`：`Init(opts)` 一次性注入全局参数/证书，`Open()` 后台线程启动事件循环，`Close()` 全局唯一关闭且为终态（进程内关闭后不可再 Open，重启需进程级重启）；`ZmHttpClient` 同构（`Uninit → Initialized → Closed`，与服务器完全解耦）。派生面（前端/JRPC/RESTful）"结构上多实例、运行时单 `app()`"——共享 drogon 全局实例，每对象只负责"端口 + 路由登记"。

- **声明与定义分离**：头文件只放声明，实现放在对应的 `.cpp` 文件中

- **成员变量命名**：`m_` 前缀（结构体除外），全局变量 `g_` 前缀，静态变量 `s_` 前缀

- **注释规范**：按 `@brief @param @return @example` 格式，中文注释，UTF-8 编码，LF 换行

- **代码组织**：public → protected → private，函数与成员变量分开

- **RAII 资源管理**：如 `RotatingLoggerBase`（`CreateLogger`/`ReleaseLogger` 成对）、`ZmEventBuffer`、`ZmByteBuffer`、`ZmSqliteDb`（`Init`/`Close` 成对，析构兜底关闭全部连接）、`ZmBroadcastClient`（析构自动断开）

- **全局惰性服务**：日志管理器 `DefaultLogger`/`PublicLogger`（`RotatingLoggerBase` 子类）经 `Ensure()` 惰性初始化，暴露 `g_default_logger`/`g_public_logger` 全局句柄，`DEFAULT_LOG_*` 宏自动兜底初始化

- **回调模式**：`std::function` 回调集合，业务不接触底层类型——`BcClientCallbacks`/`BcServerCallbacks`（`onConnected`/`onMessage` 等）、HTTP 协程 handler（`ZmHttpCoroHandler`/`ZmHttpStreamHandler`/`ZmJrpcMethodHandler`）、传输进度回调（`ZmHttpSendFileOptions::onProgress`）

- **单事件循环线程内零锁**：跨线程操作一律投递到事件循环线程执行。
  libevent 侧经 `event_base_once`（一次性投递）/ `event_active`（持久 dispatch 事件唤醒）——如 `ZmBroadcastClient::ScheduleTask` 入队后 `event_active` 唤醒 dispatch 事件、`ZmEvBaseRunLoop::Control` 经 `event_active` 唤醒控制事件；drogon/trantor 侧经 `queueInLoop`——如 `ZmHttpServer` 的回复控制/协程恢复、`RunOnPool` 工作线程回执、`ZmHttpClient`/`ZmHttpDownloadChannel` 的协程恢复。**状态机归属其连接所属事件循环**：流式发送（定时器链）与流式落盘的回执、定时器、进度/完成回调都投回请求所属 loop，状态与连接计数（如 `TcpConnection::bytesSent`，非原子）因此只被单一线程读写——既无跨线程读写，也不把并发传输挤到主 loop

- **阻塞/磁盘 I/O 离核（事件循环线程绝不读盘）**：下载读盘走专用 I/O 线程池（`ZmHttpServer::HttpIoPool`，读线程仅 `queueInLoop` 回执、由事件循环发送），阻塞任务经 `RunOnPool` 提交共享工作池（`CallbackAwaiter` 桥回事件循环），上传写盘走每连接写线程；`ZmHttpDownloadChannel` 同样离核——磁盘只在**每会话写线程**（写盘/截断/侧车）与**客户端自持工作池**（打开 `.part`、读侧车）执行，通道 loop 零磁盘，写侧积压由队列字节上限收敛。异常一律留痕：读失败/提前读到文件尾（含"文件被写者独占而打不开"）与写盘失败都有 `PUBLIC_LOG_*` 记录——这两类失败按"发完"收尾，响应本身不报错，日志是唯一线索

## 提交规范

提交信息格式:`<类型>: <描述>`

| 类型       | 用途            | 示例                      |
| -------- | ------------- | ----------------------- |
| `feat`   | 新功能           | `feat: 增加用户注册功能`        |
| `bugfix` | 修复 bug        | `bugfix: 修复登录页面崩溃的问题`   |
| `docs`   | 文档变更          | `docs: 更新README文件`      |
| `style`  | 代码风格变动(不影响逻辑) | `style: 删除多余的空行`        |
| `refactor` | 代码重构        | `refactor: 重构用户验证逻辑`    |
| `perf`   | 性能优化          | `perf: 优化图片加载速度`        |
| `test`   | 添加或修改测试       | `test: 增加用户模块的单元测试`     |
| `chore`  | 杂项(构建过程或辅助工具) | `chore: 更新依赖库`          |
| `build`  | 构建系统或外部依赖变更   | `build: 升级webpack到版本5`  |
| `ci`     | 持续集成配置变更      | `ci: 修改GitHub Actions配置文件` |
| `revert` | 回滚            | `revert: 回滚feat: 增加用户注册功能` |

## 头文件包含规范

```
优先级由上到下
1. 尽量使用前向声明
2. 对应的头文件(foo.cpp → foo.h),使用 "" 号
3. 本项目其他头文件,使用相对目录和 "" 号(../util/util_logger.h)
4. 第三方库头文件,使用相对目录和 <> 号(<../spdlog/spdlog.h>)
5. 标准库头文件,使用 <> 号(<iostream>)
```
