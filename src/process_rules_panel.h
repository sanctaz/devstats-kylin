#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>

// The "自动查杀" page: a master switch, the rules with their live state, and a
// log of what was actually done. Every edit is applied to the worker immediately.
class ProcessRulesPanel final : public QWidget {
    Q_OBJECT
public:
    explicit ProcessRulesPanel(QWidget *parent = nullptr);
    ~ProcessRulesPanel() override;
    void setConfiguration(bool enabled, const QJsonArray &rules);
    // Reads "processes" for the picker and "processAutomation" for live state.
    void setSnapshot(const QJsonObject &snapshot);
    void configurationFinished(const QJsonObject &result);
    // Opens the rule editor with this process's program path filled in.
    void addRuleFor(const QJsonObject &process);
signals:
    void configurationRequested(bool enabled, QJsonArray rules);
    void pauseRequested();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
