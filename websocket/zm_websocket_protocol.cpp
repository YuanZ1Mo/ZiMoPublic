#include "zm_websocket_protocol.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "zm_socket_utils.h"

// SHA-1 和 Base64 使用 OpenSSL
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

static const char* ZM_WS_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ======================== Base64 编码 ========================

static const char base64_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string zm_base64_encode(const unsigned char* data, size_t len)
{
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3)
    {
        unsigned int val = (data[i] << 16);
        if (i + 1 < len) val |= (data[i + 1] << 8);
        if (i + 2 < len) val |= data[i + 2];

        result.push_back(base64_table[(val >> 18) & 0x3F]);
        result.push_back(base64_table[(val >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? base64_table[(val >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? base64_table[val & 0x3F] : '=');
    }

    return result;
}

static std::vector<unsigned char> zm_base64_decode(const std::string& input)
{
    static const int decode_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::vector<unsigned char> result;
    result.reserve(input.size() * 3 / 4);

    int val = 0, valb = -8;
    for (unsigned char c : input)
    {
        if (decode_table[c] == -1) break;
        val = (val << 6) + decode_table[c];
        valb += 6;
        if (valb >= 0)
        {
            result.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return result;
}

// ======================== SHA-1 ========================

static std::vector<unsigned char> zm_sha1_hash(const char* data, size_t len)
{
    std::vector<unsigned char> hash(SHA_DIGEST_LENGTH);
    SHA1(reinterpret_cast<const unsigned char*>(data), len, hash.data());
    return hash;
}

// ======================== 公共接口 ========================

std::string zm_ws_generate_key()
{
    unsigned char key_bytes[16];
    RAND_bytes(key_bytes, sizeof(key_bytes));
    return zm_base64_encode(key_bytes, 16);
}

std::string zm_ws_compute_accept_key(const std::string& key)
{
    std::string combined = key + ZM_WS_MAGIC_STRING;
    auto hash = zm_sha1_hash(combined.c_str(), combined.length());
    return zm_base64_encode(hash.data(), hash.size());
}

// ======================== 帧编码 ========================

char* zm_ws_encode_frame(const char* payload, size_t payloadLen,
    uint8_t opcode, size_t* outFrameLen, bool mask)
{
    if (outFrameLen == nullptr) return nullptr;

    // 计算帧头大小
    size_t headerSize = 2; // 最小: FIN+opcode + MASK+payload_len(7bit)

    if (payloadLen <= 125)
    {
        // 7bit payload length, headerSize = 2
    }
    else if (payloadLen <= 65535)
    {
        headerSize += 2; // 16bit extended length
    }
    else
    {
        headerSize += 8; // 64bit extended length
    }

    if (mask)
    {
        headerSize += 4; // masking key
    }

    size_t frameLen = headerSize + payloadLen;
    char* frame = (char*)malloc(frameLen);
    if (!frame)
    {
        return nullptr;
    }

    // 第1字节: FIN(1) + RSV1-3(000) + opcode(4bit)
    frame[0] = static_cast<char>(0x80 | (opcode & 0x0F));

    // 第2字节: MASK(1bit) + payload length(7bit)
    if (payloadLen <= 125)
    {
        frame[1] = static_cast<char>((mask ? 0x80 : 0x00) | payloadLen);
    }
    else if (payloadLen <= 65535)
    {
        frame[1] = static_cast<char>((mask ? 0x80 : 0x00) | 126);
        uint16_t netLen = htons(static_cast<uint16_t>(payloadLen));
        memcpy(&frame[2], &netLen, sizeof(netLen));
    }
    else
    {
        frame[1] = static_cast<char>((mask ? 0x80 : 0x00) | 127);
        uint64_t netLen = ((uint64_t)htonl((uint32_t)(payloadLen & 0xFFFFFFFF)) << 32) |
                          (uint64_t)htonl((uint32_t)(payloadLen >> 32));
        memcpy(&frame[2], &netLen, sizeof(netLen));
    }

    // Masking key
    size_t payloadOffset = headerSize;
    if (mask)
    {
        unsigned char maskKey[4];
        RAND_bytes(maskKey, 4);
        memcpy(frame + headerSize - 4, maskKey, 4);

        // 对 payload 进行 mask
        if (payload && payloadLen > 0)
        {
            for (size_t i = 0; i < payloadLen; ++i)
            {
                frame[payloadOffset + i] = payload[i] ^ maskKey[i % 4];
            }
        }
    }
    else
    {
        if (payload && payloadLen > 0)
        {
            memcpy(frame + payloadOffset, payload, payloadLen);
        }
    }

    *outFrameLen = frameLen;
    return frame;
}

// ======================== 帧解码 ========================

char* zm_ws_decode_frame(const char* data, size_t dataLen,
    uint8_t* outOpcode, size_t* outPayloadLen, size_t* consumedBytes)
{
    if (!data || dataLen < 2 || !outOpcode || !outPayloadLen || !consumedBytes)
    {
        if (consumedBytes) *consumedBytes = 0;
        return nullptr;
    }

    *consumedBytes = 0;
    *outPayloadLen = 0;
    *outOpcode = 0;

    // 解析前2字节
    uint8_t byte0 = static_cast<uint8_t>(data[0]);
    uint8_t byte1 = static_cast<uint8_t>(data[1]);

    *outOpcode = byte0 & 0x0F;
    bool frameMask = (byte1 & 0x80) != 0;
    uint64_t payloadLen = byte1 & 0x7F;

    size_t headerSize = 2;

    if (payloadLen == 126)
    {
        // 16bit extended length
        if (dataLen < 4) return nullptr;
        uint16_t netLen;
        memcpy(&netLen, data + 2, sizeof(netLen));
        payloadLen = ntohs(netLen);
        headerSize = 4;
    }
    else if (payloadLen == 127)
    {
        // 64bit extended length
        if (dataLen < 10) return nullptr;
        uint64_t netLen;
        memcpy(&netLen, data + 2, sizeof(netLen));
        payloadLen = ((uint64_t)ntohl((uint32_t)(netLen & 0xFFFFFFFF)) << 32) |
                     (uint64_t)ntohl((uint32_t)(netLen >> 32));
        headerSize = 10;
    }

    if (frameMask)
    {
        headerSize += 4; // masking key
    }

    // 检查数据是否够一个完整帧
    if (dataLen < headerSize + payloadLen)
    {
        return nullptr;
    }

    *consumedBytes = static_cast<size_t>(headerSize + payloadLen);
    *outPayloadLen = static_cast<size_t>(payloadLen);

    if (payloadLen == 0)
    {
        return nullptr; // 无 payload，返回 nullptr 但 consumedBytes > 0
    }

    char* payload = (char*)malloc(static_cast<size_t>(payloadLen));
    if (!payload)
    {
        *consumedBytes = 0;
        return nullptr;
    }

    const char* payloadSrc = data + headerSize;

    if (frameMask)
    {
        const unsigned char* maskKey = reinterpret_cast<const unsigned char*>(data + headerSize - 4);
        for (uint64_t i = 0; i < payloadLen; ++i)
        {
            payload[i] = payloadSrc[i] ^ maskKey[i % 4];
        }
    }
    else
    {
        memcpy(payload, payloadSrc, static_cast<size_t>(payloadLen));
    }

    return payload;
}

// ======================== 握手 ========================

std::string zm_ws_build_handshake_request(const char* host, uint16_t port, const char* path)
{
    std::string key = zm_ws_generate_key();

    char portStr[8] = { 0 };
    // 非标准端口时包含端口号
    if (port != 80)
    {
        snprintf(portStr, sizeof(portStr), ":%u", port);
    }

    std::string request;
    request.reserve(256);
    request += "GET ";
    request += (path ? path : "/");
    request += " HTTP/1.1\r\n";
    request += "Host: ";
    request += (host ? host : "127.0.0.1");
    request += portStr;
    request += "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: ";
    request += key;
    request += "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "\r\n";

    return request;
}

bool zm_ws_validate_handshake_response(const char* response, size_t responseLen,
    const std::string& expectedKey)
{
    if (!response || responseLen == 0) return false;

    std::string resp(response, responseLen);

    // 检查 HTTP/1.1 101
    if (resp.find("101") == std::string::npos) return false;

    // 检查 Upgrade: websocket
    if (resp.find("Upgrade: websocket") == std::string::npos &&
        resp.find("Upgrade: Websocket") == std::string::npos &&
        resp.find("upgrade: websocket") == std::string::npos)
    {
        return false;
    }

    // 计算 Sec-WebSocket-Accept 并比对
    std::string expectedAccept = zm_ws_compute_accept_key(expectedKey);

    // 查找 Sec-WebSocket-Accept 头
    std::string acceptHeader = "Sec-WebSocket-Accept: ";
    size_t pos = resp.find(acceptHeader);
    if (pos == std::string::npos)
    {
        // 尝试小写
        acceptHeader = "sec-websocket-accept: ";
        pos = resp.find(acceptHeader);
        if (pos == std::string::npos) return false;
    }

    pos += acceptHeader.length();
    size_t endPos = resp.find("\r\n", pos);
    if (endPos == std::string::npos) return false;

    std::string acceptValue = resp.substr(pos, endPos - pos);
    // 去除前后空格
    while (!acceptValue.empty() && acceptValue.back() == ' ') acceptValue.pop_back();
    while (!acceptValue.empty() && acceptValue.front() == ' ') acceptValue.erase(acceptValue.begin());

    return acceptValue == expectedAccept;
}
