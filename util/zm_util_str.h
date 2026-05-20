#ifndef ZM_UTIL_STR_H
#define ZM_UTIL_STR_H

#include "zm_util_container.h"

#include <string>
#include <vector>

typedef unsigned char BYTE;


#ifdef UNICODE
typedef std::wstring String;

#ifndef strcasecmp
//字符串比较（不区分大小写）比较整个宽字符字符串，直到遇到空字符。
#define strcasecmp   _wcsicmp  // 使用 _wcsicmp 作为宽字符版本
#endif

#ifndef strncasecmp
//字符串比较（不区分大小写）比较两个宽字符字符串的前 count 个字符，不区分大小写。
#define strncasecmp  _wcsnicmp
#endif

#define zm_strdup        _wcsdup  // 使用 _wcsdup 作为宽字符版本的 strdup
#define zm_strncpy       wcsncpy_s
#define zm_strnlen       wcsnlen


#else
typedef std::string String;
#ifndef strcasecmp
#define strcasecmp   _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp  _strnicmp
#endif
#define y_strdup        _strdup
#define y_strncpy       strncpy_s
#define y_strnlen       strnlen
#endif


/**
 * @brief 判断字符是否为十六进制字符（0-9, a-f, A-F）
 * @param c 待判断的字符
 * @return 非零值表示是十六进制字符，零表示不是
 */
#define ZM_IS_HEX_CHAR(c) ( ((c)>='0'&&(c)<='9') || ((c)>='a'&&(c)<='f') || ((c)>='A'&&(c)<='F') )

/**
 * @brief ZM_IS_HEX_CHAR 的小写别名，判断字符是否为十六进制字符
 * @param c 待判断的字符
 * @return 非零值表示是十六进制字符，零表示不是
 */
#define zm_is_hex_char(c) ZM_IS_HEX_CHAR(c)


extern "C"
{
    /**
     * @brief 复制字符串的前 n 个字节，分配新内存并返回
     * @param s1 源字符串指针
     * @param n 最多复制的字节数
     * @return 新分配内存中的字符串指针，调用者需自行释放；失败时返回 NULL
     * @example
     *   char* copy = zm_strndup("hello world", 5); // copy == "hello"
     *   free(copy);
     */
    char* zm_strndup(const char* s1, size_t n);

    /**
     * @brief 根据分隔符切分字符串（修改传入的指针位置）
     * @param stringp 指向待切分字符串指针的指针，调用后会被更新到下一个待切分位置
     * @param delim   分隔符集合
     * @return 本次切分得到的子串指针；如果无可切分内容则返回 NULL
     * @example
     *   char buf[] = "a,b,c";
     *   char* p = buf;
     *   char* token = zm_strsep(&p, ","); // token == "a", p 指向 "b,c"
     */
    char* zm_strsep(char** stringp, const char* delim);
}

/**
 * @brief 字符串工具类，提供编码转换、格式化、查找替换、Base64/Base32、URL编解码、Hex 等常用操作
 */
class ZmString
{
public:
    // ========================================================================
    // 基础判断
    // ========================================================================

    /**
     * @brief 判断 C 字符串是否为空（NULL 指针或空串）
     * @param str 待判断的 C 字符串指针
     * @return true 表示字符串为空，false 表示非空
     * @example
     *   ZmString::IsEmpty(NULL);   // true
     *   ZmString::IsEmpty("");     // true
     *   ZmString::IsEmpty("abc");  // false
     */
    static bool  IsEmpty(const char* str);

    /**
     * @brief 判断两个 C 字符串是否相等（安全处理 NULL 指针）
     * @param s1 第一个字符串指针，可以为 NULL
     * @param s2 第二个字符串指针，可以为 NULL
     * @return true 表示两个字符串内容相同（包括都为 NULL 的情况），false 表示不同
     * @example
     *   ZmString::Equals("abc", "abc");   // true
     *   ZmString::Equals(NULL, NULL);     // true
     *   ZmString::Equals("abc", NULL);    // false
     */
    static bool  Equals(const char* s1, const char* s2);

    /**
     * @brief 判断字符串是否全部由数字字符组成
     * @param str 待判断的 C 字符串
     * @return true 表示全部为数字，false 表示包含非数字字符或字符串为空
     * @example
     *   ZmString::IsNumeric("12345");   // true
     *   ZmString::IsNumeric("12a45");   // false
     */
    static bool  IsNumeric(const char* str);

    /**
     * @brief 判断字符串中是否包含扩展 ASCII 字符（值大于 127）
     * @param str 待检测的 C 字符串
     * @return true 表示包含扩展 ASCII 字符，false 表示不包含
     */
    static bool  HasExtendedAscii(const char* str);

    /**
     * @brief 判断字节流是否为合法的 UTF-8 编码
     * @param str 字节流指针
     * @param len 字节流长度
     * @return true 表示合法 UTF-8，false 表示非法
     */
    static bool  IsUTF8(const BYTE* str, size_t len);

    /**
     * @brief 判断字符串长度是否超过指定值
     * @param str 待检测的 C 字符串
     * @param len 长度阈值
     * @return true 表示字符串长度大于 len，false 表示不大于
     */
    static bool  LongerThan(const char* str, size_t len);

    // ========================================================================
    // 大小写转换
    // ========================================================================

    /**
     * @brief 将 std::string 中的所有字符原地转换为小写
     * @param str 待转换的 std::string 引用
     * @example
     *   std::string s = "Hello";
     *   ZmString::Lower(s); // s == "hello"
     */
    static void  Lower(std::string& str);

    /**
     * @brief 返回 C 字符串的小写副本（不修改原字符串）
     * @param str 输入的 C 字符串指针，可以为 NULL（视为空串）
     * @return 全部转换为小写后的 std::string
     * @example
     *   std::string s = ZmString::LowerEx("Hello World"); // s == "hello world"
     */
    static std::string LowerEx(const char* str);

    /**
     * @brief 将 std::string 中的所有字符原地转换为大写
     * @param str 待转换的 std::string 引用
     * @example
     *   std::string s = "Hello";
     *   ZmString::Upper(s); // s == "HELLO"
     */
    static void  Upper(std::string& str);

    /**
     * @brief 返回 C 字符串的大写副本（不修改原字符串）
     * @param str 输入的 C 字符串指针，可以为 NULL（视为空串）
     * @return 全部转换为大写后的 std::string
     * @example
     *   std::string s = ZmString::UpperEx("Hello World"); // s == "HELLO WORLD"
     */
    static std::string UpperEx(const char* str);

    // ========================================================================
    // 格式化与转换
    // ========================================================================

    /**
     * @brief 使用 printf 风格的格式化字符串生成 std::string
     * @param fmt 格式化模板字符串
     * @param ... 可变参数列表
     * @return 格式化后的 std::string
     * @example
     *   std::string s = ZmString::Format("id=%d,name=%s", 1, "test"); // s == "id=1,name=test"
     */
    static std::string Format(const char* fmt, ...);

    /**
     * @brief 将长整数转换为字符串表示
     * @param num   待转换的长整数
     * @param str   输出缓冲区，若为 NULL 则内部分配（调用者需释放）
     * @param radix 进制基数（如 10、16、8 等），默认为 10
     * @return 转换后的字符串指针
     * @example
     *   char* s = ZmString::L_To_A(255, NULL, 16); // s == "ff"
     */
    static char* L_To_A(long num, char* str = NULL, int radix = 10);

    // ========================================================================
    // 查找与匹配
    // ========================================================================

    /**
     * @brief 在 haystack 中从 offset 位置开始查找 needle 首次出现的位置
     * @param haystack 被搜索的主字符串
     * @param needle   要查找的子串
     * @param offset   起始搜索偏移量，默认为 0
     * @return 子串首次出现的起始索引；未找到时返回 -1
     * @example
     *   ZmString::Find("hello world", "world");       // 6
     *   ZmString::Find("hello world", "world", 7);    // -1
     */
    static int   Find(const char* haystack, const char* needle, size_t offset = 0);

    /**
     * @brief 从右向左在 haystack 中查找单个字符 needle 最后一次出现的位置
     * @param haystack 被搜索的主字符串
     * @param needle   要查找的字符
     * @return 字符最后一次出现的索引；未找到时返回 -1
     * @example
     *   ZmString::RFind("a/b/c/d", '/');  // 5
     */
    static int   RFind(const char* haystack, const char needle);

    /**
     * @brief 在 haystack 中查找 needle 首次出现的位置（等同于从偏移 0 开始的 Find）
     * @param haystack 被搜索的主字符串
     * @param needle   要查找的子串
     * @return 子串首次出现的起始索引；未找到时返回 -1
     */
    static int   Pos(const char* haystack, const char* needle);

    /**
     * @brief 判断 base 字符串是否以 head 字符串开头
     * @param base 待检测的主字符串
     * @param head 前缀字符串
     * @return true 表示 base 以 head 开头，false 表示不是
     * @example
     *   ZmString::StartsWith("hello world", "hello");  // true
     *   ZmString::StartsWith("hello world", "world");  // false
     */
    static bool  StartsWith(const char* base, const char* head);

    /**
     * @brief 判断 base 字符串是否以 tail 字符串结尾
     * @param base 待检测的主字符串
     * @param tail 后缀字符串
     * @return true 表示 base 以 tail 结尾，false 表示不是
     * @example
     *   ZmString::EndsWith("hello world", "world");  // true
     *   ZmString::EndsWith("hello world", "hello");  // false
     */
    static bool  EndsWith(const char* base, const char* tail);

    /**
     * @brief 通配符匹配，支持 '*'（匹配任意序列）和 '?'（匹配单个字符）
     * @param subject 待匹配的目标字符串
     * @param pattern 含通配符的模式字符串
     * @return true 表示匹配成功，false 表示不匹配
     * @example
     *   ZmString::WildcardMatch("hello world", "hel*o*");     // true
     *   ZmString::WildcardMatch("test.txt", "*.txt");         // true
     *   ZmString::WildcardMatch("abc", "a?c");                // true
     */
    static bool  WildcardMatch(const char* subject, const char* pattern);

    // ========================================================================
    // 裁剪与替换
    // ========================================================================

    /**
     * @brief 去除字符串首尾的空白字符（空格、制表符、换行等），原地修改
     * @param str 待处理的 C 字符串
     * @return 处理后的字符串指针（与输入相同）
     * @example
     *   char s[] = "  hello  ";
     *   ZmString::Trim(s); // s == "hello"
     */
    static char* Trim(char* str);

    /**
     * @brief 去除 std::string 首尾的空白字符，原地修改
     * @param str 待处理的 std::string 引用
     */
    static void  TrimEx(std::string& str);

    /**
     * @brief 在字符串 str 中查找所有 src 子串并替换为 dst
     * @param str 原始字符串（按值传入，不修改原字符串）
     * @param src 要被替换的子串
     * @param dst 替换后的新子串
     * @return 替换后的新字符串
     * @example
     *   std::string result = ZmString::spp("aabbccbbdd", "bb", "XX");
     *   // result == "aaXXccXXdd"
     */
    static std::string spp(std::string str, std::string src, std::string dst);

    /**
     * @brief 在 subject 中将所有 search 子串替换为 replace，可控制替换深度
     * @param search  要查找的子串
     * @param replace 替换为的新子串
     * @param subject 被操作的目标字符串（原地修改）
     * @param deep    是否深度替换。true 时 offset 始终为 0，每次从头重新搜索（替换结果可被再次匹配）；
     *                false 时 offset 跳过已替换部分，从上次替换位置之后继续搜索
     * @example
     *   // deep=true:  "ok"->"k" 时 "123oookkk"->"123kkk"（替换产生的 "ok" 会被再次替换）
     *   // deep=false: "ok"->"k" 时 "123oookkk"->"123ookkk"（跳过已替换部分，不二次匹配）
     */
    static void Replace(const char* search, const char* replace, std::string& subject, bool deep = false);

    // ========================================================================
    // 编码转换
    // ========================================================================

    /**
     * @brief 将 ASCII 字符串转换为 UTF-8 编码
     * @param output 转换结果输出缓冲区
     * @param astr   输入的 ASCII 字符串
     */
    static void   Ascii_To_UTF8(ZmByteBuffer& output, const char* astr);

    /**
    * @brief 将 ASCII 字符串转换为 Unicode 宽字符串
    * @param output 转换结果输出缓冲区
    * @param astr   输入的 ASCII 字符串
    * @param len    输入字符串长度，为 0 时自动计算
    * @return 转换后的宽字符数量
    */
    static size_t Ascii_To_Unicode(ZmByteBuffer& output, const char* astr, size_t len = 0);

    /**
     * @brief 将 Unicode 宽字符串转换为 ASCII 编码
     * @param output 转换结果输出缓冲区
     * @param wstr   输入的宽字符串
     * @param len    宽字符串长度，为 0 时自动计算
     */
    static void   Unicode_To_Ascii(ZmByteBuffer& output, const wchar_t* wstr, size_t len = 0);

    /**
     * @brief 将 Unicode 宽字符串转换为 UTF-8 编码
     * @param output 转换结果输出缓冲区
     * @param wstr   输入的宽字符串
     * @param len    宽字符串长度，为 0 时自动计算
     */
    static void   Unicode_To_UTF8(ZmByteBuffer& output, const wchar_t* wstr, size_t len = 0);

    /**
     * @brief 将 UTF-8 编码字符串转换为 ASCII
     * @param output 转换结果输出缓冲区
     * @param utf8   输入的 UTF-8 字符串
     * @param len    输入字符串长度，为 0 时自动计算
     */
    static void   UTF8_To_Ascii(ZmByteBuffer& output, const char* utf8, size_t len = 0);

    /**
     * @brief 将 UTF-8 编码字符串转换为 Unicode 宽字符串
     * @param output 转换结果输出缓冲区
     * @param utf8   输入的 UTF-8 字符串
     * @param len    输入字符串长度，为 0 时自动计算
     */
    static void   UTF8_To_Unicode(ZmByteBuffer& output, const char* utf8, size_t len = 0);

    /**
     * @brief 将 UTF-8 编码字符串转换为 ANSI 编码
     * @param output 转换结果输出缓冲区
     * @param utf8   输入的 UTF-8 字符串
     * @param len    输入字符串长度，为 0 时自动计算
     */
    static void   UTF8_To_Ansi(ZmByteBuffer& output, const char* utf8, size_t len = 0);

    /**
     * @brief 将字符串中的十六进制转义序列（\xNN）还原为对应字符
     * @param hexmixed 含 \xNN 转义序列的混合字符串，例如 "\\xE4\\xB8\\x89未"
     * @param output   输出缓冲区，为 NULL 时使用内部临时缓冲区，默认为 NULL
     * @example
     *   // 将 "\\xE4\\xB8\\x89\\xE6\\x9C\\xAA3308000000\\1\\2\\3000"
     *   // 还原为 "三未3308000000\1\2\3000"
     *   ZmString::OptimizeHexMixed("\\xE4\\xB8\\x89\\xE6\\x9C\\xAA3308...", buf);
     */
    static void   OptimizeHexMixed(const char* hexmixed, char* output = NULL);

    // ========================================================================
    // 编解码（Hex / URL / Base64 / Base32）
    // ========================================================================

    /**
     * @brief 将二进制数据转换为十六进制字符串
     * @param input  输入的二进制数据指针
     * @param output 输出缓冲区，需由调用者分配足够空间（至少 inLen * 2 + 1 字节）
     * @param inlen  输入数据的字节长度
     * @param lower  是否使用小写字母，默认为 true
     * @return 输出缓冲区指针
     * @example
     *   BYTE data[] = {0xAB, 0xCD};
     *   char hex[16];
     *   ZmString::Hex(data, hex, 2);       // hex == "abcd"
     *   ZmString::Hex(data, hex, 2, false); // hex == "ABCD"
     */
    static char* Hex(const BYTE* input, char* output, size_t inlen, bool lower = true);

    /**
     * @brief 将十六进制字符串转换为二进制数据
     * @param input  十六进制字符串
     * @param output 输出二进制数据缓冲区
     * @param inlen  输入字符串长度，为 0 时自动计算
     * @return 转换后的二进制数据字节数
     */
    static size_t FromHex(const char* input, BYTE* output, size_t inlen = 0);

    /**
     * @brief URL 编码，将特殊字符转换为 %XX 形式
     * @param output 编码结果输出缓冲区
     * @param input  待编码的 URL 字符串
     * @return 编码后的字符串指针
     */
    static char* URLEncode(ZmByteBuffer& output, const char* input);

    /**
     * @brief URL 解码，将 %XX 形式还原为原始字符
     * @param output 解码结果输出缓冲区
     * @param input  待解码的 URL 编码字符串
     * @return 解码后的字符串指针
     */
    static char* URLDecode(ZmByteBuffer& output, const char* input);

    /**
     * @brief 检查字符串是否为合法的 Base64 编码
     * @param str 待检查的字符串
     * @return true 表示合法，false 表示非法
     */
    static bool  Base64Check(const char* str);

    /**
     * @brief Base64 编码，将二进制数据编码为 Base64 字符串
     * @param output 编码结果输出缓冲区
     * @param data   待编码的二进制数据指针
     * @param inLen  待编码数据的字节长度
     */
    static void  Base64Encode(ZmByteBuffer& output, const BYTE* data, size_t inLen);

    /**
     * @brief Base64 解码，将 Base64 字符串还原为二进制数据
     * @param output 解码结果输出缓冲区
     * @param str    Base64 编码的字符串
     * @param inLen  输入字符串长度，为 0 时自动计算
     */
    static void  Base64Decode(ZmByteBuffer& output, const char* str, size_t inLen = 0);

    /**
     * @brief URL 安全的 Base64 编码（不使用 '+' 和 '/'，使用 '-' 和 '_'）
     * @param data   待编码的二进制数据指针
     * @param inLen  待编码数据的字节长度
     * @return Base64URL 编码后的 std::string
     */
    static std::string Base64URLEncode(const BYTE* data, size_t inLen);

    /**
     * @brief URL 安全的 Base64 解码
     * @param output 解码结果输出缓冲区
     * @param str    Base64URL 编码的字符串
     * @param inLen  输入字符串长度，为 0 时自动计算
     */
    static void        Base64URLDecode(ZmByteBuffer& output, const char* str, size_t inLen = 0);

    /**
     * @brief Base32 编码（字母表 ABCDEFGHIJKLMNOPQRSTUVWXYZ234567），空间膨胀比 8/5
     * @param output 编码结果输出缓冲区
     * @param data   待编码的二进制数据指针
     * @param len    待编码数据的字节长度
     * @return 编码后的数据长度，失败返回负值
     */
    static int    Base32Encode(ZmByteBuffer& output, const BYTE* data, size_t len);

    /**
     * @brief Base32 解码
     * @param output  解码结果输出缓冲区
     * @param encoded Base32 编码的字符串
     * @param len     输入字符串长度，为 0 时自动计算
     * @return 解码后的数据长度，失败返回负值
     */
    static int    Base32Decode(ZmByteBuffer& output, const char* encoded, size_t len = 0);

    // ========================================================================
    // 哈希与工具
    // ========================================================================

    /**
     * @brief 计算字符串的哈希值（32 位无符号整数，ELF Hash 算法）
     * @param str 输入的 C 字符串
     * @return 32 位哈希值
     */
    static uint32_t Hash(const char* str);

    /**
     * @brief 验证 Ticket 是否合法（64 位十六进制字符串），可同时去除空白并拷贝到输出缓冲区
     * @param ticket 待验证的 Ticket 字符串，要求去除首尾空白后恰好为 64 个十六进制字符
     * @param output 输出缓冲区，非 NULL 时将合法 Ticket 拷贝至此（至少 64 字节），默认为 NULL
     * @return true 表示 Ticket 合法，false 表示不合法或指针为 NULL
     * @example
     *   char buf[65] = {};
     *   ZmString::ValidateTicket("  0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF01  ", buf);
     *   // true, buf 中存放去空白后的 64 字符
     */
    static bool  ValidateTicket(const char* ticket, char* output = NULL);

    /**
     * @brief PKCS5 填充，在数据末尾追加填充字节使其长度对齐到 block_size 的整数倍
     * @param data       待填充的数据（需有足够空间）
     * @param block_size 块大小（通常为 8）
     * @note 针对 Des/3Des 加密，需在加密前 pad 解密后 unpad
     */
    static void PKCS5Pad(BYTE* data, size_t block_size);

    /**
     * @brief PKCS5 去填充，移除末尾的 PKCS5 填充字节
     * @param data       待去填充的数据
     * @param block_size 块大小（通常为 8）
     */
    static void PKCS5Unpad(BYTE* data, size_t block_size);

    /**
     * @brief 生成指定长度的随机字符串（由字母和数字组成）
     * @param length 生成字符串的长度，默认为 10
     * @return 随机生成的 std::string
     * @example
     *   std::string rnd = ZmString::RandomString(8); // 例如 "a3Bx9Kp2"
     */
    static std::string RandomString(int length = 10);

private:
    /**
     * @brief 将单个大写字母转换为小写字母，非大写字母原样返回
     * @param c 输入字符
     * @return 转换后的字符
     */
    static char easyToLower(char c);

    /**
     * @brief 将单个小写字母转换为大写字母，非小写字母原样返回
     * @param c 输入字符
     * @return 转换后的字符
     */
    static char easyToUpper(char c);

    /**
     * @brief Base64 编码的内部实现，使用自定义字母表
     * @param b64chars     编码字母表
     * @param b64charsSize 字母表大小
     * @param output       编码结果输出缓冲区
     * @param data         待编码的二进制数据
     * @param inLen        数据长度
     */
    static void  base64EncodeImpl(const char* b64chars, size_t b64charsSize, ZmByteBuffer& output, const BYTE* data, size_t inLen);

    /**
     * @brief Base64 解码的内部实现，使用自定义解码表
     * @param b64table 解码查找表
     * @param output   解码结果输出缓冲区
     * @param str      Base64 编码字符串
     * @param inLen    输入字符串长度，为 0 时自动计算
     */
    static void  base64DecodeImpl(const BYTE* b64table, ZmByteBuffer& output, const char* str, size_t inLen = 0);
};

/** 用一个长 buffer 存放多个短字符串 */
class ZmStringList
{
public:
    /** 未找到时的返回值 */
    enum { NPOS = 0x00FFFFFF };

    /**
     * @brief 构造函数，可选择使用分隔符切分初始字符串
     * @param str    初始字符串，默认为 NULL
     * @param delims 分隔符集合，默认为 NULL
     */
    ZmStringList(const char* str = NULL, const char* delims = NULL);

    /**
     * @brief 析构函数，释放内部资源
     */
    ~ZmStringList();

    /**
     * @brief 返回列表中字符串的个数
     * @return 字符串条目数量
     */
    size_t Count();

    /**
     * @brief 按索引获取字符串
     * @param i 索引（从 0 开始）
     * @return 对应位置的字符串指针；索引越界时返回 NULL
     */
    const char* At(size_t i);

    /**
     * @brief 按索引获取字符串（运算符重载，等价于 At）
     * @param i 索引（从 0 开始）
     * @return 对应位置的字符串指针；索引越界时返回 NULL
     */
    const char* operator[](size_t i);

    /**
     * @brief 将列表中所有字符串原地转换为小写
     */
    void  Lower();

    /**
     * @brief 使用分隔符将字符串拆分为多个条目并添加到列表
     * @param str    待拆分的字符串
     * @param delims 分隔符集合，默认为 NULL
     */
    void   AddEntries(const char* str, const char* delims = NULL);

    /**
     * @brief 向列表中添加一个条目
     * @param str 字符串指针
     * @param len 字符串长度，为 0 时自动计算
     */
    void   AddEntry(const char* str, size_t len = 0);

    /**
     * @brief 从列表中移除与指定字符串匹配的条目
     * @param str 要移除的字符串
     * @param len 字符串长度，为 0 时自动计算
     */
    void   RemoveEntry(const char* str, size_t len = 0);

    /**
     * @brief 非重复添加条目，若已存在则不重复添加
     * @param str 字符串指针
     * @param len 字符串长度，为 0 时自动计算
     * @return 新增加成功返回条目索引，已存在返回已有索引；str 为 NULL 时返回 NPOS
     */
    size_t PutEntry(const char* str, size_t len = 0);

    /**
     * @brief 查询指定字符串在列表中的索引
     * @param str 要查询的字符串
     * @return 条目索引；未找到时返回 NPOS
     */
    size_t QueryEntry(const char* str);

    /**
     * @brief 移除列表中的所有条目
     */
    void   RemoveAll();

    /**
     * @brief 去掉列表中重复的字符串条目，保留首次出现的条目
     * @param noEmpty 是否同时去除空字符串条目，默认为 false
     * @example
     *   ZmStringList list("a,b,a,c,b", ",");
     *   list.MakeUniquely(); // 列表变为 "a","b","c"
     */
    void   MakeUniquely(bool noEmpty = false);

    /**
     * @brief 将列表中的所有条目用分隔符连接为一个字符串
     * @param output    输出缓冲区
     * @param capacity  输出缓冲区的容量
     * @param delimiter 条目之间的分隔符，默认为 NULL
     * @return 实际写入的字节数
     */
    size_t Join(char* output, size_t capacity, const char* delimiter = NULL);

    /**
     * @brief 将列表中的所有条目导出到 std::vector<std::string>
     * @param outputs 输出的 vector 容器
     */
    void   Export(std::vector<std::string>& outputs);

    /**
     * @brief 解析命令行参数格式的字符串（支持引号、转义），将各参数添加到列表
     * @param options 命令行参数字符串，例如 "-f file.txt --name \"hello world\""
     * @note 内部使用 Windows CommandLineToArgvW 进行解析，回退到空格分隔
     */
    void   ParseOptions(const char* options);

private:
    typedef struct
    {
        size_t  pos;
    }_ITEM;

    /**
     * @brief 检查内部堆空间是否足够，不足时扩容
     * @param expects 期望的额外空间大小
     */
    void  CheckSpace(size_t expects);

private:
    ZmArrayList<_ITEM>  m_items;
    ZmByteBuffer        m_heap;
    size_t              m_tail;
};


#endif /* ZM_UTIL_STR_H */
