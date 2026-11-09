#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <memory>

class QObject;

// Keep one instance through inspect -> user confirmation -> close. Calls must be
// serialized by the same owning thread (construction and destruction included). Only explicitly requested inspections enumerate
// windows; no background window scanning or process signals are used.
class ApplicationActions {
public:
    ApplicationActions();
    ~ApplicationActions();
    ApplicationActions(const ApplicationActions &) = delete;
    ApplicationActions &operator=(const ApplicationActions &) = delete;

    QJsonObject inspect(const QJsonArray &identities) const;
    void discard() const; // Release a cancelled confirmation immediately.
    // Pass the complete, unchanged `windows` array returned by inspect(). A
    // snapshot is single-use and expires after 2 minutes. Any changed process or
    // window invalidates the whole request before any close message is sent.
    QJsonObject close(const QJsonArray &identities, const QJsonArray &expectedWindows) const;

private:
    struct Snapshot;
    std::unique_ptr<QObject> cleanupContext_;
    mutable std::unique_ptr<Snapshot> snapshot_;
};
