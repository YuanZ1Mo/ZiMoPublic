/**
 * @file zm_net_http_middleware.h
 * @brief 内置 HTTP 中间件：日志、异常恢复
 *
 * 以自由函数形式提供，返回 ZmHttpRouter::Middleware，可直接 Use()。
 */

#ifndef ZM_NET_HTTP_MIDDLEWARE_H
#define ZM_NET_HTTP_MIDDLEWARE_H

#include "zm_net_http_router.h"

/**
 * @brief 请求日志中间件
 *
 * 记录请求方法、路径、耗时（毫秒）。
 * 日志通过 PUBLIC_LOG_INFO 输出。
 *
 * @example
 * @code
 *   app.Use(ZmHttpMiddlewareLogging());
 * @endcode
 */
ZmHttpRouter::Middleware ZmHttpMiddlewareLogging();

/**
 * @brief 异常恢复中间件
 *
 * 捕获处理器抛出的 std::exception，自动返回 500 并输出错误日志。
 * 应放在链的最前面，确保后续中间件和处理器的异常能被兜底。
 *
 * @example
 * @code
 *   app.Use(ZmHttpMiddlewareRecovery());
 * @endcode
 */
ZmHttpRouter::Middleware ZmHttpMiddlewareRecovery();

#endif // ZM_NET_HTTP_MIDDLEWARE_H
