#ifndef SERIALTHREAD_H
#define SERIALTHREAD_H

#include <QThread>

class SerialThread : public QThread
{
    Q_OBJECT

public:
    explicit SerialThread(QObject *parent = nullptr);
    ~SerialThread();
    void stopThread();

protected:
    void run() override;

signals:
    // 发出三个传感器数值
    void dataReceived(float temperature,
                      float pressure,
                      float speed);

private:
    volatile bool m_running;
};

#endif // SERIALTHREAD_H