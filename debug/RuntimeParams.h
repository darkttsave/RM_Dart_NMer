/**
 * @file RuntimeParams.h
 * @brief 运行时参数管理（M3）：网页/控制口 → 参数校验 → 权威运行值 → 视觉算法快照
 *
 * 设计目标（对应 taskDART-WEB-M3）：
 * - C++ 运行时状态是实际生效值的唯一权威来源：Flask 只做转发，网页不持有独立状态；
 * - 视觉线程每帧取一次"完整参数快照"，同一帧处理过程中不会混用修改前后的参数；
 * - 应用（改运行值）与保存（写 YAML）分离：保存使用临时文件 + 原子替换，保留无关配置节点；
 * - 每次实际生效的修改产生一条参数事件（来源 web / trackbar），由 Recorder 在录制期间写入
 *   events.jsonl，供离线回放查看与定位；
 * - 参数元数据（名称/类型/范围/单位/是否热更新/是否允许网页修改）集中在本文件，前后端共用。
 *
 * 说明：本类只管理"可热更新"的参数；串口、预览、录制等启动期参数不在此列。
 */

#ifndef WEB_DEBUG_RUNTIME_PARAMS_H
#define WEB_DEBUG_RUNTIME_PARAMS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace webdebug
{

/// 一次实际生效的参数修改（写入录制会话 events.jsonl）
struct ParamChangeEvent
{
    std::string parameter;   // 参数名（与 YAML 字段一致）
    std::string source;      // 修改来源：web / trackbar
    double old_value = 0.0;  // 修改前运行值
    double new_value = 0.0;  // 修改后运行值
    std::string result;      // 结果：applied（仅实际生效的修改才会产生事件）
    uint64_t frame_id = 0;   // 修改生效时最近一帧的算法帧号
    int64_t timestamp_us = 0;// 修改生效时刻（steady_clock 微秒）
};

/// 参数定义（静态元数据；前端分组与控件由它生成）
struct ParamDef
{
    std::string name;        // 参数名（唯一，与 YAML key 一致）
    std::string label;       // 中文名
    std::string unit;        // 单位（可为空）
    std::string group;       // 分组：color / shape / tracking / turn / camera
    std::string yaml_section;// YAML 路径（点分，如 detection.time_filter）
    std::string yaml_key;    // YAML 键名
    bool is_int = true;      // true=整数参数；false=浮点参数
    double min_value = 0.0;  // 允许最小值（含）
    double max_value = 0.0;  // 允许最大值（含）
    bool hot = true;         // 是否支持运行时热更新
    bool web_editable = true;// 是否允许网页修改（false=仅展示/需重启）
    std::string hint;        // 一行说明
};

/**
 * @brief 运行时参数（线程安全）
 *
 * 线程模型：
 * - 视觉线程：每帧 snapshot() 取一致快照（短锁拷贝，20 项参数为微秒级）；
 * - 控制口线程（Recorder 内）：applyValues()/saveToYaml() 修改运行值或写 YAML；
 * - 主线程 GUI：Trackbar 变化通过 applyValues(source="trackbar") 统一写回运行值。
 */
class RuntimeParams
{
public:
    using Snapshot = std::map<std::string, double>;

    RuntimeParams();

    /// 参数定义表（顺序即前端展示顺序）
    const std::vector<ParamDef> &definitions() const { return defs_; }

    /// 用 YAML 实际值初始化运行值与保存值；缺失项使用内置默认值
    void initialize(const std::map<std::string, double> &from_yaml);

    /// 记录配置文件路径（保存 / 展示用）
    void setConfigPath(const std::string &path);

    /// 视觉线程：取当前完整运行值快照（一次加锁拷贝，保证同帧一致）
    Snapshot snapshot() const;

    /// 已保存到 YAML 的值（启动值 / 最近一次保存值）
    Snapshot savedSnapshot() const;
    bool dirty() const;

    /**
     * @brief 应用一组参数（校验通过才整组生效，避免部分生效）
     * @param values 参数名 -> 新值
     * @param source 来源（web / trackbar）
     * @param error  失败原因
     * @param changes 实际变化项（参数名, 旧值, 新值）
     * @return 是否成功
     */
    bool applyValues(const std::map<std::string, double> &values,
                     const std::string &source,
                     std::string *error,
                     std::vector<std::pair<std::string, std::pair<double, double>>> *changes);

    /// 控制口：解析 {"name":"value",...} 形式的 JSON 对象并应用
    bool applyJsonValues(const std::string &json_values,
                         const std::string &source,
                         std::string *error,
                         std::string *applied_names);

    /// 保存当前运行值到 YAML（临时文件 + 原子替换；保留无关节点）
    bool saveToYaml(const std::string &path, std::string *error);

    /// 控制口：完整参数状态 JSON（running / saved / dirty / params 元数据）
    std::string paramsJson() const;

    /// 仅运行值 JSON（录制会话 config_snapshot/runtime_params.json 用）
    std::string runningJson() const;

    /// 主循环每帧更新帧上下文（参数事件记录 frame_id / 时间戳）
    void setFrameContext(uint64_t frame_id, int64_t timestamp_us);

    /// 参数生效回调（由 main 绑定到 Recorder::appendParameterEvent）
    void setEventSink(std::function<void(const ParamChangeEvent &)> sink);

    std::string configPath() const;

private:
    const ParamDef *findDef(const std::string &name) const;
    bool validateValue(const ParamDef &def, double value, std::string *error) const;
    bool validateCross(const Snapshot &candidate, std::string *error) const;
    void emitEvent(const std::string &name, const std::string &source,
                   double old_value, double new_value);
    std::string jsonValue(double value, bool is_int) const;

    mutable std::mutex mutex_;
    std::vector<ParamDef> defs_;
    Snapshot values_;        // 运行值（权威）
    Snapshot saved_;         // YAML 中的值
    std::string config_path_;
    std::function<void(const ParamChangeEvent &)> sink_;
    std::atomic<uint64_t> frame_id_{0};
    std::atomic<int64_t> timestamp_us_{0};
};

} // namespace webdebug

#endif // WEB_DEBUG_RUNTIME_PARAMS_H
