#include "zm_net_http_router.h"

#include "zm_net_http.h"
#include "../util/zm_util_str.h"
#include "../spdlog/zm_logger.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>

// 线程局部：当前请求的路径参数
thread_local std::map<std::string, std::string> ZmHttpRouter::t_params;

// ============================================================================
// 构造 / 析构
// ============================================================================

ZmHttpRouter::ZmHttpRouter()
    : m_root(std::make_unique<Node>())
{
}

ZmHttpRouter::~ZmHttpRouter() = default;

// ============================================================================
// 路径解析辅助
// ============================================================================

/** @brief 将 "/api/users/:id" 拆分为 ["api", "users", ":id"] */
static std::vector<std::string> SplitPath(std::string_view pattern)
{
    std::vector<std::string> segs;
    if (pattern.empty())
        return segs;

    // 去掉开头 /
    if (pattern[0] == '/')
        pattern = pattern.substr(1);

    if (pattern.empty())
        return segs;

    std::string p(pattern);

    if (p.empty())
        return segs;

    std::istringstream ss(p);
    std::string seg;
    while (std::getline(ss, seg, '/'))
    {
        if (!seg.empty())
            segs.push_back(seg);
    }
    return segs;
}

// ============================================================================
// 路由注册
// ============================================================================

void ZmHttpRouter::AddRoute(std::string_view method, std::string_view pattern, Handler h,
                             const std::vector<Middleware>& groupMWs)
{
    auto segs = SplitPath(pattern);
    Node* node = m_root.get();

    for (const auto& seg : segs)
    {
        // 查找匹配的子节点
        Node* child = nullptr;
        for (auto& c : node->children)
        {
            if (c->segment == seg)
            {
                child = c.get();
                break;
            }
        }
        if (child == nullptr)
        {
            auto newNode = std::make_unique<Node>();
            newNode->segment = seg;
            child = newNode.get();
            node->children.push_back(std::move(newNode));
        }
        node = child;
    }

    // 在叶子节点存储处理器
    node->handlers[std::string(method)] = h;

    // 合并分组中间件到此节点
    if (!groupMWs.empty())
    {
        node->middlewares.insert(node->middlewares.end(),
                                  groupMWs.begin(), groupMWs.end());
    }
}

void ZmHttpRouter::Get(std::string_view pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + std::string(pattern);
        m_parent->AddRoute("GET", fullPath, h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("GET", pattern, h, {});
    }
}

void ZmHttpRouter::Post(std::string_view pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + std::string(pattern);
        m_parent->AddRoute("POST", fullPath, h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("POST", pattern, h, {});
    }
}

void ZmHttpRouter::Put(std::string_view pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + std::string(pattern);
        m_parent->AddRoute("PUT", fullPath, h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("PUT", pattern, h, {});
    }
}

void ZmHttpRouter::Delete(std::string_view pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + std::string(pattern);
        m_parent->AddRoute("DELETE", fullPath, h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("DELETE", pattern, h, {});
    }
}

void ZmHttpRouter::Any(std::string_view pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + std::string(pattern);
        m_parent->AddRoute("*", fullPath, h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("*", pattern, h, {});
    }
}

void ZmHttpRouter::Use(Middleware mw)
{
    m_globalMiddlewares.push_back(std::move(mw));
}

ZmHttpRouter& ZmHttpRouter::Group(std::string_view prefix)
{
    auto child = std::make_unique<ZmHttpRouter>();
    child->m_parent = this;
    child->m_groupPrefix = prefix;
    // 去掉前缀尾部 /
    while (!child->m_groupPrefix.empty() && child->m_groupPrefix.back() == '/')
        child->m_groupPrefix.pop_back();

    ZmHttpRouter& ref = *child;
    m_children.push_back(std::move(child));
    return ref;
}

// ============================================================================
// 路由匹配（单次遍历，同时返回 handlers 和中间件）
// ============================================================================

ZmHttpRouter::MatchResult
ZmHttpRouter::MatchRoute(const std::string& path,
                          std::map<std::string, std::string>& outParams) const
{
    MatchResult result;
    auto segs = SplitPath(path.c_str());
    const Node* node = m_root.get();

    // 静态/参数分支旁的通配符兄弟节点（回溯点）：沿静态分支下钻后若子树死路，
    // 回退到最近一次记录的通配符兄弟（如 "/control/asd" → 根级 "*" 兜底路由）。
    const Node* savedWildcard = nullptr;
    size_t      savedMWCount  = 0;   // 回溯点已收集的中间件数（用于裁剪误积累的中间件）

    for (size_t i = 0; i < segs.size(); i++)
    {
        const auto& seg = segs[i];
        const Node* matched  = nullptr;
        const Node* wildcard = nullptr;
        const Node* param    = nullptr;

        for (const auto& c : node->children)
        {
            if (c->segment == seg)
                matched = c.get();
            else if (c->segment == "*")
                wildcard = c.get();
            else if (!c->segment.empty() && c->segment[0] == ':')
                param = c.get();
        }

        if (matched)
        {
            // 静态分支旁存在通配符兄弟 → 记录回溯点（覆盖为更深的一次）
            if (wildcard)
            {
                savedWildcard = wildcard;
                savedMWCount  = result.nodeMWs.size();
            }
            node = matched;
            if (!matched->middlewares.empty())
                result.nodeMWs.insert(result.nodeMWs.end(),
                    matched->middlewares.begin(), matched->middlewares.end());
        }
        else if (param)
        {
            if (wildcard)
            {
                savedWildcard = wildcard;
                savedMWCount  = result.nodeMWs.size();
            }
            outParams[param->segment.substr(1)] = seg;
            node = param;
            if (!param->middlewares.empty())
                result.nodeMWs.insert(result.nodeMWs.end(),
                    param->middlewares.begin(), param->middlewares.end());
        }
        else if (wildcard)
        {
            if (!wildcard->middlewares.empty())
                result.nodeMWs.insert(result.nodeMWs.end(),
                    wildcard->middlewares.begin(), wildcard->middlewares.end());
            result.handlers = &wildcard->handlers;
            return result;
        }
        else
        {
            // 死路：子树无法匹配剩余段 → 回溯到最近记录的通配符兄弟
            if (savedWildcard)
            {
                result.nodeMWs.resize(savedMWCount);
                if (!savedWildcard->middlewares.empty())
                    result.nodeMWs.insert(result.nodeMWs.end(),
                        savedWildcard->middlewares.begin(), savedWildcard->middlewares.end());
                result.handlers = &savedWildcard->handlers;
                return result;
            }
            return result;  // 无匹配，handlers 为 nullptr
        }
    }

    // 所有段匹配完成
    if (node->handlers.empty())
    {
        // 检查通配符子节点（如 "/*" 匹配 "/" 时请求段为空）
        for (const auto& c : node->children)
        {
            if (c->segment == "*")
            {
                if (!c->middlewares.empty())
                    result.nodeMWs.insert(result.nodeMWs.end(),
                        c->middlewares.begin(), c->middlewares.end());
                result.handlers = &c->handlers;
                return result;
            }
        }
        // 段已耗尽但节点无处理器 → 同样回溯到通配符兄弟
        if (savedWildcard)
        {
            result.nodeMWs.resize(savedMWCount);
            if (!savedWildcard->middlewares.empty())
                result.nodeMWs.insert(result.nodeMWs.end(),
                    savedWildcard->middlewares.begin(), savedWildcard->middlewares.end());
            result.handlers = &savedWildcard->handlers;
            return result;
        }
        return result;  // handlers 为 nullptr
    }

    result.handlers = &node->handlers;
    return result;
}

// ============================================================================
// 请求分发
// ============================================================================

int ZmHttpRouter::Serve(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    const char* uri = task->Uri();
    if (uri == nullptr)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;

    // 去掉 query string
    std::string path(uri);
    size_t q = path.find('?');
    if (q != std::string::npos)
        path = path.substr(0, q);

    // 一次遍历完成路径匹配 + 中间件收集
    t_params.clear();
    auto matched = MatchRoute(path, t_params);
    if (matched.handlers == nullptr)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;

    // 匹配 HTTP 方法
    const char* methodStr = "*";
    switch (task->Method())
    {
    case EVHTTP_REQ_GET:    methodStr = "GET";    break;
    case EVHTTP_REQ_POST:   methodStr = "POST";   break;
    case EVHTTP_REQ_PUT:    methodStr = "PUT";    break;
    case EVHTTP_REQ_DELETE: methodStr = "DELETE"; break;
    default:                methodStr = "*";       break;
    }

    Handler handler = nullptr;
    auto it = matched.handlers->find(methodStr);
    if (it != matched.handlers->end())
        handler = it->second;
    else
    {
        auto itAny = matched.handlers->find("*");
        if (itAny != matched.handlers->end())
            handler = itAny->second;
    }

    if (!handler)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;

    return ExecuteChain(task, data, dlen, matched.nodeMWs, handler);
}

int ZmHttpRouter::ExecuteChain(ZmHttpdTask* task, const BYTE* data, size_t dlen,
                                const std::vector<Middleware>& nodeMWs, Handler handler)
{
    struct ChainFrame {
        std::vector<Middleware> chain;
        int  index = 0;
        int  depth = 0;
        bool shortCircuited = false;
        int  handlerResult = 0;
        enum : int { MAX_DEPTH = 64 };  ///< 防止中间件递归死循环导致栈溢出
    };
    auto frame = std::make_shared<ChainFrame>();

    frame->chain = m_globalMiddlewares;
    frame->chain.insert(frame->chain.end(), nodeMWs.begin(), nodeMWs.end());

    // next() 递归：推进到下一个中间件，链尾执行处理器
    std::function<void()> next;
    next = [this, task, data, dlen, handler, frame, &next]() {
        if (++frame->depth > ChainFrame::MAX_DEPTH)
        {
            frame->shortCircuited = true;
            return;  // 深度超限，强制终止
        }

        while (frame->index < (int)frame->chain.size())
        {
            auto mw = frame->chain[frame->index++];
            bool calledNext = false;
            mw(task, [&calledNext, &next]() {
                calledNext = true;
                next();
            });
            if (!calledNext)
            {
                frame->shortCircuited = true;
                return;  // 中间件短路
            }
            return;  // 中间件已调用 next()，等待递归返回
        }
        // 链尾：执行处理器
        frame->handlerResult = handler(task, data, dlen);
    };

    next();

    if (frame->shortCircuited)
        return 0;  // 中间件短路或深度超限

    return frame->handlerResult;
}

// ============================================================================
// 路径参数
// ============================================================================

std::string ZmHttpRouter::GetParam(std::string_view name)
{
    auto it = t_params.find(std::string(name));
    return (it != t_params.end()) ? it->second : std::string();
}

// ============================================================================
// 内置中间件
// ============================================================================

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

		next();

		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		PUBLIC_LOG_INFO("[#{}] HTTP {} {} → {}ms", task->Id(), method, uri, ms);
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
			PUBLIC_LOG_ERROR("[#{}] HTTP 请求异常: {}，URI: {}",
				task->Id(), e.what(), task->Uri() ? task->Uri() : "(null)");
			task->ClearReplyBody();
			task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
			task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR, "Internal Server Error");
			std::string body = "{\"error\":\"Internal Server Error\"}";
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
		}
		catch (...)
		{
			PUBLIC_LOG_ERROR("HTTP 请求未知异常，URI: {}",
				task->Uri() ? task->Uri() : "(null)");
			task->ClearReplyBody();
			task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
			task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR, "Internal Server Error");
			std::string body = "{\"error\":\"Internal Server Error\"}";
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
		}
	};
}
