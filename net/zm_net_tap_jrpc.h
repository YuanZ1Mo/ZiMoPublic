#ifndef ZM_NET_TAP_JRPC_H
#define ZM_NET_TAP_JRPC_H

#include <string>

#include "zm_net_tap.h"

/** @brief JRPC 请求读取回调函数类型
 *  @param tap     触发该回调的 TAP 上下文
 *  @param reqData JRPC 请求 JSON 字符串 */
using TapDelegateJrpcRequestReadCB = std::function<void(ZM_TAP_CTX* tap, const char* reqData)>;

/**
 * @brief JRPC 协议委托，处理经 Hub 转发或内部注入的 JSON-RPC 请求/响应
 *
 * 继承 ZmTapDelegate，实现基于长度前缀帧格式（4 字节大端长度 + JSON 体）的 JRPC 解析。
 * 工作于 ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC 模式，由 Hub 代理在协议探测后切换至此。
 * 业务处理通过 ZmThreadPool::InvokeLater 投递到工作线程，避免阻塞事件循环。
 */
class ZmTapDelegateJRPC : public ZmTapDelegate
{
public:
	ZmTapDelegateJRPC();
	virtual ~ZmTapDelegateJRPC();

	/** @brief 设置 JRPC 请求到达时的外部回调
	 *  @param cb 回调函数，参数为 TAP 上下文和请求 JSON 字符串 */
	void SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb);

public:
	// --- ZmTapDelegate 接口实现 ---

	/** @brief 请求端接受连接（JRPC 模式不接受直连，始终返回 false）
	 *  @return false */
	bool OnTapRequesterAccept(ZM_TAP_CTX* tap, evutil_socket_t fd,
		struct sockaddr* address) override;

	/** @brief delegate 内部事件回调（JRPC 模式下无内部事件，空实现） */
	void OnTapDelegateEvent(short what) override;

	/** @brief 请求端数据到达回调，解析长度前缀帧格式的 JRPC 请求
	 *
	 * 协议格式为 [4 字节大端长度] [JSON 数据体]。
	 * 解析完成后将请求通过 ZmThreadPool::InvokeLater 投递到工作线程处理。
	 *
	 * @param tap       目标 TAP 上下文
	 * @param app_input libevent 输入缓冲区
	 * @param datalen   本次到达的数据长度 */
	void OnTapRequesterRead(ZM_TAP_CTX* tap, struct evbuffer* app_input, size_t datalen) override;

	/** @brief 回传数据到达回调，将外部 JRPC 响应通过长度前缀帧写回 client
	 *
	 * 外部业务层处理完毕后通过 BackChainPop + OnTapDelegateBackEvent 触发本回调。
	 *
	 * @param tap 目标 TAP 上下文 */
	void OnTapDelegateBackEvent(ZM_TAP_CTX* tap) override;

private:
	/** @brief 向 bufferevent 输出端写入 4 字节大端长度前缀 + JSON 响应体
	 *
	 * 使用 evbuffer_add_iovec 单次提交长度头与 JSON 体，减少锁和链表操作。
	 * 写入后若回传链已空则设置 30 秒超时定时器，到期未响应则自动回收 TAP。
	 *
	 * @param tap       目标 TAP 上下文
	 * @param json_str  响应 JSON 字符串指针
	 * @param data_len  JSON 字符串长度 */
	void WriteResponse(ZM_TAP_CTX* tap, const char* json_str, size_t data_len);

	/** @brief JRPC 请求到达时的外部回调 */
	TapDelegateJrpcRequestReadCB m_tapDelegateJrpcRequestReadCB;
};

#endif  // ZM_NET_TAP_JRPC_H
