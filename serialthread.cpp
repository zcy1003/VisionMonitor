#include "serialthread.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QVector>
#include <QtMath>

namespace {
constexpr int kIoWaitMs = 100;
constexpr int kTcpConnectTimeoutMs = 3000;
constexpr int kModbusResponseTimeoutMs = 1000;
constexpr int kModbusPollIntervalMs = 1000;
constexpr quint8 kModbusReadHoldingRegisters = 0x03;
constexpr quint16 kModbusRegisterCount = 3;

bool readJsonValue(const QJsonObject &object,
                   const QStringList &keys,
                   float *value)
{
    for (const QString &key : keys) {
        if (object.contains(key) && object.value(key).isDouble()) {
            *value = static_cast<float>(object.value(key).toDouble());
            return true;
        }
    }
    return false;
}

void appendUInt16(QByteArray *data, quint16 value)
{
    data->append(static_cast<char>((value >> 8) & 0xff));
    data->append(static_cast<char>(value & 0xff));
}

quint16 readUInt16(const QByteArray &data, int offset)
{
    return (static_cast<quint8>(data.at(offset)) << 8)
        | static_cast<quint8>(data.at(offset + 1));
}

quint16 modbusCrc16(const QByteArray &data)
{
    quint16 crc = 0xffff;
    for (char byte : data) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const bool lsbSet = (crc & 0x0001) != 0;
            crc >>= 1;
            if (lsbSet) {
                crc ^= 0xa001;
            }
        }
    }
    return crc;
}

void appendModbusCrc(QByteArray *data)
{
    const quint16 crc = modbusCrc16(*data);
    data->append(static_cast<char>(crc & 0xff));
    data->append(static_cast<char>((crc >> 8) & 0xff));
}

QByteArray buildReadHoldingPdu(quint16 startAddress)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(kModbusReadHoldingRegisters));
    appendUInt16(&pdu, startAddress);
    appendUInt16(&pdu, kModbusRegisterCount);
    return pdu;
}

bool validateModbusRtuFrame(const QByteArray &response, QByteArray *body)
{
    if (response.size() < 5) {
        return false;
    }

    const int frameSize = response.size();
    const QByteArray frameBody = response.left(frameSize - 2);
    const quint16 crc = modbusCrc16(frameBody);
    const quint8 lowByte = static_cast<quint8>(response.at(frameSize - 2));
    const quint8 highByte = static_cast<quint8>(response.at(frameSize - 1));
    if (lowByte != (crc & 0xff) || highByte != ((crc >> 8) & 0xff)) {
        return false;
    }

    if (body) {
        *body = frameBody;
    }
    return true;
}
}

SerialThread::SerialThread(QObject *parent)
    : QThread(parent)
    , m_running(false)
{
}

SerialThread::~SerialThread()
{
    stopThread();
}

void SerialThread::stopThread()
{
    // Atomic flag is read by the worker thread; wait only when QThread is
    // active so shutdown remains deterministic even if capture never started.
    m_running = false;
    if (isRunning()) {
        wait();
    }
}

void SerialThread::setConfig(const Config &config)
{
    if (isRunning()) {
        return;
    }
    m_config = config;
}

void SerialThread::run()
{
    m_running = true;

    switch (m_config.mode) {
    case Mode::Simulation:
        runSimulation();
        break;
    case Mode::SerialPort:
        runSerialPort();
        break;
    case Mode::TcpClient:
        runTcpClient();
        break;
    case Mode::ModbusTcp:
        runModbusTcp();
        break;
    case Mode::ModbusRtu:
        runModbusRtu();
        break;
    }

    m_running = false;
}

void SerialThread::runSimulation()
{
    emit statusMessage("设备通信: 使用模拟数据源。");
    int tick = 0;

    while (m_running) {
        const float temp = 82.0f
                           + 10.0f * qSin(tick * 0.05f)
                           + QRandomGenerator::global()->bounded(4);
        const float pressure = 2.05f
                               + 0.25f * qCos(tick * 0.03f)
                               + QRandomGenerator::global()->bounded(10) * 0.01f;
        const float speed = 1200.0f
                            + 100.0f * qSin(tick * 0.04f);

        emit dataReceived(temp, pressure, speed);

        ++tick;
        QThread::msleep(1000);
    }
}

void SerialThread::runSerialPort()
{
    if (m_config.serialPortName.trimmed().isEmpty()) {
        emit communicationError("设备通信: 未选择串口。");
        return;
    }

    QSerialPort serial;
    serial.setPortName(m_config.serialPortName);
    serial.setBaudRate(m_config.baudRate);
    serial.setDataBits(QSerialPort::Data8);
    serial.setParity(QSerialPort::NoParity);
    serial.setStopBits(QSerialPort::OneStop);
    serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!serial.open(QIODevice::ReadOnly)) {
        emit communicationError(QString("设备通信: 串口打开失败 %1, %2")
                                    .arg(m_config.serialPortName, serial.errorString()));
        return;
    }

    emit statusMessage(QString("设备通信: 串口已连接 %1 @ %2")
                           .arg(m_config.serialPortName)
                           .arg(m_config.baudRate));

    QByteArray buffer;
    while (m_running) {
        if (serial.waitForReadyRead(kIoWaitMs)) {
            buffer += serial.readAll();
            consumeBuffer(&buffer);
        }
    }

    serial.close();
    emit statusMessage("设备通信: 串口已断开。");
}

void SerialThread::runTcpClient()
{
    if (m_config.tcpHost.trimmed().isEmpty() || m_config.tcpPort == 0) {
        emit communicationError("设备通信: TCP 地址或端口无效。");
        return;
    }

    QTcpSocket socket;
    socket.connectToHost(m_config.tcpHost, m_config.tcpPort);
    if (!socket.waitForConnected(kTcpConnectTimeoutMs)) {
        emit communicationError(QString("设备通信: TCP 连接失败 %1:%2, %3")
                                    .arg(m_config.tcpHost)
                                    .arg(m_config.tcpPort)
                                    .arg(socket.errorString()));
        return;
    }

    emit statusMessage(QString("设备通信: TCP 已连接 %1:%2")
                           .arg(m_config.tcpHost)
                           .arg(m_config.tcpPort));

    QByteArray buffer;
    while (m_running && socket.state() == QAbstractSocket::ConnectedState) {
        if (socket.waitForReadyRead(kIoWaitMs)) {
            buffer += socket.readAll();
            consumeBuffer(&buffer);
        }
    }

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.waitForDisconnected(500);
    }
    emit statusMessage("设备通信: TCP 已断开。");
}

void SerialThread::runModbusTcp()
{
    if (m_config.tcpHost.trimmed().isEmpty() || m_config.tcpPort == 0) {
        emit communicationError("设备通信: Modbus TCP 地址或端口无效。");
        return;
    }

    QTcpSocket socket;
    socket.connectToHost(m_config.tcpHost, m_config.tcpPort);
    if (!socket.waitForConnected(kTcpConnectTimeoutMs)) {
        emit communicationError(QString("设备通信: Modbus TCP 连接失败 %1:%2, %3")
                                    .arg(m_config.tcpHost)
                                    .arg(m_config.tcpPort)
                                    .arg(socket.errorString()));
        return;
    }

    emit statusMessage(QString("设备通信: Modbus TCP 已连接 %1:%2 unit=%3 addr=%4")
                           .arg(m_config.tcpHost)
                           .arg(m_config.tcpPort)
                           .arg(m_config.modbusUnitId)
                           .arg(m_config.modbusStartAddress));

    quint16 transactionId = 1;
    while (m_running && socket.state() == QAbstractSocket::ConnectedState) {
        QByteArray request;
        appendUInt16(&request, transactionId);
        appendUInt16(&request, 0);
        appendUInt16(&request, 6);
        request.append(static_cast<char>(m_config.modbusUnitId));
        request += buildReadHoldingPdu(m_config.modbusStartAddress);

        socket.write(request);
        if (!socket.waitForBytesWritten(kModbusResponseTimeoutMs)) {
            emit communicationError(QString("设备通信: Modbus TCP 写入失败，%1").arg(socket.errorString()));
            break;
        }

        QByteArray response;
        QElapsedTimer timer;
        timer.start();
        while (m_running && timer.elapsed() < kModbusResponseTimeoutMs) {
            const int remainingMs = static_cast<int>(kModbusResponseTimeoutMs - timer.elapsed());
            if (socket.waitForReadyRead(qMax(1, remainingMs))) {
                response += socket.readAll();
                if (response.size() >= 7) {
                    const int totalSize = 6 + readUInt16(response, 4);
                    if (response.size() >= totalSize) {
                        break;
                    }
                }
            }
        }

        float temperature = 0.0f;
        float pressure = 0.0f;
        float speed = 0.0f;
        if (response.size() >= 9
            && readUInt16(response, 0) == transactionId
            && readUInt16(response, 2) == 0
            && static_cast<quint8>(response.at(6)) == m_config.modbusUnitId
            && parseModbusRegisterPayload(response, 7, &temperature, &pressure, &speed)) {
            emit dataReceived(temperature, pressure, speed);
        } else if (!response.isEmpty()) {
            emit statusMessage("设备通信: Modbus TCP 响应无效或寄存器数量不匹配。");
        } else {
            emit statusMessage("设备通信: Modbus TCP 读取超时。");
        }

        ++transactionId;
        sleepPollInterval();
    }

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.waitForDisconnected(500);
    }
    emit statusMessage("设备通信: Modbus TCP 已断开。");
}

void SerialThread::runModbusRtu()
{
    if (m_config.serialPortName.trimmed().isEmpty()) {
        emit communicationError("设备通信: 未选择 Modbus RTU 串口。");
        return;
    }

    QSerialPort serial;
    serial.setPortName(m_config.serialPortName);
    serial.setBaudRate(m_config.baudRate);
    serial.setDataBits(QSerialPort::Data8);
    serial.setParity(QSerialPort::NoParity);
    serial.setStopBits(QSerialPort::OneStop);
    serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!serial.open(QIODevice::ReadWrite)) {
        emit communicationError(QString("设备通信: Modbus RTU 串口打开失败 %1, %2")
                                    .arg(m_config.serialPortName, serial.errorString()));
        return;
    }

    emit statusMessage(QString("设备通信: Modbus RTU 已连接 %1 @ %2 unit=%3 addr=%4")
                           .arg(m_config.serialPortName)
                           .arg(m_config.baudRate)
                           .arg(m_config.modbusUnitId)
                           .arg(m_config.modbusStartAddress));

    const int expectedResponseSize = 5 + kModbusRegisterCount * 2;
    while (m_running) {
        QByteArray request;
        request.append(static_cast<char>(m_config.modbusUnitId));
        request += buildReadHoldingPdu(m_config.modbusStartAddress);
        appendModbusCrc(&request);

        serial.clear(QSerialPort::AllDirections);
        serial.write(request);
        if (!serial.waitForBytesWritten(kModbusResponseTimeoutMs)) {
            emit communicationError(QString("设备通信: Modbus RTU 写入失败，%1").arg(serial.errorString()));
            break;
        }

        QByteArray response;
        QElapsedTimer timer;
        timer.start();
        while (m_running && timer.elapsed() < kModbusResponseTimeoutMs) {
            const int remainingMs = static_cast<int>(kModbusResponseTimeoutMs - timer.elapsed());
            if (serial.waitForReadyRead(qMax(1, remainingMs))) {
                response += serial.readAll();
                if (response.size() >= expectedResponseSize
                    || (response.size() >= 5 && (static_cast<quint8>(response.at(1)) & 0x80))) {
                    break;
                }
            }
        }

        QByteArray body;
        const bool crcOk = validateModbusRtuFrame(response, &body);

        float temperature = 0.0f;
        float pressure = 0.0f;
        float speed = 0.0f;
        if (crcOk
            && static_cast<quint8>(body.at(0)) == m_config.modbusUnitId
            && parseModbusRegisterPayload(body, 1, &temperature, &pressure, &speed)) {
            emit dataReceived(temperature, pressure, speed);
        } else if (!response.isEmpty()) {
            emit statusMessage("设备通信: Modbus RTU 响应无效或 CRC 校验失败。");
        } else {
            emit statusMessage("设备通信: Modbus RTU 读取超时。");
        }

        sleepPollInterval();
    }

    serial.close();
    emit statusMessage("设备通信: Modbus RTU 已断开。");
}

void SerialThread::sleepPollInterval()
{
    int sleptMs = 0;
    while (m_running && sleptMs < kModbusPollIntervalMs) {
        QThread::msleep(50);
        sleptMs += 50;
    }
}

void SerialThread::consumeBuffer(QByteArray *buffer)
{
    int newlineIndex = -1;
    while ((newlineIndex = buffer->indexOf('\n')) >= 0) {
        QByteArray line = buffer->left(newlineIndex).trimmed();
        buffer->remove(0, newlineIndex + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }

        float temperature = 0.0f;
        float pressure = 0.0f;
        float speed = 0.0f;
        if (parseLine(line, &temperature, &pressure, &speed)) {
            emit dataReceived(temperature, pressure, speed);
        } else {
            emit statusMessage(QString("设备通信: 无法解析数据 %1").arg(QString::fromUtf8(line)));
        }
    }

    // Keep malformed streams from growing forever when no newline is received.
    if (buffer->size() > 4096) {
        buffer->clear();
        emit statusMessage("设备通信: 接收缓存过长，已清空。");
    }
}

bool SerialThread::parseModbusRegisterPayload(const QByteArray &payload,
                                              int dataOffset,
                                              float *temperature,
                                              float *pressure,
                                              float *speed)
{
    if (payload.size() <= dataOffset + 1) {
        return false;
    }

    const quint8 functionCode = static_cast<quint8>(payload.at(dataOffset));
    if (functionCode & 0x80) {
        const quint8 exceptionCode = payload.size() > dataOffset + 1
            ? static_cast<quint8>(payload.at(dataOffset + 1))
            : 0;
        emit statusMessage(QString("设备通信: Modbus 异常响应 code=%1").arg(exceptionCode));
        return false;
    }
    if (functionCode != kModbusReadHoldingRegisters) {
        return false;
    }

    const int byteCountOffset = dataOffset + 1;
    const int firstRegisterOffset = dataOffset + 2;
    const int byteCount = static_cast<quint8>(payload.at(byteCountOffset));
    if (byteCount < kModbusRegisterCount * 2
        || payload.size() < firstRegisterOffset + kModbusRegisterCount * 2) {
        return false;
    }

    QVector<quint16> registers;
    registers.reserve(kModbusRegisterCount);
    for (int i = 0; i < kModbusRegisterCount; ++i) {
        registers.push_back(readUInt16(payload, firstRegisterOffset + i * 2));
    }

    // The demo register map is: temperature x10, pressure x100, speed rpm.
    *temperature = static_cast<qint16>(registers.at(0)) / 10.0f;
    *pressure = registers.at(1) / 100.0f;
    *speed = registers.at(2);
    return true;
}

bool SerialThread::parseLine(const QByteArray &line,
                             float *temperature,
                             float *pressure,
                             float *speed) const
{
    const QString text = QString::fromUtf8(line).trimmed();
    if (text.isEmpty()) {
        return false;
    }

    if (text.startsWith('{')) {
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            return false;
        }
        const QJsonObject object = document.object();
        return readJsonValue(object, {"temp", "temperature", "t"}, temperature)
            && readJsonValue(object, {"pressure", "p"}, pressure)
            && readJsonValue(object, {"speed", "rpm", "s"}, speed);
    }

    if (text.contains('=') || text.contains(':')) {
        bool hasTemperature = false;
        bool hasPressure = false;
        bool hasSpeed = false;
        const QStringList tokens = text.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
        for (const QString &token : tokens) {
            const int equalIndex = token.indexOf('=');
            const int colonIndex = token.indexOf(':');
            const int separatorIndex = equalIndex >= 0 ? equalIndex : colonIndex;
            if (separatorIndex <= 0) {
                continue;
            }

            const QString key = token.left(separatorIndex).trimmed().toLower();
            const QString rawValue = token.mid(separatorIndex + 1).trimmed();
            bool ok = false;
            const float value = rawValue.toFloat(&ok);
            if (!ok) {
                continue;
            }

            if (key == "temp" || key == "temperature" || key == "t") {
                *temperature = value;
                hasTemperature = true;
            } else if (key == "pressure" || key == "p") {
                *pressure = value;
                hasPressure = true;
            } else if (key == "speed" || key == "rpm" || key == "s") {
                *speed = value;
                hasSpeed = true;
            }
        }
        return hasTemperature && hasPressure && hasSpeed;
    }

    const QStringList values = text.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    if (values.size() < 3) {
        return false;
    }

    bool okTemperature = false;
    bool okPressure = false;
    bool okSpeed = false;
    *temperature = values.at(0).toFloat(&okTemperature);
    *pressure = values.at(1).toFloat(&okPressure);
    *speed = values.at(2).toFloat(&okSpeed);
    return okTemperature && okPressure && okSpeed;
}
