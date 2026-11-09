#pragma once

#include <QJsonObject>
#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <memory>

class QTimer;

// All methods, including destruction, belong to the monitoring worker thread.
class HistoryStore {
public:
    explicit HistoryStore(const QString &dataRoot);
    ~HistoryStore();
    HistoryStore(const HistoryStore &) = delete;
    HistoryStore &operator=(const HistoryStore &) = delete;
    bool open();
    bool record(const QJsonObject &snapshot);
    QJsonObject query(qint64 startEpochSeconds, qint64 endEpochSeconds, int maxPoints = 600);
    void flush();
    QString error() const;

private:
    struct Aggregate {
        double total = 0;
        double minimum = 0;
        double maximum = 0;
        qint64 count = 0;
        double first = 0;
        double last = 0;
        void add(double value, double timestamp);
        void merge(const Aggregate &other);
    };
    bool commit();
    void secureSidecars();
    QString m_root;
    QString m_path;
    QString m_connection;
    QString m_error;
    QSqlDatabase m_database;
    QMap<qint64, QMap<QString, Aggregate>> m_pending;
    std::unique_ptr<QTimer> m_flushTimer;
};
