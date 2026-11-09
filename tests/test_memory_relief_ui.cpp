#include "../src/memory_relief_dialog.h"
#include "../src/monitor_window.h"
#include "../src/ui_theme.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
QString evidence;
const double mib = 1024 * 1024;
void check(bool condition, const QString &message) {
    ++checks; if (condition) return;
    std::fprintf(stderr, "FAIL memory relief UI: %s\n", qPrintable(message)); std::exit(1);
}
void settle() { QApplication::sendPostedEvents(); QApplication::processEvents(); }
QJsonObject process(int pid, const QString &id, const QJsonValue &rss, const QString &name = QStringLiteral("Same name")) {
    return {{"pid", pid}, {"startTicks", 10000 + pid}, {"executablePath", "/fixture/application"}, {"name", name},
        {"applicationIdentity", id}, {"memoryBytes", rss}};
}
QJsonObject fresh(QJsonObject snapshot) {
    snapshot["timestamp"] = QDateTime::currentMSecsSinceEpoch() / 1000.; snapshot["processSampleAgeSeconds"] = 0.;
    return snapshot;
}
QJsonObject fixture() {
    QJsonArray processes{process(900001, "alpha", 900 * mib), process(900002, "alpha", QJsonValue(QJsonValue::Null)),
        process(900003, "beta", 700 * mib), process(900004, "", QJsonValue(QJsonValue::Null)), process(900005, "", QJsonValue(QJsonValue::Null)),
        process(int(QCoreApplication::applicationPid()), "self", 100 * mib, "DevStats")};
    return fresh(QJsonObject{{"processes", processes}, {"activityApplications", QJsonArray{QJsonObject{{"identity", "alpha"}, {"name", "Editor"}}, QJsonObject{{"identity", "beta"}, {"name", "Editor"}}}}});
}
QTreeWidgetItem *find(QTreeWidget *tree, const QString &key) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == key) return tree->topLevelItem(i);
    return nullptr;
}
int checkedCount(QTreeWidget *tree) {
    int count = 0; for (int i = 0; i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->checkState(0) == Qt::Checked) ++count;
    return count;
}
void applications(MemoryReliefDialog &dialog) {
    QSignalSpy requests(&dialog, &MemoryReliefDialog::closeApplicationsRequested);
    auto *apps = dialog.findChild<QTreeWidget *>("memoryReliefApplications"); auto *close = dialog.findChild<QPushButton *>("closeMemoryReliefApplications");
    dialog.setSnapshot(fixture()); settle();
    check(apps->topLevelItemCount() == 5 && checkedCount(apps) == 0 && !close->isEnabled(), "exact application grouping, same name kept separate, no default checks");
    auto *alpha = find(apps, "a:alpha"), *beta = find(apps, "a:beta");
    check(alpha && beta && alpha->text(0) == beta->text(0) && alpha->text(1) == "2", "application identity groups every process without merging names");
    apps->setCurrentItem(alpha); apps->setFocus(); QTest::keyClick(apps, Qt::Key_Space);
    check(alpha->checkState(0) == Qt::Checked && close->isEnabled(), "native Space interaction checks the current application");
    QTest::keyClick(apps, Qt::Key_Space); check(alpha->checkState(0) == Qt::Unchecked && !close->isEnabled(), "native Space interaction can clear the check");
    check(alpha->text(2).startsWith(QString::fromUtf8("≥")) && !find(apps, "p:900004:910004")->text(2).contains("0 B"), "partial RSS is lower bound, completely unknown is not zero");
    check(find(apps, "p:900004:910004") && find(apps, "p:900005:910005"), "process fallback uses pid and start, not name");
    check(!(find(apps, "a:self")->flags() & Qt::ItemIsEnabled), "self group is disabled and separate trim remains available");
    alpha->setCheckState(0, Qt::Checked); apps->setCurrentItem(beta); check(close->isEnabled(), "checked application can request a confirmed normal close");
    dialog.setSnapshot(fixture()); settle();
    check(checkedCount(apps) == 1 && find(apps, "a:alpha")->checkState(0) == Qt::Checked && apps->currentItem()->data(0, Qt::UserRole).toString() == "a:beta", "refresh retains exact checks and current row");
    close->click(); check(requests.count() == 1 && !close->isEnabled(), "one explicit request enters busy state"); close->click();
    check(requests.count() == 1, "busy prevents duplicate close requests");
    const auto identities = requests.first()[0].toJsonArray();
    check(identities.size() == 2 && identities[0].toObject().value("pid").toInt() == 900001 && identities[1].toObject().value("startTicks").toInt() == 910002,
        "request includes all selected member identities without truncation");
    check(identities[0].toObject().value("executablePath") == "/fixture/application" && identities[0].toObject().value("applicationIdentity") == "alpha" &&
        requests.first()[1].toStringList() == QStringList{"Editor"}, "request captures full identity and application name");
    dialog.setBusy(false); auto stale = fixture(); stale["processSampleAgeSeconds"] = 6.; dialog.setSnapshot(stale);
    check(!close->isEnabled(), "stale sample age disables close");
    stale = fixture(); stale["timestamp"] = QDateTime::currentMSecsSinceEpoch() / 1000. - 7; dialog.setSnapshot(stale);
    check(!close->isEnabled(), "stale wall timestamp disables close even with fresh age field");
    check(requests.count() == 1, "refresh and state changes never request a real action");
    auto large = fixture(); QJsonArray processes; for (int i = 0; i < 65; ++i) processes.append(process(910000 + i, "large", mib)); large["processes"] = processes; dialog.setSnapshot(large);
    check(apps->topLevelItem(0)->text(1) == "65" && apps->topLevelItem(0)->data(0, Qt::UserRole + 1).toJsonArray().size() == 65 &&
        !(apps->topLevelItem(0)->flags() & Qt::ItemIsEnabled) && apps->topLevelItem(0)->toolTip(0).contains("64"), "oversized application retains every identity and explicitly disables close");
    processes = {}; for (int i = 0; i < 66; ++i) processes.append(process(920000 + i, i < 33 ? "one" : "two", mib)); large["processes"] = processes; dialog.setSnapshot(fresh(large));
    apps->topLevelItem(0)->setCheckState(0, Qt::Checked); apps->topLevelItem(1)->setCheckState(0, Qt::Checked);
    check(!close->isEnabled() && dialog.findChild<QLabel *>("memoryReliefApplicationHint")->text().contains("66"), "combined selection over 64 is refused without dropping members");
    processes = {}; for (int i = 0; i < 70; ++i) processes.append(process(930000 + i, QStringLiteral("app-%1").arg(i), i * mib)); large["processes"] = processes; dialog.setSnapshot(fresh(large)); settle();
    check(apps->topLevelItemCount() == 50 && checkedCount(apps) == 0 && apps->topLevelItem(0)->data(0, Qt::UserRole).toString() == "a:app-69", "visible top fifty applications use descending current RSS and no newly automatic checks");
    apps->verticalScrollBar()->setValue(apps->verticalScrollBar()->maximum()); const int scroll = apps->verticalScrollBar()->value(); dialog.setSnapshot(fresh(large)); settle();
    check(scroll > 0 && apps->verticalScrollBar()->value() == scroll, "application table refresh retains scroll");
    dialog.setSnapshot(fixture());
}
void feedback(MemoryReliefDialog &dialog) {
    QSignalSpy trim(&dialog, &MemoryReliefDialog::trimMemoryRequested), refresh(&dialog, &MemoryReliefDialog::refreshRequested);
    auto *refreshButton = dialog.findChild<QPushButton *>("refreshMemoryRelief"); refreshButton->click(); check(refresh.count() == 1, "refresh is one explicit request");
    auto *trimButton = dialog.findChild<QPushButton *>("trimMemoryReliefSelf"); trimButton->click(); trimButton->click(); refreshButton->click();
    check(trim.count() == 1 && refresh.count() == 1 && !trimButton->isEnabled(), "self trim is explicit and busy protects every action"); dialog.setBusy(false);
    dialog.setFeedback("No supported windows; nothing was closed."); check(dialog.findChild<QLabel *>("memoryReliefFeedback")->text() == "No supported windows; nothing was closed.", "worker feedback preserved as plain text");
    dialog.setOutcome(QJsonObject{{"beforeAvailableBytes", 1000 * mib}, {"afterAvailableBytes", 900 * mib}, {"availableDeltaBytes", -100 * mib}, {"selectedBeforeRssBytes", 500 * mib}, {"remainingKnownRssBytes", QJsonValue(QJsonValue::Null)}, {"missingProcessCount", 2}});
    auto *outcome = dialog.findChild<QLabel *>("memoryReliefOutcome");
    check(outcome->text().contains(QString::fromUtf8("−100")) && outcome->text().contains(QString::fromUtf8("仍可观测 —")) && outcome->text().contains(QString::fromUtf8("其他程序也可能影响")), "negative available delta and unknown remaining RSS never claim released memory");
    dialog.setOutcome(QJsonObject{{"message", "Observation unavailable"}}); check(outcome->text().contains(QString::fromUtf8("变化 —")) && !outcome->text().contains("0 B"), "missing observations retain unknown values"); dialog.setOutcome({});
    check(outcome->isHidden(), "empty result can clear outcome");
}
void bridge(MonitorWindow &window) {
    QSignalSpy entries(&window, &MonitorWindow::memoryReliefRequested);
    window.findChild<QPushButton *>("memoryReliefFromMemory")->click(); window.findChild<QPushButton *>("memoryReliefFromDiagnostics")->click();
    check(entries.count() == 2, "memory and diagnostics entries bridge into relief signal");
}
void layout(MonitorWindow &window, MemoryReliefDialog &dialog) {
    QMap<QString, QImage> previews;
    for (const int mode : {1, 2}) {
        window.setAppearance(mode); dialog.setPalette(window.palette()); dialog.resize(800, 620); dialog.setSnapshot(fixture()); dialog.show(); settle();
        auto *applications = dialog.findChild<QTreeWidget *>("memoryReliefApplications"); applications->verticalScrollBar()->setValue(0); applications->setCurrentItem(nullptr);
        for (int i = 0; i < applications->topLevelItemCount(); ++i) applications->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        dialog.setFeedback(QStringLiteral("默认不选任何应用。操作前会再次确认。"));
        auto *scroll = dialog.findChild<QScrollArea *>("memoryReliefScroll"); scroll->verticalScrollBar()->setValue(0); settle();
        check(dialog.size() == QSize(800, 620) && scroll->horizontalScrollBar()->maximum() == 0 && scroll->widget()->width() <= scroll->viewport()->width(), "800x620 scroll layout fits in both themes");
        for (auto *label : dialog.findChildren<QLabel *>()) if (!label->text().isEmpty()) {
            check(label->textFormat() == Qt::PlainText, "all relief labels render plain text");
            check(QFontInfo(label->font()).pixelSize() >= int(9 * dialog.logicalDpiY() / 72.) - 1, "readable font at least 9pt captions");
        }
        check((dialog.palette().color(QPalette::Window).lightness() < 128) == (mode == 1), "dialog palette matches dark/light parent");
        check((dialog.findChild<QLabel *>("memoryReliefApplicationHint")->palette().color(QPalette::WindowText).lightness() > 128) == (mode == 1), "actual styled label foreground follows theme");
                if (!evidence.isEmpty()) { const QString name = mode == 1 ? "dark" : "light"; const QImage image = dialog.grab().toImage(); check(image.save(evidence + '/' + name + "-relief.png"), "dialog preview saved"); previews[name] = image; }
    }
    if (!evidence.isEmpty()) {
        QImage contact(960, 400, QImage::Format_RGB32); contact.fill(QColor("#d5d9df")); QPainter painter(&contact);
        for (int i = 0; i < 2; ++i) { const QString name = i == 0 ? "dark" : "light"; painter.setPen(QColor("#202733")); painter.drawText(i * 480 + 8, 18, name); painter.drawImage(QRect(i * 480, 24, 480, 372), previews.value(name)); }
        painter.end(); check(contact.save(evidence + "/memory-relief-contact.png"), "one bounded two-theme relief contact saved");
    }
}
}
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling); QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv); app.setStyle("Fusion"); app.setApplicationName("DevStats");
    qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QStringList>();
    QFont font = app.font(); font.setFamily("Noto Sans CJK SC"); font.setPointSizeF(11.5); app.setFont(font);
    if (app.arguments().size() > 1) { evidence = app.arguments().value(1); check(QDir().mkpath(evidence), "create isolated evidence directory"); }
    MonitorWindow window; window.setAppearance(1); window.resize(800, 620); window.show();
    MemoryReliefDialog dialog(&window); dialog.setSnapshot(fixture()); dialog.show(); settle();
    applications(dialog); feedback(dialog); bridge(window); layout(window, dialog);
    std::printf("PASS memory relief UI: %d checks; identity grouping / partial RSS / selection and scroll preservation / no defaults / fresh guards / 64 process bound / request-only actions / busy guard / unknown and negative observations / two entry bridges / 800x620 dark+light / DPR=%.1f\n", checks, dialog.devicePixelRatioF());
}
