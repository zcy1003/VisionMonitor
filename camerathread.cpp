#include "camerathread.h"

#include <QStringList>

namespace {
struct CameraBackend
{
    int api;
    const char *name;
};

const CameraBackend kBackends[] = {
#if defined(_WIN32)
    // Windows 当前机器验证过 MSMF 可能出现“能打开但读不到帧”，所以优先
    // DirectShow，再保留 MSMF 和默认后端用于兼容其他 Windows 环境。
    {cv::CAP_DSHOW, "DirectShow"},
    {cv::CAP_MSMF, "Media Foundation"},
#elif defined(__linux__)
    // Linux 桌面和工控机常见摄像头链路是 V4L2，默认后端作为发行版
    // OpenCV 编译选项不一致时的兜底。
    {cv::CAP_V4L2, "Video4Linux2"},
#endif
    {cv::CAP_ANY, "Default"}
};

// CPU 上 ONNX 推理通常慢于摄像头采集，因此实时 YOLO 每隔几帧运行一次，
// 跳过的帧复用上次检测框，兼顾演示流畅度和检测结果连续性。
constexpr int kYoloDetectEveryFrames = 3;

QString backendListText()
{
    QStringList names;
    for (const CameraBackend &backend : kBackends) {
        names << backend.name;
    }
    return names.join(", ");
}
}

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
    // 先通知采集循环退出再等待线程结束，避免析构时工作线程仍在读取
    // 摄像头帧或检测器状态。
    m_running = false;
    if (isRunning()) {
        wait();
    }
}

void CameraThread::setDetectionEnabled(bool enabled)
{
    m_detectionEnabled = enabled;
}

bool CameraThread::loadYoloModel(const QString &modelPath, QString *message)
{
    if (isRunning()) {
        if (message) {
            *message = "请先停止摄像头采集，再加载或切换模型。";
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

bool CameraThread::openCamera(cv::VideoCapture *capture, QString *message)
{
    QStringList errors;

    // 遍历多个设备索引，兼容内置摄像头、虚拟摄像头和外接 USB 摄像头在
    // 不同系统上枚举顺序不一致的情况。
    for (const CameraBackend &backend : kBackends) {
        for (int index = 0; index < 4; ++index) {
            cv::VideoCapture candidate;
            const bool opened = backend.api == cv::CAP_ANY
                ? candidate.open(index)
                : candidate.open(index, backend.api);

            if (!opened || !candidate.isOpened()) {
                errors << QString("index=%1 backend=%2: open failed")
                              .arg(index)
                              .arg(backend.name);
                continue;
            }

            candidate.set(cv::CAP_PROP_FRAME_WIDTH, 640);
            candidate.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
            candidate.set(cv::CAP_PROP_FPS, 30);
            candidate.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));

            cv::Mat frame;
            bool gotFrame = false;
            // 有些摄像头刚打开需要短暂预热；同时用非空帧过滤掉
            // isOpened() 成功但后续读不到图像的后端。
            for (int retry = 0; retry < 30; ++retry) {
                if (candidate.read(frame) && !frame.empty()) {
                    gotFrame = true;
                    break;
                }
                QThread::msleep(50);
            }

            if (!gotFrame) {
                errors << QString("index=%1 backend=%2: opened but no frame")
                              .arg(index)
                              .arg(backend.name);
                candidate.release();
                continue;
            }

            // 只把已经读到有效帧的 VideoCapture 移入工作线程，避免采集
            // 主循环启动后才发现后端不可用。
            *capture = std::move(candidate);
            if (message) {
                *message = QString("Camera opened: index=%1, backend=%2, %3x%4")
                               .arg(index)
                               .arg(backend.name)
                               .arg(frame.cols)
                               .arg(frame.rows);
            }
            return true;
        }
    }

    if (message) {
        *message = QString("Cannot open camera. Tried OpenCV backends: %1.\n"
                           "Common causes are camera permission, another process occupying the camera, "
                           "or OpenCV backend compatibility issues.\n\nAttempts:\n%2")
                       .arg(backendListText(), errors.join('\n'));
    }
    return false;
}

void CameraThread::run()
{
    cv::VideoCapture cap;
    QString openMessage;
    // 后端选择放在线程内部执行，失败信息通过信号回到 UI 线程，避免
    // 主界面在摄像头探测和预热期间卡住。
    if (!openCamera(&cap, &openMessage)) {
        emit cameraError(openMessage);
        return;
    }
    emit cameraOpened(openMessage);

    m_running = true;
    cv::Mat frame;
    int yoloFrameCounter = 0;
    bool hasCachedYoloResults = false;
    QVector<DetectionResult> cachedYoloResults;
    double cachedYoloInferenceMs = 0.0;

    while (m_running) {
        if (!cap.read(frame) || frame.empty()) {
            QThread::msleep(10);
            continue;
        }

        QVector<DetectionResult> results;
        double inferenceMs = 0.0;
        cv::Mat output;

        if (!m_detectionEnabled) {
            // 关闭检测时清空缓存框，避免用户再次开启检测后画出旧结果。
            hasCachedYoloResults = false;
            yoloFrameCounter = 0;
            output = frame.clone();
        } else if (m_detector.hasYoloModel()) {
            const bool shouldRunYolo = !hasCachedYoloResults
                || yoloFrameCounter % kYoloDetectEveryFrames == 0;

            if (shouldRunYolo) {
                // CPU YOLO 推理较重，只在选定帧上执行以降低实时预览卡顿。
                output = m_detector.processFrame(frame, &cachedYoloResults, &cachedYoloInferenceMs);
                hasCachedYoloResults = true;
            } else {
                // 跳过推理的帧绘制上次缓存框，让画面继续更新而不是每帧
                // 都阻塞在模型推理上。
                output = m_detector.annotateFrame(frame, cachedYoloResults);
            }

            results = cachedYoloResults;
            inferenceMs = cachedYoloInferenceMs;
            ++yoloFrameCounter;
        } else {
            // 传统规则检测计算量较小，可以每帧执行，便于无模型时演示。
            output = m_detector.processFrame(frame, &results, &inferenceMs);
        }

        // Qt 排队信号会跨线程异步传递，发送前 clone 图像，避免下一轮采集
        // 覆盖 cv::Mat 底层数据。
        emit frameReady(output.clone(), results, inferenceMs);
        // 推理不是瓶颈时，把采集循环控制在接近 30 FPS。
        QThread::msleep(33);
    }

    cap.release();
}
