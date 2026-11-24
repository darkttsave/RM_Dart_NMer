/**
 * @file RuntimeParams.cpp
 * @brief 运行时参数管理实现（M3）
 */

#include "RuntimeParams.h"
#include "JsonUtil.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace webdebug
{
namespace fs = std::filesystem;

namespace
{
/** 拆点分路径，"detection.time_filter" -> {"detection", "time_filter"} */
std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path)
    {
        if (c == '.')
        {
            if (!cur.empty())
                parts.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
        parts.push_back(cur);
    return parts;
}

/** 参数值转 YAML 字面量文本（int 输出整数；double 保留 6 位有效数字） */
std::string formatYamlValue(double value, bool is_int)
{
    char buf[40];
    if (is_int)
    {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(value)));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    std::string text(buf);
    // 保持与原配置一致的浮点写法（如 4 -> 4.0），便于人工阅读
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos)
        text += ".0";
    return text;
}

/**
 * @brief 在 YAML 原文中替换 section 路径下某个标量的值
 *
 * 采用文本级替换而不是"解析后重新 dump"：这样能保留原有注释、字段顺序与数值写法，
 * 只改动目标参数的值；替换后由调用方用 yaml-cpp 重新解析校验，保证文件仍然合法。
 *
 * @return 是否找到并替换（找不到时返回 false，调用方跳过该参数）
 */
bool replaceYamlScalar(std::string &text, const std::vector<std::string> &section,
                       const std::string &key, const std::string &new_value)
{
    std::istringstream in(text);
    std::ostringstream out;
    std::vector<std::pair<int, std::string>> stack; // (缩进, key)
    std::string line;
    bool replaced = false;
    while (std::getline(in, line))
    {
        std::string stripped = line;
        if (!stripped.empty() && stripped.back() == '\r')
            stripped.pop_back();
        std::size_t indent = 0;
        while (indent < stripped.size() && stripped[indent] == ' ')
            ++indent;
        const std::string body = stripped.substr(indent);
        if (body.empty() || body[0] == '#')
        {
            out << line << "\n";
            continue;
        }
        const std::size_t colon = body.find(':');
        if (colon == std::string::npos)
        {
            out << line << "\n";
            continue;
        }
        std::string current_key = body.substr(0, colon);
        while (!current_key.empty() && (current_key.back() == ' ' || current_key.back() == '\t'))
            current_key.pop_back();

        while (!stack.empty() && stack.back().first >= static_cast<int>(indent))
            stack.pop_back();

        bool matched = (stack.size() == section.size() && current_key == key);
        if (matched)
        {
            for (std::size_t i = 0; i < section.size(); ++i)
            {
                if (stack[i].second != section[i])
                {
                    matched = false;
                    break;
                }
            }
        }
        if (matched)
        {
            const std::string rest = body.substr(colon + 1);
            std::size_t lead = 0;
            while (lead < rest.size() && (rest[lead] == ' ' || rest[lead] == '\t'))
                ++lead;
            const std::string lead_ws = rest.substr(0, lead);
            std::string comment;
            const std::size_t hash_pos = rest.find('#');
            if (hash_pos != std::string::npos && hash_pos > 0 &&
                (rest[hash_pos - 1] == ' ' || rest[hash_pos - 1] == '\t'))
            {
                std::size_t comment_start = hash_pos;
                while (comment_start > 0 && (rest[comment_start - 1] == ' ' ||
                                             rest[comment_start - 1] == '\t'))
                    --comment_start;
                comment = rest.substr(comment_start); // 保留注释前的原始空白
            }
            out << std::string(indent, ' ') << current_key << ":" << lead_ws << new_value << comment
                << "\n";
            replaced = true;
            continue;
        }

        std::string rest = body.substr(colon + 1);
        bool has_value = false;
        for (char c : rest)
        {
            if (c != ' ' && c != '\t')
            {
                has_value = true;
                break;
            }
        }
        if (!has_value)
            stack.push_back({static_cast<int>(indent), current_key});
        out << line << "\n";
    }
    if (replaced)
        text = out.str();
    return replaced;
}

bool parseDouble(const std::string &text, double *out)
{
    if (text.empty())
        return false;
    char *end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;
    *out = v;
    return true;
}

bool isIntegerLiteral(const std::string &text)
{
    if (text.empty())
        return false;
    std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (i >= text.size())
        return false;
    for (; i < text.size(); ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

/**
 * @brief 解析一层 JSON 对象 {"name": 90, "other": "1.5", ...} 为数值映射
 *
 * 之所以自己解析：C++ 侧不引入 JSON 依赖；控制口协议只需要一层 key->number。
 */
bool parseFlatJsonObject(const std::string &json, std::map<std::string, double> *out,
                         std::string *error)
{
    std::size_t i = 0;
    const std::size_t n = json.size();
    auto skipWs = [&]() {
        while (i < n && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
            ++i;
    };

    skipWs();
    if (i >= n || json[i] != '{')
    {
        *error = "values 必须是 JSON 对象";
        return false;
    }
    ++i;
    skipWs();
    if (i < n && json[i] == '}')
    {
        ++i;
        return true;
    }

    while (i < n)
    {
        skipWs();
        if (i >= n || json[i] != '"')
        {
            *error = "JSON 对象键必须是字符串";
            return false;
        }
        ++i;
        std::string key;
        while (i < n && json[i] != '"')
        {
            if (json[i] == '\\' && i + 1 < n)
            {
                key += json[i + 1];
                i += 2;
                continue;
            }
            key += json[i];
            ++i;
        }
        if (i >= n)
        {
            *error = "JSON 字符串未闭合";
            return false;
        }
        ++i; // 跳过结尾引号
        skipWs();
        if (i >= n || json[i] != ':')
        {
            *error = "JSON 对象缺少冒号";
            return false;
        }
        ++i;
        skipWs();
        std::string valueText;
        bool isString = false;
        if (i < n && json[i] == '"')
        {
            isString = true;
            ++i;
            while (i < n && json[i] != '"')
            {
                if (json[i] == '\\' && i + 1 < n)
                {
                    valueText += json[i + 1];
                    i += 2;
                    continue;
                }
                valueText += json[i];
                ++i;
            }
            if (i >= n)
            {
                *error = "JSON 字符串未闭合";
                return false;
            }
            ++i;
        }
        else
        {
            while (i < n && json[i] != ',' && json[i] != '}' && json[i] != ' ' &&
                   json[i] != '\t' && json[i] != '\n' && json[i] != '\r')
            {
                valueText += json[i];
                ++i;
            }
        }
        double value = 0.0;
        if (key == "dart_color" && !isString && (valueText == "true" || valueText == "false"))
        {
            *error = "dart_color 必须为 red、blue、0 或 1";
            return false;
        }
        if (key == "dart_color" && isString && (valueText == "red" || valueText == "blue"))
            value = valueText == "blue" ? 1.0 : 0.0;
        else if (valueText == "true")
            value = 1.0;
        else if (valueText == "false")
            value = 0.0;
        else if (!parseDouble(valueText, &value))
        {
            *error = "参数 " + key + " 的值不是数字: " + valueText;
            return false;
        }
        (*out)[key] = value;

        skipWs();
        if (i >= n)
        {
            *error = "JSON 对象未闭合";
            return false;
        }
        if (json[i] == ',')
        {
            ++i;
            continue;
        }
        if (json[i] == '}')
        {
            ++i;
            break;
        }
        *error = "JSON 对象格式错误";
        return false;
    }
    return true;
}
} // namespace

RuntimeParams::RuntimeParams()
{
    auto add = [this](const char *name, const char *label, const char *unit, const char *group,
                      const char *section, const char *yamlKey, bool isInt, double minV, double maxV,
                      const char *hint) {
        ParamDef def;
        def.name = name;
        def.label = label;
        def.unit = unit;
        def.group = group;
        def.yaml_section = section;
        def.yaml_key = yamlKey;
        def.is_int = isInt;
        def.min_value = minV;
        def.max_value = maxV;
        def.hot = true;
        def.web_editable = true;
        def.hint = hint;
        defs_.push_back(def);
    };

    // A. 颜色识别
    add("bright_thresh", "亮度阈值", "", "color", "detection", "bright_thresh", true, 0, 255,
        "HSV V 通道亮度下限");
    add("green_weight_thresh", "绿色权重阈值", "", "color", "detection", "green_weight_thresh",
        false, 0.0, 1.0, "G/(R+G+B+1) 归一化绿色权重下限");

    // B. 候选目标筛选
    add("min_area", "最小面积", "px²", "shape", "detection.shape_filter", "min_area", true, 1,
        1000000, "轮廓面积下限");
    add("max_area", "最大面积", "px²", "shape", "detection.shape_filter", "max_area", true, 1,
        1000000, "轮廓面积上限");
    add("min_radius", "最小半径", "px", "shape", "detection", "min_radius", true, 1, 500,
        "目标半径下限");
    add("max_radius", "最大半径", "px", "shape", "detection", "max_radius", true, 1, 1000,
        "目标半径上限");
    add("min_circularity", "最小圆形度", "", "shape", "detection.shape_filter", "min_circularity",
        false, 0.0, 1.0, "4πA/P²，圆=1");
    add("min_aspect_ratio", "宽高比下限", "", "shape", "detection.shape_filter",
        "min_aspect_ratio", false, 0.01, 20.0, "外接矩形宽/高下限");
    add("max_aspect_ratio", "宽高比上限", "", "shape", "detection.shape_filter",
        "max_aspect_ratio", false, 0.01, 20.0, "外接矩形宽/高上限");
    add("kernel_size", "形态学核大小", "px", "shape", "detection.morphology", "kernel_size", true,
        1, 31, "偶数会按下一个奇数使用");
    add("open_iterations", "开运算次数", "", "shape", "detection.morphology", "open_iterations",
        true, 0, 10, "去噪点");
    add("close_iterations", "闭运算次数", "", "shape", "detection.morphology", "close_iterations",
        true, 0, 10, "补断裂");

    // C. 追踪与连续帧判断
    add("required_frames", "连续确认帧数", "帧", "tracking", "detection.time_filter",
        "required_frames", true, 1, 100, "连续命中 N 帧进入追踪");
    add("required_lost_frames", "连续丢失帧数", "帧", "tracking", "detection.time_filter",
        "required_lost_frames", true, 1, 1000, "连续丢失 N 帧退出追踪");
    add("max_center_dist", "中心跳变阈值", "px", "tracking", "detection.time_filter",
        "max_center_dist", false, 1.0, 5000.0, "相邻帧中心位移超过则重置连续计数");

    // D. 转向与距离
    add("turn_threshold", "转向阈值", "px", "turn", "turn_control", "turn_threshold", false, 0.0,
        2000.0, "偏差小于该值判定对准");
    add("dist_far_thresh", "远距离阈值", "px", "turn", "turn_control", "dist_far_thresh", false,
        0.0, 5000.0, "横向偏差大于该值判为远");
    add("dist_near_thresh", "近距离阈值", "px", "turn", "turn_control", "dist_near_thresh", false,
        0.0, 5000.0, "横向偏差大于该值判为中");

    // E. 相机（每帧下发到相机 SDK，实测可热更新）
    add("exposure", "曝光", "us", "camera", "camera", "exposure", true, 10, 1000000,
        "手动曝光时间（每帧下发）");
    add("gain", "增益", "", "camera", "camera", "gain", true, 0, 100, "模拟增益（每帧下发）");

    // P2-B: 飞镖参数采用 dart_ 前缀，避免与绿灯参数同名。颜色为 0=红，1=蓝。
    add("dart_enabled", "飞镖检测", "", "dart", "dart_detection", "enabled", true, 0, 1, "飞镖检测参数");
    add("dart_color", "飞镖颜色", "", "dart", "dart_detection", "color", true, 0, 1, "0=红，1=蓝");
    add("dart_saturation_min", "饱和度下限", "", "dart", "dart_detection", "saturation_min", true, 0, 255, "飞镖检测参数");
    add("dart_value_min", "亮度下限", "", "dart", "dart_detection", "value_min", true, 0, 255, "飞镖检测参数");
    add("dart_red_low_max", "红色色相低段上限", "", "dart", "dart_detection", "red_low_max", true, 0, 179, "飞镖检测参数");
    add("dart_red_high_min", "红色色相高段下限", "", "dart", "dart_detection", "red_high_min", true, 0, 179, "飞镖检测参数");
    add("dart_blue_hue_min", "蓝色色相下限", "", "dart", "dart_detection", "blue_hue_min", true, 0, 179, "飞镖检测参数");
    add("dart_blue_hue_max", "蓝色色相上限", "", "dart", "dart_detection", "blue_hue_max", true, 0, 179, "飞镖检测参数");
    add("dart_min_area", "飞镖最小面积", "", "dart", "dart_detection", "min_area", false, 0, 1000000, "飞镖检测参数");
    add("dart_max_area", "飞镖最大面积", "", "dart", "dart_detection", "max_area", false, 0, 1000000, "飞镖检测参数");
    add("dart_min_aspect", "飞镖宽高比下限", "", "dart", "dart_detection", "min_aspect", false, 0.001, 100, "飞镖检测参数");
    add("dart_max_aspect", "飞镖宽高比上限", "", "dart", "dart_detection", "max_aspect", false, 0.001, 100, "飞镖检测参数");
    add("dart_close_kernel", "飞镖闭运算核", "", "dart", "dart_detection", "close_kernel", true, 1, 31, "飞镖检测参数");
    add("dart_max_distance", "最大关联距离", "", "dart", "dart_detection", "max_distance", false, 0.001, 10000, "飞镖检测参数");
    add("dart_min_motion", "最小运动距离", "", "dart", "dart_detection", "min_motion", false, 0.001, 10000, "飞镖检测参数");
    add("dart_max_lost_frames", "飞镖容许丢帧", "", "dart", "dart_detection", "max_lost_frames", true, 0, 1000, "飞镖检测参数");
    add("dart_max_track_points", "轨迹点上限", "", "dart", "dart_detection", "max_track_points", true, 2, 1000, "飞镖检测参数");
    add("dart_draw_debug", "飞镖标注", "", "dart", "dart_detection", "draw_debug", true, 0, 1, "飞镖检测参数");
}

void RuntimeParams::initialize(const std::map<std::string, double> &from_yaml)
{
    // 与 config/debug.yaml 当前值一致的内置默认值（YAML 缺项时使用）
    static const std::map<std::string, double> kDefaults = {
        {"bright_thresh", 80.0},
        {"green_weight_thresh", 0.4},
        {"min_area", 50.0},
        {"max_area", 5000.0},
        {"min_radius", 10.0},
        {"max_radius", 30.0},
        {"min_circularity", 0.6},
        {"min_aspect_ratio", 0.5},
        {"max_aspect_ratio", 2.0},
        {"kernel_size", 7.0},
        {"open_iterations", 1.0},
        {"close_iterations", 3.0},
        {"required_frames", 5.0},
        {"required_lost_frames", 10.0},
        {"max_center_dist", 100.0},
        {"turn_threshold", 4.0},
        {"dist_far_thresh", 300.0},
        {"dist_near_thresh", 100.0},
        {"exposure", 4000.0},
        {"gain", 10.0},
        {"dart_enabled", 0},
        {"dart_color", 0},
        {"dart_saturation_min", 100},
        {"dart_value_min", 80},
        {"dart_red_low_max", 10},
        {"dart_red_high_min", 170},
        {"dart_blue_hue_min", 90},
        {"dart_blue_hue_max", 135},
        {"dart_min_area", 20},
        {"dart_max_area", 5000},
        {"dart_min_aspect", 0.15},
        {"dart_max_aspect", 8},
        {"dart_close_kernel", 3},
        {"dart_max_distance", 120},
        {"dart_min_motion", 4},
        {"dart_max_lost_frames", 5},
        {"dart_max_track_points", 100},
        {"dart_draw_debug", 1},
    };

    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    saved_.clear();
    dart_revision_ = 0;
    for (const auto &def : defs_)
    {
        auto it = from_yaml.find(def.name);
        double value = def.min_value;
        if (it != from_yaml.end())
            value = it->second;
        else if (kDefaults.count(def.name))
            value = kDefaults.at(def.name);
        values_[def.name] = value;
        saved_[def.name] = value;
    }
}

void RuntimeParams::setConfigPath(const std::string &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    config_path_ = path;
}

RuntimeParams::Snapshot RuntimeParams::snapshot(uint64_t *dart_revision) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (dart_revision) *dart_revision = dart_revision_;
    return values_;
}

RuntimeParams::Snapshot RuntimeParams::savedSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return saved_;
}

bool RuntimeParams::dirty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &def : defs_)
    {
        auto v = values_.find(def.name);
        auto s = saved_.find(def.name);
        if (v == values_.end() || s == saved_.end() || std::abs(v->second - s->second) > 1e-9)
            return true;
    }
    return false;
}

const ParamDef *RuntimeParams::findDef(const std::string &name) const
{
    for (const auto &def : defs_)
        if (def.name == name)
            return &def;
    return nullptr;
}

bool RuntimeParams::validateValue(const ParamDef &def, double value, std::string *error) const
{
    if (!std::isfinite(value))
    {
        *error = def.label + "（" + def.name + "）不是有效数字";
        return false;
    }
    if (value < def.min_value - 1e-9 || value > def.max_value + 1e-9)
    {
        std::ostringstream oss;
        oss << def.label << "（" << def.name << "）超出范围 [" << def.min_value << ", "
            << def.max_value << "]";
        *error = oss.str();
        return false;
    }
    if (def.is_int && std::abs(value - std::llround(value)) > 1e-9)
    {
        *error = def.label + "（" + def.name + "）必须是整数";
        return false;
    }
    return true;
}

bool RuntimeParams::validateCross(const Snapshot &candidate, std::string *error) const
{
    auto at = [&candidate](const std::string &name) {
        auto it = candidate.find(name);
        return (it == candidate.end()) ? 0.0 : it->second;
    };
    if (at("min_radius") > at("max_radius"))
    {
        *error = "最小半径不能大于最大半径";
        return false;
    }
    if (at("min_area") > at("max_area"))
    {
        *error = "最小面积不能大于最大面积";
        return false;
    }
    if (at("min_aspect_ratio") > at("max_aspect_ratio"))
    {
        *error = "宽高比下限不能大于上限";
        return false;
    }
    if (at("dist_near_thresh") > at("dist_far_thresh"))
    {
        *error = "近距离阈值不能大于远距离阈值";
        return false;
    }
    if (at("dart_min_area") > at("dart_max_area") || at("dart_min_aspect") > at("dart_max_aspect") ||
        at("dart_blue_hue_min") > at("dart_blue_hue_max") || at("dart_red_low_max") >= at("dart_red_high_min") ||
        at("dart_min_motion") > at("dart_max_distance") || static_cast<int>(at("dart_close_kernel")) % 2 == 0)
    {
        *error = "飞镖参数范围冲突或形态学核不是奇数";
        return false;
    }
    return true;
}

bool RuntimeParams::applyValues(
    const std::map<std::string, double> &values, const std::string &source, std::string *error,
    std::vector<std::pair<std::string, std::pair<double, double>>> *changes)
{
    if (values.empty())
    {
        if (error)
            *error = "没有要应用的参数";
        return false;
    }

    std::vector<std::pair<std::string, std::pair<double, double>>> applied_changes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 1) 整组校验：任一失败则整组不生效，避免参数部分更新导致不一致
        Snapshot candidate = values_;
        for (const auto &kv : values)
        {
            const ParamDef *def = findDef(kv.first);
            if (!def)
            {
                if (error)
                    *error = "未知参数: " + kv.first;
                return false;
            }
            if (!def->web_editable || !def->hot)
            {
                if (error)
                    *error = def->label + "（" + kv.first + "）不支持热更新";
                return false;
            }
            std::string value_error;
            if (!validateValue(*def, kv.second, &value_error))
            {
                if (error)
                    *error = value_error;
                return false;
            }
            candidate[kv.first] = kv.second;
        }
        std::string cross_error;
        if (!validateCross(candidate, &cross_error))
        {
            if (error)
                *error = cross_error;
            return false;
        }

        // 2) 应用并记录实际变化项
        for (const auto &kv : values)
        {
            const double old_value = values_.count(kv.first) ? values_[kv.first] : 0.0;
            if (std::abs(old_value - kv.second) < 1e-9)
                continue;
            values_[kv.first] = kv.second;
            applied_changes.push_back({kv.first, {old_value, kv.second}});
        }
        for (const auto &change : applied_changes)
            if (change.first.rfind("dart_", 0) == 0 && change.first != "dart_draw_debug") {
                ++dart_revision_;
                break;
            }
    }

    // 3) 通知事件（锁外调用，避免回调与参数锁互相等待）
    for (const auto &change : applied_changes)
        emitEvent(change.first, source, change.second.first, change.second.second);

    if (changes)
        *changes = applied_changes;
    return true;
}

bool RuntimeParams::applyJsonValues(const std::string &json_values, const std::string &source,
                                   std::string *error, std::string *applied_names)
{
    std::map<std::string, double> values;
    std::string parse_error;
    if (!parseFlatJsonObject(json_values, &values, &parse_error))
    {
        if (error)
            *error = parse_error;
        return false;
    }
    std::vector<std::pair<std::string, std::pair<double, double>>> changes;
    if (!applyValues(values, source, error, &changes))
        return false;
    if (applied_names)
    {
        std::ostringstream oss;
        for (std::size_t i = 0; i < changes.size(); ++i)
        {
            if (i > 0)
                oss << ",";
            oss << changes[i].first;
        }
        *applied_names = oss.str();
    }
    return true;
}

void RuntimeParams::emitEvent(const std::string &name, const std::string &source, double old_value,
                              double new_value)
{
    std::function<void(const ParamChangeEvent &)> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = sink_;
    }
    if (!sink)
        return;
    ParamChangeEvent event;
    event.parameter = name;
    event.source = source;
    event.old_value = old_value;
    event.new_value = new_value;
    event.result = "applied";
    event.frame_id = frame_id_.load();
    event.timestamp_us = timestamp_us_.load();
    sink(event);
}

bool RuntimeParams::saveToYaml(const std::string &path, std::string *error)
{
    Snapshot snapshot_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_copy = values_;
    }
    std::string cross_error;
    if (!validateCross(snapshot_copy, &cross_error))
    {
        if (error)
            *error = cross_error;
        return false;
    }

    try
    {
        // 读原文（保留注释/顺序），只替换目标参数的值，最后交给 yaml-cpp 解析校验
        std::ifstream in(path, std::ios::in);
        if (!in.is_open())
        {
            if (error)
                *error = "无法读取配置文件: " + path;
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string text = buffer.str();

        int replaced_count = 0;
        std::vector<std::string> missing;
        for (const auto &def : defs_)
        {
            if (!def.web_editable || !def.hot)
                continue;
            auto it = snapshot_copy.find(def.name);
            if (it == snapshot_copy.end())
                continue;
            std::string value_text = formatYamlValue(it->second, def.is_int);
            if (def.name == "dart_color") value_text = it->second == 0 ? "red" : "blue";
            if (def.name == "dart_enabled" || def.name == "dart_draw_debug") value_text = it->second == 0 ? "false" : "true";
            if (replaceYamlScalar(text, splitPath(def.yaml_section), def.yaml_key, value_text))
                ++replaced_count;
            else
                missing.push_back(def.name);
        }
        if (!missing.empty())
        {
            if (error) *error = "配置缺少可保存参数，请补充节点后重试: " + missing.front();
            return false;
        }
        if (replaced_count == 0)
        {
            if (error)
                *error = "配置文件中未找到任何可保存的参数节点";
            return false;
        }

        // 校验替换后的文本仍是合法 YAML，避免写出损坏配置
        try
        {
            YAML::Load(text);
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = std::string("保存后配置校验失败: ") + e.what();
            return false;
        }

        const fs::path final_path(path);
        fs::path tmp_path = final_path;
        tmp_path += ".tmp";
        {
            std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
            if (!out.is_open())
            {
                if (error)
                    *error = "无法写入临时配置文件: " + tmp_path.string();
                return false;
            }
            out << text;
            if (!out.good())
            {
                if (error)
                    *error = "配置写入失败: " + tmp_path.string();
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp_path, final_path, ec);
        if (ec)
        {
            if (error)
                *error = "配置文件替换失败: " + ec.message();
            std::error_code rm_ec;
            fs::remove(tmp_path, rm_ec);
            return false;
        }
        if (!missing.empty())
        {
            std::cout << "[Params] 配置保存: 以下参数在文件中未找到对应节点，未写入: ";
            for (std::size_t i = 0; i < missing.size(); ++i)
                std::cout << (i > 0 ? ", " : "") << missing[i];
            std::cout << std::endl;
        }
    }
    catch (const std::exception &e)
    {
        if (error)
            *error = std::string("配置保存失败: ") + e.what();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        saved_ = snapshot_copy;
        config_path_ = path;
    }
    return true;
}

std::string RuntimeParams::jsonValue(double value, bool is_int) const
{
    std::ostringstream oss;
    if (is_int)
    {
        oss << static_cast<long long>(std::llround(value));
    }
    else
    {
        oss.precision(10);
        oss << value;
    }
    return oss.str();
}

std::string RuntimeParams::paramsJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":true,";
    oss << "\"config_path\":" << jsonQuoted(config_path_) << ",";
    oss << "\"running\":{";
    bool first = true;
    for (const auto &def : defs_)
    {
        auto it = values_.find(def.name);
        if (it == values_.end())
            continue;
        if (!first)
            oss << ",";
        first = false;
        oss << jsonQuoted(def.name) << ":" << jsonValue(it->second, def.is_int);
    }
    oss << "},\"saved\":{";
    first = true;
    for (const auto &def : defs_)
    {
        auto it = saved_.find(def.name);
        if (it == saved_.end())
            continue;
        if (!first)
            oss << ",";
        first = false;
        oss << jsonQuoted(def.name) << ":" << jsonValue(it->second, def.is_int);
    }
    oss << "},\"dirty\":[";
    first = true;
    for (const auto &def : defs_)
    {
        auto v = values_.find(def.name);
        auto s = saved_.find(def.name);
        const bool differs = v == values_.end() || s == saved_.end() ||
                             std::abs(v->second - s->second) > 1e-9;
        if (!differs)
            continue;
        if (!first)
            oss << ",";
        first = false;
        oss << jsonQuoted(def.name);
    }
    oss << "],\"params\":[";
    for (std::size_t i = 0; i < defs_.size(); ++i)
    {
        const auto &def = defs_[i];
        if (i > 0)
            oss << ",";
        oss << "{";
        oss << "\"name\":" << jsonQuoted(def.name) << ",";
        oss << "\"label\":" << jsonQuoted(def.label) << ",";
        oss << "\"unit\":" << jsonQuoted(def.unit) << ",";
        oss << "\"group\":" << jsonQuoted(def.group) << ",";
        oss << "\"type\":" << jsonQuoted(def.is_int ? "int" : "double") << ",";
        oss << "\"min\":" << jsonValue(def.min_value, def.is_int) << ",";
        oss << "\"max\":" << jsonValue(def.max_value, def.is_int) << ",";
        oss << "\"hot\":" << (def.hot ? "true" : "false") << ",";
        oss << "\"web_editable\":" << (def.web_editable ? "true" : "false") << ",";
        oss << "\"hint\":" << jsonQuoted(def.hint);
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string RuntimeParams::runningJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto &def : defs_)
    {
        auto it = values_.find(def.name);
        if (it == values_.end())
            continue;
        if (!first)
            oss << ",";
        first = false;
        oss << jsonQuoted(def.name) << ":" << jsonValue(it->second, def.is_int);
    }
    oss << "}";
    return oss.str();
}

void RuntimeParams::setFrameContext(uint64_t frame_id, int64_t timestamp_us)
{
    frame_id_.store(frame_id);
    timestamp_us_.store(timestamp_us);
}

void RuntimeParams::setEventSink(std::function<void(const ParamChangeEvent &)> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

std::string RuntimeParams::configPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return config_path_;
}

} // namespace webdebug
