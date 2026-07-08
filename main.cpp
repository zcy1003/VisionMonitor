#include "mainwindow.h"

#include <QApplication>
#include <QMetaObject>
#include <QTimer>
#include <opencv2/opencv.hpp>

#include "visiondetector.h"

// 必须注册 cv::Mat 类型，才能在跨线程信号槽中传递
// Q_DECLARE_METATYPE 是一个 Qt 宏，告诉 Qt 的类型系统："cv::Mat 这个类型请认识一下"
// 为什么要这个：Qt 的信号槽机制在跨线程传递数据时，需要把数据打包（序列化）然后放到事件队列里，再拆包传给槽函数。Qt 内置的类型（int、QString 等）它天然认识，但 cv::Mat 是 OpenCV 的类型，Qt 不认识
Q_DECLARE_METATYPE(cv::Mat)

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 注册自定义类型（跨线程信号槽的必要步骤）
    // Q_DECLARE_METATYPE 只是声明，qRegisterMetaType 才是真正"注册"——它在程序启动时告诉 Qt 的元对象系统：运行时遇到 cv::Mat 类型，请按这个规则处理。名字字符串 "cv::Mat" 用于调试和错误信息
    // 这两行缺一不可，顺序也不能反（先声明再注册）
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<QVector<DetectionResult>>("QVector<DetectionResult>");

    // Keep MainWindow alive until the event loop exits. Deleting the main
    // widget inside close-event processing can leave platform events and Qt
    // child cleanup interleaved in Debug builds.
    MainWindow w;
    w.show();
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.contains("--auto-start")) {
        // Hidden smoke-test switch for validating worker-thread shutdown.
        QTimer::singleShot(500, &w, [&w]() {
            QMetaObject::invokeMethod(&w, "onBtnStartClicked", Qt::QueuedConnection);
        });
    }
    const int autoExitIndex = arguments.indexOf("--auto-exit-ms");
    if (autoExitIndex >= 0 && autoExitIndex + 1 < arguments.size()) {
        bool ok = false;
        const int delayMs = arguments.at(autoExitIndex + 1).toInt(&ok);
        if (ok && delayMs > 0) {
            // Hidden smoke-test switch used to verify shutdown without manual UI.
            QTimer::singleShot(delayMs, &w, &QWidget::close);
        }
    }
    return a.exec();
}
