#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <memory>

// Own and call a sampler from one thread. First samples and counter resets have
// null rates: zero is reserved for a measured interval without activity.
// CPU fractions include iowait separately; PSI averages are percentages. Missing
// PSI resources are null; CPU full is always null (undefined system-wide).
// vm swap rates use the system page size; process fault rates reuse stat scans.
class LinuxSampler {
public:
    explicit LinuxSampler(const QString &procRoot = QStringLiteral("/proc"),
                          const QString &sysRoot = QStringLiteral("/sys"));
    ~LinuxSampler();
    LinuxSampler(const LinuxSampler &) = delete;
    LinuxSampler &operator=(const LinuxSampler &) = delete;

    void setProjectRoots(const QStringList &roots);
    QJsonObject sample(bool scanProcesses = true);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
