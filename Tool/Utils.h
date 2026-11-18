/**
 * @file Utils.h
 * @brief 飞镖制导系统工具函数
 * @author lxy
 * @date 2025-10-24
 */

#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <filesystem>
#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

/**
 * @brief 生成运行时间戳（格式：yyMMddHHmm）
 * @return 时间戳字符串
 */
std::string makeRunTimestamp();

/**
 * @brief 解析项目根目录
 * @param loadedConfigPath 已加载的配置文件路径
 * @return 项目根目录路径
 */
fs::path resolveProjectRoot(const std::string &loadedConfigPath);

/**
 * @brief 设置终端日志捕获（将终端输出同时写入文件）
 * @param logFilePath 日志文件路径
 * @param lineLimit 捕获的行数限制
 * @return 是否成功设置
 */
bool setupTerminalLogCapture(const fs::path &logFilePath, int lineLimit);

/**
 * @brief 恢复终端输出到原始状态
 */
void restoreTerminalOutput();

#endif // UTILS_H
