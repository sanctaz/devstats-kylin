#pragma once

#include <QJsonObject>
#include <memory>

// Measures whether the machine actually stalls, rather than inferring it from
// load figures. Two small threads, both owned by this object:
//  - scheduler: sleeps for a fixed period and records how late it woke up. A
//    delayed wake-up is what every application on the desktop experiences.
//  - display: times a round trip to the X server (Xorg or Xwayland), which catches
//    a stuck compositor or GPU driver even while the CPU looks idle.
// Background rates are 5 Hz and 1 Hz. setFast(true) raises them during a one-click
// diagnosis only. Neither thread touches shared state beyond its own counters.
class ResponsivenessProbe final {
public:
    ResponsivenessProbe();
    ~ResponsivenessProbe();
    ResponsivenessProbe(const ResponsivenessProbe &) = delete;
    ResponsivenessProbe &operator=(const ResponsivenessProbe &) = delete;

    void start();
    void stop();
    void setFast(bool fast);
    // Statistics since the previous call. A display request still unanswered is
    // reported by its current age, so a hung server is visible while it is hung.
    QJsonObject takeWindow();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
