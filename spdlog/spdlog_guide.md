# spdlog 使用指南

## 1. 架构原理

spdlog 是一个 header-only 的 C++ 日志库，核心架构分三层：

```
Logger（日志器）
  └─ Formatter（格式化器）── 将日志内容格式化为字符串
  └─ Sink（输出目标）────── 将格式化后的字符串写入目标
       ├─ file_sink        写入文件
       ├─ stdout_sink      输出到控制台
       ├─ msvc_sink        输出到 VS 调试窗口
       └─ ...
```

- **Logger**：日志入口，持有多个 Sink，每条日志分发给所有 Sink
- **Formatter**：通过 pattern 模板将时间、级别、位置等信息拼接为字符串
- **Sink**：实际的输出目标，每个 Sink 可以独立配置
- **Registry**：全局注册表，管理所有 Logger 实例，支持通过名称查找

线程安全后缀：
- `_mt`（multi-threaded）：带互斥锁，线程安全
- `_st`（single-threaded）：无锁，单线程场景性能更高

## 2. Sink 类型

| Sink | 头文件 | 说明 |
|------|--------|------|
| `basic_file_sink` | `spdlog/sinks/basic_file_sink.h` | 写入单个文件 |
| `rotating_file_sink` | `spdlog/sinks/rotating_file_sink.h` | 按文件大小轮转 |
| `daily_file_sink` | `spdlog/sinks/daily_file_sink.h` | 按天轮转 |
| `hourly_file_sink` | `spdlog/sinks/hourly_file_sink.h` | 按小时轮转 |
| `stdout_sink` | `spdlog/sinks/stdout_sinks.h` | 输出到控制台 |
| `stdout_color_sink` | `spdlog/sinks/stdout_color_sinks.h` | 控制台带颜色 |
| `wincolor_sink` | `spdlog/sinks/wincolor_sink.h` | Windows 控制台颜色 |
| `msvc_sink` | `spdlog/sinks/msvc_sink.h` | VS 输出窗口 |
| `win_eventlog_sink` | `spdlog/sinks/win_eventlog_sink.h` | Windows 事件日志 |
| `ostream_sink` | `spdlog/sinks/ostream_sink.h` | 输出到 std::ostream |
| `callback_sink` | `spdlog/sinks/callback_sink.h` | 自定义回调 |
| `null_sink` | `spdlog/sinks/null_sink.h` | 丢弃所有日志 |
| `dist_sink` | `spdlog/sinks/dist_sink.h` | 分发到多个子 Sink |
| `dup_filter_sink` | `spdlog/sinks/dup_filter_sink.h` | 过滤连续重复日志 |

## 3. 日志级别

从低到高：

| 级别 | 宏版本 | 函数版本 | 说明 |
|------|--------|----------|------|
| trace | `SPDLOG_TRACE(...)` | `logger->trace(...)` | 最详细的追踪信息 |
| debug | `SPDLOG_DEBUG(...)` | `logger->debug(...)` | 调试信息 |
| info | `SPDLOG_INFO(...)` | `logger->info(...)` | 常规信息 |
| warn | `SPDLOG_WARN(...)` | `logger->warn(...)` | 警告 |
| error | `SPDLOG_ERROR(...)` | `logger->error(...)` | 错误 |
| critical | `SPDLOG_CRITICAL(...)` | `logger->critical(...)` | 严重错误 |
| off | — | — | 关闭日志 |

通过 `set_level` 控制最低输出级别：

```cpp
spdlog::set_level(spdlog::level::debug); // debug 及以上级别都输出
```

## 4. 格式化（Pattern）

通过 `set_pattern` 设置日志格式。

### 占位符

| 占位符 | 含义 |
|--------|------|
| `%Y-%m-%d` | 年月日 |
| `%H:%M:%S` | 时分秒 |
| `%e` | 毫秒 |
| `%f` | 微秒 |
| `%F` | 纳秒 |
| `%l` | 日志级别全称 (info/warn/error...) |
| `%L` | 日志级别首字母 (I/W/E...) |
| `%n` | Logger 名称 |
| `%t` | 线程 ID |
| `%P` | 进程 ID |
| `%v` | 日志正文 |
| `%s` | 源文件名 |
| `%#` | 行号 |
| `%!` | 函数名 |
| `%^` / `%$` | 颜色范围起始/结束 |
| `%%` | 输出 `%` 字面量 |

### 示例

```cpp
logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%P] [%t] [%l] [%s] [%!] [%#] %v");
```

输出效果：

```
[2026-05-17 14:30:00.123] [rotating] [1234] [5678] [info] [service_main.cpp] [_tmain] [40] AstraliserService started
```

## 5. 刷盘机制

spdlog 默认使用 `std::ofstream`，数据会先进入 C 运行时的用户态缓冲区，不一定立即写入磁盘。

### flush_on

达到指定级别时立即调用 `fflush`：

```cpp
logger->flush_on(spdlog::level::trace);  // 所有级别都立即刷盘
logger->flush_on(spdlog::level::warn);   // warn 及以上立即刷盘
```

### flush_every

定时刷盘，每隔 N 秒调用一次 `fflush`：

```cpp
spdlog::flush_every(std::chrono::seconds(3));
```

### flush 手动触发

```cpp
logger->flush();
```

## 6. 配置项汇总

### Logger 级别

| 方法 | 说明 |
|------|------|
| `set_level(level)` | 设置最低输出级别 |
| `set_pattern(pattern)` | 设置日志格式 |
| `flush_on(level)` | 达到指定级别立即刷盘 |
| `flush_every(duration)` | 定时刷盘 |
| `flush()` | 手动触发刷盘 |
| `set_error_handler(handler)` | 自定义错误回调 |
| `should_log(level)` | 判断某级别是否会被输出 |

### 全局（spdlog::）

| 方法 | 说明 |
|------|------|
| `set_default_logger(logger)` | 设置默认 Logger |
| `get(name)` | 通过名称获取已注册的 Logger |
| `drop(name)` | 移除指定 Logger |
| `drop_all()` | 移除所有 Logger |
| `shutdown()` | 释放所有 Logger 资源 |
| `set_level(level)` | 设置所有 Logger 的最低级别 |
| `set_pattern(pattern)` | 设置所有 Logger 的格式 |
| `set_automatic_registration(bool)` | 是否自动注册新创建的 Logger（默认 true） |

## 7. 使用示例

### 按大小轮转的文件日志

```cpp
#include "spdlog/spdlog.h"
#include "spdlog/sinks/rotating_file_sink.h"

// 单文件最大 10MB，最多保留 10 个文件
auto logger = spdlog::rotating_logger_mt("rotating", "logs/app.log", 1024 * 1024 * 10, 10);
logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%P] [%t] [%l] %v");
logger->flush_on(spdlog::level::trace);
spdlog::set_default_logger(logger);

SPDLOG_INFO("Service started");
```

### 按天轮转的文件日志

```cpp
#include "spdlog/sinks/daily_file_sink.h"

// 每天凌晨 2:30 轮转
auto logger = spdlog::daily_logger_mt("daily", "logs/daily.log", 2, 30);
```

### 多 Sink 组合

```cpp
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"

std::vector<spdlog::sink_ptr> sinks;
sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/app.log", 1024 * 1024 * 10, 10));
sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());

auto logger = std::make_shared<spdlog::logger>("multi", sinks.begin(), sinks.end());
spdlog::set_default_logger(logger);
```

### 异步模式

```cpp
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"

// 队列大小 8192 条
auto logger = spdlog::rotating_logger_mt<spdlog::async_factory>(
    "async_rotating", "logs/app.log", 1024 * 1024 * 10, 10);
```

## 8. 注意事项

1. **宏 vs 函数**：`SPDLOG_INFO(...)` 等宏会自动注入 `__FILE__`、`__FUNCTION__`、`__LINE__`，使 `%s`、`%!`、`%#` 占位符有值。`spdlog::info(...)` 函数版本不携带位置信息，这三个占位符输出为空。

2. **flush 行为**：默认情况下日志写入 `std::ofstream` 后不一定会立即落盘，如果程序崩溃可能丢失最后几条日志。建议使用 `flush_on(spdlog::level::trace)` 或 `flush_every` 来保证数据安全。

3. **自动注册**：默认开启，创建的 Logger 会注册到全局 Registry，其他文件可通过 `spdlog::get("name")` 获取。如果不需要跨文件共享，可关闭以减少开销。

4. **线程安全**：`_mt` 后缀的 Logger/Sink 是线程安全的，`_st` 后缀不是。多线程环境必须用 `_mt`。

5. **程序退出**：建议在程序退出前调用 `spdlog::shutdown()` 确保所有缓冲数据刷盘、资源释放。

6. **路径创建**：spdlog 不会自动创建日志目录，需要提前用 `std::filesystem::create_directories` 确保目录存在。

7. **性能**：`%s`、`%!`、`%#` 等位置信息会带来微小的性能开销，对性能极致敏感的场景可考虑去掉这些占位符。
