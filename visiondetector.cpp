#include "visiondetector.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace {
QRect clampRect(const QRect &rect, const cv::Size &size)
{
    const int x = std::clamp(rect.x(), 0, std::max(0, size.width - 1));
    const int y = std::clamp(rect.y(), 0, std::max(0, size.height - 1));
    const int right = std::clamp(rect.right(), 0, std::max(0, size.width - 1));
    const int bottom = std::clamp(rect.bottom(), 0, std::max(0, size.height - 1));
    return QRect(QPoint(x, y), QPoint(right, bottom));
}
}

VisionDetector::VisionDetector()
{
    m_classNames << "defect";
}

bool VisionDetector::loadYoloModel(const QString &modelPath, QString *message)
{
    try {
        cv::dnn::Net candidateNet = cv::dnn::readNetFromONNX(modelPath.toStdString());
        if (candidateNet.empty()) {
            if (message) {
                *message = "模型加载失败：OpenCV 返回空网络。";
            }
            return false;
        }

        candidateNet.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        candidateNet.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

        // readNetFromONNX 只能证明模型结构被解析，旧版 OpenCV DNN 仍可能在 forward
        // 阶段因算子或动态形状不兼容而崩溃；加载时预热一次可以把问题提前暴露给 UI。
        const cv::Mat warmupFrame = cv::Mat::zeros(m_inputSize, CV_8UC3);
        const cv::Mat warmupBlob = cv::dnn::blobFromImage(warmupFrame,
                                                          1.0 / 255.0,
                                                          m_inputSize,
                                                          cv::Scalar(),
                                                          true,
                                                          false);
        candidateNet.setInput(warmupBlob);
        std::vector<cv::Mat> warmupOutputs;
        candidateNet.forward(warmupOutputs, candidateNet.getUnconnectedOutLayersNames());
        if (warmupOutputs.empty()) {
            if (message) {
                *message = "模型加载失败：OpenCV DNN 预热推理没有输出。";
            }
            return false;
        }

        m_net = candidateNet;
        m_modelName = QFileInfo(modelPath).fileName();
        loadClassNames(modelPath);

        if (message) {
            *message = QString("已加载 YOLOv8 ONNX 模型：%1").arg(m_modelName);
        }
        return true;
    } catch (const cv::Exception &e) {
        m_net = cv::dnn::Net();
        m_modelName.clear();
        if (message) {
            *message = QString("模型加载失败：OpenCV DNN 无法执行该 ONNX。%1").arg(e.what());
        }
        return false;
    }
}

bool VisionDetector::hasYoloModel() const
{
    return !m_net.empty();
}

QString VisionDetector::modelName() const
{
    return hasYoloModel() ? m_modelName : "OpenCV 规则检测";
}

cv::Mat VisionDetector::processFrame(const cv::Mat &frame,
                                     QVector<DetectionResult> *results,
                                     double *inferenceMs)
{
    if (frame.empty()) {
        if (results) {
            results->clear();
        }
        if (inferenceMs) {
            *inferenceMs = 0.0;
        }
        return {};
    }

    QVector<DetectionResult> detections;
    try {
        detections = hasYoloModel()
            ? runYolo(frame, inferenceMs)
            : runRuleBasedInspection(frame, inferenceMs);
    } catch (const cv::Exception &) {
        // 摄像头线程里不能让 OpenCV 异常越过 Qt 线程边界，否则 Linux 下会直接终止进程；
        // 禁用当前模型并回退规则检测，保证演示程序还能继续运行和排错。
        m_net = cv::dnn::Net();
        m_modelName.clear();
        detections = runRuleBasedInspection(frame, inferenceMs);
    }

    cv::Mat annotated = frame.clone();
    drawResults(annotated, detections);

    if (results) {
        *results = detections;
    }
    return annotated;
}

cv::Mat VisionDetector::annotateFrame(const cv::Mat &frame, const QVector<DetectionResult> &results) const
{
    if (frame.empty()) {
        return {};
    }

    // Reuse cached YOLO boxes on skipped frames so display FPS stays higher
    // while the user still sees the latest detection result.
    cv::Mat annotated = frame.clone();
    drawResults(annotated, results);
    return annotated;
}

QVector<DetectionResult> VisionDetector::runYolo(const cv::Mat &frame, double *inferenceMs)
{
    QVector<DetectionResult> results;
    const int64 start = cv::getTickCount();

    cv::Mat blob = cv::dnn::blobFromImage(frame,
                                          1.0 / 255.0,
                                          m_inputSize,
                                          cv::Scalar(),
                                          true,
                                          false);
    m_net.setInput(blob);

    std::vector<cv::Mat> outputs;
    m_net.forward(outputs, m_net.getUnconnectedOutLayersNames());

    if (inferenceMs) {
        *inferenceMs = (cv::getTickCount() - start) * 1000.0 / cv::getTickFrequency();
    }
    if (outputs.empty()) {
        return results;
    }

    const cv::Mat &raw = outputs.front();
    cv::Mat data;
    if (raw.dims == 3) {
        const int first = raw.size[1];
        const int second = raw.size[2];
        if (first < second) {
            cv::Mat view(first, second, CV_32F, const_cast<float *>(raw.ptr<float>()));
            cv::transpose(view, data);
        } else {
            data = cv::Mat(first, second, CV_32F, const_cast<float *>(raw.ptr<float>()));
        }
    } else if (raw.dims == 2) {
        data = raw;
    } else {
        return results;
    }

    std::vector<cv::Rect> boxes;
    std::vector<float> confidences;
    std::vector<int> classIds;
    const float scaleX = static_cast<float>(frame.cols) / static_cast<float>(m_inputSize.width);
    const float scaleY = static_cast<float>(frame.rows) / static_cast<float>(m_inputSize.height);

    for (int i = 0; i < data.rows; ++i) {
        const float *row = data.ptr<float>(i);
        if (data.cols < 5) {
            continue;
        }

        float bestScore = 0.0f;
        int classId = 0;
        for (int j = 4; j < data.cols; ++j) {
            if (row[j] > bestScore) {
                bestScore = row[j];
                classId = j - 4;
            }
        }

        if (bestScore < m_confThreshold) {
            continue;
        }

        const float cx = row[0] * scaleX;
        const float cy = row[1] * scaleY;
        const float width = row[2] * scaleX;
        const float height = row[3] * scaleY;
        const int left = static_cast<int>(cx - width * 0.5f);
        const int top = static_cast<int>(cy - height * 0.5f);

        boxes.emplace_back(left,
                           top,
                           static_cast<int>(width),
                           static_cast<int>(height));
        confidences.push_back(bestScore);
        classIds.push_back(classId);
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, confidences, m_confThreshold, m_nmsThreshold, keep);
    for (int index : keep) {
        QRect rect(boxes[index].x, boxes[index].y, boxes[index].width, boxes[index].height);
        DetectionResult result;
        result.box = clampRect(rect, frame.size());
        result.confidence = confidences[index];
        result.label = classIds[index] < m_classNames.size()
            ? m_classNames[classIds[index]]
            : QString("class_%1").arg(classIds[index]);
        results.push_back(result);
    }

    return results;
}

QVector<DetectionResult> VisionDetector::runRuleBasedInspection(const cv::Mat &frame,
                                                                double *inferenceMs)
{
    QVector<DetectionResult> results;
    const int64 start = cv::getTickCount();

    cv::Mat gray;
    if (frame.channels() == 3) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = frame.clone();
    }

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    cv::Mat brightMask;
    cv::Mat darkMask;
    cv::threshold(blurred, brightMask, 245, 255, cv::THRESH_BINARY);
    cv::threshold(blurred, darkMask, 25, 255, cv::THRESH_BINARY_INV);

    cv::Mat mask = brightMask | darkMask;
    cv::morphologyEx(mask,
                     mask,
                     cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const double minArea = std::max(80.0, frame.cols * frame.rows * 0.0004);
    for (const auto &contour : contours) {
        const double area = cv::contourArea(contour);
        if (area < minArea) {
            continue;
        }

        const cv::Rect rect = cv::boundingRect(contour);
        if (rect.width < 8 || rect.height < 8) {
            continue;
        }

        DetectionResult result;
        result.label = "surface_defect";
        result.confidence = static_cast<float>(std::min(0.99, 0.45 + area / (frame.cols * frame.rows)));
        result.box = clampRect(QRect(rect.x, rect.y, rect.width, rect.height), frame.size());
        results.push_back(result);
    }

    std::sort(results.begin(), results.end(), [](const DetectionResult &a, const DetectionResult &b) {
        return a.confidence > b.confidence;
    });

    if (results.size() > 20) {
        results.resize(20);
    }

    if (inferenceMs) {
        *inferenceMs = (cv::getTickCount() - start) * 1000.0 / cv::getTickFrequency();
    }
    return results;
}

void VisionDetector::drawResults(cv::Mat &frame, const QVector<DetectionResult> &results) const
{
    const bool ng = !results.isEmpty();
    const cv::Scalar statusColor = ng ? cv::Scalar(40, 40, 230) : cv::Scalar(50, 170, 60);
    const QString status = ng ? "NG" : "OK";

    cv::rectangle(frame, cv::Rect(0, 0, frame.cols, 42), statusColor, cv::FILLED);
    cv::putText(frame,
                status.toStdString(),
                cv::Point(16, 29),
                cv::FONT_HERSHEY_SIMPLEX,
                0.85,
                cv::Scalar(255, 255, 255),
                2,
                cv::LINE_AA);

    for (const DetectionResult &result : results) {
        const cv::Rect rect(result.box.x(), result.box.y(), result.box.width(), result.box.height());
        cv::rectangle(frame, rect, cv::Scalar(20, 30, 230), 2);

        const QString label = QString("%1 %2%")
            .arg(result.label)
            .arg(result.confidence * 100.0f, 0, 'f', 1);
        const int baseline = 0;
        cv::putText(frame,
                    label.toStdString(),
                    cv::Point(rect.x, std::max(18, rect.y - 6)),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.52,
                    cv::Scalar(20, 30, 230),
                    2,
                    cv::LINE_AA);
        Q_UNUSED(baseline);
    }
}

void VisionDetector::loadClassNames(const QString &modelPath)
{
    m_classNames.clear();
    const QFileInfo modelInfo(modelPath);
    const QString namesPath = modelInfo.dir().filePath("classes.txt");
    QFile file(namesPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine().trimmed();
            if (!line.isEmpty()) {
                m_classNames << line;
            }
        }
    }

    if (m_classNames.isEmpty()) {
        m_classNames << "defect";
    }
}
