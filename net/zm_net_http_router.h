/**
 * @file zm_net_http_router.h
 * @brief Express/Gin 风格的 HTTP 路由中间件系统
 *
 * 基于前缀树（Radix Tree）实现路径匹配，支持：
 *   - 静态路径：/api/status
 *   - 路径参数：/api/users/:id
 *   - 通配符：  /static/*
 *   - 方法路由：GET/POST/PUT/DELETE/ANY
 *   - 中间件链：(task, next) 管道模式
 *   - 路由分组：共享前缀 + 共享中间件
 *
 * 使用示例：
 * @code
 *   ZmHttpRouter app;
 *   app.Use(ZmHttpMiddlewareLogging());
 *   app.Get("/api/status", [](ZmHttpdTask* task, const BYTE*, size_t) {
 *       task->PutReplyHeader("Content-type", "application/json");
 *       task->SetReplyData((const BYTE*)"ok", 2);
 *       return 200;
 *   });
 *   app.Get("/api/users/:id", [](ZmHttpdTask* task, const BYTE*, size_t) {
 *       std::string uid = ZmHttpRouter::GetParam("id");
 *       // ...
 *       return 200;
 *   });
 *   app.Serve(task, data, dlen);
 * @endcode
 */

#ifndef ZM_NET_HTTP_ROUTER_H
#define ZM_NET_HTTP_ROUTER_H

#include "zm_net_http.h"

#include <functional>
#include <string>
#include <vector>
#include <map>
#include <memory>

/**
 * @brief HTTP 路由分发器
 *
 * 每个实例持有一棵前缀树和全局中间件列表。
 * Group() 返回的子路由共享父路由的中间件列表引用。
 *
 * 线程安全：每个请求在独立工作线程中执行，Serve() 通过 thread_local
 * 存储当前请求的路径参数，无锁竞争。
 */
class ZmHttpRouter
{
public:
    /** @brief 路由处理器：处理请求并返回 HTTP 状态码 */
    using Handler    = std::function<int(ZmHttpdTask*, const BYTE* data, size_t dlen)>;

    /** @brief 中间件链的下一站回调，调用即继续，不调用即短路 */
    using Next       = std::function<void()>;

    /** @brief 中间件：（请求上下文，下一站） */
    using Middleware = std::function<void(ZmHttpdTask*, Next)>;

    ZmHttpRouter();
    ~ZmHttpRouter();

    // ---- 中间件 ----

    /**
     * @brief 注册全局中间件（对所有路由生效）
     * @param mw 中间件函数
     *
     * 中间件按注册顺序执行，每个中间件调用 next() 继续，
     * 不调用 next() 则链路短路。
     */
    void Use(Middleware mw);

    // ---- 路由注册 ----

    /** @brief 注册 GET 路由 */
    void Get(const char* pattern, Handler h);
    /** @brief 注册 POST 路由 */
    void Post(const char* pattern, Handler h);
    /** @brief 注册 PUT 路由 */
    void Put(const char* pattern, Handler h);
    /** @brief 注册 DELETE 路由 */
    void Delete(const char* pattern, Handler h);
    /** @brief 注册匹配任意 HTTP 方法的路由 */
    void Any(const char* pattern, Handler h);

    /**
     * @brief 创建路由分组（共享前缀）
     * @param prefix 路径前缀，如 "/admin"
     * @return 子路由引用，可在其上注册子路由和中间件
     *
     * @example
     * @code
     *   auto& admin = app.Group("/admin");
     *   admin.Use(authMiddleware);
     *   admin.Get("/dashboard", dashboardHandler);
     * @endcode
     */
    ZmHttpRouter& Group(const char* prefix);

    // ---- 请求分发 ----

    /**
     * @brief 匹配路径并执行中间件链 + 处理器
     * @param task  请求上下文
     * @param data  请求体
     * @param dlen  请求体长度
     * @return      HTTP 状态码（未匹配返回 404）
     */
    int Serve(ZmHttpdTask* task, const BYTE* data, size_t dlen);

    // ---- 路径参数 ----

    /**
     * @brief 获取当前请求的路径参数值
     * @param name 参数名（不含冒号），如 "id" 对应 :id
     * @return 参数值，未匹配返回空字符串
     *
     * 仅在 Handler 中调用有效。底层用 thread_local 存储，线程安全。
     */
    static std::string GetParam(const char* name);

private:
    // ---- 路由树节点 ----
    struct Node;
    using NodePtr = std::unique_ptr<Node>;

    struct Node {
        std::string              segment;    ///< 当前段："api", ":id", "*"
        std::vector<NodePtr>     children;   ///< 子节点
        std::map<std::string, Handler> handlers;  ///< method → handler ("GET", "POST", "*")
        std::vector<Middleware> middlewares;   ///< 本节点及子路由共享的中间件
    };

    /** @brief 内部路由注册
     *  @param method    HTTP 方法（"GET"/"POST"/"*"）
     *  @param pattern   路径模式（"/api/users/:id"）
     *  @param h         处理器
     *  @param groupMWs  分组中间件（来自 Group() 创建的子路由器） */
    void AddRoute(const char* method, const char* pattern, Handler h,
                  const std::vector<Middleware>& groupMWs = {});

    /**
     * @brief 在路由树中查找匹配的节点
     * @param path 请求路径（如 "/api/users/42"）
     * @param outParams 输出：捕获的路径参数
     * @return 匹配节点的处理器映射，未匹配返回 nullptr
     */
    const std::map<std::string, Handler>* FindRoute(
        const std::string& path, std::map<std::string, std::string>& outParams) const;

    /** @brief 构建并执行中间件链 + 处理器 */
    int ExecuteChain(ZmHttpdTask* task, const BYTE* data, size_t dlen,
                     const std::vector<Middleware>& nodeMWs, Handler handler);

    // ---- 成员变量 ----
    NodePtr                  m_root;             ///< 路由树根节点
    std::vector<Middleware>  m_globalMiddlewares; ///< 全局中间件列表
    std::string              m_groupPrefix;       ///< 分组前缀（子路由器非空）

    // ZmHttpRouter 可嵌套分组，Group() 创建的子路由共享父路由的成员
    ZmHttpRouter*            m_parent = nullptr;  ///< 父路由器（分组时非空）
    std::vector<std::unique_ptr<ZmHttpRouter>> m_children;  ///< 子路由器（分组创建）

    // 线程局部存储：当前请求的路径参数
    static thread_local std::map<std::string, std::string> t_params;
};

#endif // ZM_NET_HTTP_ROUTER_H
