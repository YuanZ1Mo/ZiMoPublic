#include "zm_net_http_middleware.h"
#include "../spdlog/zm_logger.h"

#include <chrono>
#include <exception>

ZmHttpRouter::Middleware ZmHttpMiddlewareLogging()
{
    return [](ZmHttpdTask* task, ZmHttpRouter::Next next) {
        auto t0 = std::chrono::steady_clock::now();
        const char* uri = task->Uri() ? task->Uri() : "(null)";
        const char* method = "UNKNOWN";
        switch (task->Method())
        {
        case EVHTTP_REQ_GET:    method = "GET";    break;
        case EVHTTP_REQ_POST:   method = "POST";   break;
        case EVHTTP_REQ_PUT:    method = "PUT";    break;
        case EVHTTP_REQ_DELETE: method = "DELETE"; break;
        default: break;
        }

        next();  // 执行后续链

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        PUBLIC_LOG_INFO("HTTP {} {} → {}ms", method, uri, ms);
    };
}

ZmHttpRouter::Middleware ZmHttpMiddlewareRecovery()
{
    return [](ZmHttpdTask* task, ZmHttpRouter::Next next) {
        try
        {
            next();
        }
        catch (const std::exception& e)
        {
            PUBLIC_LOG_ERROR("HTTP 请求异常: {}，URI: {}",
                e.what(), task->Uri() ? task->Uri() : "(null)");
            // 清空 handler 可能已部分写入的脏数据
            task->ClearReplyBody();
            task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
            task->SetReply(500, "Internal Server Error");
            std::string body = "{\"error\":\"Internal Server Error\"}";
            task->SetReplyData((const BYTE*)body.c_str(), body.size());
        }
        catch (...)
        {
            PUBLIC_LOG_ERROR("HTTP 请求未知异常，URI: {}",
                task->Uri() ? task->Uri() : "(null)");
            task->ClearReplyBody();
            task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
            task->SetReply(500, "Internal Server Error");
            std::string body = "{\"error\":\"Internal Server Error\"}";
            task->SetReplyData((const BYTE*)body.c_str(), body.size());
        }
    };
}
