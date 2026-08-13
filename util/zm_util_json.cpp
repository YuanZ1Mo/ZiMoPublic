/**
 * @file zm_util_json.cpp
 * @brief JSON 辅助工具函数库实现
 */

#include "zm_util_json.h"

#include <stdlib.h>  // for atof()

// ============================================================================
// zm_json_get_int
// ============================================================================

int zm_json_get_int(const ZMJSON& json, std::string_view kname, int default_value)
{
    if (!json.is_object() || !json.contains(kname)) return default_value;
    const ZMJSON& node = json[kname];

    if (node.is_number_integer()) {
        return node.get<int>();
    }
    else if (node.is_string()) {
        try {
            std::string str = node.get<std::string>();
            // 支持 "0x" / "0X" 前缀的十六进制字符串
            if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
                return std::stoi(str, nullptr, 16);
            }
            return std::stoi(str);
        }
        catch (...) {
            return default_value;
        }
    }
    else if (node.is_boolean()) {
        return node.get<bool>() ? 1 : 0;
    }
    return default_value;
}

// ============================================================================
// zm_json_get_str
// ============================================================================

std::string zm_json_get_str(const ZMJSON& json, std::string_view kname, std::string default_value)
{
    if (!json.is_object() || !json.contains(kname)) return default_value;
    const ZMJSON& node = json[kname];

    if (node.is_string()) {
        return node.get<std::string>();
    }
    else if (node.is_number()) {
        return node.dump();
    }
    else if (node.is_object() || node.is_array()) {
        return node.dump();
    }
    else if (node.is_boolean()) {
        return node.get<bool>() ? "true" : "false";
    }
    return default_value;
}

// ============================================================================
// zm_json_get_float
// ============================================================================

double zm_json_get_float(const ZMJSON& json, std::string_view kname, double default_value)
{
    if (!json.is_object() || !json.contains(kname)) return default_value;
    const ZMJSON& node = json[kname];

    if (node.is_number()) {
        return node.get<double>();
    }
    else if (node.is_string()) {
        return atof(node.get<std::string>().c_str());
    }
    else if (node.is_boolean()) {
        return node.get<bool>() ? 1.0 : 0.0;
    }
    return default_value;
}

// ============================================================================
// zm_json_get_bool
// ============================================================================

bool zm_json_get_bool(const ZMJSON& json, std::string_view kname, bool default_value)
{
    if (!json.is_object() || !json.contains(kname)) return default_value;
    const ZMJSON& node = json[kname];

    if (node.is_boolean()) {
        return node.get<bool>();
    }
    else if (node.is_number_integer()) {
        return node.get<int>() != 0;
    }
    else if (node.is_string()) {
        std::string str = node.get<std::string>();
        if (str.empty() || str == "0") return false;
        if (str.size() == 5 &&
            (str[0] == 'f' || str[0] == 'F') &&
            (str[1] == 'a' || str[1] == 'A') &&
            (str[2] == 'l' || str[2] == 'L') &&
            (str[3] == 's' || str[3] == 'S') &&
            (str[4] == 'e' || str[4] == 'E')) {
            return false;
        }
        return true;
    }
    return default_value;
}

// ============================================================================
// zm_json_has
// ============================================================================

bool zm_json_has(const ZMJSON& json, std::string_view kname)
{
    return json.is_object() && json.contains(kname);
}

// ============================================================================
// zm_json_size
// ============================================================================

size_t zm_json_size(const ZMJSON& json)
{
    if (json.is_array() || json.is_object()) {
        return json.size();
    }
    return 0;
}

// ============================================================================
// zm_json_erase
// ============================================================================

void zm_json_erase(ZMJSON& obj, std::string_view name)
{
    ZMJSON::iterator pos = obj.find(name);
    if (pos != obj.end())
    {
        obj.erase(pos);
    }
}

// ============================================================================
// zm_json_dump
// ============================================================================

std::string zm_json_dump(const ZMJSON& json, int indent)
{
    return json.dump(indent);
}

// ============================================================================
// zm_json_merge
// ============================================================================

void zm_json_merge(ZMJSON& target, const ZMJSON& source)
{
    if (!source.is_object()) return;
    if (target.is_null()) target = ZMJSON::object();
    if (!target.is_object()) return;

    for (auto it = source.begin(); it != source.end(); ++it) {
        target[it.key()] = it.value();
    }
}

// ============================================================================
// zm_json_parse
// ============================================================================

ZMJSON zm_json_parse(const std::string& json_str, std::string& error_message)
{
    try {
        return ZMJSON::parse(json_str);
    }
    catch (const nlohmann::json::parse_error& e) {
        error_message = e.what();
    }
    return ZMJSON();
}
