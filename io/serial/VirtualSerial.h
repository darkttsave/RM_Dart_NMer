/**
 * @file VirtualSerial.h
 * @brief 飞镖制导串口通信类
 * @author lxy
 * @date 2025-10-24
 */

#ifndef VIRTUAL_SERIAL_H
#define VIRTUAL_SERIAL_H

#include <string>
#include <cstdint>
#include <vector>
#include <chrono>

/**
 * @brief 飞镖制导串口通信类
 * 提供与电控系统的串口通信功能（仅发送）
 * 发送数据包格式：帧头(1字节) + turnFlag(4字节) + CRC校验(2字节) = 7字节
 * turnFlag：0=停止转动, 1=向左转, 2=向右转
 */

class VirtualSerial
{
public:
    // 帧格式常量
    static const uint8_t FRAME_HEADER = 0x5A; // 帧头标识

    /**
     * @brief 构造函数
     * @param portName 串口设备名称，默认为"/dev/ttyACM0"
     */
    explicit VirtualSerial(const std::string &portName = "/dev/ttyACM0");

    /**
     * @brief 析构函数
     */
    ~VirtualSerial();

    // 禁用拷贝
    VirtualSerial(const VirtualSerial &) = delete;
    VirtualSerial &operator=(const VirtualSerial &) = delete;

    /**
     * @brief 打开串口
     * @return 是否成功
     */
    bool Open();

    struct YawFeedback {
        bool seen = false;
        bool valid = false;
        float degrees = 0;
        int64_t received_epoch_ms = 0;
        std::chrono::steady_clock::time_point received_at{};
    };
    // 非阻塞读取，按 T0.2 解析 0x5B；不改变 0x5A 发送。
    YawFeedback PollYawFeedback(int timeout_ms);

    /**
     * @brief 关闭串口
     */
    void Close();

    /**
     * @brief 发送浮点数控制指令（带重试机制）
     * @param turnFlag 转向标志（0=停止转动, 1=向左转, 2=向右转）
     * @param maxRetries 最大重试次数，默认3次
     * @return 是否成功
     */
    bool sendCommand(float turnFlag, float distanceFlag = 0.0f, float clearFlag = 1.0f, int maxRetries = 3);

    /**
     * @brief 检查串口是否已打开
     * @return 是否已打开
     */
    bool IsOpen() const { return serialFd_ >= 0; }

    /**
     * @brief 设置是否为模拟模式（无串口运行）
     * @param simulated 是否启用模拟模式
     */
    void SetSimulated(bool simulated) { simulated_ = simulated; }

    /**
     * @brief 设置是否在终端输出发送到电控端的值
     * @param enabled true=输出，false=关闭
     */
    void SetTxLogEnabled(bool enabled) { txLogEnabled_ = enabled; }

    /**
     * @brief 检查是否开启发送日志输出
     * @return 是否开启
     */
    bool IsTxLogEnabled() const { return txLogEnabled_; }

    /**
     * @brief 检查是否处于模拟模式
     * @return 是否处于模拟模式
     */
    bool IsSimulated() const { return simulated_; }

    /**
     * @brief 获取串口设备名称
     * @return 设备名称
     */
    const std::string &GetPortName() const { return portName_; }

    /**
     * @brief 启用自动重连功能
     * @param enabled true=启用自动重连，false=禁用
     */
    void SetAutoReconnect(bool enabled) { autoReconnect_ = enabled; }

    /**
     * @brief 尝试重新连接串口（扫描可用设备）
     * @return 是否成功重连
     */
    bool TryReconnect();

private:
    /**
     * @brief 配置串口参数
     * @return 是否成功
     */
    bool ConfigurePort();

    /**
     * @brief 扫描可用的串口设备（/dev/ttyACM*）
     * @return 找到的设备名称，未找到返回空字符串
     */
    std::string FindAvailablePort();

    std::vector<uint8_t> rxBuffer_;
    YawFeedback yawFeedback_;
    int serialFd_;              // 串口文件描述符
    std::string portName_;      // 串口设备名称
    bool simulated_ = false;    // 模拟模式标志
    bool txLogEnabled_ = false; // 是否输出发送日志
    bool autoReconnect_ = true; // 自动重连标志
};

#endif // VIRTUAL_SERIAL_H