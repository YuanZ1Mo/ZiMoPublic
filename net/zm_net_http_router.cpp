#include "zm_net_http_router.h"
#include "../util/zm_util_str.h"

#include <algorithm>
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
static std::vector<std::string> SplitPath(const char* pattern)
{
    std::vector<std::string> segs;
    if (!pattern || !pattern[0])
        return segs;

    std::string p(pattern);
    // 去掉开头 /
    if (!p.empty() && p[0] == '/')
        p = p.substr(1);

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

void ZmHttpRouter::AddRoute(const char* method, const char* pattern, Handler h,
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
        if (!child)
        {
            auto newNode = std::make_unique<Node>();
            newNode->segment = seg;
            child = newNode.get();
            node->children.push_back(std::move(newNode));
        }
        node = child;
    }

    // 在叶子节点存储处理器
    node->handlers[method] = h;

    // 合并分组中间件到此节点
    if (!groupMWs.empty())
    {
        node->middlewares.insert(node->middlewares.end(),
                                  groupMWs.begin(), groupMWs.end());
    }
}

void ZmHttpRouter::Get(const char* pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + pattern;
        m_parent->AddRoute("GET", fullPath.c_str(), h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("GET", pattern, h, {});
    }
}

void ZmHttpRouter::Post(const char* pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + pattern;
        m_parent->AddRoute("POST", fullPath.c_str(), h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("POST", pattern, h, {});
    }
}

void ZmHttpRouter::Put(const char* pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + pattern;
        m_parent->AddRoute("PUT", fullPath.c_str(), h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("PUT", pattern, h, {});
    }
}

void ZmHttpRouter::Delete(const char* pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + pattern;
        m_parent->AddRoute("DELETE", fullPath.c_str(), h, m_globalMiddlewares);
    }
    else
    {
        AddRoute("DELETE", pattern, h, {});
    }
}

void ZmHttpRouter::Any(const char* pattern, Handler h)
{
    if (m_parent)
    {
        std::string fullPath = m_groupPrefix + pattern;
        m_parent->AddRoute("*", fullPath.c_str(), h, m_globalMiddlewares);
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

ZmHttpRouter& ZmHttpRouter::Group(const char* prefix)
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
// 路由匹配
// ============================================================================

const std::map<std::string, ZmHttpRouter::Handler>*
ZmHttpRouter::FindRoute(const std::string& path,
                        std::map<std::string, std::string>& outParams) const
{
    auto segs = SplitPath(path.c_str());
    const Node* node = m_root.get();

    // 收集沿途节点的中间件
    // (outside this function, via returned node)

    for (size_t i = 0; i < segs.size(); i++)
    {
        const auto& seg = segs[i];
        const Node* matched = nullptr;
        const Node* wildcard = nullptr;
        const Node* param = nullptr;

        for (const auto& c : node->children)
        {
            if (c->segment == seg)
                matched = c.get();
            else if (c->segment == "*")
                wildcard = c.get();
            else if (!c->segment.empty() && c->segment[0] == ':')
                param = c.get();
        }

        // 优先级：精确 > 参数 > 通配符
        if (matched)
        {
            node = matched;
        }
        else if (param)
        {
            outParams[param->segment.substr(1)] = seg;  // ":id" → "id" = value
            node = param;
        }
        else if (wildcard)
        {
            // 通配符匹配所有剩余段
            return &wildcard->handlers;
        }
        else
        {
            return nullptr;  // 无匹配
        }
    }

    // 所有段匹配完成，当前节点若无处理器则检查通配符子节点
    //（如 "/*" 匹配 "/" 时请求段为空，需从根节点找 * 子节点）
    if (node->handlers.empty())
    {
        for (const auto& c : node->children)
        {
            if (c->segment == "*")
                return &c->handlers;
        }
        return nullptr;
    }
    return &node->handlers;
}

// ============================================================================
// 请求分发
// ============================================================================

int ZmHttpRouter::Serve(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
    const char* uri = task->Uri();
    if (!uri)
        return 404;

    // 去掉 query string
    std::string path(uri);
    size_t q = path.find('?');
    if (q != std::string::npos)
        path = path.substr(0, q);

    // 路径参数
    t_params.clear();
    const auto* handlers = FindRoute(path, t_params);
    if (!handlers)
        return 404;

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
    auto it = handlers->find(methodStr);
    if (it != handlers->end())
        handler = it->second;
    else
    {
        // 尝试 "*" 通配方法
        auto itAny = handlers->find("*");
        if (itAny != handlers->end())
            handler = itAny->second;
    }

    if (!handler)
        return 404;

    // 收集节点中间件：遍历路由树收集沿途 middlewares
    std::vector<Middleware> nodeMWs;

    // 重新遍历以收集中间件
    auto segs = SplitPath(path.c_str());
    const Node* node = m_root.get();

    auto collectMWs = [&](const Node* n) {
        if (n && !n->middlewares.empty())
            nodeMWs.insert(nodeMWs.end(), n->middlewares.begin(), n->middlewares.end());
    };

    for (size_t i = 0; i < segs.size(); i++)
    {
        const auto& seg = segs[i];
        const Node* matched = nullptr;
        const Node* wildcard = nullptr;
        const Node* param = nullptr;

        for (const auto& c : node->children)
        {
            if (c->segment == seg)
                matched = c.get();
            else if (c->segment == "*")
                wildcard = c.get();
            else if (!c->segment.empty() && c->segment[0] == ':')
                param = c.get();
        }

        if (matched)       { node = matched;  collectMWs(matched); }
        else if (param)    { node = param;    collectMWs(param); break; }
        else if (wildcard) { collectMWs(wildcard); break; }
        else               { break; }
    }

    // 空路径（如 "/" 匹配 "/*"）需检查根节点的通配符子节点
    if (segs.empty())
    {
        for (const auto& c : node->children)
        {
            if (c->segment == "*") { collectMWs(c.get()); break; }
        }
    }

    return ExecuteChain(task, data, dlen, nodeMWs, handler);
}

int ZmHttpRouter::ExecuteChain(ZmHttpdTask* task, const BYTE* data, size_t dlen,
                                const std::vector<Middleware>& nodeMWs, Handler handler)
{
    // 合并链：全局中间件 + 节点中间件 + 处理器
    // 使用 shared_ptr 让 lambda 捕获存活到链执行完
    struct ChainFrame {
        std::vector<Middleware> chain;
        int index = 0;
        bool shortCircuited = false;
        int handlerResult = 0;
    };
    auto frame = std::make_shared<ChainFrame>();

    // 构建链
    frame->chain = m_globalMiddlewares;
    frame->chain.insert(frame->chain.end(), nodeMWs.begin(), nodeMWs.end());

    // next() 递归函数：推进到下一个中间件，链尾执行处理器
    std::function<void()> next;
    next = [this, task, data, dlen, handler, frame, &next]() {
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
            return;  // 中间件已调用 next()，等递归回来
        }
        // 链尾：执行处理器
        frame->handlerResult = handler(task, data, dlen);
    };

    next();

    if (frame->shortCircuited)
        return 0;  // 中间件短路，无返回值

    return frame->handlerResult;
}

// ============================================================================
// 路径参数
// ============================================================================

std::string ZmHttpRouter::GetParam(const char* name)
{
    auto it = t_params.find(name);
    return (it != t_params.end()) ? it->second : std::string();
}
