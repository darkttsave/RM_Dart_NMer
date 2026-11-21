/**
 * @file JsonUtil.h
 * @brief 最小 JSON 工具（避免为调试层引入第三方 JSON 库）
 */

#ifndef WEB_DEBUG_JSON_UTIL_H
#define WEB_DEBUG_JSON_UTIL_H

#include <cmath>
#include <cstdio>
#include <string>

namespace webdebug
{

/** 字符串转义（保留 UTF-8 原文，仅转义控制字符与引号/反斜杠） */
inline std::string jsonEscape(const std::string &input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

/** 输出 JSON 字符串字面量（含引号） */
inline std::string jsonQuoted(const std::string &value)
{
    return "\"" + jsonEscape(value) + "\"";
}

/** 输出 JSON 数值（非有限值输出 null） */
inline std::string jsonNumber(double value)
{
    if (!std::isfinite(value))
        return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

} // namespace webdebug

#endif // WEB_DEBUG_JSON_UTIL_H
