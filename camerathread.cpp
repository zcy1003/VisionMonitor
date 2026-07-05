#include "camerathread.h"

CameraThread::CameraThread(QObject *parent)
    : QThread(parent)
    , m_running(false)
    , m_detectionEnabled(true)
{
}

CameraThread::~CameraThread()
{
    stopThread();
}

void CameraThread::stopThread()
{
    m_running = false;   // 告诉run()："该停了"
    if (isRunning()) {
        wait(); // 阻塞等待线程真正结束，防止野指针
    }
    // 如果不调用 wait()，可能主线程已经把 CameraThread 对象析构了，但子线程还在跑——访问已释放的内存，程序崩溃
}

void CameraThread::setDetectionEnabled(bool enabled)
{
    m_detectionEnabled = enabled;
}

bool CameraThread::loadYoloModel(const QString &modelPath, QString *message)
{
    if (isRunning()) {
        if (message) {
            *message = "请先停止摄像头，再加载或切换模型。";
        }
        return false;
    }
    return m_detector.loadYoloModel(modelPath, message);
}

QString CameraThread::detectorName() const
{
    return m_detector.modelName();
}

cv::Mat CameraThread::inspectImage(const cv::Mat &image,
                                   QVector<DetectionResult> *results,
                                   double *inferenceMs)
{
    return m_detector.processFrame(image, results, inferenceMs);
}

void CameraThread::run()
{
    // 打开默认摄像头（笔记本内置）
    cv::VideoCapture cap(0);

    if (!cap.isOpened()) {
        emit cameraError("无法打开摄像头，请检查设备！");
        return;
    }

    // 设置分辨率
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    m_running = true;
    cv::Mat frame;   // cv::Mat 是 OpenCV 的"矩阵"类，图像在 OpenCV 里本质上就是一个多维矩阵（宽 × 高 × 颜色通道）

    while (m_running) {
        cap >> frame;    // 从摄像头读一帧到 frame 里
        //   打个比方：cv::Mat 就像一个快递盒，里面装着像素数据的数组。cap >> frame 相当于"摄像头拍一张照片，装进这个盒子里"

        if (frame.empty()) {
            QThread::msleep(10);
            continue;
        }

        QVector<DetectionResult> results;
        double inferenceMs = 0.0;
        cv::Mat output = m_detectionEnabled
            ? m_detector.processFrame(frame, &results, &inferenceMs)
            : frame.clone();

        // 注意：必须用 clone()。信号槽是异步的，下一轮采集可能覆盖当前帧数据。
        emit frameReady(output.clone(), results, inferenceMs);

        // 控制帧率不超过30fps
        QThread::msleep(33);
    }

    cap.release();
}
