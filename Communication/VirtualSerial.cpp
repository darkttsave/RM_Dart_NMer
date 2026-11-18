/**
 * @file VirtualSerial.cpp
 * @brief 飞镖制导串口通信类实现
 * @author lxy
 * @date 2025-10-24
 */

#include "VirtualSerial.h"
#include "CRC16.hpp"
#include "ByteConverter.h"
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iomanip>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

VirtualSerial::VirtualSerial(const std::string &portName)
    : serialFd_(-1), portName_(portName)
{
}

VirtualSerial::~VirtualSerial()
{
    Close();
}

bool VirtualSerial::Open()
{
    if (IsOpen())
    {
        return true;
    }

    serialFd_ = open(portName_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serialFd_ < 0)
    {
        std::cerr << "[VirtualSerial] Error: Failed to open serial port" << std::endl;
        return false;
    }

    if (!ConfigurePort())
    {
        Close();
        return false;
    }

    return true;
}

bool VirtualSerial::sendCommand(float turnFlag, float distanceFlag, float clearFlag, int maxRetries)
{
    if (simulated_)
    {
        if (txLogEnabled_)
            std::cout << "[VirtualSerial] Simulated TX turnFlag = "<< turnFlag << "distanceFlag =" << distanceFlag << " clearFlag =" << clearFlag << std::endl;
        return true;
    }

    if (!IsOpen())
    {
        std::cerr << "[VirtualSerial] Error : Serial port not open" << std::endl;
        return false;
    }

    // 帧格式：帧头(1) + turnFlag(4) + distanceFlag(4) + clearFlag(4) + CRC(2) = 15字节
    uint8_t frame[15];
    frame[0] = FRAME_HEADER;

    auto t = ByteConverter::floatToBytesLittle(turnFlag);
    frame[1] = t[0];
    frame[2] = t[1];
    frame[3] = t[2];
    frame[4] = t[3];

    auto d = ByteConverter::floatToBytesLittle(distanceFlag);
    frame[5] = d[0];
    frame[6] = d[1];
    frame[7] = d[2];
    frame[8] = d[3];

    auto c = ByteConverter::floatToBytesLittle(clearFlag);
    frame[9] = c[0];
    frame[10] = c[1];
    frame[11] = c[2];
    frame[12] = c[3];

    frame[13] = 0x00;
    frame[14] = 0x00;
    crc16::Append_CRC16_Check_Sum(frame, 15);

    if (txLogEnabled_)
    {
        std::cout << "[VirtualSerial] TX turnFlag ="<< turnFlag << " distanceFlag =" << distanceFlag << " clearFlag =" << clearFlag << "frame =";
        std::ios oldState(nullptr);
        oldState.copyfmt(std::cout);
        for (int i = 0; i < 15; ++i)
            std::cout << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(frame[i]) << (i < 14 ? ""  : "");
        std::cout.copyfmt(oldState);
        std::cout << std::endl;
    }

    for (int retry = 0; retry < maxRetries; retry++)
    {
        if (write(serialFd_, frame, 15) == 15)
            return true;
        if (retry < maxRetries - 1)
            usleep(1000);
    }

    std::cerr << "[VirtualSerial] Error : Failed to send command after " << maxRetries << " retries" << std::endl;

    if (autoReconnect_ && TryReconnect())
    {
        std::cout << "[VirtualSerial] Retrying send after reconnect..." << std::endl;
        if (write(serialFd_, frame, 15) == 15)
            return true;
    }

    return false;
}

void VirtualSerial::Close()
{
    if (IsOpen())
    {
        close(serialFd_);
        serialFd_ = -1;
    }
}

bool VirtualSerial::ConfigurePort()
{
    struct termios tty;
    std::memset(&tty, 0, sizeof(tty));

    if (tcgetattr(serialFd_, &tty) != 0)
    {
        std::cerr << "[VirtualSerial] Error: Failed to get port attributes" << std::endl;
        return false;
    }

    // 波特率设置
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    // 控制模式标志
    tty.c_cflag &= ~PARENB; // 无校验
    tty.c_cflag &= ~CSTOPB; // 1个停止位
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8位数据位
    tty.c_cflag &= ~CRTSCTS;       // 禁用硬件流控
    tty.c_cflag |= CREAD | CLOCAL; // 启用接收器,忽略调制解调器状态线

    // 本地模式标志
    tty.c_lflag &= ~ICANON; // 原始模式
    tty.c_lflag &= ~ECHO;   // 禁用回显
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ECHONL;
    tty.c_lflag &= ~ISIG; // 禁用信号

    // 输入模式标志
    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // 禁用软件流控
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // 输出模式标志
    tty.c_oflag &= ~OPOST; // 原始输出
    tty.c_oflag &= ~ONLCR;

    // 应用设置
    if (tcsetattr(serialFd_, TCSANOW, &tty) != 0)
    {
        std::cerr << "[VirtualSerial] Error: Failed to set port attributes" << std::endl;
        return false;
    }

    // 清空缓冲区
    tcflush(serialFd_, TCIOFLUSH);

    return true;
}

std::string VirtualSerial::FindAvailablePort()
{
    DIR *dir = opendir("/dev");
    if (!dir)
        return "";

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name.find("ttyACM") == 0 || name.find("ttyUSB") == 0)
        {
            std::string fullPath = "/dev/" + name;
            int testFd = open(fullPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (testFd >= 0)
            {
                close(testFd);
                closedir(dir);
                return fullPath;
            }
        }
    }
    closedir(dir);
    return "";
}

bool VirtualSerial::TryReconnect()
{
    if (!autoReconnect_)
        return false;

    Close();

    // 等待设备节点重新出现（MCU 复位后 USB 重新枚举需要时间）
    constexpr int kMaxWaitMs = 5000;
    constexpr int kIntervalMs = 200;
    std::string newPort;
    for (int waited = 0; waited < kMaxWaitMs; waited += kIntervalMs)
    {
        newPort = FindAvailablePort();
        if (!newPort.empty())
            break;
        usleep(kIntervalMs * 1000);
    }

    if (newPort.empty())
    {
        std::cerr << "[VirtualSerial] Reconnect: No available port found" << std::endl;
        return false;
    }

    if (newPort != portName_)
    {
        std::cout << "[VirtualSerial] Reconnect: Port changed from " << portName_ << " to " << newPort << std::endl;
        portName_ = newPort;
    }

    if (Open())
    {
        std::cout << "[VirtualSerial] Reconnect: Successfully reconnected to " << portName_ << std::endl;
        return true;
    }

    return false;
}
