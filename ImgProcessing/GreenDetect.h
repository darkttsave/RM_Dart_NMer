/**
 * @file GreenDetect.h
 * @brief 绿灯识别
 * @author lxy
 * @date 2025-10-24
 */

#ifndef GREEN_TARGET_DETECTOR_H
#define GREEN_TARGET_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <vector>

using namespace cv;

/**
 * @brief 绿灯识别
 * 用于绿灯识别，计算转向方向
 */
class GreenDetect
{
public:
    /**
     * @brief 目标信息结构体
     */
    struct TargetInfo
    {
        Point2f center;     // 目标中心点
        float radius;       // 目标半径
        bool detected;      // 是否检测到目标
        float turnFlag;     // 转向标志：0.0=停止，1.0=左转，2.0=右转
        float distanceFlag; // 距离阶段：1.0=加速，2.0=减速，3.0=对准/无目标
        Rect boundingBox;   // 边界框
    };

    /**
     * @brief 构造函数
     */
    GreenDetect();

    /**
     * @brief 析构函数
     */
    ~GreenDetect();

    /**
     * @brief 检测绿灯
     * @param image 输入图像
     * @return 目标信息
     */
    TargetInfo detectGreenTarget(const Mat &image);

    /**
     * @brief 获取调试图像
     * @return 调试图像
     */
    Mat getDebugImage() const { return debugImage_; }

    /**
     * @brief 获取二值化图像
     * @return 二值化图像
     */
    Mat getBinaryImage() const { return binaryImage_; }

    /**
     * @brief 设置转向阈值
     * @param threshold 转向阈值（像素）
     */
    void setTurnThreshold(float threshold) { turnThreshold_ = threshold; }
    void setRadiusFilter(int minRadius, int maxRadius) { minRadius_ = minRadius; maxRadius_ = maxRadius; }
    void setBrightThresh(int v) { brightThresh_ = v; }
    void setGreenWeightThresh(float v) { greenWeightThresh_ = v; }

    void setShapeFilter(double minCircularity, double minAspectRatio, double maxAspectRatio)
    {
        minCircularity_ = minCircularity;
        minAspectRatio_ = minAspectRatio;
        maxAspectRatio_ = maxAspectRatio;
    }

    void setAreaFilter(double minArea, double maxArea)
    {
        minArea_ = minArea;
        maxArea_ = maxArea;
    }

    // 设置距离控制阈值（像素）
    void setDistanceThresholds(float farThresh, float nearThresh)
    {
        distFarThresh_ = farThresh;
        distNearThresh_ = nearThresh;
    }

private:
    /**
     * @brief 预处理图像，提取绿色通道
     * @param image 输入图像
     * @return 绿色通道二值化图像
     */
    Mat preprocessGreenChannel(const Mat &image);

    /**
     * @brief 检测圆形目标
     * @param binaryImage 二值化图像
     * @return 检测到的圆形目标
     */
    std::vector<Vec3f> detectCircles(const Mat &binaryImage);

    /**
     * @brief 计算转向方向
     * @param targetCenter 目标中心点
     * @param result 目标信息结构体引用
     */
    void calculateTurnDirection(const Point2f &targetCenter, TargetInfo &result);

    // 参数设置
    int minRadius_;       // 最小半径
    int maxRadius_;       // 最大半径
    Point2f imageCenter_; // 图像中心点
    float turnThreshold_; // 转向阈值（像素）

    // 亮区+绿色权重参数
    int brightThresh_;        // HSV V通道亮度阈值
    float greenWeightThresh_; // 归一化绿色权重阈值 G/(R+G+B+1)

    // 时间滤波参数
    int consecutiveFrames_; // 连续检测到的帧数
    int requiredFrames_;    // 进入追踪所需连续检测帧数
    int lostFrames_;        // 连续未检测到的帧数
    int requiredLostFrames_;// 进入丢失所需连续未检测帧数
    bool tracking_;         // 当前是否处于追踪状态
    Point2f lastCenter_;    // 上一帧的中心点
    float maxCenterDist_;   // 中心点最大移动距离

    // 形态学参数
    int kernelSize_;      // 形态学核大小
    int openIterations_;  // 开运算迭代次数
    int closeIterations_; // 闭运算迭代次数

    // 距离控制阈值（像素）
    float distFarThresh_;
    float distNearThresh_;

    // 形状过滤参数
    double minCircularity_; // 最小圆形度（0~1），矩形≈0.785，圆=1
    double minAspectRatio_; // 外接矩形最小宽高比
    double maxAspectRatio_; // 外接矩形最大宽高比
    double minArea_;        // 轮廓最小面积（像素²）
    double maxArea_;        // 轮廓最大面积（像素²）

    // 调试图像
    Mat debugImage_;  // 调试图像
    Mat binaryImage_; // 二值化图像
};

#endif // GREEN_TARGET_DETECTOR_H
