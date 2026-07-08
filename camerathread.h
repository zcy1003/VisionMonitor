#ifndef CAMERATHREAD_H
#define CAMERATHREAD_H

#include <QThread>
#include <QString>
#include <QVector>
#include <atomic>
#include <opencv2/opencv.hpp>
#include "visiondetector.h"

class CameraThread : public QThread
{
    Q_OBJECT

public:
    explicit CameraThread(QObject *parent = nullptr);
    ~CameraThread();

    // 外部调用这个来停止线程
    void stopThread();
    void setDetectionEnabled(bool enabled);
    bool loadYoloModel(const QString &modelPath, QString *message = nullptr);
    QString detectorName() const;

    cv::Mat inspectImage(const cv::Mat &image,
                         QVector<DetectionResult> *results,
                         double *inferenceMs = nullptr);

protected:
    // 线程执行体，重写这个方法
    void run() override;

signals:
    // 每帧画面准备好后发出这个信号
    void frameReady(cv::Mat frame, QVector<DetectionResult> results, double inferenceMs);
    // 出错时(摄像头打不开)发出
    void cameraError(QString message);
    // 摄像头真正读到首帧后发出，用于在界面日志里记录实际使用的后端。
    void cameraOpened(QString message);

private:
    // 逐个尝试 Windows 摄像头后端，并用实际读帧结果判断后端是否可用。
    bool openCamera(cv::VideoCapture *capture, QString *message);

    std::atomic_bool m_running;
    std::atomic_bool m_detectionEnabled;
    VisionDetector m_detector;
};

#endif // CAMERATHREAD_H
