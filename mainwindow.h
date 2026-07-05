#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QPixmap>
#include <opencv2/opencv.hpp>

#include "camerathread.h"
#include "serialthread.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onFrameReady(cv::Mat frame, QVector<DetectionResult> results, double inferenceMs);
    void onDataReceived(float temp, float pressure, float speed);
    void onCameraError(QString message);
    void onBtnStartClicked();
    void onBtnStopClicked();
    void onLoadModelClicked();
    void onOpenImageClicked();
    void onSaveSnapshotClicked();
    void onDetectionToggled(bool checked);

private:
    void setupUi();
    void displayFrame(const cv::Mat &frame);
    void updateInspectionSummary(const QVector<DetectionResult> &results, double inferenceMs);
    void appendLog(const QString &message);
    QPixmap matToPixmap(const cv::Mat &mat);

    CameraThread *m_cameraThread = nullptr;
    SerialThread *m_serialThread = nullptr;

    QLabel *m_labelCamera = nullptr;
    QLabel *m_labelFps = nullptr;
    QLabel *m_labelModel = nullptr;
    QLabel *m_labelResult = nullptr;
    QLabel *m_labelDefectCount = nullptr;
    QLabel *m_labelInference = nullptr;
    QLabel *m_labelYield = nullptr;
    QLabel *m_labelTemp = nullptr;
    QLabel *m_labelPressure = nullptr;
    QLabel *m_labelSpeed = nullptr;
    QListWidget *m_logList = nullptr;
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnStop = nullptr;
    QPushButton *m_btnLoadModel = nullptr;
    QPushButton *m_btnOpenImage = nullptr;
    QPushButton *m_btnSaveSnapshot = nullptr;
    QCheckBox *m_checkDetection = nullptr;

    QElapsedTimer m_fpsTimer;
    int m_frameCount = 0;
    int m_totalInspected = 0;
    int m_ngCount = 0;
    bool m_lastNg = false;

    cv::Mat m_lastFrame;
};

#endif // MAINWINDOW_H
