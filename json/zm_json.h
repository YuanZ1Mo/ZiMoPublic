/**
 * @file zm_json.h
 * @brief JSON 辅助工具函数库
 *
 * 基于 nlohmann::json 封装的 JSON 读写辅助函数，提供：
 *   - 类型安全的值读取（自动类型转换）：int / str / float / bool
 *   - 嵌套路径读取（"a.b.c" 风格）
 *   - 字段存在检查、删除
 *   - 值写入
 *   - JSON 序列化与解析（带异常捕获）
 *   - 对象合并
 *   - 数组读取、元素计数
 */

#ifndef ZM_JSON_H
#define ZM_JSON_H

#include <string>
#include <vector>
#include <sstream>

#include <json.hpp>

using ZMJSON = nlohmann::ordered_json;

// ============================================================================
// 非模板函数声明（实现在 zm_json.cpp）
// ============================================================================

/**
 * @brief 从 JSON 对象中读取整数值
 *
 * 支持多种源类型自动转换为 int：
 *   - 整数类型：直接返回
 *   - 字符串类型：支持十进制和 0x/0X 前缀的十六进制
 *   - 布尔类型：true → 1, false → 0
 *
 * @param json          JSON 对象（必须为 object 类型）
 * @param kname         字段名
 * @param default_value 字段不存在或类型不匹配时的默认返回值
 * @return 解析后的 int 值，或 default_value
 */
int zm_json_get_int(const ZMJSON& json, const char* kname, int default_value = 0);

/**
 * @brief 从 JSON 对象中读取字符串值
 *
 * 支持多种源类型自动转换为 string：
 *   - 字符串类型：直接返回
 *   - 数值类型：通过 dump() 序列化为字符串（自动处理小数位）
 *   - 对象/数组类型：序列化为 JSON 字符串
 *   - 布尔类型：转换为 "true" / "false"
 *
 * @param json          JSON 对象（必须为 object 类型）
 * @param kname         字段名
 * @param default_value 字段不存在或类型不匹配时的默认返回值
 * @return 解析后的 string 值，或 default_value
 */
std::string zm_json_get_str(const ZMJSON& json, const char* kname, std::string default_value = "");

/**
 * @brief 从 JSON 对象中读取浮点数值
 *
 * 支持多种源类型自动转换为 double：
 *   - 数值类型：直接返回（整数自动提升为 double）
 *   - 字符串类型：通过 atof() 转换
 *   - 布尔类型：true → 1.0, false → 0.0
 *
 * @param json          JSON 对象（必须为 object 类型）
 * @param kname         字段名
 * @param default_value 字段不存在或类型不匹配时的默认返回值
 * @return 解析后的 double 值，或 default_value
 */
double zm_json_get_float(const ZMJSON& json, const char* kname, double default_value = 0.0);

/**
 * @brief 从 JSON 对象中读取布尔值
 *
 * 支持多种源类型自动转换为 bool：
 *   - 布尔类型：直接返回
 *   - 整数类型：非零 → true，零 → false
 *   - 字符串类型：空串和 "0" 和 "false"（不区分大小写）→ false，其余 → true
 *
 * @param json          JSON 对象（必须为 object 类型）
 * @param kname         字段名
 * @param default_value 字段不存在或类型不匹配时的默认返回值
 * @return 解析后的 bool 值，或 default_value
 */
bool zm_json_get_bool(const ZMJSON& json, const char* kname, bool default_value = false);

/**
 * @brief 安全检查 JSON 对象中是否存在指定字段
 *
 * 内部会先判断是否为 object 类型，避免对非对象调用 contains() 抛异常。
 *
 * @param json  JSON 对象
 * @param kname 字段名
 * @return 存在返回 true，不存在或 json 非 object 返回 false
 */
bool zm_json_has(const ZMJSON& json, const char* kname);

/**
 * @brief 安全获取 JSON 数组或对象的元素个数
 *
 * @param json JSON 对象
 * @return 元素个数，若非数组/对象返回 0
 */
size_t zm_json_size(const ZMJSON& json);

/**
 * @brief 安全删除 JSON 对象中的指定字段
 *
 * 若字段不存在则不做任何操作，不会抛出异常。
 *
 * @param obj   JSON 对象（必须为 object 类型）
 * @param name  要删除的字段名
 */
void zm_json_erase(ZMJSON& obj, const char* name);

/**
 * @brief 将 JSON 对象序列化为字符串
 *
 * @param json   JSON 对象
 * @param indent 缩进空格数，-1 表示紧凑格式（无空格换行）
 * @return JSON 字符串
 */
std::string zm_json_dump(const ZMJSON& json, int indent = -1);

/**
 * @brief 将 source 中的字段合并到 target 中
 *
 * source 中的同名字段会覆盖 target 中的值（浅合并，不递归）。
 *
 * @param target  目标 JSON 对象，合并结果直接写入其中
 * @param source  来源 JSON 对象
 */
void zm_json_merge(ZMJSON& target, const ZMJSON& source);

/**
 * @brief 解析 JSON 字符串，带异常捕获
 *
 * @param json_str       待解析的 JSON 字符串
 * @param error_message  [out] 解析失败时写入错误信息，成功时不变
 * @return 解析成功返回 JSON 对象，失败返回空 JSON 对象
 */
ZMJSON zm_json_parse(const std::string& json_str, std::string& error_message);

// ============================================================================
// 模板函数（必须保留在头文件中）
// ============================================================================

/**
 * @brief 按 "a.b.c" 风格路径从嵌套 JSON 中读取值
 *
 * 逐层遍历路径，任意中间层不存在或类型不匹配则返回 default_value。
 *
 * @param json          JSON 对象
 * @param path          点分隔的路径字符串，如 "user.profile.name"
 * @param default_value 路径不存在时的默认返回值
 * @return 找到的值，或 default_value
 */
template<typename T>
inline T zm_json_get_path(const ZMJSON& json, const std::string& path, T default_value = T{})
{
    if (!json.is_object() || path.empty()) return default_value;

    // 按 '.' 分割路径
    std::vector<std::string> keys;
    std::istringstream iss(path);
    std::string key;
    while (std::getline(iss, key, '.')) {
        if (!key.empty()) keys.push_back(key);
    }
    if (keys.empty()) return default_value;

    // 逐层深入
    const ZMJSON* current = &json;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (!current->is_object() || !current->contains(keys[i])) {
            return default_value;
        }
        current = &(*current)[keys[i]];
    }

    try {
        return current->get<T>();
    }
    catch (...) {
        return default_value;
    }
}

/**
 * @brief 从 JSON 对象中读取数组字段，转换为 std::vector<T>
 *
 * @param json          JSON 对象
 * @param kname         字段名
 * @param default_value 字段不存在或不是数组时的默认返回值
 * @return 包含数组元素的 std::vector<T>
 */
template<typename T>
inline std::vector<T> zm_json_get_array(const ZMJSON& json, const char* kname, std::vector<T> default_value = {})
{
    if (!json.is_object() || !json.contains(kname)) return default_value;
    const ZMJSON& node = json[kname];

    if (!node.is_array()) return default_value;

    try {
        return node.get<std::vector<T>>();
    }
    catch (...) {
        return default_value;
    }
}

/**
 * @brief 向 JSON 对象中写入键值对
 *
 * @param obj   JSON 对象（若为 null 会自动变为 object）
 * @param kname 字段名
 * @param value 要写入的值（支持 nlohmann::json 可接受的任意类型）
 */
template<typename T>
inline void zm_json_set(ZMJSON& obj, const char* kname, const T& value)
{
    if (obj.is_null()) obj = ZMJSON::object();
    obj[kname] = value;
}

#endif  // ZM_JSON_H
