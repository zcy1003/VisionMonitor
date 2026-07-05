#include "serialthread.h"
#include <QtMath>
#include <QRandomGenerator>

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
    m_running = false;
    wait();
}

void SerialThread::run()
{
    m_running = true;
    int tick = 0;

    while (m_running) {
        // ============================================
        // Week 1阶段：用正弦波模拟传感器数据
        // Week 3阶段会替换成真实串口读取
        // ============================================

        // 温度：75~90°C 之间波动，偶尔超过90触发报警
        // 10.0f * qSin(tick * 0.05f) — 在 ±10°C 范围内正弦波动。tick * 0.05f 控制波动频率，值越大波动越密
        // QRandomGenerator::global()->bounded(4) — 加一个 0~4 的随机噪声，让数据看起来不那么"完美"，更像真实传感器
        float temp = 82.0f
                     + 10.0f * qSin(tick * 0.05f)
                     + QRandomGenerator::global()->bounded(4);

        // 压力：1.8~2.3 MPa 波动
        float pressure = 2.05f
                         + 0.25f * qCos(tick * 0.03f)
                         + QRandomGenerator::global()->bounded(10) * 0.01f;

        // 转速：1100~1300 rpm 波动
        float speed = 1200.0f
                      + 100.0f * qSin(tick * 0.04f);

        emit dataReceived(temp, pressure, speed);

        tick++;
        QThread::msleep(1000); // 每秒发一次数据 -- 模拟传感器常见的 1Hz 采样率。
    }
}