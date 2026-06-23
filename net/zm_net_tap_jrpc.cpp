#include "zm_net_tap_jrpc.h"

#include "../spdlog/zm_logger.h"
#include "../util/zm_util_thread.h"

#include <../libevent/include/event2/buffer.h>

// ============================================================================
// 构造与析构
// ============================================================================

ZmTapDelegateJRPC::ZmTapDelegateJRPC()
	: ZmTapDelegate()
	, m_threadPool(nullptr)
{
	TapDelegateName("ZmTapDelegateJRPC");
	m_threadPool = new ZmThreadPool(4, "JrpcDelegate");
}

ZmTapDelegateJRPC::~ZmTapDelegateJRPC()
{
	if (m_threadPool)
	{
		delete m_threadPool;
		m_threadPool = nullptr;
	}
}

void ZmTapDelegateJRPC::StopThreadPool()
{
	if (m_threadPool)
	{
		delete m_threadPool;
		m_threadPool = nullptr;
	}
}

// ============================================================================
// 回调设置
// ============================================================================

void ZmTapDelegateJRPC::SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb)
{
	m_tapDelegateJrpcRequestReadCB = cb;
}

// ============================================================================
// ZmTapDelegate 接口实现
// ============================================================================

bool ZmTapDelegateJRPC::OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd,
	struct sockaddr* address)
{
	ZM_UNUSED(tap);
	ZM_UNUSED(fd);
	ZM_UNUSED(address);
	return false;
}

void ZmTapDelegateJRPC::OnTapDelegateEvent(short what)
{
	ZM_UNUSED(what);
}

/**
 * @brief 请求端数据到达回调，解析长度前缀帧格式的 JRPC 请求
 *
 * 协议格式：[4字节大端长度] [JSON 数据体]
 * 处理流程:
 *   ① 首次到达时从头部读取 4 字节长度头，获取消息总长度
 *   ② 分多次读取完整消息体（使用 evbuffer_copyout 避免 evbuffer_pullup 的线性化开销）
 *   ③ 收齐完整消息后通过 ZmThreadPool::InvokeLater 投递到工作线程，避免阻塞事件循环
 */
void ZmTapDelegateJRPC::OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen)
{
	if (tap->delegate->TapDelegateMode() != m_mode)
		return;

	//PUBLIC_LOG_INFO("Received JRPC message forwarded by HUB, TAP:{}, datalen: {}", (void*)tap, datalen);

	// 首次到达：解析 4 字节长度头
	if (tap->requester_data_len == 0)
	{
		if (datalen < 4)
			return;

		const uint32_t* p_msg_len = (const uint32_t*)evbuffer_pullup(app_input, 4);
		if (!p_msg_len)
		{
			PUBLIC_LOG_ERROR("Tap: {}, evbuffer_pullup failed in JRPC header", (void*)tap);
			tap->Drop("JRPC header pullup failed");
			return;
		}

		uint32_t msg_len = ntohl(*p_msg_len);
		if (msg_len > ZM_BUF_SIZE_4M)
		{
			PUBLIC_LOG_ERROR("Tap: {}, JRPC request too large: {} bytes (max {})",
				(void*)tap, msg_len, (size_t)ZM_BUF_SIZE_4M);
			tap->Drop("JRPC request too large");
			return;
		}

		ZmTapContext::SetOptData(tap, msg_len + 1);
		tap->requester_data_len = msg_len;
		tap->requester_received_len = 0;
		evbuffer_drain(app_input, 4);
		datalen -= 4;
	}

	// 增量读取 JSON 体
	size_t remain = tap->requester_data_len - tap->requester_received_len;
	size_t read_len = ZM_MIN(datalen, remain);
	ev_ssize_t copy_ret = evbuffer_copyout(app_input,
		tap->requester_data + tap->requester_received_len, read_len);
	if (copy_ret < 0 || (size_t)copy_ret != read_len)
	{
		PUBLIC_LOG_ERROR("Tap: {}, evbuffer_copyout failed in JRPC body, expected:{}, got:{}",
			(void*)tap, read_len, copy_ret);
		tap->Drop("JRPC body copyout failed");
		return;
	}
	tap->requester_received_len += (uint32_t)read_len;
	evbuffer_drain(app_input, read_len);

	// 未收齐完整消息则等待后续数据
	if (tap->requester_received_len < tap->requester_data_len)
		return;

	//PUBLIC_LOG_INFO("Received JRPC message forwarded by HUB, TAP:{}, content:{}",
	//	(void*)tap, (const char*)tap->requester_data);

	// 有外部回调：拷贝数据后投递到工作线程，业务层使用 delegate 的异步方法操作 TAP
	if (m_tapDelegateJrpcRequestReadCB && m_threadPool)
	{
		ZmTapContext::BackChainPush(tap, this);
		std::string reqCopy((const char*)tap->requester_data);
		auto cb = m_tapDelegateJrpcRequestReadCB;
		m_threadPool->Submit([tap, reqCopy, cb]() {
			cb(tap, reqCopy.c_str());
			}, "Business");
	}
	// 无外部回调：直接返回错误
	else
	{
		PUBLIC_LOG_INFO("Internal portal does not have JRPC processing channel set up, TAP:{}", (void*)tap);

		ZMJSON json_rsp;
		json_rsp["error"] = ZMJSON{
			{"code", 32000},
			{"message", "Internal portal does not have JRPC processing channel set up"}
		};
		std::string json_str = ZMJSON(json_rsp).dump();
		WriteResponse(tap, json_str.data(), json_str.size());
	}
}

/**
 * @brief 回传数据到达回调，将外部 JRPC 响应写回客户端
 *
 * 业务层处理完成后通过 BackChainPop + SetOnBackData 设置回传数据，
 * 再调用 OnTapDelegateBackEvent 触发本回调，将响应通过长度前缀帧写回客户端。
 */
void ZmTapDelegateJRPC::OnTapDelegateBackEvent(ZM_TAP_CTX* tap)
{
	//PUBLIC_LOG_INFO("Received reply message from JRPC external callback, TAP:{}, onback_data={}",
	//	(void*)tap, (const char*)tap->onback_data);

	tap->delegate = this;
	WriteResponse(tap, (const char*)tap->onback_data, tap->onback_dlen);
}

// ============================================================================
// 内部方法
// ============================================================================

/**
 * @brief 向 bufferevent 输出端写入 4 字节大端长度前缀 + JSON 响应体
 *
 * 使用 evbuffer_add_iovec 将长度头与 JSON 体单次提交到输出缓冲区，
 * 减少锁获取和内部链表操作次数。
 * 写入后若回传链已空则设置 30 秒超时定时器——若此期间发生 EOF/ERROR
 * 则由 OnRequesterEventCB 立即回收 TAP。
 */
void ZmTapDelegateJRPC::WriteResponse(ZM_TAP_CTX* tap, const char* json_str, size_t data_len)
{
	//PUBLIC_LOG_INFO("Received JRPC Response, TAP:{}, content: {}", (void*)tap, json_str);

	uint32_t rsp_len = htonl((uint32_t)data_len);

	struct evbuffer_iovec iov[2];
	iov[0].iov_base = &rsp_len;
	iov[0].iov_len = 4;
	iov[1].iov_base = (void*)json_str;
	iov[1].iov_len = data_len;

	int ret = (int)evbuffer_add_iovec(bufferevent_get_output(tap->requester_bev), iov, 2);
	if (ret < 0)
	{
		PUBLIC_LOG_ERROR("evbuffer_add_iovec failed, TAP:{}, ret:{}", (void*)tap, ret);
	}

	ret = bufferevent_flush(tap->requester_bev, EV_WRITE, BEV_FLUSH);
	if (ret < 0)
	{
		PUBLIC_LOG_ERROR("bufferevent_flush failed, TAP:{}, ret:{}", (void*)tap, ret);
	}

	// ★ 标记数据已写出：后续 tap->Drop() → ReleasePair1 看到此标记跳
	//    过 EOF 触发，pair0 读到响应数据后自行回收，从根源消灭双回调。
	if (tap->pair_handle)
		tap->pair_handle->MarkDataWritten();

	if (ZmTapContext::IsBackChainEmpty(tap))
	{
		tap->Drop();
	}
}
