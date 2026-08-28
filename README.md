# ZiMoPublic

ZiMo 生态的 C++ 公共基础库，为 ZiMoService 及其他上层项目提供网络通信、Windows 服务框架、SSL/TLS 安全、日志、JSON 处理、SQLite、线程工具等通用能力。

## 模块总览


## 各模块详解


## 依赖关系



## 构建与集成



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
