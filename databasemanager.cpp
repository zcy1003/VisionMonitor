#include "databasemanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace {
QString writableDataRoot()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) {
        root = QDir(QCoreApplication::applicationDirPath()).filePath("data");
    }

    QDir dir(root);
    if (!dir.exists() && !dir.mkpath(".")) {
        // AppData can fail in restricted or portable runs, so keep a local
        // fallback beside the executable instead of failing before SQLite opens.
        root = QDir(QCoreApplication::applicationDirPath()).filePath("data");
        dir.setPath(root);
        if (!dir.exists()) {
            dir.mkpath(".");
        }
    }
    return dir.absolutePath();
}

QString queryError(const QSqlQuery &query)
{
    return query.lastError().text();
}
}

DatabaseManager::DatabaseManager()
    : m_connectionName(QString("vision_monitor_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

DatabaseManager::~DatabaseManager()
{
    if (!m_connectionName.isEmpty()) {
        {
            // Close the handle before removeDatabase; Qt warns if a connection
            // is removed while references to it are still alive.
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isValid()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool DatabaseManager::initialize(QString *errorMessage)
{
    const QString dataRoot = writableDataRoot();
    m_databasePath = QDir(dataRoot).filePath("vision_monitor.db");
    m_snapshotDirectory = QDir(dataRoot).filePath("snapshots");
    m_ngArchiveDirectory = QDir(dataRoot).filePath("ng_archive");

    QDir snapshotDir(m_snapshotDirectory);
    if (!snapshotDir.exists() && !snapshotDir.mkpath(".")) {
        if (errorMessage) {
            *errorMessage = QString("Cannot create snapshot directory: %1").arg(m_snapshotDirectory);
        }
        return false;
    }

    QDir ngArchiveDir(m_ngArchiveDirectory);
    if (!ngArchiveDir.exists() && !ngArchiveDir.mkpath(".")) {
        if (errorMessage) {
            *errorMessage = QString("Cannot create NG archive directory: %1").arg(m_ngArchiveDirectory);
        }
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(m_databasePath);
    if (!db.open()) {
        if (errorMessage) {
            *errorMessage = db.lastError().text();
        }
        return false;
    }

    return createTables(errorMessage);
}

bool DatabaseManager::saveInspectionRecord(const InspectionRecord &record, QString *errorMessage)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorMessage) {
            *errorMessage = "SQLite database is not open.";
        }
        return false;
    }

    QSqlQuery query(db);
    query.prepare("INSERT INTO inspection_records "
                  "(timestamp, source, image_path, model_name, result, defect_count, "
                  "defect_labels, max_confidence, inference_ms, snapshot_path) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(record.timestamp.toString(Qt::ISODate));
    query.addBindValue(record.source);
    query.addBindValue(record.imagePath);
    query.addBindValue(record.modelName);
    query.addBindValue(record.result);
    query.addBindValue(record.defectCount);
    query.addBindValue(record.defectLabels);
    query.addBindValue(record.maxConfidence);
    query.addBindValue(record.inferenceMs);
    query.addBindValue(record.snapshotPath);

    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }
    return true;
}

QVector<InspectionRecord> DatabaseManager::recentInspectionRecords(int limit, QString *errorMessage) const
{
    QVector<InspectionRecord> records;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorMessage) {
            *errorMessage = "SQLite database is not open.";
        }
        return records;
    }

    QSqlQuery query(db);
    QString sql = "SELECT id, timestamp, source, image_path, model_name, result, defect_count, "
                  "defect_labels, max_confidence, inference_ms, snapshot_path "
                  "FROM inspection_records ORDER BY id DESC";
    if (limit > 0) {
        sql += " LIMIT ?";
    }
    query.prepare(sql);
    if (limit > 0) {
        query.addBindValue(limit);
    }
    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return records;
    }

    while (query.next()) {
        InspectionRecord record;
        record.id = query.value(0).toInt();
        record.timestamp = QDateTime::fromString(query.value(1).toString(), Qt::ISODate);
        record.source = query.value(2).toString();
        record.imagePath = query.value(3).toString();
        record.modelName = query.value(4).toString();
        record.result = query.value(5).toString();
        record.defectCount = query.value(6).toInt();
        record.defectLabels = query.value(7).toString();
        record.maxConfidence = query.value(8).toDouble();
        record.inferenceMs = query.value(9).toDouble();
        record.snapshotPath = query.value(10).toString();
        records.push_back(record);
    }
    return records;
}

bool DatabaseManager::saveAlarmRecord(const AlarmRecord &record, QString *errorMessage)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorMessage) {
            *errorMessage = "SQLite database is not open.";
        }
        return false;
    }

    QSqlQuery query(db);
    query.prepare("INSERT INTO alarm_records "
                  "(timestamp, level, source, metric, value, threshold, message, acknowledged) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(record.timestamp.toString(Qt::ISODate));
    query.addBindValue(record.level);
    query.addBindValue(record.source);
    query.addBindValue(record.metric);
    query.addBindValue(record.value);
    query.addBindValue(record.threshold);
    query.addBindValue(record.message);
    query.addBindValue(record.acknowledged ? 1 : 0);

    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }
    return true;
}

QVector<AlarmRecord> DatabaseManager::recentAlarmRecords(int limit, QString *errorMessage) const
{
    QVector<AlarmRecord> records;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorMessage) {
            *errorMessage = "SQLite database is not open.";
        }
        return records;
    }

    QSqlQuery query(db);
    QString sql = "SELECT id, timestamp, level, source, metric, value, threshold, message, acknowledged "
                  "FROM alarm_records ORDER BY id DESC";
    if (limit > 0) {
        sql += " LIMIT ?";
    }
    query.prepare(sql);
    if (limit > 0) {
        query.addBindValue(limit);
    }
    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return records;
    }

    while (query.next()) {
        AlarmRecord record;
        record.id = query.value(0).toInt();
        record.timestamp = QDateTime::fromString(query.value(1).toString(), Qt::ISODate);
        record.level = query.value(2).toString();
        record.source = query.value(3).toString();
        record.metric = query.value(4).toString();
        record.value = query.value(5).toDouble();
        record.threshold = query.value(6).toDouble();
        record.message = query.value(7).toString();
        record.acknowledged = query.value(8).toInt() != 0;
        records.push_back(record);
    }
    return records;
}

bool DatabaseManager::acknowledgeAllAlarms(QString *errorMessage)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorMessage) {
            *errorMessage = "SQLite database is not open.";
        }
        return false;
    }

    QSqlQuery query(db);
    if (!query.exec("UPDATE alarm_records SET acknowledged = 1 WHERE acknowledged = 0")) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }
    return true;
}

QString DatabaseManager::databasePath() const
{
    return m_databasePath;
}

QString DatabaseManager::snapshotDirectory() const
{
    return m_snapshotDirectory;
}

QString DatabaseManager::ngArchiveDirectory() const
{
    return m_ngArchiveDirectory;
}

bool DatabaseManager::createTables(QString *errorMessage)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));

    // This table is intentionally broad enough to support later alarms,
    // reports, and traceability screens without changing the first schema.
    const QString sql =
        "CREATE TABLE IF NOT EXISTS inspection_records ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "image_path TEXT,"
        "model_name TEXT,"
        "result TEXT NOT NULL,"
        "defect_count INTEGER NOT NULL,"
        "defect_labels TEXT,"
        "max_confidence REAL,"
        "inference_ms REAL,"
        "snapshot_path TEXT"
        ")";

    if (!query.exec(sql)) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }

    // Keep the common history query fast as the demo database grows.
    if (!query.exec("CREATE INDEX IF NOT EXISTS idx_inspection_records_timestamp "
                    "ON inspection_records(timestamp)")) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }

    // Alarm history is independent from image history so operators can confirm
    // and review equipment warnings even when no visual NG record exists.
    const QString alarmSql =
        "CREATE TABLE IF NOT EXISTS alarm_records ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp TEXT NOT NULL,"
        "level TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "metric TEXT NOT NULL,"
        "value REAL NOT NULL,"
        "threshold REAL NOT NULL,"
        "message TEXT NOT NULL,"
        "acknowledged INTEGER NOT NULL DEFAULT 0"
        ")";
    if (!query.exec(alarmSql)) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }

    // Keep alarm-management queries fast as the demo runs for a long time.
    if (!query.exec("CREATE INDEX IF NOT EXISTS idx_alarm_records_timestamp "
                    "ON alarm_records(timestamp)")) {
        if (errorMessage) {
            *errorMessage = queryError(query);
        }
        return false;
    }
    return true;
}
