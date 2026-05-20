#ifndef ZM_WEBSOCKET_PROTOCOL_H
#define ZM_WEBSOCKET_PROTOCOL_H

#include <string>
#include <cstdint>
#include <cstddef>

// WebSocket opcode (RFC 6455)
enum ZmWsOpcode {
    ZM_WS_OPCODE_CONTINUATION = 0x0,
    ZM_WS_OPCODE_TEXT         = 0x1,
    ZM_WS_OPCODE_BINARY       = 0x2,
    ZM_WS_OPCODE_RESERVED_3   = 0x3,
    ZM_WS_OPCODE_RESERVED_4   = 0x4,
    ZM_WS_OPCODE_RESERVED_5   = 0x5,
    ZM_WS_OPCODE_RESERVED_6   = 0x6,
    ZM_WS_OPCODE_RESERVED_7   = 0x7,
    ZM_WS_OPCODE_CLOSE        = 0x8,
    ZM_WS_OPCODE_PING         = 0x9,
    ZM_WS_OPCODE_PONG         = 0xA
};

// WebSocket close status codes (RFC 6455 Section 7.4)
enum ZmWsCloseCode {
    ZM_WS_CLOSE_NORMAL          = 1000,
    ZM_WS_CLOSE_GOING_AWAY      = 1001,
    ZM_WS_CLOSE_PROTO_ERR       = 1002,
    ZM_WS_CLOSE_UNSUPPORTED     = 1003,
    ZM_WS_CLOSE_INVALID_DATA    = 1007,
    ZM_WS_CLOSE_POLICY_ERR      = 1008,
    ZM_WS_CLOSE_DATA_TOO_BIG    = 1009,
    ZM_WS_CLOSE_EXTENSION_ERR   = 1010,
    ZM_WS_CLOSE_UNEXPECTED_ERR  = 1011
};

/**
 * WebSocket 帧编码。
 * 服务端发送不 mask，客户端发送需 mask。
 * @param payload    数据载荷
 * @param payloadLen 载荷长度
 * @param opcode     帧类型 (ZM_WS_OPCODE_TEXT / ZM_WS_OPCODE_BINARY / ...)
 * @param outFrameLen 输出帧总长度
 * @param mask       是否设置 mask 位（客户端发送时为 true）
 * @return 动态分配的帧缓冲区，调用者负责 free()；失败返回 nullptr
 */
char* zm_ws_encode_frame(const char* payload, size_t payloadLen,
    uint8_t opcode, size_t* outFrameLen, bool mask = false);

/**
 * WebSocket 帧解码。
 * 客户端→服务端帧需 unmask，服务端→客户端帧无 mask。
 * @param data         接收到的原始数据
 * @param dataLen      数据总长度
 * @param outOpcode    输出帧 opcode
 * @param outPayloadLen 输出载荷长度
 * @param consumedBytes 输出本帧消耗的字节数（0 表示数据不足一个完整帧）
 * @return 解码后的载荷数据（调用者负责 free()）；数据不足时返回 nullptr 且 consumedBytes=0
 */
char* zm_ws_decode_frame(const char* data, size_t dataLen,
    uint8_t* outOpcode, size_t* outPayloadLen, size_t* consumedBytes);

/**
 * 构造客户端 WebSocket 握手请求（HTTP Upgrade）。
 * @param host 目标主机
 * @param port 目标端口
 * @param path WebSocket 路径（默认 "/"）
 * @return 完整的 HTTP Upgrade 请求字符串
 */
std::string zm_ws_build_handshake_request(const char* host, uint16_t port, const char* path = "/");

/**
 * 验证服务端握手响应。
 * 检查 HTTP 状态码 101 和 Sec-WebSocket-Accept 头。
 * @param response 服务端返回的原始响应
 * @param responseLen 响应长度
 * @param expectedKey 客户端发送的 Sec-WebSocket-Key（用于计算期望的 Accept 值）
 * @return 验证通过返回 true
 */
bool zm_ws_validate_handshake_response(const char* response, size_t responseLen,
    const std::string& expectedKey);

/**
 * 生成随机 Sec-WebSocket-Key（16 字节 base64 编码）。
 */
std::string zm_ws_generate_key();

/**
 * 根据 Sec-WebSocket-Key 计算 Sec-WebSocket-Accept。
 * accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
 */
std::string zm_ws_compute_accept_key(const std::string& key);

#endif /* ZM_WEBSOCKET_PROTOCOL_H */
