#include "HaiKang.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace
{
    YAML::Node loadConfigWithCandidates()
    {
        const std::vector<std::string> candidates = {
            "config/debug.yaml",
            "./config/debug.yaml",
            "../config/debug.yaml"};

        for (const auto &path : candidates)
        {
            try
            {
                return YAML::LoadFile(path);
            }
            catch (const std::exception &)
            {
            }
        }

        throw std::runtime_error("config/debug.yaml not found");
    }
} // namespace

Camera_::Camera_()
{
    loadCameraConfig();
    enumerateDevices();
    createCameraHandle();
    setCameraParameters();
    setupDebugMode();
}

Camera_::~Camera_()
{
    if (cap != nullptr)
    {
        delete cap;
        cap = nullptr;
    }
}

void Camera_::loadCameraConfig()
{
    try
    {
        YAML::Node config = loadConfigWithCandidates();
        exposure = config["camera"]["exposure"].as<int>();
        gain = config["camera"]["gain"].as<int>();
        serial_number = config["camera"]["serial_number"].as<std::string>();
    }
    catch (const std::exception &)
    {
        exposure = 3200;
        gain = 16;
        serial_number = "DA3694227";
    }
}

bool Camera_::enumerateDevices()
{
    MV_CC_DEVICE_INFO_LIST camera_list;
    int ret = MV_CC_EnumDevices(MV_USB_DEVICE, &camera_list);
    return ret == MV_OK && camera_list.nDeviceNum > 0;
}

bool Camera_::createCameraHandle()
{
    try
    {
        auto config = rm::CameraConfig::create(
            rm::GrabMode::Continuous,
            rm::RetrieveMode::OpenCV);
        cap = new rm::HikCamera(config, serial_number);
        return cap->isOpened();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool Camera_::setCameraParameters()
{
    if (cap == nullptr || !cap->isOpened()) return false;
    try
    {
        cap->set(rm::CAMERA_MANUAL_EXPOSURE);
        cap->set(rm::CAMERA_EXPOSURE, exposure);
        cap->set(rm::CAMERA_GAIN, gain);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool Camera_::isOpened() const
{
    return cap != nullptr && cap->isOpened();
}

void Camera_::setupDebugMode()
{
    // 保留接口，当前无额外控制窗口需要创建
}

void Camera_::getimage(cv::Mat &image)
{
    if (cap != nullptr)
    {
        cap->read(image);
    }
}

void Camera_::setCameraExposureGain(int exp, int g)
{
    if (cap == nullptr) return;
    try
    {
        cap->set(rm::CAMERA_EXPOSURE, exp);
        cap->set(rm::CAMERA_GAIN, g);
    }
    catch (const std::exception &) {}
}
