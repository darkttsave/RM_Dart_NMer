// std
#include <string>

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
    void getimage(cv::Mat &image);
    void setCameraExposureGain(int exp, int g);
};
