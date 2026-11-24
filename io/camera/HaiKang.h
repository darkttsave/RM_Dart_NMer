// std
#include <string>
#include <cstdint>
#include <chrono>

// cv
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <MvCameraControl.h>

#include <yaml-cpp/yaml.h>

// rmvl
#include <rmvl/camera/hik_camera.h>

class Camera_
{
private:
    // 成员变量声明
    int exposure;
    int gain;
    std::string serial_number;
    rm::HikCamera* cap = nullptr;
    std::uint64_t applied_generation = 0;
    int applied_exposure = 0;
    int applied_gain = 0;
    std::chrono::steady_clock::time_point last_parameter_warning{};
    
    // 成员函数声明
    void loadCameraConfig();
    bool enumerateDevices();
    bool createCameraHandle();
    bool setCameraParameters();
    void setupDebugMode();
    
public:
    Camera_();
    ~Camera_();
    bool isOpened() const;
    void enableDiagnostics(bool enabled) { if (cap) cap->enableDiagnostics(enabled); }
    rm::HikFrameDiagnostics diagnostics() const { return cap ? cap->diagnostics() : rm::HikFrameDiagnostics{}; }
    double actualExposure() const { return isOpened() ? cap->get(rm::CAMERA_EXPOSURE) : -1; }
    double actualGain() const { return isOpened() ? cap->get(rm::CAMERA_GAIN) : -1; }
    void getimage(cv::Mat &image);
    void setCameraExposureGain(int exp, int g);
};
