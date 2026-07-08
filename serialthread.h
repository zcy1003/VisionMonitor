#ifndef SERIALTHREAD_H
#define SERIALTHREAD_H

#include <QSerialPort>
#include <QThread>
#include <QByteArray>
#include <QString>
#include <atomic>

class SerialThread : public QThread
{
    Q_OBJECT

public:
    enum class Mode
    {
        Simulation,
        SerialPort,
        TcpClient,
        ModbusTcp,
        ModbusRtu
    };

    struct Config
    {
        Mode mode = Mode::Simulation;
        QString serialPortName;
        qint32 baudRate = QSerialPort::Baud115200;
        QString tcpHost = "127.0.0.1";
        quint16 tcpPort = 9000;
        quint8 modbusUnitId = 1;
        quint16 modbusStartAddress = 0;
    };

    explicit SerialThread(QObject *parent = nullptr);
    ~SerialThread();
    void stopThread();
    void setConfig(const Config &config);

protected:
    void run() override;

signals:
    // 发出三个传感器数值
    void dataReceived(float temperature,
                      float pressure,
                      float speed);
    void statusMessage(QString message);
    void communicationError(QString message);

private:
    void runSimulation();
    void runSerialPort();
    void runTcpClient();
    void runModbusTcp();
    void runModbusRtu();
    void sleepPollInterval();
    void consumeBuffer(QByteArray *buffer);
    bool parseModbusRegisterPayload(const QByteArray &payload,
                                     int dataOffset,
                                     float *temperature,
                                     float *pressure,
                                     float *speed);
    bool parseLine(const QByteArray &line, float *temperature, float *pressure, float *speed) const;

    std::atomic_bool m_running;
    Config m_config;
};

#endif // SERIALTHREAD_H
