/**
 * @file FrameSource.h
 * @brief 可选模拟视频源（无工业相机时验证调试链路，M1）
 *
 * 说明：这是"无相机测试"专用入口，默认生产路径仍然直接使用 Camera_（海康相机），
 * 本模块不参与、不改变原有相机采集配置。
 *
 * 支持的 spec：
 *   synthetic       合成 1440x1080 绿色引导灯视频（带网格背景），可验证识别/锁定/曲线
 *   synthetic:none  合成画面但无绿色目标（用于验证"未检测"状态）
 *   image:<path>    循环读取单张测试图片
 *   video:<path>    循环播放视频文件
 */

#ifndef WEB_DEBUG_FRAME_SOURCE_H
#define WEB_DEBUG_FRAME_SOURCE_H

#include <memory>
#include <string>

#include <opencv2/core.hpp>

namespace webdebug
{

class FrameSource
{
public:
    virtual ~FrameSource() = default;

    /// 取下一帧；返回 false 表示源不可用（主循环应跳过本帧）
    virtual bool next(cv::Mat &out) = 0;

    /// 数据来源标识（写入调试数据 source 字段），如 "synthetic" / "sim_image"
    virtual const std::string &sourceKind() const = 0;

    /// 人类可读数据来源描述（用于界面"当前数据来源标识"）
    virtual const std::string &sourceLabel() const = 0;
};

/**
 * @brief 创建模拟帧源
 * @param spec 见文件头说明
 * @param error 失败原因（可为 nullptr）
 * @return 失败返回 nullptr
 */
std::unique_ptr<FrameSource> createFrameSource(const std::string &spec, std::string *error = nullptr);

} // namespace webdebug

#endif // WEB_DEBUG_FRAME_SOURCE_H
