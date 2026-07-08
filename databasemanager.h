#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QDateTime>
#include <QString>
#include <QVector>

struct InspectionRecord
{
    int id = 0;
    QDateTime timestamp;
    QString source;
    QString imagePath;
    QString modelName;
    QString result;
    int defectCount = 0;
    QString defectLabels;
    double maxConfidence = 0.0;
    double inferenceMs = 0.0;
    QString snapshotPath;
};

struct AlarmRecord
{
    int id = 0;
    QDateTime timestamp;
    QString level;
    QString source;
    QString metric;
    double value = 0.0;
    double threshold = 0.0;
    QString message;
    bool acknowledged = false;
};

class DatabaseManager
{
public:
    DatabaseManager();
    ~DatabaseManager();

    // Open the SQLite database and create tables required by the inspection flow.
    bool initialize(QString *errorMessage = nullptr);
    // Persist one inspection result for later traceability, reports, and alarms.
    bool saveInspectionRecord(const InspectionRecord &record, QString *errorMessage = nullptr);
    // Return newest records first; pass limit <= 0 when exporting the full report.
    QVector<InspectionRecord> recentInspectionRecords(int limit, QString *errorMessage = nullptr) const;
    // Alarm records are stored separately so alarm review does not scan image records.
    bool saveAlarmRecord(const AlarmRecord &record, QString *errorMessage = nullptr);
    // Return newest alarms first; pass limit <= 0 when exporting the full report.
    QVector<AlarmRecord> recentAlarmRecords(int limit, QString *errorMessage = nullptr) const;
    // Mark historical alarms as acknowledged after the operator confirms them.
    bool acknowledgeAllAlarms(QString *errorMessage = nullptr);
    QString databasePath() const;
    QString snapshotDirectory() const;
    // NG images are separated from normal snapshots for defect review and reports.
    QString ngArchiveDirectory() const;

private:
    // Keep schema creation in one place so future tables can be added safely.
    bool createTables(QString *errorMessage);

    QString m_connectionName;
    QString m_databasePath;
    QString m_snapshotDirectory;
    QString m_ngArchiveDirectory;
};

#endif // DATABASEMANAGER_H
