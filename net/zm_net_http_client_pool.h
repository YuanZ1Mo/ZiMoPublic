#ifndef ZM_NET_HTTP_CLIENT_POOL_H
#define ZM_NET_HTTP_CLIENT_POOL_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

class ZmHttpClient;

/**
 * @brief ZmHttpClient 实例池(镜像 ZmReqLoopPool 语义)
 *
 * 预创建(每实例自带循环线程)+ 扩容(上限)+ 排队(50ms 轮询,带中止标志)。
 * 纯净契约:回池前必须无在飞请求(CancelAll)+ 全部连接已销毁(CloseAll),
 * 否则回池调用断言失败(Release 返回 false)。
 */
class ZmHttpClientPool
{
public:
    ZmHttpClientPool();
    ~ZmHttpClientPool();

    bool Init(int preCreate, int maxCount);
    ZmHttpClient* Acquire(int timeoutMs = 0, const std::atomic<bool>* abort = nullptr);
    bool Release(ZmHttpClient* client);   ///< 纯净校验后回池
    void Shutdown();                      ///< 终止池并删除全部实例;调用方须保证此前已无在借客户端

private:
    std::vector<ZmHttpClient*> m_all;
    std::vector<ZmHttpClient*> m_idle;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    int                     m_maxCount;
    bool                    m_shutdown;
};

#endif // ZM_NET_HTTP_CLIENT_POOL_H
