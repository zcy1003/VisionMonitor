#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

namespace {
struct Backend
{
    int api;
    const char *name;
};

std::vector<Backend> platformBackends()
{
    // 诊断工具要和主程序保持同一套后端优先级，排查结果才有参考价值。
    // Windows 保留 DirectShow/MSMF/default；Linux 优先 V4L2，再尝试默认后端。
    return {
#if defined(_WIN32)
        {cv::CAP_DSHOW, "DirectShow"},
        {cv::CAP_MSMF, "Media Foundation"},
#elif defined(__linux__)
        {cv::CAP_V4L2, "Video4Linux2"},
#endif
        {cv::CAP_ANY, "Default"}
    };
}

bool readWarmupFrame(cv::VideoCapture *capture, cv::Mat *frame)
{
    // 摄像头刚打开时可能需要预热；必须读到非空帧才认为后端真正可用。
    for (int retry = 0; retry < 30; ++retry) {
        if (capture->read(*frame) && !frame->empty()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}
}

int main()
{
    const std::vector<Backend> backends = platformBackends();

    bool anyFrame = false;
    std::cout << "OpenCV version: " << CV_VERSION << '\n';

    for (const Backend &backend : backends) {
        for (int index = 0; index < 6; ++index) {
            cv::VideoCapture cap;
            const bool opened = backend.api == cv::CAP_ANY
                ? cap.open(index)
                : cap.open(index, backend.api);

            std::cout << "index=" << index
                      << " backend=" << backend.name
                      << " open=" << (opened && cap.isOpened() ? "yes" : "no");

            if (!opened || !cap.isOpened()) {
                std::cout << '\n';
                continue;
            }

            cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
            cap.set(cv::CAP_PROP_FPS, 30);
            cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));

            cv::Mat frame;
            if (readWarmupFrame(&cap, &frame)) {
                anyFrame = true;
                std::cout << " frame=yes size=" << frame.cols << 'x' << frame.rows;
            } else {
                std::cout << " frame=no";
            }
            std::cout << '\n';
        }
    }

    return anyFrame ? 0 : 2;
}
