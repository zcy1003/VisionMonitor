#ifndef VISIONDETECTOR_H
#define VISIONDETECTOR_H

#include <QRect>
#include <QString>
#include <QStringList>
#include <QMetaType>
#include <QVector>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>

struct DetectionResult
{
    QString label;
    float confidence = 0.0f;
    QRect box;
};

Q_DECLARE_METATYPE(DetectionResult)
Q_DECLARE_METATYPE(QVector<DetectionResult>)

class VisionDetector
{
public:
    VisionDetector();

    bool loadYoloModel(const QString &modelPath, QString *message = nullptr);
    bool hasYoloModel() const;
    QString modelName() const;

    cv::Mat processFrame(const cv::Mat &frame,
                         QVector<DetectionResult> *results,
                         double *inferenceMs = nullptr);

private:
    QVector<DetectionResult> runYolo(const cv::Mat &frame, double *inferenceMs);
    QVector<DetectionResult> runRuleBasedInspection(const cv::Mat &frame,
                                                    double *inferenceMs);
    void drawResults(cv::Mat &frame, const QVector<DetectionResult> &results) const;
    void loadClassNames(const QString &modelPath);

    cv::dnn::Net m_net;
    QString m_modelName;
    QStringList m_classNames;
    float m_confThreshold = 0.35f;
    float m_nmsThreshold = 0.45f;
    cv::Size m_inputSize = cv::Size(640, 640);
};

#endif // VISIONDETECTOR_H
