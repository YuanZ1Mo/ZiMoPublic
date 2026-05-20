#include "zm_util_str.h"

#include <algorithm>
#include <random>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#pragma warning(disable:4018)
#pragma warning(disable:4996)

static const char g_hexUpper[17] = "0123456789ABCDEF";
static const char g_hexLower[17] = "0123456789abcdef";


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// extern "C"

extern "C"
{
    char* zm_strsep(char** stringp, const char* delim)
    {
        char* start = *stringp;
        char* p = start ? strpbrk(start, delim) : NULL;
        if (p)
        {
            *p = '\0';
            *stringp = p + 1;
        }
        else
        {
            *stringp = NULL;
        }
        return start;
    }

    char* zm_strndup(const char* s1, size_t n)
    {
        char* clone = (char*)malloc(n + 1);
        if (clone)
        {
            strncpy_s(clone, n + 1, s1, n);
            clone[n] = '\0';
        }
        return clone;
    }
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 辅助函数

inline BYTE zm_hex_to_char(char hex)
{
    if (hex >= '0' && hex <= '9')
    {
        return hex - '0';
    }
    else if (hex >= 'a' && hex <= 'f')
    {
        return hex - 'a' + '\x0A';
    }
    else if (hex >= 'A' && hex <= 'F')
    {
        return hex - 'A' + '\x0A';
    }
    return 0;
}

inline char zm_char_to_hex(char code)
{
    return g_hexLower[code & 15];
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 基础判断

bool ZmString::IsEmpty(const char* str)
{
    return (NULL == str || '\0' == str[0]);
}

bool ZmString::Equals(const char* s1, const char* s2)
{
    return (NULL == s1) ? (NULL == s2) : (NULL != s2 && 0 == strcmp(s1, s2));
}

bool ZmString::IsNumeric(const char* str)
{
    if (!IsEmpty(str))
    {
        size_t len = strlen(str);
        for (size_t i = 0; i < len; i++)
        {
            if (str[i] < '0' || str[i]>'9')
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool ZmString::HasExtendedAscii(const char* str)
{
    if (!IsEmpty(str))
    {
        size_t len = strlen(str);
        for (size_t i = 0; i < len; i++)
        {
            if (str[i] & 0x80)
            {
                return true;
            }
        }
    }
    return false;
}

bool ZmString::IsUTF8(const BYTE* str, size_t len)
{
    if (NULL == str || len < 1) { return false; }
    else
    {
        bool isAscii = true;
        for (size_t i = 0; i < len && isAscii; i++) { isAscii = ((str[i] & 0x80) == 0x00); }
        if (isAscii) { return true; }
    }
    int expectedLength = 0;
    for (size_t i = 0; i < len; i++)
    {
        if ((str[i] & 0x80) == 0x00) { expectedLength = 1; }
        else if ((str[i] & 0xe0) == 0xc0) { expectedLength = 2; }
        else if ((str[i] & 0xf0) == 0xe0) { expectedLength = 3; }
        else if ((str[i] & 0xf8) == 0xf0) { expectedLength = 4; }
        else if ((str[i] & 0xfc) == 0xf8) { expectedLength = 5; }
        else if ((str[i] & 0xfe) == 0xfc) { expectedLength = 6; }
        else { return false; }
        while (--expectedLength > 0)
        {
            if (++i >= len) { return false; }
            if ((str[i] & 0xc0) != 0x80) { return false; }
        }
    }
    return true;
}

bool ZmString::LongerThan(const char* str, size_t len)
{
    for (size_t i = 0; i <= len; i++)
    {
        if (str[i] == 0)
        {
            return false;
        }
    }
    return true;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 大小写转换

void ZmString::Lower(std::string& str)
{
    std::transform(str.begin(), str.end(), str.begin(), ZmString::easyToLower);
}

std::string ZmString::LowerEx(const char* str)
{
    std::string lowers(str ? str : "");
    Lower(lowers);
    return lowers;
}

void ZmString::Upper(std::string& str)
{
    std::transform(str.begin(), str.end(), str.begin(), ZmString::easyToUpper);
}

std::string ZmString::UpperEx(const char* str)
{
    std::string upers(str ? str : "");
    Upper(upers);
    return upers;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 格式化与转换

std::string ZmString::Format(const char* fmt, ...)
{
    ZmByteBuffer buf(128);

    va_list ap;
    va_start(ap, fmt);

    va_list vtmp;
    va_copy(vtmp, ap);
    size_t dlen = vsnprintf(NULL, 0, fmt, vtmp);
    buf.Expand(dlen + 1);
    va_end(vtmp);

    vsnprintf(buf.Str(), buf.Size(), fmt, ap);
    va_end(ap);

    return std::string(buf.Str());
}

#define NUM_BUFSIZE (sizeof(long) * 8 + 1)

static char g_innerBuf[NUM_BUFSIZE] = { 0 };
char* ZmString::L_To_A(long num, char* str, int radix)
{
    int     i = 2;
    long    uarg;
    char* tail;
    char* head = str ? str : g_innerBuf;
    char    buf[NUM_BUFSIZE];
    char* ptr = head;

    memset(g_innerBuf, 0, sizeof(g_innerBuf));
    memset(buf, 0, sizeof(buf));
    if (36 < radix || 2 > radix)
    {
        radix = 10;
    }
    tail = &buf[NUM_BUFSIZE - 1];
    *tail-- = '\0';

    if (10 == radix && num < 0L)
    {
        *ptr++ = '-';
        uarg = (unsigned long)(-(num + 1)) + 1;
    }
    else
    {
        uarg = num;
    }

    if (uarg)
    {
        for (i = 1; uarg; ++i)
        {
            ldiv_t r = ldiv(uarg, radix);
            *tail-- = (char)(r.rem + ((9L < r.rem) ? ('A' - 10L) : '0'));
            uarg = r.quot;
        }
    }
    else
    {
        *tail-- = '0';
    }
    memcpy(ptr, ++tail, i);
    ptr[i] = '\0';

    return head;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 查找与匹配

int ZmString::Find(const char* haystack, const char* needle, size_t offset)
{
    if (NULL == haystack || NULL == needle) { return -1; }
    char* ptr = const_cast<char*>(haystack) + offset;
    char* str = strstr(ptr, needle);
    return str ? (int)(str - haystack) : -1;
}

int ZmString::RFind(const char* haystack, const char needle)
{
    if (NULL == haystack) { return -1; }
    for (int i = (int)(strlen(haystack) - 1); i >= 0; i--)
    {
        if (haystack[i] == needle)
        {
            return i;
        }
    }
    return -1;
}

int ZmString::Pos(const char* haystack, const char* needle)
{
    if (NULL != haystack && NULL != needle)
    {
        const char* str = strstr(haystack, needle);
        if (str)
        {
            return (int)(str - haystack);
        }
    }
    return -1;
}

bool ZmString::StartsWith(const char* base, const char* head)
{
    if (NULL != base && NULL != head)
    {
        size_t total = strlen(base);
        size_t needle = strlen(head);

        if (needle <= total)
        {
            for (size_t i = 0; i < needle; i++)
            {
                if (base[i] != head[i])
                {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool ZmString::EndsWith(const char* base, const char* tail)
{
    if (NULL != base && NULL != tail)
    {
        size_t blen = strlen(base);
        size_t slen = strlen(tail);
        return (blen >= slen) && (0 == strcmp(base + blen - slen, tail));
    }
    return false;
}

bool ZmString::WildcardMatch(const char* subject, const char* pattern)
{
    if (*pattern == '\0' && *subject == '\0')
    {
        return true;
    }

    if (*pattern == '*' && *(pattern + 1) != '\0' && *subject == '\0')
    {
        return false;
    }

    if (*pattern == '?' || *pattern == *subject)
    {
        return WildcardMatch(subject + 1, pattern + 1);
    }

    if (*pattern == '*')
    {
        return WildcardMatch(subject, pattern + 1) || WildcardMatch(subject + 1, pattern);
    }
    return false;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 裁剪与替换

char* ZmString::Trim(char* str)
{
    if (NULL != str)
    {
        while (isspace(*str))
        {
            str++;
        }
        if (*str == 0)
        {
            return str;
        }

        char* end = str + strlen(str) - 1;
        while (end > str && isspace(*end))
        {
            end--;
        }
        *(end + 1) = 0;
    }
    return str;
}

static const char g_strBlanks[] = "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10"
"\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F\x20";
void ZmString::TrimEx(std::string& str)
{
    str.erase(str.find_last_not_of(g_strBlanks) + 1);
    str.erase(0, str.find_first_not_of(g_strBlanks));
}

std::string ZmString::spp(std::string str, std::string src, std::string dst)
{
    size_t oldPos = 0;
    while (str.find(src, oldPos) != std::string::npos)
    {
        size_t start = str.find(src, oldPos);

        str.replace(start, src.size(), dst);

        oldPos = start + dst.size();
    }
    return str;
}

void ZmString::Replace(const char* search, const char* replace, std::string& subject, bool deep)
{
    std::string::size_type offset = 0;
    std::string::size_type pos = std::string::npos;
    size_t                 slen = strlen(search);
    size_t                 rlen = strlen(replace);
    while (std::string::npos != (pos = subject.find(search, offset)))
    {
        subject.replace(pos, slen, replace);
        if (!deep)
        {
            offset = pos + rlen;
        }
    }
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 编码转换

void ZmString::Ascii_To_UTF8(ZmByteBuffer& output, const char* astr)
{
    int wlen = MultiByteToWideChar(CP_ACP, 0, astr, -1, 0, 0);
    ZmByteBuffer wtemp(wlen * sizeof(wchar_t));
    wchar_t* wstr = (wchar_t*)wtemp.Head();
    MultiByteToWideChar(CP_ACP, 0, astr, -1, wstr, wlen);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, 0, 0, 0, 0);
    output.Reset(u8len);
    WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, output.Str(), u8len, 0, 0);
}

size_t ZmString::Ascii_To_Unicode(ZmByteBuffer& output, const char* astr, size_t len)
{
    size_t wlen = 0;

    int alen = (int)(len ? len : -1);
    wlen = MultiByteToWideChar(CP_ACP, 0, astr, alen, 0, 0);
    output.Reset(sizeof(wchar_t) * wlen);
    MultiByteToWideChar(CP_ACP, 0, astr, alen, (wchar_t*)output.Head(), (int)wlen);

    return wlen;
}

void ZmString::Unicode_To_Ascii(ZmByteBuffer& output, const wchar_t* wstr, size_t len)
{
    int wlen = (int)(len ? len : -1);
    int alen = WideCharToMultiByte(CP_ACP, 0, wstr, wlen, 0, 0, 0, 0);
    output.Reset(alen);
    WideCharToMultiByte(CP_ACP, 0, wstr, wlen, output.Str(), alen, 0, 0);
}

void ZmString::Unicode_To_UTF8(ZmByteBuffer& output, const wchar_t* wstr, size_t len)
{
    int wlen = (int)(len ? len : -1);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, NULL, 0, 0, 0);
    output.Reset(u8len);
    WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, output.Str(), u8len, 0, 0);
}

void ZmString::UTF8_To_Ascii(ZmByteBuffer& output, const char* utf8, size_t len)
{
    int u8len = len ? (int)len : -1;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, u8len, 0, 0);
    ZmByteBuffer wtemp(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, u8len, (wchar_t*)wtemp.Head(), wlen);

    int alen = WideCharToMultiByte(CP_ACP, 0, (wchar_t*)wtemp.Head(), wlen, 0, 0, 0, 0);
    output.Reset(alen);
    output.Zero();
    WideCharToMultiByte(CP_ACP, 0, (wchar_t*)wtemp.Head(), wlen, output.Str(), alen, 0, 0);
}

void ZmString::UTF8_To_Unicode(ZmByteBuffer& output, const char* utf8, size_t len)
{
    int u8len = len ? (int)len : -1;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, u8len, 0, 0);
    output.Reset(sizeof(wchar_t) * wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8, u8len, (wchar_t*)output.Head(), wlen);
}

void ZmString::UTF8_To_Ansi(ZmByteBuffer& output, const char* utf8, size_t len)
{
    int u8len = len ? (int)len : -1;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, u8len, NULL, 0);
    if (wlen > 0)
    {
        std::unique_ptr<wchar_t[]> buffer(new wchar_t[wlen]);
        MultiByteToWideChar(CP_UTF8, 0, utf8, u8len, buffer.get(), wlen);

        int alen = WideCharToMultiByte(CP_ACP, 0, buffer.get(), wlen, NULL, 0, NULL, NULL);
        if (alen > 0)
        {
            output.Reset(alen);
            WideCharToMultiByte(CP_ACP, 0, buffer.get(), wlen, output.Str(), alen, NULL, NULL);
        }
    }
}

void ZmString::OptimizeHexMixed(const char* hexmixed, char* output)
{
    if (NULL == hexmixed) { return; }
    if (strstr(hexmixed, "\\x") || strstr(hexmixed, "\\X"))
    {
        ZmStringList pieces(hexmixed, "\\");
        ZmByteBuffer localBuf(strlen(hexmixed) + 1);
        char* writePtr = (output != NULL) ? output : localBuf.Str();
        memset(writePtr, 0, (output != NULL) ? strlen(hexmixed) + 1 : localBuf.Size());
        for (size_t i = 0; i < pieces.Count(); i++)
        {
            const char* piece = pieces.At(i);
            size_t plen = strlen(piece);
            if (plen > 2 && (piece[0] == 'x' || piece[0] == 'X') && ZM_IS_HEX_CHAR(piece[1]) && ZM_IS_HEX_CHAR(piece[2]))
            {
                *writePtr++ = (zm_hex_to_char(piece[1]) << 4) + zm_hex_to_char(piece[2]);
                if (plen > 3)
                {
                    writePtr += snprintf(writePtr, plen, "%s", piece + 3);
                }
            }
            else
            {
                writePtr += snprintf(writePtr, plen + 2, "\\%s", piece);
            }
        }
    }
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 编解码（Hex / URL / Base64 / Base32）

char* ZmString::Hex(const BYTE* input, char* output, size_t inlen, bool lower)
{
    char* ptr = output;
    const char* hex = lower ? g_hexLower : g_hexUpper;
    for (size_t i = 0; i < inlen; i++)
    {
        *ptr++ = hex[(input[i] >> 4) & 0x0F];
        *ptr++ = hex[input[i] & 0x0F];
    }
    *ptr = '\0';
    return output;
}

size_t ZmString::FromHex(const char* input, BYTE* output, size_t inlen)
{
    BYTE* out = output;
    inlen = (0 == inlen) ? strlen(input) : inlen;
    if (inlen > 0)
    {
        size_t from = 0;
        if ((inlen % 2) == 1)
        {
            *out++ = zm_hex_to_char(input[0]);
            from = 1;
        }
        for (size_t i = from; i < inlen; i += 2)
        {
            *out++ = (zm_hex_to_char(input[i]) << 4) + zm_hex_to_char(input[i + 1]);
        }
    }
    size_t outlen = inlen / 2;
    return ((inlen % 2) == 1) ? (outlen + 1) : outlen;
}

char* ZmString::URLEncode(ZmByteBuffer& output, const char* input)
{
    char* pin = const_cast<char*>(input);
    output.Reset(strlen(input) * 3 + 1);
    char* pout = output.Str();

    while (*pin)
    {
        if (isalnum(*pin) || *pin == '-' || *pin == '_' || *pin == '.' || *pin == '~')
        {
            *pout++ = *pin;
        }
        else if (*pin == ' ')
        {
            *pout++ = '+';
        }
        else
        {
            *pout++ = '%';
            *pout++ = zm_char_to_hex(*pin >> 4);
            *pout++ = zm_char_to_hex(*pin & 15);
        }
        pin++;
    }
    *pout = '\0';
    return output.Str();
}

char* ZmString::URLDecode(ZmByteBuffer& output, const char* input)
{
    char* pin = const_cast<char*>(input);
    output.Reset(strlen(input) + 1);
    char* pout = output.Str();
    while (*pin)
    {
        if (*pin == '%')
        {
            if (pin[1] && pin[2])
            {
                *pout++ = (zm_hex_to_char(pin[1]) << 4) | zm_hex_to_char(pin[2]);
                pin += 2;
            }
        }
        else if (*pin == '+')
        {
            *pout++ = ' ';
        }
        else
        {
            *pout++ = *pin;
        }
        pin++;
    }
    *pout = '\0';
    return output.Str();
}

#define xx 255  // xx 标记无效字符，用于解码时检测非法输入
static const char    g_base64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const BYTE    g_base64Table[] = { xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx,
                                          xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx,
                                          xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, 62, xx, xx, xx, 63,
                                          52, 53, 54, 55, 56, 57, 58, 59, 60, 61, xx, xx, xx, xx, xx, xx,
                                          xx,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
                                          15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, xx, xx, xx, xx, xx,
                                          xx, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
                                          41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, xx, xx, xx, xx, xx };

static const char g_base64UrlChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static const BYTE g_base64UrlTable[] = { xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx,
                                          xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx,
                                          xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, xx, 62, xx, xx,
                                          52, 53, 54, 55, 56, 57, 58, 59, 60, 61, xx, xx, xx, xx, xx, xx,
                                          xx,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
                                          15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, xx, xx, xx, xx, 63,
                                          xx, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
                                          41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, xx, xx, xx, xx, xx };
#undef xx

bool ZmString::Base64Check(const char* str)
{
    size_t len = strlen(str);
    for (size_t i = 0; i < len; i++)
    {
        char c = str[i];
        if (ZM_BETWEEN(c, '0', '9') || ZM_BETWEEN(c, 'A', 'Z') || ZM_BETWEEN(c, 'a', 'z')
            || c == '+' || c == '/' || c == '=')
        {
        }
        else
        {
            return false;
        }
    }
    return true;
}

void ZmString::Base64Encode(ZmByteBuffer& output, const BYTE* data, size_t inLen)
{
    base64EncodeImpl(g_base64Chars, sizeof(g_base64Chars), output, data, inLen);
}

void ZmString::Base64Decode(ZmByteBuffer& output, const char* str, size_t inLen)
{
    base64DecodeImpl(g_base64Table, output, str, inLen);
}

std::string ZmString::Base64URLEncode(const BYTE* data, size_t inLen)
{
    ZmByteBuffer b64;
    base64EncodeImpl(g_base64UrlChars, sizeof(g_base64UrlChars), b64, data, inLen);
    if (b64.Size() > 0)
    {
        size_t pos = b64.Size() - 1;
        while (pos > 0 && b64[pos] == '=')
        {
            b64[pos] = '\0';
            pos--;
        }
    }
    return std::string(b64.Str());
}

void ZmString::Base64URLDecode(ZmByteBuffer& output, const char* str, size_t inLen)
{
    base64DecodeImpl(g_base64UrlTable, output, str, inLen);
}

int ZmString::Base32Encode(ZmByteBuffer& output, const BYTE* inBytes, size_t inLen)
{
    if (inLen > (1 << 28))
    {
        return -1;
    }
    int count = 0;
    int bufSize = ((int)inLen + 4) / 5 * 8 + 1;
    output.Reset(bufSize);
    uint8_t* result = output.Head();

    if (inLen > 0)
    {
        int    buffer = inBytes[0];
        size_t next = 1;
        int    bitsLeft = 8;
        while (count < bufSize && (bitsLeft > 0 || next < inLen))
        {
            if (bitsLeft < 5)
            {
                if (next < inLen)
                {
                    buffer <<= 8;
                    buffer |= inBytes[next++] & 0xFF;
                    bitsLeft += 8;
                }
                else
                {
                    int pad = 5 - bitsLeft;
                    buffer <<= pad;
                    bitsLeft += pad;
                }
            }
            int index = 0x1F & (buffer >> (bitsLeft - 5));
            bitsLeft -= 5;
            result[count++] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"[index];
        }
    }
    if (count < bufSize)
    {
        result[count] = '\000';
    }
    return count;
}

int ZmString::Base32Decode(ZmByteBuffer& output, const char* inStr, size_t inLen)
{
    int buffer = 0;
    int bitsLeft = 0;
    int count = 0;
    inLen = (inLen < 1) ? strlen(inStr) : inLen;
    int bufSize = ((int)inLen + 7) / 8 * 5 + 1;
    output.Reset(bufSize);
    uint8_t* result = output.Head();

    for (const uint8_t* ptr = (const uint8_t*)inStr; count < bufSize && *ptr; ++ptr)
    {
        uint8_t ch = *ptr;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '-')
        {
            continue;
        }
        buffer <<= 5;

        if (ch == '0')
        {
            ch = 'O';
        }
        else if (ch == '1')
        {
            ch = 'L';
        }
        else if (ch == '8')
        {
            ch = 'B';
        }

        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
        {
            ch = (ch & 0x1F) - 1;
        }
        else if (ch >= '2' && ch <= '7')
        {
            ch -= '2' - 26;
        }
        else
        {
            return -1;
        }

        buffer |= ch;
        bitsLeft += 5;
        if (bitsLeft >= 8)
        {
            result[count++] = buffer >> (bitsLeft - 8);
            bitsLeft -= 8;
        }
    }
    if (count < bufSize)
    {
        result[count] = '\000';
    }
    return count;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 哈希与工具

uint32_t ZmString::Hash(const char* str)
{
    const uint8_t* name = (const uint8_t*)str;
    uint32_t       h = 0;
    uint32_t       g;
    while (*name)
    {
        h = (h << 4) + *name++;
        g = h & 0xf0000000;
        if (g)
        {
            h ^= g;
            h ^= g >> 24;
        }
    }
    return h;
}

bool ZmString::ValidateTicket(const char* ticket, char* output)
{
    if (NULL == ticket) { return false; }
    ZmByteBuffer buf(strlen(ticket), ticket);
    char* str = Trim(buf.Str());
    if (64 == strlen(str))
    {
        for (char* p = str; *p && p; p++)
        {
            if (!ZM_IS_HEX_CHAR(*p))
            {
                return false;
            }
        }
        if (output)
        {
            memcpy(output, str, 64);
        }
        return true;
    }
    return false;
}

void ZmString::PKCS5Pad(BYTE* data, size_t block_size)
{
    size_t len = strlen((const char*)data);
    BYTE   pad = (BYTE)(block_size - (len % block_size));
    for (BYTE i = 0; i < pad; i++)
    {
        data[len + i] = pad;
    }
}

void ZmString::PKCS5Unpad(BYTE* data, size_t block_size)
{
    size_t len = strlen((const char*)data);
    BYTE   pad = data[len - 1];
    if (pad < block_size)
    {
        for (BYTE i = 0; i < pad; i++)
        {
            data[len - i - 1] = '\0';
        }
    }
}

std::string ZmString::RandomString(int length)
{
    std::string chars{ "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890" };
    std::random_device rd;
    std::mt19937 generator(rd());
    std::string output;

    output.reserve(length);

    auto len_chars = chars.length();

    while (length > 0)
    {
        auto randNumb = generator();
        while (randNumb > len_chars && length--)
        {
            output.push_back(chars[randNumb % len_chars]);
            randNumb /= static_cast<decltype(randNumb)>(len_chars);
        }
    }
    return output;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmString - 私有实现

char ZmString::easyToLower(char c)
{
    return ZM_BETWEEN(c, 'A', 'Z') ? (c - 'A' + 'a') : c;
}

char ZmString::easyToUpper(char c)
{
    return ZM_BETWEEN(c, 'a', 'z') ? (c - 'a' + 'A') : c;
}

void ZmString::base64EncodeImpl(const char* b64chars, size_t b64charsSize, ZmByteBuffer& output, const BYTE* inBytes, size_t inLen)
{
    if (NULL == inBytes || inLen < 1)
    {
        output.Reset(0);
        return;
    }

    int encodeOk = 0;
    size_t missing = 0;
    size_t ret_size = inLen;
    while ((ret_size % 3) != 0)
    {
        ++ret_size;
        ++missing;
    }

    ret_size = 4 * ret_size / 3;
    output.Reset(ret_size);
    char* optr = output.Str();
    for (unsigned int i = 0; i < ret_size / 4; ++i)
    {
        size_t index = i * 3;
        BYTE b3[3];
        b3[0] = (index + 0 < inLen) ? inBytes[index + 0] : 0;
        b3[1] = (index + 1 < inLen) ? inBytes[index + 1] : 0;
        b3[2] = (index + 2 < inLen) ? inBytes[index + 2] : 0;

        BYTE b4[4];
        b4[0] = ((b3[0] & 0xfc) >> 2);
        b4[1] = ((b3[0] & 0x03) << 4) + ((b3[1] & 0xf0) >> 4);
        b4[2] = ((b3[1] & 0x0f) << 2) + ((b3[2] & 0xc0) >> 6);
        b4[3] = ((b3[2] & 0x3f) << 0);

        if (b4[0] >= b64charsSize || b4[1] >= b64charsSize || b4[2] >= b64charsSize || b4[3] >= b64charsSize)
        {
            encodeOk = -1;
            break;
        }

        *optr++ = b64chars[b4[0]];
        *optr++ = b64chars[b4[1]];
        *optr++ = b64chars[b4[2]];
        *optr++ = b64chars[b4[3]];
    }
    if ((encodeOk != 0))
    {
        output.Reset(0);
        return;
    }

    for (size_t i = 0; i < missing; ++i)
    {
        auto index = ret_size - i - 1;
        if (index >= 0 && index < (decltype(index))output.Capacity())
        {
            output.Str()[ret_size - i - 1] = '=';
        }
    }
}

void ZmString::base64DecodeImpl(const BYTE* b64table, ZmByteBuffer& output, const char* inStr, size_t inLen)
{
    inLen = (inLen < 1) ? strlen(inStr) : inLen;
    if (NULL == inStr || inLen < 1)
    {
        output.Reset(0);
        return;
    }

    size_t b64Len = ((inLen % 4) == 0) ? inLen : (inLen + 4 - (inLen % 4));
    ZmByteBuffer temp(b64Len);
    BYTE* input = temp.Head();
    memcpy(input, inStr, inLen);
    for (size_t i = inLen; i < b64Len; i++)
    {
        input[i] = '=';
    }

    output.Reset(3 * b64Len / 4);
    BYTE* optr = output.Head();
    for (size_t i = 0; i < b64Len; i += 4)
    {
        BYTE b4[4];
        b4[0] = (input[i + 0] < 128) ? b64table[input[i + 0]] : 0xff;
        b4[1] = (input[i + 1] < 128) ? b64table[input[i + 1]] : 0xff;
        b4[2] = (input[i + 2] < 128) ? b64table[input[i + 2]] : 0xff;
        b4[3] = (input[i + 3] < 128) ? b64table[input[i + 3]] : 0xff;

        BYTE b3[3];
        b3[0] = ((b4[0] & 0x3f) << 2) + ((b4[1] & 0x30) >> 4);
        b3[1] = ((b4[1] & 0x0f) << 4) + ((b4[2] & 0x3c) >> 2);
        b3[2] = ((b4[2] & 0x03) << 6) + ((b4[3] & 0x3f) >> 0);

        if (b4[1] != 0xff) { *optr++ = b3[0]; }
        if (b4[2] != 0xff) { *optr++ = b3[1]; }
        if (b4[3] != 0xff) { *optr++ = b3[2]; }
    }
    output.Expand(optr - output.Head());
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////
// ZmStringList

ZmStringList::ZmStringList(const char* str, const char* delims) : m_items(8), m_heap(512)
{
    m_tail = 0;
    AddEntries(str, delims);
}

ZmStringList::~ZmStringList()
{
}

size_t ZmStringList::Count()
{
    return m_items.Count();
}

const char* ZmStringList::At(size_t i)
{
    return i < m_items.Count() ? (m_heap.Str() + m_items.At(i)->pos) : NULL;
}

const char* ZmStringList::operator[](size_t i)
{
    return At(i);
}

void ZmStringList::Lower()
{
    char* p = m_heap.Str();
    for (size_t i = 0; i < m_tail; i++, p++)
    {
        if (ZM_BETWEEN(*p, 'A', 'Z'))
        {
            *p = (*p - 'A' + 'a');
        }
    }
}

void ZmStringList::AddEntries(const char* str, const char* delims)
{
    if (NULL == str)
    {
        return;
    }
    if (NULL == delims || strlen(delims) < 1)
    {
        AddEntry(str);
    }
    else
    {
        size_t len = strlen(str);
        CheckSpace(len + 1);
        strncpy_s(m_heap.Str() + m_tail, len + 1, str, len);

        char* from = m_heap.Str() + m_tail;
        char* to = m_heap.Str() + m_tail + len;
        bool   blank = true;
        for (char* ptr = from; ptr < to; ptr++)
        {
            if (strchr(delims, *ptr))
            {
                blank = true;
                *ptr = '\0';
            }
            else if (blank)
            {
                blank = false;
                m_items.Add()->pos = (ptr - m_heap.Str());
            }
        }
        m_tail += len + 1;
    }
}

void ZmStringList::AddEntry(const char* str, size_t len)
{
    if (len < 1)
    {
        len = strlen(str);
    }
    CheckSpace(len + 1);
    m_items.Add()->pos = m_tail;
    strncpy_s(m_heap.Str() + m_tail, len + 1, str, len);
    m_tail += len + 1;
}

void ZmStringList::RemoveEntry(const char* str, size_t len)
{
    if (ZmString::IsEmpty(str)) { return; }
    if (len < 1) { len = strlen(str); }

    ZmStringList fresh;
    for (size_t i = 0; i < m_items.Count(); i++)
    {
        const char* xstr = m_heap.Str() + m_items.At(i)->pos;
        if (0 != strcmp(xstr, str))
        {
            fresh.PutEntry(xstr);
        }
    }

    RemoveAll();
    m_heap.Reset(fresh.m_heap.Size(), fresh.m_heap.Head());
    m_tail = fresh.m_tail;
    for (size_t i = 0; i < fresh.m_items.Count(); i++)
    {
        m_items.Add()->pos = fresh.m_items.At(i)->pos;
    }
}

size_t ZmStringList::PutEntry(const char* str, size_t len)
{
    if (str)
    {
        size_t pos = QueryEntry(str);
        if (NPOS == pos)
        {
            AddEntry(str, len);
            pos = m_items.Count() - 1;
        }
        return pos;
    }
    return NPOS;
}

size_t ZmStringList::QueryEntry(const char* str)
{
    for (size_t i = 0; i < m_items.Count(); i++)
    {
        if (0 == strcmp(m_heap.Str() + m_items.At(i)->pos, str))
        {
            return i;
        }
    }
    return NPOS;
}

void ZmStringList::RemoveAll()
{
    m_tail = 0;
    m_heap.Zero();
    m_items.Clear();
}

void ZmStringList::MakeUniquely(bool noEmpty)
{
    ZmStringList unique;
    for (size_t i = 0; i < m_items.Count(); i++)
    {
        const char* str = m_heap.Str() + m_items.At(i)->pos;
        if (!noEmpty || strlen(str) > 0)
        {
            unique.PutEntry(str);
        }
    }

    RemoveAll();

    m_heap.Reset(unique.m_heap.Size(), unique.m_heap.Head());
    m_tail = unique.m_tail;
    for (size_t i = 0; i < unique.m_items.Count(); i++)
    {
        m_items.Add()->pos = unique.m_items.At(i)->pos;
    }
}

size_t ZmStringList::Join(char* output, size_t capacity, const char* delimiter)
{
    size_t offset = 0;
    if (m_items.Count() > 0)
    {
        if (output && capacity > 0)
        {
            char* ptr = output;
            for (size_t i = 0; i < m_items.Count() && offset < capacity; i++)
            {
                offset += snprintf(ptr + offset, capacity - offset, "%s%s",
                    (i > 0 && delimiter) ? delimiter : "", m_heap.Str() + m_items.At(i)->pos);
            }
        }
        else
        {
            _ITEM* tail = m_items.At(m_items.Count() - 1);
            offset = tail->pos + strlen(m_heap.Str() + tail->pos);
            if (delimiter)
            {
                offset += strlen(delimiter) * (m_items.Count() - 1);
            }
        }
    }
    return offset;
}

void ZmStringList::Export(std::vector<std::string>& outputs)
{
    outputs.clear();
    for (size_t i = 0; i < m_items.Count(); i++)
    {
        const char* str = m_heap.Str() + m_items.At(i)->pos;
        if (strlen(str) > 0)
        {
            outputs.emplace_back(std::string(str));
        }
    }
}

void ZmStringList::ParseOptions(const char* options)
{
    RemoveAll();

    if (NULL == options || '\0' == options[0]) { return; }

    ZmByteBuffer wbuf;
    ZmString::Ascii_To_Unicode(wbuf, options);
    int     argc;
    LPWSTR* wargv = ::CommandLineToArgvW((LPCWSTR)wbuf.Head(), &argc);
    if (wargv)
    {
        ZmByteBuffer buf;
        for (int i = 0; i < argc; i++)
        {
            ZmString::Unicode_To_UTF8(buf, wargv[i]);
            AddEntry(buf.Str());
        }
        ::LocalFree(wargv);
    }
    else
    {
        AddEntries(options, " ");
    }
}

void ZmStringList::CheckSpace(size_t expects)
{
    if ((m_tail + expects) > m_heap.Size())
    {
        m_heap.Expand(m_heap.Size() + ZM_MAX(expects, m_heap.Size() / 2));
    }
}
