// Deterministic, frame-by-frame comparison harness. Build once against each
// version of dart_vision and compare the two output files byte-for-byte.
#include "GreenDetect.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <opencv2/opencv.hpp>

static std::uint64_t hashImage(const cv::Mat &image)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < image.rows; ++y) {
        const auto *p = image.ptr<unsigned char>(y);
        for (size_t x = 0; x < image.cols * image.elemSize(); ++x) {
            hash ^= p[x]; hash *= 1099511628211ull;
        }
    }
    return hash;
}
static void check(GreenDetect &detector, const cv::Mat &image, std::ofstream &masks, std::ofstream &data)
{
    const auto result = detector.detectGreenTarget(image);
    const auto mask = detector.getBinaryImage();
    for (int y = 0; y < mask.rows; ++y)
        masks.write(reinterpret_cast<const char *>(mask.ptr(y)), mask.cols);
    data << std::setprecision(9) << result.detected << ',' << result.center.x << ',' << result.center.y
         << ',' << result.radius << ',' << result.turnFlag << ',' << result.distanceFlag
         << ',' << result.boundingBox.x << ',' << result.boundingBox.y << ',' << result.boundingBox.width
         << ',' << result.boundingBox.height << ',' << detector.getCandidateCount() << ','
         << detector.getGreenPixelCount() << ',' << detector.isTracking() << ','
         << detector.getConsecutiveFrames() << ',' << detector.getLostFrames() << ','
         << hashImage(detector.getDebugImage()) << '\n';
}
int main(int argc, char **argv)
{
    if (argc != 4) { std::cerr << "usage: test_green_equivalence masks.bin results.csv raw_video.avi\n"; return 2; }
    std::ofstream masks(argv[1], std::ios::binary), data(argv[2]);
    if (!masks || !data) return 2;
    // Verify the float division itself for all possible numerator/denominator
    // values, not just for the thresholds sampled below.
    cv::Mat numerator(256, 766, CV_32F), denominator(256, 766, CV_32F), quotient;
    for (int g = 0; g < 256; ++g) for (int s = 1; s <= 766; ++s) {
        numerator.at<float>(g,s-1) = static_cast<float>(g);
        denominator.at<float>(g,s-1) = static_cast<float>(s);
    }
    cv::divide(numerator, denominator, quotient);
    for (int g = 0; g < 256; ++g) for (int s = 1; s <= 766; ++s) {
        const float native = static_cast<float>(g) / static_cast<float>(s);
        const float reference = quotient.at<float>(g,s-1);
        if (std::memcmp(&native, &reference, sizeof(float)) != 0) {
            std::cerr << "float division mismatch g=" << g << " sum=" << s << '\n'; return 1;
        }
    }
    GreenDetect detector;
    detector.setMorphology(1, 0, 0);
    detector.setBrightThresh(0);
    // Cover every 8-bit BGR combination at thresholds with important boundaries.
    const float weights[] = {0.4f, 1.0f / 3.0f, 0.5f, 0.7f, 0.0f};
    cv::Mat colors(256, 256, CV_8UC3);
    for (float threshold : weights) {
        detector.setGreenWeightThresh(threshold);
        for (int b = 0; b < 256; ++b) {
            for (int g = 0; g < 256; ++g)
                for (int r = 0; r < 256; ++r)
                    colors.at<cv::Vec3b>(g,r) = cv::Vec3b(b,g,r);
            check(detector, colors, masks, data);
        }
    }
    detector.setGreenWeightThresh(0.4f);
    detector.setBrightThresh(80);
    detector.setMorphology(7, 1, 3);
    detector.setRequiredFrames(5);
    detector.setRequiredLostFrames(4);
    cv::RNG rng(12345);
    for (int i = 0; i < 100; ++i) {
        if (i % 20 == 0) detector.setMorphology((i / 20) % 2 ? 3 : 7, 1, 3);
        cv::Mat frame(360, 640, CV_8UC3);
        rng.fill(frame, cv::RNG::UNIFORM, 0, 45);
        if ((i < 30 || i >= 40) && i % 17 != 0)
            cv::circle(frame, cv::Point(110 + (i*7)%430, 160), 18, cv::Scalar(10,240,10), -1);
        cv::circle(frame, cv::Point(75, 75), 13, cv::Scalar(200,210,0), -1);
        if (i % 2 == 0) {
            cv::Mat storage(364, 648, CV_8UC3);
            cv::Mat region = storage(cv::Rect(4, 2, 640, 360));
            frame.copyTo(region);
            check(detector, region, masks, data);
        } else check(detector, frame, masks, data);
    }
    cv::VideoCapture video(argv[3]);
    if (!video.isOpened()) { std::cerr << "cannot open raw camera recording\n"; return 1; }
    cv::Mat frame; int index = 0, tested = 0;
    while (video.read(frame)) {
        if (index++ % 30 == 0) { check(detector, frame, masks, data); ++tested; }
    }
    if (!tested || !masks || !data) return 1;
    std::cout << "PASS wrote 1280 exhaustive color frames, 100 synthetic frames, " << tested << " real camera frames\n";
}
