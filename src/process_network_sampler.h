#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVector>

// Used only by the monitoring worker while the network page is visible.
// Counts TCP payload received and acknowledged; these are not interface wire bytes.
class ProcessNetworkSampler {
public:
    explicit ProcessNetworkSampler(const QString &procRoot = QStringLiteral("/proc"));
    QJsonObject sample(const QJsonObject &freshSnapshot);
    void reset();
    static QJsonObject retainedObservation(const QJsonObject &current, const QJsonObject &previous);

private:
    friend class ProcessNetworkSamplerTest;
    struct Connection {
        quint64 inode = 0, cookie = 0, received = 0, acknowledged = 0;
        int family = 0;
        bool counters = false;
    };
    struct Dump {
        QVector<Connection> connections;
        QStringList issues;
        bool available = true, complete = true, limited = false;
    };
    struct Previous {
        Connection counters;
        QString application;
        QSet<QString> owners;
    };
    struct ProcessBaseline {
        QString application, executable;
        QSet<QString> sockets;
        bool complete = false;
    };
    static bool parseDatagram(const QByteArray &data, quint32 sequence, quint32 port,
                              quint32 uid, Dump &dump, bool &done);
    static Dump collect(double deadline);
    QJsonObject sampleDump(const QJsonObject &snapshot, const Dump &dump,
                           double now, qint64 sampledAt, double deadline);
    QString procRoot_;
    QHash<QString, Previous> previous_;
    QHash<QString, ProcessBaseline> processes_;
    double previousTime_ = -1;
};
