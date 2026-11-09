#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <memory>

class MemoryReliefDialog final : public QDialog {
    Q_OBJECT
public:
    explicit MemoryReliefDialog(QWidget *parent = nullptr);
    ~MemoryReliefDialog() override;
    void setSnapshot(const QJsonObject &snapshot);
    void setBusy(bool busy);
    void setFeedback(const QString &message);
    void setOutcome(const QJsonObject &outcome);
signals:
    void closeApplicationsRequested(QJsonArray identities, QStringList applicationNames);
    void trimMemoryRequested();
    void refreshRequested();
protected:
    void changeEvent(QEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
