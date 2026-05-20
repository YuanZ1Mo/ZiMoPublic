#include "zm_net_tap_dnr.h"
#include "zm_net_dns.h"
#include "zm_net_tap.h"

ZmTapDomainNameResolver::ZmTapDomainNameResolver()
    : ZmThread("ZmTapDomainNameResolver")
{

}

ZmTapDomainNameResolver::~ZmTapDomainNameResolver()
{

}

ZM_DNS_ASYNC_REQUEST* ZmTapDomainNameResolver::Resolve(struct event_base* evbase,
    const char* hostname, uint16_t port,
    FN_OnNameResolved cb, ZM_TAP_CTX* tap, uint32_t option)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    //_log.Trace("Pending-Resolve current_thread=%8X", SPThread::CurrentThreadID());
    _RESOLVE_TASK* task = _task_pool.Apply();

    memset(task, 0, sizeof(_RESOLVE_TASK));
    task->delegate = this;

    snprintf(task->hostname, sizeof(task->hostname), "%s", hostname);
    task->port = port;
    task->tap = tap;
    task->option = option;
    task->pfn_onresolved = cb;
    if (evbase)
    {
        task->rsp_event = event_new(evbase, -1, EV_PERSIST | EV_READ, ZmTapDomainNameResolver::OnEventResolved, task);
        event_add(task->rsp_event, NULL);
        //_log.Trace("Resolve() tsp_event=%p", task->rsp_event);
    }

    task->state = STATE_PENDING;
    _task_pends.push_back(task);
    //SP_DEV_LOGT("%s tap[%p] pends hostname=%s, task[%p], callback=%p",
    //    __SP_FUNC__, tap, task->hostname, task, cb);

    Start();
    return task;
}

void ZmTapDomainNameResolver::Cancel(ZM_DNS_ASYNC_REQUEST* req)
{
    if (req)
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);

        _RESOLVE_TASK* task = (_RESOLVE_TASK*)req;

        task->state = STATE_CANCELED;
        if (task->rsp_event)
        {
            event_free(task->rsp_event);
        }
        task->rsp_event = NULL;
        task->pfn_onresolved = NULL;
        /** 为了避免与正在工作的任务冲突，这里不做回收处理，而是在任务检查的时候做回收 */
    }
}

ZmTapDomainNameResolver::_RESOLVE_TASK* ZmTapDomainNameResolver::GetPendingTask()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    for (std::vector<_RESOLVE_TASK*>::size_type i = 0; i < _task_pends.size(); i++)
    {
        _RESOLVE_TASK* task = _task_pends.at(i);
        if (task->state == STATE_PENDING)
        {
            return task;
        }
        else if (task->state == STATE_CANCELED)
        {
            _task_pends.erase(_task_pends.begin() + i);
            Release(task, false);
        }
        else
        {
            // SP_DEV_LOGT("LookupPending task[%p], state=%d", task, task->state);
            i++;
        }
    }
    return (_RESOLVE_TASK*)NULL;
}

void ZmTapDomainNameResolver::Run()
{
    int times = 0;
    while (IsRunning())
    {
        _RESOLVE_TASK* task = GetPendingTask();
        // SP_DEV_LOGT("LookupPending returns task[%p], remains=%ld", task, (long)_task_pends.size());
        if (task)
        {
            times = 0;
            ResolveRequest(task);
        }
        else if (times < 100)
        {
            times++;
            // SP_DEV_LOGT("%s times=%d", __SP_FUNC__, times);
            ZmSleepMS(20);
        }
        else
        {
            break;
        }
    }
}

void ZmTapDomainNameResolver::ResolveRequest(_RESOLVE_TASK* task)
{
    //_log.Trace("%s task[%p] %s", __SP_FUNC__, task, task->hostname);

    task->state = STATE_RESOLVING;

    task->salen = ZmNetDNS::GetAddressByName(&task->sa, task->hostname, task->port ? task->port : 80);

    //_log.Trace("%s task[%p] %s, state=%d returns salen=%d",
    //    __SP_FUNC__, task, task->hostname, task->state, task->salen);

    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (task->state == STATE_RESOLVING)
    {
        if (task->salen)
        {
            ZmNetIP::SockaddrToStr((struct sockaddr*)&task->sa, task->ipaddr, sizeof(task->ipaddr));
            task->state = STATE_RESOLVED;
        }
        else
        {
            task->state = STATE_ERROR;
        }
        if (task->rsp_event)
        {
            event_active(task->rsp_event, 0, 0);
        }
        else
        {
            FireResponse(task);
        }
    }
    else
    {
        Release(task, true);
    }
}

void ZmTapDomainNameResolver::FireResponse(_RESOLVE_TASK* task)
{
    //SP_DEV_LOGT("%s, task[%p], pfn=%p", __SP_FUNC__, task, task->pfn_onresolved);
    if (task->pfn_onresolved)
    {
        task->pfn_onresolved(task->tap, task->option, task->hostname,
            task->salen ? 0 : -1, &task->sa, task->salen, task->ipaddr);
    }
    Release(task, true);
}

void ZmTapDomainNameResolver::Release(_RESOLVE_TASK* task, bool erasePending)
{
    if (erasePending)
    {
        for (std::vector<_RESOLVE_TASK*>::size_type i = 0; i < _task_pends.size(); i++)
        {
            _RESOLVE_TASK* t = _task_pends.at(i);
            if (t == task)
            {
                _task_pends.erase(_task_pends.begin() + i);
                break;
            }
        }
    }
    if (task->rsp_event)
    {
        event_free(task->rsp_event);
    }
    _task_pool.Release(task);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// SPTapNameResolver
//
/**
 * TODO: 2019.04.29 改进设想
 *  1、在 SPTapNameResolver::Resolve 通过 std::async(...) 创建一个 future 放入队列
 *  2、在 SPTapNameResolver::Run 中遍历 future 队列获取结果然后响应
 */
void ZmTapDomainNameResolver::OnEventResolved(evutil_socket_t fd, short what, void* ctx)
{
    //SP_DEV_LOGT("[dns] SPTapNameResolver::OnEvent_Resolved() current_thread=%8X", SPThread::CurrentThreadID());
    _RESOLVE_TASK* task = (_RESOLVE_TASK*)ctx;
    if (task)
    {
        struct event* ev = task->rsp_event;
        task->rsp_event = NULL;
        ZmTapDomainNameResolver* ns = (ZmTapDomainNameResolver*)task->delegate;
        if (ns)
        {
            std::lock_guard<std::recursive_mutex> lock(ns->_mutex);
            ns->FireResponse(task);
        }
        else
        {
            memset(task, 0, sizeof(_RESOLVE_TASK));
        }
        if (ev)
        {
            event_free(ev);
        }
    }
}