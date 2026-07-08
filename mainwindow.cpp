#include "mainwindow.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringConverter>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "databasemanager.h"

namespace {
constexpr qint64 kLiveRecordIntervalMs = 2000;
constexpr int kMaxDeviceSamples = 120;
constexpr double kTemperatureWarning = 85.0;
constexpr double kTemperatureCritical = 90.0;
constexpr double kPressureLowWarning = 1.85;
constexpr double kPressureHighWarning = 2.30;
constexpr double kSpeedLowWarning = 1120.0;
constexpr double kSpeedHighWarning = 1280.0;

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

QString defectLabelsText(const QVector<DetectionResult> &results)
{
    QStringList labels;
    for (const DetectionResult &result : results) {
        if (!labels.contains(result.label)) {
            labels << result.label;
        }
    }
    return labels.join(',');
}

double maxConfidence(const QVector<DetectionResult> &results)
{
    double value = 0.0;
    for (const DetectionResult &result : results) {
        value = std::max(value, static_cast<double>(result.confidence));
    }
    return value;
}

QString formatHistoryItem(const InspectionRecord &record)
{
    return QString("#%1 [%2] %3 %4 defects=%5 %6ms")
        .arg(record.id)
        .arg(record.timestamp.toString("MM-dd HH:mm:ss"))
        .arg(record.source)
        .arg(record.result)
        .arg(record.defectCount)
        .arg(record.inferenceMs, 0, 'f', 1);
}

QString formatAlarmItem(const AlarmRecord &record)
{
    return QString("#%1 [%2] %3 %4 value=%5 threshold=%6 %7")
        .arg(record.id)
        .arg(record.timestamp.toString("MM-dd HH:mm:ss"))
        .arg(record.acknowledged ? "ACK" : "OPEN")
        .arg(record.level)
        .arg(record.value, 0, 'f', 2)
        .arg(record.threshold, 0, 'f', 2)
        .arg(record.message);
}

QString percentText(double value)
{
    return QString("%1%").arg(value, 0, 'f', 2);
}

QString reportTimeRange(const QVector<InspectionRecord> &inspections,
                        const QVector<AlarmRecord> &alarms)
{
    QDateTime start;
    QDateTime end;
    auto includeTime = [&start, &end](const QDateTime &timestamp) {
        if (!timestamp.isValid()) {
            return;
        }
        if (!start.isValid() || timestamp < start) {
            start = timestamp;
        }
        if (!end.isValid() || timestamp > end) {
            end = timestamp;
        }
    };

    for (const InspectionRecord &record : inspections) {
        includeTime(record.timestamp);
    }
    for (const AlarmRecord &record : alarms) {
        includeTime(record.timestamp);
    }

    if (!start.isValid() || !end.isValid()) {
        return "暂无历史数据";
    }
    return QString("%1 至 %2")
        .arg(start.toString("yyyy-MM-dd HH:mm:ss"),
             end.toString("yyyy-MM-dd HH:mm:ss"));
}

QString topCountsText(const QMap<QString, int> &counts, int maxItems)
{
    QVector<QPair<QString, int>> items;
    items.reserve(counts.size());
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        items.push_back(qMakePair(it.key(), it.value()));
    }
    std::sort(items.begin(), items.end(), [](const auto &left, const auto &right) {
        if (left.second == right.second) {
            return left.first < right.first;
        }
        return left.second > right.second;
    });

    QStringList parts;
    const int count = std::min(maxItems, static_cast<int>(items.size()));
    for (int i = 0; i < count; ++i) {
        parts << QString("%1(%2次)").arg(items.at(i).first).arg(items.at(i).second);
    }
    return parts.isEmpty() ? "暂无" : parts.join("、");
}

QString csvCell(QString value)
{
    value.replace("\r\n", "\n");
    value.replace('\r', '\n');
    if (!value.isEmpty() && QString("=+-@").contains(value.front())) {
        // Avoid spreadsheet formula execution when reports are opened in Excel.
        value.prepend('\'');
    }
    value.replace('"', "\"\"");
    return QString("\"%1\"").arg(value);
}

QString safeSourceName(const QString &source)
{
    QString value = source.isEmpty() ? "unknown" : source;
    value.replace(' ', '_');
    value.replace('/', '_');
    value.replace('\\', '_');
    return value;
}

QString buildSnapshotPath(const QString &rootDirectory,
                          const QString &prefix,
                          const QString &source,
                          const QDateTime &timestamp)
{
    const QString fileName = QString("%1_%2_%3.jpg")
        .arg(prefix,
             safeSourceName(source),
             timestamp.toString("yyyyMMdd_hhmmss_zzz"));
    return QDir(rootDirectory).filePath(fileName);
}

QString buildNgArchivePath(const QString &archiveRoot,
                           const QString &source,
                           const QDateTime &timestamp)
{
    // Archive NG images by production date so later reports can group defects
    // without scanning every file in one large directory.
    QDir archiveDir(archiveRoot);
    const QString dayFolder = timestamp.toString("yyyyMMdd");
    if (!archiveDir.exists(dayFolder)) {
        archiveDir.mkpath(dayFolder);
    }
    return buildSnapshotPath(archiveDir.filePath(dayFolder), "NG", source, timestamp);
}
}

class DeviceTrendWidget : public QWidget
{
public:
    DeviceTrendWidget(const QString &title,
                      const QString &unit,
                      double minY,
                      double maxY,
                      QWidget *parent = nullptr)
        : QWidget(parent)
        , m_title(title)
        , m_unit(unit)
        , m_minY(minY)
        , m_maxY(maxY)
    {
        setMinimumHeight(190);
    }

    void setSamples(const QVector<QPointF> &points)
    {
        m_points = points;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#ffffff"));

        const QRect plot = rect().adjusted(58, 28, -18, -34);
        painter.setPen(QPen(QColor("#d5dbe3"), 1));
        painter.drawRect(plot);

        painter.setPen(QColor("#2f3a45"));
        painter.drawText(QRect(12, 4, width() - 24, 22), Qt::AlignLeft | Qt::AlignVCenter,
                         QString("%1 (%2)").arg(m_title, m_unit));

        drawGrid(&painter, plot);
        drawCurve(&painter, plot);
    }

private:
    void drawGrid(QPainter *painter, const QRect &plot)
    {
        painter->setPen(QPen(QColor("#eef1f5"), 1));
        for (int i = 1; i < 4; ++i) {
            const int y = plot.top() + plot.height() * i / 4;
            painter->drawLine(plot.left(), y, plot.right(), y);
        }

        painter->setPen(QColor("#6b7280"));
        painter->drawText(8, plot.top() + 5, QString::number(m_maxY, 'f', 1));
        painter->drawText(8, plot.bottom(), QString::number(m_minY, 'f', 1));
    }

    void drawCurve(QPainter *painter, const QRect &plot)
    {
        if (m_points.size() < 2) {
            painter->setPen(QColor("#8a94a3"));
            painter->drawText(plot, Qt::AlignCenter, "等待设备数据...");
            return;
        }

        QPainterPath path;
        for (int i = 0; i < m_points.size(); ++i) {
            const QPointF source = m_points.at(i);
            const double xRatio = m_points.size() == 1
                ? 0.0
                : static_cast<double>(i) / static_cast<double>(m_points.size() - 1);
            const double yRatio = std::clamp((source.y() - m_minY) / (m_maxY - m_minY), 0.0, 1.0);
            const QPointF point(plot.left() + xRatio * plot.width(),
                                plot.bottom() - yRatio * plot.height());
            if (i == 0) {
                path.moveTo(point);
            } else {
                path.lineTo(point);
            }
        }

        painter->setPen(QPen(QColor("#1f7aec"), 2));
        painter->drawPath(path);

        const QPointF last = m_points.back();
        painter->setPen(QColor("#111827"));
        painter->drawText(QRect(plot.left(), plot.bottom() + 6, plot.width(), 22),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QString("当前值: %1 %2").arg(last.y(), 0, 'f', 2).arg(m_unit));
    }

    QString m_title;
    QString m_unit;
    double m_minY = 0.0;
    double m_maxY = 1.0;
    QVector<QPointF> m_points;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

    m_cameraThread = new CameraThread;
    m_serialThread = new SerialThread;
    m_database = std::make_unique<DatabaseManager>();

    connect(m_cameraThread, &CameraThread::frameReady,
            this, &MainWindow::onFrameReady,
            Qt::QueuedConnection);
    connect(m_cameraThread, &CameraThread::cameraError,
            this, &MainWindow::onCameraError,
            Qt::QueuedConnection);
    // 记录实际成功采集的摄像头后端，方便定位 MSMF/DirectShow 兼容问题。
    connect(m_cameraThread, &CameraThread::cameraOpened,
            this, &MainWindow::onCameraOpened,
            Qt::QueuedConnection);
    connect(m_serialThread, &SerialThread::dataReceived,
            this, &MainWindow::onDataReceived,
            Qt::QueuedConnection);
    connect(m_serialThread, &SerialThread::statusMessage,
            this, &MainWindow::onCommunicationStatus,
            Qt::QueuedConnection);
    connect(m_serialThread, &SerialThread::communicationError,
            this, &MainWindow::onCommunicationError,
            Qt::QueuedConnection);

    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onBtnStartClicked);
    connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::onBtnStopClicked);
    connect(m_btnLoadModel, &QPushButton::clicked, this, &MainWindow::onLoadModelClicked);
    connect(m_btnOpenImage, &QPushButton::clicked, this, &MainWindow::onOpenImageClicked);
    connect(m_btnSaveSnapshot, &QPushButton::clicked, this, &MainWindow::onSaveSnapshotClicked);
    connect(m_btnOpenHistory, &QPushButton::clicked, this, &MainWindow::onOpenHistoryClicked);
    connect(m_btnOpenAlarms, &QPushButton::clicked, this, &MainWindow::onOpenAlarmsClicked);
    connect(m_btnAcknowledgeAlarms, &QPushButton::clicked, this, &MainWindow::onAcknowledgeAlarmsClicked);
    connect(m_btnOpenDeviceCurve, &QPushButton::clicked, this, &MainWindow::onOpenDeviceCurveClicked);
    connect(m_btnExportCsv, &QPushButton::clicked, this, &MainWindow::onExportCsvClicked);
    connect(m_btnAiReport, &QPushButton::clicked, this, &MainWindow::onGenerateAiReportClicked);
    connect(m_btnRefreshSerialPorts, &QPushButton::clicked, this, &MainWindow::onRefreshSerialPortsClicked);
    connect(m_comboCommMode, &QComboBox::currentIndexChanged,
            this, &MainWindow::onCommunicationModeChanged);
    connect(m_checkDetection, &QCheckBox::toggled, this, &MainWindow::onDetectionToggled);
    connect(m_historyList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onRecentHistoryActivated);

    QString databaseError;
    if (m_database->initialize(&databaseError)) {
        appendLog(QString("SQLite database ready: %1").arg(m_database->databasePath()));
        refreshHistoryList();
    } else {
        appendLog(QString("SQLite init failed: %1").arg(databaseError));
        QMessageBox::warning(this, "SQLite", databaseError);
    }

    m_fpsTimer.start();
    m_recordTimer.start();
    refreshSerialPortList();
    updateCommunicationControls();
    appendLog("系统就绪：未加载 ONNX 模型时使用 OpenCV 规则检测。");
}

MainWindow::~MainWindow()
{
    shutdownRuntime();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    shutdownRuntime();
    QMainWindow::closeEvent(event);
}

void MainWindow::shutdownRuntime()
{
    if (m_deviceCurveDialog) {
        disconnect(m_deviceCurveDialog, nullptr, this, nullptr);
        delete m_deviceCurveDialog.data();
        m_deviceCurveDialog = nullptr;
    }
    if (m_aiReportDialog) {
        disconnect(m_aiReportDialog, nullptr, this, nullptr);
        delete m_aiReportDialog.data();
        m_aiReportDialog = nullptr;
    }
    m_temperatureTrend = nullptr;
    m_pressureTrend = nullptr;
    m_speedTrend = nullptr;

    if (m_cameraThread) {
        // Disconnect before stopping so queued camera frames cannot enter a
        // MainWindow that is already tearing down its widgets and database.
        disconnect(m_cameraThread, nullptr, this, nullptr);
        m_cameraThread->stopThread();
        delete m_cameraThread;
        m_cameraThread = nullptr;
    }
    if (m_serialThread) {
        // Stop and delete the worker explicitly instead of waiting for QObject
        // child cleanup after MainWindow members have started destructing.
        disconnect(m_serialThread, nullptr, this, nullptr);
        m_serialThread->stopThread();
        delete m_serialThread;
        m_serialThread = nullptr;
    }

    if (m_historyList) {
        m_historyList->clear();
    }
    if (m_alarmList) {
        m_alarmList->clear();
    }
    if (m_logList) {
        m_logList->clear();
    }

    m_lastFrame.release();
    m_deviceSamples.clear();
    m_activeAlarms.clear();
    m_database.reset();
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
    // 预览区仍然优先占用主窗口空间，但降低硬性最小尺寸，避免小屏或
    // 高 DPI 全屏时把右侧工具面板挤到不可见区域。
    m_labelCamera->setMinimumSize(640, 480);
    m_labelCamera->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_labelCamera->setAlignment(Qt::AlignCenter);
    m_labelCamera->setStyleSheet("background:#15171a;color:#d7dde5;border:1px solid #30343a;font-size:18px;");
    mainLayout->addWidget(m_labelCamera, 1);

    auto *sideScroll = new QScrollArea(this);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setWidgetResizable(true);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setMinimumWidth(360);
    sideScroll->setMaximumWidth(460);
    sideScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *sidePanel = new QWidget(sideScroll);
    sidePanel->setMinimumWidth(340);
    sidePanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
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
    m_labelModel->setWordWrap(true);
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
    m_labelCommStatus = new QLabel("通信: 未启动", this);
    m_labelCommStatus->setWordWrap(true);
    m_labelCommStatus->setStyleSheet("color:#54616f;");
    deviceLayout->addWidget(m_labelCommStatus);

    auto *commForm = new QFormLayout();
    // 通信参数字段比较多，允许字段横向扩展并在窄屏时换行，避免输入框
    // 和标签在右侧面板宽度变化时互相挤压。
    commForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    commForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_comboCommMode = new QComboBox(this);
    m_comboCommMode->addItem("模拟数据", static_cast<int>(SerialThread::Mode::Simulation));
    m_comboCommMode->addItem("真实串口", static_cast<int>(SerialThread::Mode::SerialPort));
    m_comboCommMode->addItem("TCP客户端", static_cast<int>(SerialThread::Mode::TcpClient));
    m_comboCommMode->addItem("Modbus TCP", static_cast<int>(SerialThread::Mode::ModbusTcp));
    m_comboCommMode->addItem("Modbus RTU", static_cast<int>(SerialThread::Mode::ModbusRtu));
    commForm->addRow("通信方式", m_comboCommMode);

    auto *serialLayout = new QHBoxLayout();
    m_comboSerialPort = new QComboBox(this);
    m_btnRefreshSerialPorts = new QPushButton("刷新", this);
    serialLayout->addWidget(m_comboSerialPort, 1);
    serialLayout->addWidget(m_btnRefreshSerialPorts);
    commForm->addRow("串口", serialLayout);

    m_spinBaudRate = new QSpinBox(this);
    m_spinBaudRate->setRange(1200, 921600);
    m_spinBaudRate->setValue(115200);
    m_spinBaudRate->setSingleStep(9600);
    commForm->addRow("波特率", m_spinBaudRate);

    m_editTcpHost = new QLineEdit("127.0.0.1", this);
    commForm->addRow("TCP地址", m_editTcpHost);

    m_spinTcpPort = new QSpinBox(this);
    m_spinTcpPort->setRange(1, 65535);
    m_spinTcpPort->setValue(9000);
    commForm->addRow("TCP端口", m_spinTcpPort);

    m_spinModbusUnitId = new QSpinBox(this);
    m_spinModbusUnitId->setRange(1, 247);
    m_spinModbusUnitId->setValue(1);
    commForm->addRow("Modbus站号", m_spinModbusUnitId);

    m_spinModbusStartAddress = new QSpinBox(this);
    m_spinModbusStartAddress->setRange(0, 65535);
    m_spinModbusStartAddress->setValue(0);
    commForm->addRow("起始寄存器", m_spinModbusStartAddress);
    deviceLayout->addLayout(commForm);

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
    m_btnOpenAlarms = new QPushButton("报警记录", this);
    m_btnOpenDeviceCurve = new QPushButton("设备曲线", this);
    m_btnExportCsv = new QPushButton("导出CSV报表", this);
    m_btnAiReport = new QPushButton("AI自动报告", this);
    m_btnOpenHistory = new QPushButton("查看历史记录", this);

    auto *controlGrid = new QGridLayout();
    controlGrid->setContentsMargins(0, 0, 0, 0);
    controlGrid->setHorizontalSpacing(8);
    controlGrid->setVerticalSpacing(8);
    // 控制按钮改成两列，减少右侧纵向堆叠；按钮文本仍保留完整中文，
    // 方便学习者按功能直接定位对应槽函数。
    controlGrid->addWidget(m_btnStart, 0, 0);
    controlGrid->addWidget(m_btnStop, 0, 1);
    controlGrid->addWidget(m_btnLoadModel, 1, 0);
    controlGrid->addWidget(m_btnOpenImage, 1, 1);
    controlGrid->addWidget(m_btnSaveSnapshot, 2, 0);
    controlGrid->addWidget(m_btnOpenHistory, 2, 1);
    controlGrid->addWidget(m_btnOpenAlarms, 3, 0);
    controlGrid->addWidget(m_btnOpenDeviceCurve, 3, 1);
    controlGrid->addWidget(m_btnExportCsv, 4, 0);
    controlGrid->addWidget(m_btnAiReport, 4, 1);
    controlLayout->addLayout(controlGrid);
    controlLayout->addWidget(m_checkDetection);

    auto *sideTabs = new QTabWidget(this);
    sideTabs->setMinimumHeight(250);
    sideTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *alarmPage = new QWidget(sideTabs);
    auto *alarmLayout = new QVBoxLayout(alarmPage);
    m_labelAlarmState = new QLabel("当前报警: 0", this);
    m_labelAlarmState->setAlignment(Qt::AlignCenter);
    m_labelAlarmState->setStyleSheet("background:#1b5e20;color:white;font-weight:bold;padding:6px;");
    m_alarmList = new QListWidget(this);
    // 右侧改为标签页后不再限制最大高度，避免报警较多时列表内容被截断。
    m_alarmList->setMinimumHeight(130);
    m_btnAcknowledgeAlarms = new QPushButton("确认当前报警", this);
    m_btnAcknowledgeAlarms->setEnabled(false);
    alarmLayout->addWidget(m_labelAlarmState);
    alarmLayout->addWidget(m_alarmList, 1);
    alarmLayout->addWidget(m_btnAcknowledgeAlarms);
    sideTabs->addTab(alarmPage, "报警");

    auto *historyPage = new QWidget(sideTabs);
    auto *historyLayout = new QVBoxLayout(historyPage);
    m_historyList = new QListWidget(this);
    // 历史记录在标签页内保留更多可视行，完整追溯仍通过双击打开大窗口。
    m_historyList->setMinimumHeight(170);
    m_historyList->setToolTip("双击可打开完整历史记录界面");
    historyLayout->addWidget(m_historyList, 1);
    sideTabs->addTab(historyPage, "历史");

    auto *logPage = new QWidget(sideTabs);
    auto *logLayout = new QVBoxLayout(logPage);
    m_logList = new QListWidget(this);
    m_logList->setMinimumHeight(170);
    logLayout->addWidget(m_logList, 1);
    sideTabs->addTab(logPage, "日志");

    sideLayout->addWidget(qualityBox);
    sideLayout->addWidget(deviceBox);
    sideLayout->addWidget(controlBox);
    // 报警、历史和日志都属于辅助信息，放入标签页能显著降低右侧高度；
    // 外层滚动区兜底处理后续继续增加控件时的显示完整性。
    sideLayout->addWidget(sideTabs, 1);
    sideScroll->setWidget(sidePanel);
    mainLayout->addWidget(sideScroll);

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
    m_recordTimer.restart();
    const SerialThread::Config commConfig = currentCommunicationConfig();
    const bool needsSerialPort = commConfig.mode == SerialThread::Mode::SerialPort
        || commConfig.mode == SerialThread::Mode::ModbusRtu;
    const bool needsTcpHost = commConfig.mode == SerialThread::Mode::TcpClient
        || commConfig.mode == SerialThread::Mode::ModbusTcp;
    if (needsSerialPort && commConfig.serialPortName.isEmpty()) {
        QMessageBox::warning(this, "设备通信", "请选择一个可用串口，或切换为模拟数据。");
        return;
    }
    if (needsTcpHost && commConfig.tcpHost.trimmed().isEmpty()) {
        QMessageBox::warning(this, "设备通信", "请输入 TCP 服务器地址，或切换为模拟数据。");
        return;
    }
    m_serialThread->setConfig(commConfig);
    m_cameraThread->start();
    m_serialThread->start();

    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_btnLoadModel->setEnabled(false);
    m_btnOpenImage->setEnabled(false);
    m_comboCommMode->setEnabled(false);
    m_comboSerialPort->setEnabled(false);
    m_btnRefreshSerialPorts->setEnabled(false);
    m_spinBaudRate->setEnabled(false);
    m_editTcpHost->setEnabled(false);
    m_spinTcpPort->setEnabled(false);
    m_spinModbusUnitId->setEnabled(false);
    m_spinModbusStartAddress->setEnabled(false);
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
    m_comboCommMode->setEnabled(true);
    updateCommunicationControls();
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
    if (m_recordTimer.elapsed() >= kLiveRecordIntervalMs) {
        // Live camera mode records at a fixed interval to avoid writing one
        // SQLite row per preview frame.
        persistInspectionRecord("camera", QString(), results, inferenceMs, frame, false);
        m_recordTimer.restart();
    }
}

void MainWindow::onDataReceived(float temp, float pressure, float speed)
{
    updateDeviceSamples(temp, pressure, speed);
    evaluateDeviceAlarms(temp, pressure, speed);

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

void MainWindow::onCameraOpened(QString message)
{
    // 摄像头线程已经读到首帧后才会进入这里，日志中的后端信息可作为排障依据。
    appendLog(message);
    statusBar()->showMessage(message);
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
    persistInspectionRecord("image", path, results, inferenceMs, annotated, true);
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

void MainWindow::onOpenHistoryClicked()
{
    showHistoryDialog();
}

void MainWindow::onRecentHistoryActivated(QListWidgetItem *item)
{
    Q_UNUSED(item);
    showHistoryDialog();
}

void MainWindow::onOpenAlarmsClicked()
{
    showAlarmDialog();
}

void MainWindow::onAcknowledgeAlarmsClicked()
{
    QString error;
    if (!m_database || !m_database->acknowledgeAllAlarms(&error)) {
        QMessageBox::warning(this, "SQLite", error.isEmpty() ? "SQLite database is not open." : error);
        return;
    }

    for (auto it = m_activeAlarms.begin(); it != m_activeAlarms.end(); ++it) {
        ActiveAlarm alarm = it.value();
        alarm.acknowledged = true;
        it.value() = alarm;
    }
    refreshAlarmList();
    appendLog("当前报警已确认。");
}

void MainWindow::onOpenDeviceCurveClicked()
{
    showDeviceCurveDialog();
}

void MainWindow::onExportCsvClicked()
{
    exportCsvReport();
}

void MainWindow::onGenerateAiReportClicked()
{
    statusBar()->showMessage("正在生成AI自动报告...");
    appendLog("AI report button clicked.");
    showAiReportDialog();
}

void MainWindow::onCommunicationModeChanged(int)
{
    updateCommunicationControls();
}

void MainWindow::onRefreshSerialPortsClicked()
{
    refreshSerialPortList();
}

void MainWindow::onCommunicationStatus(QString message)
{
    if (m_labelCommStatus) {
        m_labelCommStatus->setText(message);
        m_labelCommStatus->setStyleSheet("color:#2e7d32;");
    }
    appendLog(message);
}

void MainWindow::onCommunicationError(QString message)
{
    if (m_labelCommStatus) {
        m_labelCommStatus->setText(message);
        m_labelCommStatus->setStyleSheet("color:#c62828;font-weight:bold;");
    }
    appendLog(message);
    statusBar()->showMessage(message);
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

void MainWindow::updateDeviceSamples(double temperature, double pressure, double speed)
{
    DeviceSample sample;
    sample.index = ++m_deviceSampleIndex;
    sample.temperature = temperature;
    sample.pressure = pressure;
    sample.speed = speed;

    m_deviceSamples.push_back(sample);
    while (m_deviceSamples.size() > kMaxDeviceSamples) {
        m_deviceSamples.removeFirst();
    }
    updateDeviceCurveSeries();
}

void MainWindow::evaluateDeviceAlarms(double temperature, double pressure, double speed)
{
    updateAlarmState("temperature_critical",
                     temperature >= kTemperatureCritical,
                     "CRITICAL",
                     "temperature",
                     temperature,
                     kTemperatureCritical,
                     QString("温度严重超限: %1 C").arg(temperature, 0, 'f', 1));
    updateAlarmState("temperature_warning",
                     temperature >= kTemperatureWarning && temperature < kTemperatureCritical,
                     "WARNING",
                     "temperature",
                     temperature,
                     kTemperatureWarning,
                     QString("温度接近上限: %1 C").arg(temperature, 0, 'f', 1));
    updateAlarmState("pressure_low",
                     pressure <= kPressureLowWarning,
                     "WARNING",
                     "pressure",
                     pressure,
                     kPressureLowWarning,
                     QString("压力偏低: %1 MPa").arg(pressure, 0, 'f', 2));
    updateAlarmState("pressure_high",
                     pressure >= kPressureHighWarning,
                     "WARNING",
                     "pressure",
                     pressure,
                     kPressureHighWarning,
                     QString("压力偏高: %1 MPa").arg(pressure, 0, 'f', 2));
    updateAlarmState("speed_low",
                     speed <= kSpeedLowWarning,
                     "WARNING",
                     "speed",
                     speed,
                     kSpeedLowWarning,
                     QString("转速偏低: %1 rpm").arg(speed, 0, 'f', 0));
    updateAlarmState("speed_high",
                     speed >= kSpeedHighWarning,
                     "WARNING",
                     "speed",
                     speed,
                     kSpeedHighWarning,
                     QString("转速偏高: %1 rpm").arg(speed, 0, 'f', 0));
}

void MainWindow::updateAlarmState(const QString &key,
                                  bool active,
                                  const QString &level,
                                  const QString &metric,
                                  double value,
                                  double threshold,
                                  const QString &message)
{
    if (active) {
        if (!m_activeAlarms.contains(key)) {
            ActiveAlarm alarm;
            alarm.level = level;
            alarm.metric = metric;
            alarm.message = message;
            alarm.value = value;
            alarm.threshold = threshold;
            alarm.timestamp = QDateTime::currentDateTime();
            m_activeAlarms.insert(key, alarm);

            AlarmRecord record;
            record.timestamp = alarm.timestamp;
            record.level = level;
            record.source = "device";
            record.metric = metric;
            record.value = value;
            record.threshold = threshold;
            record.message = message;

            QString error;
            if (!m_database || !m_database->saveAlarmRecord(record, &error)) {
                appendLog(QString("Alarm save failed: %1").arg(error));
            } else {
                appendLog(QString("ALARM %1: %2").arg(level, message));
            }
        } else {
            ActiveAlarm alarm = m_activeAlarms.value(key);
            alarm.value = value;
            alarm.message = message;
            m_activeAlarms.insert(key, alarm);
        }
    } else if (m_activeAlarms.contains(key)) {
        appendLog(QString("Alarm recovered: %1").arg(m_activeAlarms.value(key).metric));
        m_activeAlarms.remove(key);
    }

    refreshAlarmList();
}

void MainWindow::refreshAlarmList()
{
    if (!m_alarmList || !m_labelAlarmState || !m_btnAcknowledgeAlarms) {
        return;
    }

    m_alarmList->clear();
    bool hasCritical = false;
    for (auto it = m_activeAlarms.cbegin(); it != m_activeAlarms.cend(); ++it) {
        const ActiveAlarm &alarm = it.value();
        auto *item = new QListWidgetItem(QString("[%1] %2 %3")
                                             .arg(alarm.acknowledged ? "ACK" : "OPEN",
                                                  alarm.level,
                                                  alarm.message),
                                         m_alarmList);
        item->setToolTip(QString("%1 value=%2 threshold=%3")
                             .arg(alarm.metric)
                             .arg(alarm.value, 0, 'f', 2)
                             .arg(alarm.threshold, 0, 'f', 2));
        if (alarm.level == "CRITICAL") {
            hasCritical = true;
            item->setForeground(QColor("#b71c1c"));
        } else if (!alarm.acknowledged) {
            item->setForeground(QColor("#ef6c00"));
        } else {
            item->setForeground(QColor("#607d8b"));
        }
    }

    const int alarmCount = m_activeAlarms.size();
    m_labelAlarmState->setText(QString("当前报警: %1").arg(alarmCount));
    if (alarmCount == 0) {
        m_labelAlarmState->setStyleSheet("background:#1b5e20;color:white;font-weight:bold;padding:6px;");
    } else if (hasCritical) {
        m_labelAlarmState->setStyleSheet("background:#b71c1c;color:white;font-weight:bold;padding:6px;");
        statusBar()->showMessage("存在严重设备报警，请立即确认。");
    } else {
        m_labelAlarmState->setStyleSheet("background:#ef6c00;color:white;font-weight:bold;padding:6px;");
        statusBar()->showMessage("存在设备报警，请检查设备状态。");
    }
    bool hasUnacknowledged = false;
    for (const ActiveAlarm &alarm : m_activeAlarms) {
        if (!alarm.acknowledged) {
            hasUnacknowledged = true;
            break;
        }
    }
    m_btnAcknowledgeAlarms->setEnabled(hasUnacknowledged);
}

void MainWindow::persistInspectionRecord(const QString &source,
                                         const QString &imagePath,
                                         const QVector<DetectionResult> &results,
                                         double inferenceMs,
                                         const cv::Mat &annotatedFrame,
                                         bool forceSnapshot)
{
    InspectionRecord record;
    if (!m_database) {
        appendLog("SQLite save failed: database is not open.");
        return;
    }

    record.timestamp = QDateTime::currentDateTime();
    record.source = source;
    record.imagePath = imagePath;
    record.modelName = m_cameraThread->detectorName();
    record.result = results.isEmpty() ? "OK" : "NG";
    record.defectCount = results.size();
    record.defectLabels = defectLabelsText(results);
    record.maxConfidence = maxConfidence(results);
    record.inferenceMs = inferenceMs;

    if (record.result == "NG" && !annotatedFrame.empty()) {
        // NG evidence goes into a dedicated archive because production users
        // usually review defect images independently from normal snapshots.
        const QString snapshotPath = buildNgArchivePath(m_database->ngArchiveDirectory(),
                                                        record.source,
                                                        record.timestamp);
        if (writeImageFile(snapshotPath, annotatedFrame)) {
            record.snapshotPath = snapshotPath;
        }
    } else if (forceSnapshot && !annotatedFrame.empty()) {
        // OK offline tests still keep a snapshot for learning/debugging, but
        // they stay outside the NG archive to keep defect folders clean.
        const QString snapshotPath = buildSnapshotPath(m_database->snapshotDirectory(),
                                                       "OK",
                                                       record.source,
                                                       record.timestamp);
        if (writeImageFile(snapshotPath, annotatedFrame)) {
            record.snapshotPath = snapshotPath;
        }
    }

    QString error;
    if (!m_database->saveInspectionRecord(record, &error)) {
        appendLog(QString("SQLite save failed: %1").arg(error));
        return;
    }
    if (record.result == "NG" && !record.snapshotPath.isEmpty()) {
        appendLog(QString("NG archived: %1").arg(record.snapshotPath));
    }
    refreshHistoryList();
}

void MainWindow::refreshHistoryList()
{
    if (!m_historyList || !m_database) {
        return;
    }

    QString error;
    const QVector<InspectionRecord> records = m_database->recentInspectionRecords(30, &error);
    if (!error.isEmpty()) {
        appendLog(QString("SQLite query failed: %1").arg(error));
        return;
    }

    m_historyList->clear();
    for (const InspectionRecord &record : records) {
        auto *item = new QListWidgetItem(formatHistoryItem(record), m_historyList);
        item->setData(Qt::UserRole, record.id);
        if (record.result == "NG") {
            item->setForeground(QColor("#b71c1c"));
        }
        item->setToolTip(QString("model=%1\nlabels=%2\nimage=%3\nsnapshot=%4")
                             .arg(record.modelName,
                                  record.defectLabels,
                                  record.imagePath,
                                  record.snapshotPath));
    }
}

void MainWindow::exportCsvReport()
{
    if (!m_database) {
        QMessageBox::warning(this, "CSV", "SQLite database is not open.");
        return;
    }

    QString inspectionError;
    const QVector<InspectionRecord> inspections =
        m_database->recentInspectionRecords(0, &inspectionError);
    if (!inspectionError.isEmpty()) {
        QMessageBox::warning(this, "CSV", inspectionError);
        return;
    }

    QString alarmError;
    const QVector<AlarmRecord> alarms = m_database->recentAlarmRecords(0, &alarmError);
    if (!alarmError.isEmpty()) {
        QMessageBox::warning(this, "CSV", alarmError);
        return;
    }

    const QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString basePath = documentsPath.isEmpty()
        ? QDir::currentPath()
        : documentsPath;
    const QString defaultName = QString("VisionMonitor_Report_%1.csv")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString filePath = QFileDialog::getSaveFileName(this,
                                                    "导出CSV报表",
                                                    QDir(basePath).filePath(defaultName),
                                                    "CSV Files (*.csv)");
    if (filePath.isEmpty()) {
        return;
    }
    if (QFileInfo(filePath).suffix().isEmpty()) {
        filePath += ".csv";
    }

    int okCount = 0;
    int ngCount = 0;
    for (const InspectionRecord &record : inspections) {
        if (record.result == "NG") {
            ++ngCount;
        } else {
            ++okCount;
        }
    }

    int openAlarmCount = 0;
    for (const AlarmRecord &record : alarms) {
        if (!record.acknowledged) {
            ++openAlarmCount;
        }
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "CSV", QString("无法创建报表文件：%1").arg(file.errorString()));
        return;
    }

    file.write("\xEF\xBB\xBF", 3);
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    auto writeRow = [&stream](const QStringList &cells) {
        QStringList escaped;
        escaped.reserve(cells.size());
        for (const QString &cell : cells) {
            escaped << csvCell(cell);
        }
        stream << escaped.join(',') << '\n';
    };

    writeRow({"VisionMonitor CSV Report"});
    writeRow({"导出时间", QDateTime::currentDateTime().toString(Qt::ISODate)});
    writeRow({"数据库", m_database->databasePath()});
    writeRow({"NG图片归档目录", m_database->ngArchiveDirectory()});
    stream << '\n';

    writeRow({"质检统计"});
    writeRow({"检测总数", QString::number(inspections.size())});
    writeRow({"OK数量", QString::number(okCount)});
    writeRow({"NG数量", QString::number(ngCount)});
    writeRow({"良品率",
              inspections.isEmpty()
                  ? "--"
                  : QString("%1%").arg(okCount * 100.0 / inspections.size(), 0, 'f', 2)});
    stream << '\n';

    writeRow({"质检明细"});
    writeRow({"ID", "时间", "来源", "图片路径", "模型", "结果", "缺陷数",
              "缺陷标签", "最高置信度", "推理耗时ms", "证据图/NG归档图"});
    for (const InspectionRecord &record : inspections) {
        writeRow({
            QString::number(record.id),
            record.timestamp.toString(Qt::ISODate),
            record.source,
            record.imagePath,
            record.modelName,
            record.result,
            QString::number(record.defectCount),
            record.defectLabels,
            QString::number(record.maxConfidence, 'f', 4),
            QString::number(record.inferenceMs, 'f', 2),
            record.snapshotPath,
        });
    }
    stream << '\n';

    writeRow({"报警统计"});
    writeRow({"报警总数", QString::number(alarms.size())});
    writeRow({"未确认报警", QString::number(openAlarmCount)});
    writeRow({"已确认报警", QString::number(alarms.size() - openAlarmCount)});
    stream << '\n';

    writeRow({"报警明细"});
    writeRow({"ID", "时间", "等级", "来源", "指标", "当前值", "阈值", "报警信息", "确认状态"});
    for (const AlarmRecord &record : alarms) {
        writeRow({
            QString::number(record.id),
            record.timestamp.toString(Qt::ISODate),
            record.level,
            record.source,
            record.metric,
            QString::number(record.value, 'f', 2),
            QString::number(record.threshold, 'f', 2),
            record.message,
            record.acknowledged ? "ACK" : "OPEN",
        });
    }

    stream.flush();
    if (!file.commit()) {
        QMessageBox::warning(this, "CSV", QString("保存报表失败：%1").arg(file.errorString()));
        return;
    }

    appendLog(QString("CSV report exported: %1").arg(filePath));
    QMessageBox::information(this, "CSV", QString("CSV报表已导出：\n%1").arg(filePath));
}

QString MainWindow::buildAiReport(const QVector<InspectionRecord> &inspections,
                                  const QVector<AlarmRecord> &alarms) const
{
    int okCount = 0;
    int ngCount = 0;
    int defectTotal = 0;
    double confidenceSum = 0.0;
    double inferenceSum = 0.0;
    int inferenceCount = 0;
    QMap<QString, int> defectCounts;
    QMap<QString, int> sourceCounts;
    QMap<QString, int> modelCounts;

    for (const InspectionRecord &record : inspections) {
        sourceCounts[record.source.isEmpty() ? "unknown" : record.source]++;
        modelCounts[record.modelName.isEmpty() ? "unknown" : record.modelName]++;
        if (record.result == "NG") {
            ++ngCount;
            defectTotal += record.defectCount;
            confidenceSum += record.maxConfidence;
            const QStringList labels = record.defectLabels.split(',', Qt::SkipEmptyParts);
            for (QString label : labels) {
                label = label.trimmed();
                if (!label.isEmpty()) {
                    defectCounts[label]++;
                }
            }
        } else {
            ++okCount;
        }
        if (record.inferenceMs > 0.0) {
            inferenceSum += record.inferenceMs;
            ++inferenceCount;
        }
    }

    int openAlarmCount = 0;
    int criticalAlarmCount = 0;
    int warningAlarmCount = 0;
    QMap<QString, int> alarmMetricCounts;
    QMap<QString, int> alarmLevelCounts;
    for (const AlarmRecord &record : alarms) {
        if (!record.acknowledged) {
            ++openAlarmCount;
        }
        if (record.level == "CRITICAL") {
            ++criticalAlarmCount;
        } else if (record.level == "WARNING") {
            ++warningAlarmCount;
        }
        alarmMetricCounts[record.metric.isEmpty() ? "unknown" : record.metric]++;
        alarmLevelCounts[record.level.isEmpty() ? "unknown" : record.level]++;
    }

    const int total = inspections.size();
    const double ngRate = total > 0 ? ngCount * 100.0 / total : 0.0;
    const double yieldRate = total > 0 ? okCount * 100.0 / total : 0.0;
    const double avgInference = inferenceCount > 0 ? inferenceSum / inferenceCount : 0.0;
    const double avgNgConfidence = ngCount > 0 ? confidenceSum / ngCount : 0.0;

    QString riskLevel = "低";
    QString conclusion = "当前样本未显示明显质量风险，设备状态整体平稳。";
    if (criticalAlarmCount > 0 || openAlarmCount > 0 || ngRate >= 10.0) {
        riskLevel = "高";
        conclusion = "当前存在需要优先处理的质量或设备风险，建议暂停批量生产并复核设备状态。";
    } else if (warningAlarmCount > 0 || ngRate >= 3.0) {
        riskLevel = "中";
        conclusion = "当前存在一定波动风险，建议加强抽检并关注设备趋势变化。";
    }
    if (total == 0 && alarms.isEmpty()) {
        riskLevel = "暂无数据";
        conclusion = "当前数据库中还没有检测或报警记录，建议先完成图片检测或摄像头采集后再生成报告。";
    }

    QStringList causes;
    if (ngCount > 0) {
        causes << QString("视觉检测发现 %1 条 NG 记录，主要缺陷集中在：%2。")
                      .arg(ngCount)
                      .arg(topCountsText(defectCounts, 5));
    }
    if (criticalAlarmCount > 0 || warningAlarmCount > 0) {
        causes << QString("设备侧累计 %1 条报警，主要异常指标为：%2。")
                      .arg(alarms.size())
                      .arg(topCountsText(alarmMetricCounts, 5));
    }
    if (avgInference > 80.0) {
        causes << QString("平均推理耗时为 %1 ms，实时检测性能可能影响产线节拍。")
                      .arg(avgInference, 0, 'f', 1);
    }
    if (causes.isEmpty()) {
        causes << "未发现稳定的缺陷聚类或设备异常模式。";
    }

    QStringList suggestions;
    if (riskLevel == "高") {
        suggestions << "立即复核最近 NG 图片和报警记录，必要时暂停当前批次。";
        suggestions << "优先检查相机光源、镜头焦距、工装定位和被测件表面状态。";
        suggestions << "对报警最多的设备指标进行点检，确认传感器、气压、温度和转速是否稳定。";
    } else if (riskLevel == "中") {
        suggestions << "提高最近一小时或当前批次的抽检比例，观察 NG 率是否继续上升。";
        suggestions << "对高频缺陷类别补充样本，后续可针对工业缺陷重新训练 YOLO 模型。";
        suggestions << "持续观察设备曲线，确认报警是否由短时波动引起。";
    } else if (riskLevel == "低") {
        suggestions << "保持当前检测参数，继续积累正常样本和少量边界样本。";
        suggestions << "定期导出 CSV 与 AI 报告，形成可追溯的质检记录。";
    } else {
        suggestions << "先运行图片检测或摄像头采集，让系统写入历史记录。";
    }

    QStringList lines;
    lines << "# VisionMonitor AI自动质检报告";
    lines << "";
    lines << QString("- 生成时间：%1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    lines << QString("- 数据范围：%1").arg(reportTimeRange(inspections, alarms));
    lines << QString("- 数据库：%1").arg(m_database ? m_database->databasePath() : QString("未打开"));
    lines << QString("- NG图片归档：%1").arg(m_database ? m_database->ngArchiveDirectory() : QString("未打开"));
    lines << "";
    lines << "## 一、AI综合结论";
    lines << QString("- 风险等级：%1").arg(riskLevel);
    lines << QString("- 结论：%1").arg(conclusion);
    lines << QString("- 主要检测来源：%1").arg(topCountsText(sourceCounts, 3));
    lines << QString("- 使用模型：%1").arg(topCountsText(modelCounts, 3));
    lines << "";
    lines << "## 二、检测数据分析";
    lines << QString("- 检测总数：%1").arg(total);
    lines << QString("- OK数量：%1").arg(okCount);
    lines << QString("- NG数量：%1").arg(ngCount);
    lines << QString("- 良品率：%1").arg(percentText(yieldRate));
    lines << QString("- NG率：%1").arg(percentText(ngRate));
    lines << QString("- 缺陷总数：%1").arg(defectTotal);
    lines << QString("- 高频缺陷：%1").arg(topCountsText(defectCounts, 5));
    lines << QString("- NG平均置信度：%1").arg(ngCount > 0 ? QString::number(avgNgConfidence, 'f', 4) : QString("--"));
    lines << QString("- 平均推理耗时：%1 ms").arg(avgInference, 0, 'f', 1);
    lines << "";
    lines << "## 三、设备报警分析";
    lines << QString("- 报警总数：%1").arg(alarms.size());
    lines << QString("- 未确认报警：%1").arg(openAlarmCount);
    lines << QString("- 严重报警：%1").arg(criticalAlarmCount);
    lines << QString("- 预警报警：%1").arg(warningAlarmCount);
    lines << QString("- 报警级别分布：%1").arg(topCountsText(alarmLevelCounts, 5));
    lines << QString("- 高频异常指标：%1").arg(topCountsText(alarmMetricCounts, 5));
    lines << "";
    lines << "## 四、原因推断";
    for (const QString &cause : causes) {
        lines << QString("- %1").arg(cause);
    }
    lines << "";
    lines << "## 五、处理建议";
    for (const QString &suggestion : suggestions) {
        lines << QString("- %1").arg(suggestion);
    }
    lines << "";
    lines << "## 六、报告说明";
    lines << "- 本报告由本地规则推理生成，输入数据来自 SQLite 检测历史和报警记录。";
    lines << "- 后续可将本报告生成函数替换为大模型 API 调用，实现更自然的故障分析和维修建议。";
    return lines.join('\n');
}

void MainWindow::showAiReportDialog()
{
    if (m_aiReportDialog) {
        m_aiReportDialog->raise();
        m_aiReportDialog->activateWindow();
        statusBar()->showMessage("AI自动报告窗口已在前台。");
        return;
    }

    if (!m_database) {
        QMessageBox::warning(this, "AI自动报告", "SQLite database is not open.");
        return;
    }

    QString inspectionError;
    const QVector<InspectionRecord> inspections =
        m_database->recentInspectionRecords(0, &inspectionError);
    if (!inspectionError.isEmpty()) {
        QMessageBox::warning(this, "AI自动报告", inspectionError);
        return;
    }

    QString alarmError;
    const QVector<AlarmRecord> alarms = m_database->recentAlarmRecords(0, &alarmError);
    if (!alarmError.isEmpty()) {
        QMessageBox::warning(this, "AI自动报告", alarmError);
        return;
    }

    const QString report = buildAiReport(inspections, alarms);

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("AI自动检测 / 故障报告");
    dialog->resize(920, 680);
    m_aiReportDialog = dialog;

    auto *layout = new QVBoxLayout(dialog);
    auto *editor = new QTextEdit(dialog);
    editor->setReadOnly(true);
    editor->setPlainText(report);
    editor->setLineWrapMode(QTextEdit::WidgetWidth);
    layout->addWidget(editor, 1);

    auto *buttonLayout = new QHBoxLayout();
    auto *exportButton = new QPushButton("导出Markdown", dialog);
    auto *closeButton = new QPushButton("关闭", dialog);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(exportButton);
    buttonLayout->addWidget(closeButton);
    layout->addLayout(buttonLayout);

    connect(exportButton, &QPushButton::clicked, dialog, [this, report]() {
        const QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString basePath = documentsPath.isEmpty()
            ? QDir::currentPath()
            : documentsPath;
        QString filePath = QFileDialog::getSaveFileName(this,
                                                        "导出AI报告",
                                                        QDir(basePath).filePath(QString("VisionMonitor_AI_Report_%1.md")
                                                            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"))),
                                                        "Markdown Files (*.md);;Text Files (*.txt)");
        if (filePath.isEmpty()) {
            return;
        }
        if (QFileInfo(filePath).suffix().isEmpty()) {
            filePath += ".md";
        }

        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "AI自动报告", QString("无法创建报告文件：%1").arg(file.errorString()));
            return;
        }
        QTextStream stream(&file);
        stream.setEncoding(QStringConverter::Utf8);
        stream << report << '\n';
        stream.flush();
        if (!file.commit()) {
            QMessageBox::warning(this, "AI自动报告", QString("保存报告失败：%1").arg(file.errorString()));
            return;
        }

        appendLog(QString("AI report exported: %1").arg(filePath));
        QMessageBox::information(this, "AI自动报告", QString("AI报告已导出：\n%1").arg(filePath));
    });
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QObject::destroyed, this, [this]() {
        m_aiReportDialog = nullptr;
    });

    appendLog("AI report generated from inspection and alarm history.");
    statusBar()->showMessage("AI自动报告已生成。");
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::showAlarmDialog()
{
    if (!m_database) {
        QMessageBox::warning(this, "SQLite", "SQLite database is not open.");
        return;
    }

    QString error;
    const QVector<AlarmRecord> records = m_database->recentAlarmRecords(200, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, "SQLite", error);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("报警管理");
    dialog.resize(980, 520);

    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(records.size(), 9, &dialog);
    table->setHorizontalHeaderLabels(QStringList()
                                     << "ID" << "时间" << "状态" << "级别" << "来源"
                                     << "指标" << "当前值" << "阈值" << "报警信息");
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);

    for (int row = 0; row < records.size(); ++row) {
        const AlarmRecord &record = records.at(row);
        const QStringList values = {
            QString::number(record.id),
            record.timestamp.toString("yyyy-MM-dd HH:mm:ss"),
            record.acknowledged ? "ACK" : "OPEN",
            record.level,
            record.source,
            record.metric,
            QString::number(record.value, 'f', 2),
            QString::number(record.threshold, 'f', 2),
            record.message
        };

        for (int column = 0; column < values.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.at(column));
            if (!record.acknowledged && record.level == "CRITICAL") {
                cell->setForeground(QColor("#b71c1c"));
            } else if (!record.acknowledged) {
                cell->setForeground(QColor("#ef6c00"));
            }
            table->setItem(row, column, cell);
        }
    }
    table->resizeColumnsToContents();
    layout->addWidget(table, 1);

    auto *buttonLayout = new QHBoxLayout();
    auto *ackButton = new QPushButton("确认全部报警", &dialog);
    auto *closeButton = new QPushButton("关闭", &dialog);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(ackButton);
    buttonLayout->addWidget(closeButton);
    layout->addLayout(buttonLayout);

    connect(ackButton, &QPushButton::clicked, &dialog, [this, &dialog]() {
        onAcknowledgeAlarmsClicked();
        dialog.accept();
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}

void MainWindow::showDeviceCurveDialog()
{
    if (m_deviceCurveDialog) {
        m_deviceCurveDialog->raise();
        m_deviceCurveDialog->activateWindow();
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("设备状态实时曲线");
    dialog->resize(980, 720);
    m_deviceCurveDialog = dialog;

    auto *layout = new QVBoxLayout(dialog);
    auto createTrend = [dialog, layout](const QString &title,
                                        const QString &unit,
                                        double minY,
                                        double maxY,
                                        DeviceTrendWidget **trendOut) {
        auto *trend = new DeviceTrendWidget(title, unit, minY, maxY, dialog);
        layout->addWidget(trend, 1);
        *trendOut = trend;
    };

    createTrend("温度趋势", "C", 65.0, 95.0, &m_temperatureTrend);
    createTrend("压力趋势", "MPa", 1.60, 2.50, &m_pressureTrend);
    createTrend("转速趋势", "rpm", 1000.0, 1350.0, &m_speedTrend);

    connect(dialog, &QObject::destroyed, this, [this]() {
        // The modeless trend window owns these child widgets; clear cached
        // pointers so timer-style data updates never touch deleted widgets.
        m_deviceCurveDialog = nullptr;
        m_temperatureTrend = nullptr;
        m_pressureTrend = nullptr;
        m_speedTrend = nullptr;
    });

    updateDeviceCurveSeries();
    dialog->show();
}

void MainWindow::updateDeviceCurveSeries()
{
    if (!m_temperatureTrend || !m_pressureTrend || !m_speedTrend) {
        return;
    }

    QVector<QPointF> temperaturePoints;
    QVector<QPointF> pressurePoints;
    QVector<QPointF> speedPoints;
    temperaturePoints.reserve(m_deviceSamples.size());
    pressurePoints.reserve(m_deviceSamples.size());
    speedPoints.reserve(m_deviceSamples.size());

    for (const DeviceSample &sample : m_deviceSamples) {
        temperaturePoints.push_back(QPointF(sample.index, sample.temperature));
        pressurePoints.push_back(QPointF(sample.index, sample.pressure));
        speedPoints.push_back(QPointF(sample.index, sample.speed));
    }

    m_temperatureTrend->setSamples(temperaturePoints);
    m_pressureTrend->setSamples(pressurePoints);
    m_speedTrend->setSamples(speedPoints);
}

void MainWindow::showHistoryDialog()
{
    if (!m_database) {
        QMessageBox::warning(this, "SQLite", "SQLite database is not open.");
        return;
    }

    QString error;
    const QVector<InspectionRecord> records = m_database->recentInspectionRecords(200, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, "SQLite", error);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("检测历史记录");
    dialog.resize(1080, 620);

    auto *mainLayout = new QVBoxLayout(&dialog);

    auto *pathLayout = new QHBoxLayout();
    auto *pathLabel = new QLabel(QString("数据库: %1\nNG归档: %2")
                                     .arg(m_database->databasePath(),
                                          m_database->ngArchiveDirectory()),
                                 &dialog);
    auto *openArchiveButton = new QPushButton("打开NG归档目录", &dialog);
    pathLayout->addWidget(pathLabel, 1);
    pathLayout->addWidget(openArchiveButton);
    mainLayout->addLayout(pathLayout);

    auto *contentLayout = new QHBoxLayout();
    auto *table = new QTableWidget(records.size(), 10, &dialog);
    table->setHorizontalHeaderLabels(QStringList()
                                     << "ID" << "时间" << "结果" << "来源" << "缺陷数"
                                     << "标签" << "最高置信度" << "耗时ms" << "模型" << "证据图");
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);

    for (int row = 0; row < records.size(); ++row) {
        const InspectionRecord &record = records.at(row);
        const QStringList values = {
            QString::number(record.id),
            record.timestamp.toString("yyyy-MM-dd HH:mm:ss"),
            record.result,
            record.source,
            QString::number(record.defectCount),
            record.defectLabels,
            QString::number(record.maxConfidence, 'f', 3),
            QString::number(record.inferenceMs, 'f', 1),
            record.modelName,
            QFileInfo(record.snapshotPath).fileName()
        };

        for (int column = 0; column < values.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.at(column));
            if (record.result == "NG") {
                cell->setForeground(QColor("#b71c1c"));
            }
            if (column == 9) {
                cell->setData(Qt::UserRole, record.snapshotPath);
                cell->setToolTip(record.snapshotPath);
            }
            table->setItem(row, column, cell);
        }
    }
    table->resizeColumnsToContents();

    auto *previewLayout = new QVBoxLayout();
    auto *previewLabel = new QLabel("选择一条有证据图的记录预览", &dialog);
    previewLabel->setMinimumSize(340, 260);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setStyleSheet("background:#15171a;color:#d7dde5;border:1px solid #30343a;");
    auto *openImageButton = new QPushButton("打开证据图", &dialog);
    previewLayout->addWidget(previewLabel, 1);
    previewLayout->addWidget(openImageButton);

    contentLayout->addWidget(table, 1);
    contentLayout->addLayout(previewLayout);
    mainLayout->addLayout(contentLayout, 1);

    auto selectedSnapshotPath = [table]() -> QString {
        const int row = table->currentRow();
        if (row < 0) {
            return {};
        }
        QTableWidgetItem *pathItem = table->item(row, 9);
        return pathItem ? pathItem->data(Qt::UserRole).toString() : QString();
    };

    auto updatePreview = [previewLabel, selectedSnapshotPath]() {
        const QString path = selectedSnapshotPath();
        if (path.isEmpty()) {
            previewLabel->setPixmap(QPixmap());
            previewLabel->setText("当前记录没有证据图");
            return;
        }

        const QPixmap pixmap(path);
        if (pixmap.isNull()) {
            previewLabel->setPixmap(QPixmap());
            previewLabel->setText("证据图不存在或无法读取");
            return;
        }

        previewLabel->setText(QString());
        previewLabel->setPixmap(pixmap.scaled(previewLabel->size(),
                                              Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
    };

    connect(table, &QTableWidget::currentCellChanged, &dialog,
            [updatePreview](int, int, int, int) {
                updatePreview();
            });
    connect(openImageButton, &QPushButton::clicked, &dialog, [this, selectedSnapshotPath]() {
        const QString path = selectedSnapshotPath();
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            QMessageBox::information(this, "证据图", "当前记录没有可打开的证据图。");
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect(openArchiveButton, &QPushButton::clicked, &dialog, [this]() {
        // Opening the archive directory is intentionally user-triggered so
        // the app does not pop up Explorer during automatic inspection.
        if (m_database) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_database->ngArchiveDirectory()));
        }
    });

    if (table->rowCount() > 0) {
        table->selectRow(0);
        updatePreview();
    }

    dialog.exec();
}

void MainWindow::refreshSerialPortList()
{
    const QString previousPort = m_comboSerialPort
        ? m_comboSerialPort->currentData().toString()
        : QString();
    m_comboSerialPort->clear();

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        const QString label = port.description().isEmpty()
            ? port.portName()
            : QString("%1 - %2").arg(port.portName(), port.description());
        m_comboSerialPort->addItem(label, port.portName());
    }

    if (m_comboSerialPort->count() == 0) {
        m_comboSerialPort->addItem("无可用串口", QString());
    } else {
        const int previousIndex = m_comboSerialPort->findData(previousPort);
        if (previousIndex >= 0) {
            m_comboSerialPort->setCurrentIndex(previousIndex);
        }
    }
}

void MainWindow::updateCommunicationControls()
{
    const auto mode = static_cast<SerialThread::Mode>(m_comboCommMode->currentData().toInt());
    const bool editable = m_comboCommMode->isEnabled();
    const bool serialMode = editable
        && (mode == SerialThread::Mode::SerialPort || mode == SerialThread::Mode::ModbusRtu);
    const bool tcpMode = editable
        && (mode == SerialThread::Mode::TcpClient || mode == SerialThread::Mode::ModbusTcp);
    const bool modbusMode = editable
        && (mode == SerialThread::Mode::ModbusTcp || mode == SerialThread::Mode::ModbusRtu);

    m_comboSerialPort->setEnabled(serialMode);
    m_btnRefreshSerialPorts->setEnabled(serialMode);
    m_spinBaudRate->setEnabled(serialMode);
    m_editTcpHost->setEnabled(tcpMode);
    m_spinTcpPort->setEnabled(tcpMode);
    m_spinModbusUnitId->setEnabled(modbusMode);
    m_spinModbusStartAddress->setEnabled(modbusMode);
}

SerialThread::Config MainWindow::currentCommunicationConfig() const
{
    SerialThread::Config config;
    config.mode = static_cast<SerialThread::Mode>(m_comboCommMode->currentData().toInt());
    config.serialPortName = m_comboSerialPort->currentData().toString();
    config.baudRate = m_spinBaudRate->value();
    config.tcpHost = m_editTcpHost->text().trimmed();
    config.tcpPort = static_cast<quint16>(m_spinTcpPort->value());
    config.modbusUnitId = static_cast<quint8>(m_spinModbusUnitId->value());
    config.modbusStartAddress = static_cast<quint16>(m_spinModbusStartAddress->value());
    return config;
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
    if (mat.empty()) {
        return {};
    }

    cv::Mat rgb;
    if (mat.channels() == 1) {
        cv::cvtColor(mat, rgb, cv::COLOR_GRAY2RGB);
    } else if (mat.channels() == 3) {
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
    } else if (mat.channels() == 4) {
        cv::cvtColor(mat, rgb, cv::COLOR_BGRA2RGB);
    } else {
        cv::Mat firstChannel;
        cv::extractChannel(mat, firstChannel, 0);
        firstChannel.convertTo(firstChannel, CV_8U);
        cv::cvtColor(firstChannel, rgb, cv::COLOR_GRAY2RGB);
    }

    QImage img(rgb.data,
               rgb.cols,
               rgb.rows,
               rgb.step,
               QImage::Format_RGB888);
    return QPixmap::fromImage(img.copy());
}
