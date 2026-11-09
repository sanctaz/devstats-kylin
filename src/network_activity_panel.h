#pragma once
#include <QFrame>
#include <QJsonObject>
#include <memory>

class NetworkActivityPanel final : public QFrame {
    Q_OBJECT
public:
    explicit NetworkActivityPanel(QWidget *parent = nullptr);
    ~NetworkActivityPanel() override;
    void setObservation(const QJsonObject &observation, const QJsonObject &previous = {});
protected:
    void showEvent(QShowEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
