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
    setCameraExposureGain(exposure, gain);
    return cap != nullptr && cap->isOpened() &&
           applied_generation == cap->connectionGeneration();
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
    if (cap == nullptr || !cap->read(image))
        image.release(); // 失败时不得把上一帧误认为新帧。
}

void Camera_::setCameraExposureGain(int exp, int g)
{
    if (cap == nullptr || !cap->isOpened()) return;
    try
    {
        const auto generation = cap->connectionGeneration();
        if (applied_generation == generation && applied_exposure == exp && applied_gain == g)
            return;
        const bool manualOk = applied_generation == generation || cap->set(rm::CAMERA_MANUAL_EXPOSURE);
        const bool exposureOk = manualOk && cap->set(rm::CAMERA_EXPOSURE, exp);
        const bool gainOk = cap->set(rm::CAMERA_GAIN, g);
        if (manualOk && exposureOk && gainOk)
        {
            applied_generation = generation;
            applied_exposure = exp;
            applied_gain = g;
            return;
        }
    }
    catch (const std::exception &) {}
    // 部分写入也可能改变设备，失效缓存以保证后续完整重试。
    applied_generation = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_parameter_warning >= std::chrono::seconds(1))
    {
        std::cerr << "[Camera] 曝光或增益设置失败，将重试" << std::endl;
        last_parameter_warning = now;
    }
}
