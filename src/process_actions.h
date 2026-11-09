#pragma once

#include <QJsonObject>
#include <QString>

// Operates only on a fresh, native /proc identity (pid/startTicks/executablePath).
// The caller obtains confirmation before execute(); this class never prompts.
class ProcessActions {
public:
    // Checks native identity and the same ownership/session protection policy,
    // without probing or sending any signal (also available on Linux 4.19).
    QJsonObject inspectIdentity(const QJsonObject &identity) const;
    QJsonObject inspect(const QJsonObject &identity) const;
    // action: "terminate" (SIGTERM) or "kill" (SIGKILL), sent through the same
    // verified process handle after the same identity and protection checks.
    QJsonObject execute(const QJsonObject &identity, const QString &action) const;
    // True while this exact pid/start identity still exists and has not exited.
    bool isRunning(const QJsonObject &identity) const;
};
