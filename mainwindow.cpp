#include "mainwindow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {
cv::Mat readImageFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray bytes = file.readAll();
    const std::vector<uchar> buffer(bytes.begin(), bytes.end());
    return cv::imdecode(buffer, cv::IMREAD_COLOR);
}

bool writeImageFile(const QString &path, const cv::Mat &image)
{
    std::vector<uchar> buffer;
    const QString suffix = QFileInfo(path).suffix().isEmpty()
        ? "jpg"
        : QFileInfo(path).suffix();
    if (!cv::imencode(QString(".%1").arg(suffix).toStdString(), image, buffer)) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(reinterpret_cast<const char *>(buffer.data()),
                      static_cast<qint64>(buffer.size())) == static_cast<qint64>(buffer.size());
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

    m_cameraThread = new CameraThread(this);
    m_serialThread = new SerialThread(this);

    connect(m_cameraThread, &CameraThread::frameReady,
            this, &MainWindow::onFrameReady,
            Qt::QueuedConnection);
    connect(m_cameraThread, &CameraThread::cameraError,
            this, &MainWindow::onCameraError,
            Qt::QueuedConnection);
    connect(m_serialThread, &SerialThread::dataReceived,
            this, &MainWindow::onDataReceived,
            Qt::QueuedConnection);

    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onBtnStartClicked);
    connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::onBtnStopClicked);
    connect(m_btnLoadModel, &QPushButton::clicked, this, &MainWindow::onLoadModelClicked);
    connect(m_btnOpenImage, &QPushButton::clicked, this, &MainWindow::onOpenImageClicked);
    connect(m_btnSaveSnapshot, &QPushButton::clicked, this, &MainWindow::onSaveSnapshotClicked);
    connect(m_checkDetection, &QCheckBox::toggled, this, &MainWindow::onDetectionToggled);

    m_fpsTimer.start();
    appendLog("系统就绪：未加载 ONNX 模型时使用 OpenCV 规则检测。");
}

MainWindow::~MainWindow()
{
    m_cameraThread->stopThread();
    m_serialThread->stopThread();
}

void MainWindow::setupUi()
{
    setWindowTitle("工业视觉质检系统 - Qt + OpenCV + YOLOv8");
    resize(1180, 720);

    auto *central = new QWidget(this);
    auto *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(12);

    m_labelCamera = new QLabel("摄像头未启动\n可先点击“打开图片测试”验证检测流程", this);
    m_labelCamera->setMinimumSize(760, 570);
    m_labelCamera->setAlignment(Qt::AlignCenter);
    m_labelCamera->setStyleSheet("background:#15171a;color:#d7dde5;border:1px solid #30343a;font-size:18px;");
    mainLayout->addWidget(m_labelCamera, 1);

    auto *sidePanel = new QWidget(this);
    sidePanel->setFixedWidth(360);
    auto *sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(10);

    auto *qualityBox = new QGroupBox("质检结果", this);
    auto *qualityLayout = new QVBoxLayout(qualityBox);
    m_labelResult = new QLabel("结果: --", this);
    m_labelResult->setAlignment(Qt::AlignCenter);
    m_labelResult->setMinimumHeight(64);
    m_labelResult->setStyleSheet("background:#2b3138;color:white;font-size:26px;font-weight:bold;");
    m_labelDefectCount = new QLabel("缺陷数量: 0", this);
    m_labelInference = new QLabel("推理耗时: 0.0 ms", this);
    m_labelYield = new QLabel("良品率: --", this);
    m_labelFps = new QLabel("FPS: 0", this);
    m_labelModel = new QLabel("模型: OpenCV 规则检测", this);
    qualityLayout->addWidget(m_labelResult);
    qualityLayout->addWidget(m_labelDefectCount);
    qualityLayout->addWidget(m_labelInference);
    qualityLayout->addWidget(m_labelYield);
    qualityLayout->addWidget(m_labelFps);
    qualityLayout->addWidget(m_labelModel);

    auto *deviceBox = new QGroupBox("设备实时状态", this);
    auto *deviceLayout = new QVBoxLayout(deviceBox);
    m_labelTemp = new QLabel("温度: -- °C", this);
    m_labelPressure = new QLabel("压力: -- MPa", this);
    m_labelSpeed = new QLabel("转速: -- rpm", this);
    deviceLayout->addWidget(m_labelTemp);
    deviceLayout->addWidget(m_labelPressure);
    deviceLayout->addWidget(m_labelSpeed);

    auto *controlBox = new QGroupBox("控制", this);
    auto *controlLayout = new QVBoxLayout(controlBox);
    m_btnStart = new QPushButton("开始采集", this);
    m_btnStop = new QPushButton("停止采集", this);
    m_btnLoadModel = new QPushButton("加载 YOLOv8 ONNX", this);
    m_btnOpenImage = new QPushButton("打开图片测试", this);
    m_btnSaveSnapshot = new QPushButton("保存当前结果", this);
    m_checkDetection = new QCheckBox("启用视觉检测", this);
    m_checkDetection->setChecked(true);
    m_btnStop->setEnabled(false);
    m_btnSaveSnapshot->setEnabled(false);
    controlLayout->addWidget(m_btnStart);
    controlLayout->addWidget(m_btnStop);
    controlLayout->addWidget(m_btnLoadModel);
    controlLayout->addWidget(m_btnOpenImage);
    controlLayout->addWidget(m_btnSaveSnapshot);
    controlLayout->addWidget(m_checkDetection);

    auto *logBox = new QGroupBox("检测日志", this);
    auto *logLayout = new QVBoxLayout(logBox);
    m_logList = new QListWidget(this);
    logLayout->addWidget(m_logList);

    sideLayout->addWidget(qualityBox);
    sideLayout->addWidget(deviceBox);
    sideLayout->addWidget(controlBox);
    sideLayout->addWidget(logBox, 1);
    mainLayout->addWidget(sidePanel);

    setCentralWidget(central);
    statusBar()->showMessage("就绪");
}

void MainWindow::onBtnStartClicked()
{
    if (m_cameraThread->isRunning()) {
        return;
    }

    m_frameCount = 0;
    m_fpsTimer.restart();
    m_cameraThread->start();
    m_serialThread->start();

    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_btnLoadModel->setEnabled(false);
    m_btnOpenImage->setEnabled(false);
    statusBar()->showMessage("采集中...");
    appendLog("开始摄像头实时质检。");
}

void MainWindow::onBtnStopClicked()
{
    m_cameraThread->stopThread();
    m_serialThread->stopThread();

    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_btnLoadModel->setEnabled(true);
    m_btnOpenImage->setEnabled(true);
    statusBar()->showMessage("已停止");
    appendLog("采集已停止。");
}

void MainWindow::onFrameReady(cv::Mat frame, QVector<DetectionResult> results, double inferenceMs)
{
    m_frameCount++;
    if (m_fpsTimer.elapsed() >= 1000) {
        m_labelFps->setText(QString("FPS: %1").arg(m_frameCount));
        m_frameCount = 0;
        m_fpsTimer.restart();
    }

    displayFrame(frame);
    updateInspectionSummary(results, inferenceMs);
}

void MainWindow::onDataReceived(float temp, float pressure, float speed)
{
    m_labelTemp->setText(QString("温度: %1 °C").arg(temp, 0, 'f', 1));
    m_labelPressure->setText(QString("压力: %1 MPa").arg(pressure, 0, 'f', 2));
    m_labelSpeed->setText(QString("转速: %1 rpm").arg(static_cast<int>(speed)));

    if (temp >= 90.0f) {
        m_labelTemp->setStyleSheet("color:#c62828;font-weight:bold;");
        statusBar()->showMessage(QString("温度报警: %1°C").arg(temp, 0, 'f', 1));
    } else if (temp >= 85.0f) {
        m_labelTemp->setStyleSheet("color:#ef6c00;");
    } else {
        m_labelTemp->setStyleSheet("color:#2e7d32;");
    }
}

void MainWindow::onCameraError(QString message)
{
    QMessageBox::warning(this, "摄像头错误", message);
    onBtnStopClicked();
}

void MainWindow::onLoadModelClicked()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      "选择 YOLOv8 ONNX 模型",
                                                      QDir::currentPath(),
                                                      "ONNX Model (*.onnx)");
    if (path.isEmpty()) {
        return;
    }

    QString message;
    if (m_cameraThread->loadYoloModel(path, &message)) {
        m_labelModel->setText(QString("模型: %1").arg(m_cameraThread->detectorName()));
        appendLog(message);
        statusBar()->showMessage(message);
    } else {
        QMessageBox::warning(this, "模型加载失败", message);
        appendLog(message);
    }
}

void MainWindow::onOpenImageClicked()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      "选择待检测图片",
                                                      QDir::currentPath(),
                                                      "Images (*.png *.jpg *.jpeg *.bmp)");
    if (path.isEmpty()) {
        return;
    }

    cv::Mat image = readImageFile(path);
    if (image.empty()) {
        QMessageBox::warning(this, "图片错误", "无法读取图片，请检查文件路径。");
        return;
    }

    QVector<DetectionResult> results;
    double inferenceMs = 0.0;
    const cv::Mat annotated = m_cameraThread->inspectImage(image, &results, &inferenceMs);
    displayFrame(annotated);
    updateInspectionSummary(results, inferenceMs);
    appendLog(QString("完成图片检测：%1，缺陷 %2 个。")
                  .arg(QFileInfo(path).fileName())
                  .arg(results.size()));
}

void MainWindow::onSaveSnapshotClicked()
{
    if (m_lastFrame.empty()) {
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty()) {
        dir = QDir::currentPath();
    }

    const QString fileName = QString("inspection_%1.jpg")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString path = QDir(dir).filePath(fileName);

    if (writeImageFile(path, m_lastFrame)) {
        appendLog(QString("已保存结果图：%1").arg(path));
        statusBar()->showMessage(QString("已保存：%1").arg(path));
    } else {
        QMessageBox::warning(this, "保存失败", "无法保存当前结果图。");
    }
}

void MainWindow::onDetectionToggled(bool checked)
{
    m_cameraThread->setDetectionEnabled(checked);
    appendLog(checked ? "已启用视觉检测。" : "已关闭视觉检测，仅显示原始画面。");
}

void MainWindow::displayFrame(const cv::Mat &frame)
{
    if (frame.empty()) {
        return;
    }

    m_lastFrame = frame.clone();
    m_btnSaveSnapshot->setEnabled(true);
    const QPixmap pixmap = matToPixmap(frame);
    m_labelCamera->setPixmap(pixmap.scaled(m_labelCamera->size(),
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation));
}

void MainWindow::updateInspectionSummary(const QVector<DetectionResult> &results, double inferenceMs)
{
    m_totalInspected++;
    const bool ng = !results.isEmpty();
    if (ng) {
        m_ngCount++;
    }

    m_labelResult->setText(ng ? "NG" : "OK");
    m_labelResult->setStyleSheet(ng
        ? "background:#b71c1c;color:white;font-size:30px;font-weight:bold;"
        : "background:#1b5e20;color:white;font-size:30px;font-weight:bold;");
    m_labelDefectCount->setText(QString("缺陷数量: %1").arg(results.size()));
    m_labelInference->setText(QString("推理耗时: %1 ms").arg(inferenceMs, 0, 'f', 1));

    const double yield = m_totalInspected > 0
        ? 100.0 * (m_totalInspected - m_ngCount) / m_totalInspected
        : 0.0;
    m_labelYield->setText(QString("良品率: %1%  统计: %2/%3 NG")
                              .arg(yield, 0, 'f', 1)
                              .arg(m_ngCount)
                              .arg(m_totalInspected));

    if (ng && (!m_lastNg || m_totalInspected % 30 == 0)) {
        appendLog(QString("检测 NG：发现 %1 个缺陷，耗时 %2 ms。")
                      .arg(results.size())
                      .arg(inferenceMs, 0, 'f', 1));
    } else if (!ng && m_lastNg) {
        appendLog("检测恢复 OK。");
    }
    m_lastNg = ng;
}

void MainWindow::appendLog(const QString &message)
{
    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logList->insertItem(0, QString("[%1] %2").arg(time, message));
    while (m_logList->count() > 100) {
        delete m_logList->takeItem(m_logList->count() - 1);
    }
}

QPixmap MainWindow::matToPixmap(const cv::Mat &mat)
{
    cv::Mat rgb;
    if (mat.channels() == 1) {
        cv::cvtColor(mat, rgb, cv::COLOR_GRAY2RGB);
    } else {
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
    }

    QImage img(rgb.data,
               rgb.cols,
               rgb.rows,
               rgb.step,
               QImage::Format_RGB888);
    return QPixmap::fromImage(img.copy());
}
