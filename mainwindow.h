#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QSet>
#include <QSpinBox>
#include <QVector>
#include <memory>
#include <opencv2/opencv.hpp>

#include "camerathread.h"
#include "databasemanager.h"
#include "serialthread.h"

class QDialog;
class DeviceTrendWidget;
class DatabaseManager;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onFrameReady(cv::Mat frame, QVector<DetectionResult> results, double inferenceMs);
    void onDataReceived(float temp, float pressure, float speed);
    void onCameraError(QString message);
    // 摄像头真正可读后更新状态栏和日志，保留后端诊断信息。
    void onCameraOpened(QString message);
    void onBtnStartClicked();
    void onBtnStopClicked();
    void onLoadModelClicked();
    void onOpenImageClicked();
    void onSaveSnapshotClicked();
    void onDetectionToggled(bool checked);
    void onOpenHistoryClicked();
    void onRecentHistoryActivated(QListWidgetItem *item);
    void onOpenAlarmsClicked();
    void onAcknowledgeAlarmsClicked();
    void onOpenDeviceCurveClicked();
    void onExportCsvClicked();
    void onGenerateAiReportClicked();
    void onCommunicationModeChanged(int index);
    void onRefreshSerialPortsClicked();
    void onCommunicationStatus(QString message);
    void onCommunicationError(QString message);

private:
    struct DeviceSample
    {
        int index = 0;
        double temperature = 0.0;
        double pressure = 0.0;
        double speed = 0.0;
    };

    struct ActiveAlarm
    {
        QString level;
        QString metric;
        QString message;
        double value = 0.0;
        double threshold = 0.0;
        QDateTime timestamp;
        bool acknowledged = false;
    };

    void setupUi();
    void shutdownRuntime();
    void displayFrame(const cv::Mat &frame);
    void updateInspectionSummary(const QVector<DetectionResult> &results, double inferenceMs);
    // Append device samples to the rolling cache used by the realtime charts.
    void updateDeviceSamples(double temperature, double pressure, double speed);
    // Evaluate sensor thresholds and create alarm records only on state changes.
    void evaluateDeviceAlarms(double temperature, double pressure, double speed);
    void updateAlarmState(const QString &key,
                          bool active,
                          const QString &level,
                          const QString &metric,
                          double value,
                          double threshold,
                          const QString &message);
    void refreshAlarmList();
    // Show an operator-facing alarm table backed by SQLite alarm_records.
    void showAlarmDialog();
    // Show realtime trend charts for temperature, pressure, and speed.
    void showDeviceCurveDialog();
    void updateDeviceCurveSeries();
    // Save a throttled live record or a full offline-image record into SQLite.
    void persistInspectionRecord(const QString &source,
                                 const QString &imagePath,
                                 const QVector<DetectionResult> &results,
                                 double inferenceMs,
                                 const cv::Mat &annotatedFrame,
                                 bool forceSnapshot);
    // Reload the newest SQLite rows into the compact history list.
    void refreshHistoryList();
    // Show a larger traceability view with table details and archived NG image preview.
    void showHistoryDialog();
    // Export inspection and alarm history into one UTF-8 CSV report for Excel.
    void exportCsvReport();
    QString buildAiReport(const QVector<InspectionRecord> &inspections,
                          const QVector<AlarmRecord> &alarms) const;
    void showAiReportDialog();
    void refreshSerialPortList();
    void updateCommunicationControls();
    SerialThread::Config currentCommunicationConfig() const;
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
    QLabel *m_labelAlarmState = nullptr;
    QLabel *m_labelCommStatus = nullptr;
    QListWidget *m_logList = nullptr;
    QListWidget *m_historyList = nullptr;
    QListWidget *m_alarmList = nullptr;
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnStop = nullptr;
    QPushButton *m_btnLoadModel = nullptr;
    QPushButton *m_btnOpenImage = nullptr;
    QPushButton *m_btnSaveSnapshot = nullptr;
    QPushButton *m_btnOpenHistory = nullptr;
    QPushButton *m_btnOpenAlarms = nullptr;
    QPushButton *m_btnAcknowledgeAlarms = nullptr;
    QPushButton *m_btnOpenDeviceCurve = nullptr;
    QPushButton *m_btnExportCsv = nullptr;
    QPushButton *m_btnAiReport = nullptr;
    QPushButton *m_btnRefreshSerialPorts = nullptr;
    QCheckBox *m_checkDetection = nullptr;
    QComboBox *m_comboCommMode = nullptr;
    QComboBox *m_comboSerialPort = nullptr;
    QSpinBox *m_spinBaudRate = nullptr;
    QLineEdit *m_editTcpHost = nullptr;
    QSpinBox *m_spinTcpPort = nullptr;
    QSpinBox *m_spinModbusUnitId = nullptr;
    QSpinBox *m_spinModbusStartAddress = nullptr;

    QElapsedTimer m_fpsTimer;
    QElapsedTimer m_recordTimer;
    int m_frameCount = 0;
    int m_totalInspected = 0;
    int m_ngCount = 0;
    bool m_lastNg = false;
    int m_deviceSampleIndex = 0;

    cv::Mat m_lastFrame;
    QVector<DeviceSample> m_deviceSamples;
    QMap<QString, ActiveAlarm> m_activeAlarms;
    QPointer<QDialog> m_deviceCurveDialog;
    QPointer<QDialog> m_aiReportDialog;
    DeviceTrendWidget *m_temperatureTrend = nullptr;
    DeviceTrendWidget *m_pressureTrend = nullptr;
    DeviceTrendWidget *m_speedTrend = nullptr;
    std::unique_ptr<DatabaseManager> m_database;
};

#endif // MAINWINDOW_H
