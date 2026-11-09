#include "../src/controller.h"
#include "../src/monitor_window.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {
int checks=0;
void check(bool ok,const char *message) { ++checks; if(ok) return; std::fprintf(stderr,"FAIL network controller: %s\n",message); std::exit(1); }
void write(const QString &path,const QByteArray &bytes) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()),"create private fixture parent"); QFile f(path);
    check(f.open(QIODevice::WriteOnly|QIODevice::Truncate) && f.write(bytes)==bytes.size(),"write private fixture");
}
bool wait(const std::function<bool()> &predicate,int milliseconds=2600) {
    QElapsedTimer timer; timer.start(); while(!predicate() && timer.elapsed()<milliseconds) QTest::qWait(30); return predicate();
}
QString state(const QJsonObject &snapshot) { return snapshot.value("networkApplications").toObject().value("state").toString(); }
void inactive(const QJsonObject &snapshot) {
    const auto network=snapshot.value("networkApplications").toObject();
    check(network.value("state").toString()=="inactive","hidden/non-network presentation emits inactive state");
    check(network.value("applications").toArray().isEmpty(),"inactive observation clears previous applications");
    check(network.value("observing").isBool() && !network.value("observing").toBool(),"inactive snapshot explicitly marks observation disabled");
}
void selectModule(MonitorWindow *window,int index) {
    auto *nav=window->findChild<QListWidget*>("navigation"); check(nav && nav->count()==12,"nine modules under three sections");
    QTest::mouseClick(nav->viewport(),Qt::LeftButton,Qt::NoModifier,nav->visualItemRect(nav->item(index)).center());
}
}
int main(int argc,char **argv) {
    QTemporaryDir isolated; check(isolated.isValid(),"fresh private fixture root");
    const auto proc=isolated.path()+"/proc", sys=isolated.path()+"/sys";
     qputenv("XDG_CONFIG_HOME",(isolated.path()+"/config").toUtf8());
    qputenv("XDG_DATA_HOME",(isolated.path()+"/share").toUtf8()); qputenv("DEVSTATS_DATA_DIR",(isolated.path()+"/data").toUtf8());
    qputenv("DEVSTATS_PROC_ROOT",proc.toUtf8()); qputenv("DEVSTATS_SYS_ROOT",sys.toUtf8()); 
    QApplication application(argc,argv); application.setQuitOnLastWindowClosed(false);
    qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>();
    write(proc+"/meminfo","MemTotal: 8192000 kB\nMemAvailable: 6144000 kB\nSwapTotal: 1024000 kB\nSwapFree: 1024000 kB\n");
    write(proc+"/stat","cpu 100 0 0 900 0 0 0 0\ncpu0 100 0 0 900 0 0 0 0\nprocs_running 1\nprocs_blocked 0\n");
    write(proc+"/loadavg","0.1 0.1 0.1 1/10 1\n"); write(proc+"/uptime","120 100\n");
    write(proc+"/vmstat","pswpin 0\npswpout 0\npgmajfault 0\noom_kill 0\n");
    write(proc+"/net/dev","Inter-| Receive | Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    for(const auto &resource:{"cpu","memory","io"}) write(proc+"/pressure/"+resource,"some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    QDir().mkpath(sys+"/class/power_supply");
    QJsonArray transitions;
    {
        MonitorWorker worker(isolated.path()+"/worker-data",{}); QJsonObject latest; int samples=0;
        QObject::connect(&worker,&MonitorWorker::snapshotReady,[&](const QJsonObject &snapshot) { latest=snapshot; ++samples; transitions.append(state(snapshot)); });
        worker.start(); check(wait([&]{return !latest.isEmpty();}),"worker initial snapshot"); inactive(latest);
        const int timers=worker.findChildren<QTimer*>().size(); worker.setNetworkObservation(true);
        check(wait([&]{return state(latest)=="unavailable";}),"fakeproc observation is unsupported instead of collecting host netlink");
        auto network=latest.value("networkApplications").toObject();
        check(network.value("applications").toArray().isEmpty() && !network.value("coverage").toArray().isEmpty(),"unsupported collector reports reason without fabricated rates");
        check(network.value("observing").toBool(),"unsupported capability remains distinct from requested observation state");
        check(worker.findChildren<QTimer*>().size()==timers,"network observation reuses existing worker cadence without another timer");
        worker.setNetworkObservation(false); check(wait([&]{return state(latest)=="inactive";}),"leaving observation publishes inactive reset"); inactive(latest);
        worker.setNetworkObservation(true); check(wait([&]{return state(latest)=="unavailable";}),"returning observation reevaluates collector capability");
        worker.setSleeping(true); const int asleep=samples; QTest::qWait(2200); check(samples==asleep,"sleep stops all worker polling");
        worker.setNetworkObservation(false); worker.setSleeping(false); check(wait([&]{return state(latest)=="inactive";}),"wake respects stopped network observation");
        worker.setNetworkObservation(true); check(wait([&]{return state(latest)=="unavailable";}),"network observation can resume after wake");
        worker.stop(); const int stopped=samples; QTest::qWait(150); check(samples==stopped,"stop ends network polling");
    }
    {
        AppController controller; controller.start(false); check(wait([&]{return !controller.latestSnapshot().isEmpty();}),"controller initial snapshot"); inactive(controller.latestSnapshot());
        controller.showModule("network"); check(wait([&]{return state(controller.latestSnapshot())=="unavailable";}),"visible network selection reaches worker via controller");
        selectModule(controller.window(),1); check(wait([&]{return state(controller.latestSnapshot())=="inactive";}),"real module click stops controller network collection");
        selectModule(controller.window(),5); check(wait([&]{return state(controller.latestSnapshot())=="unavailable";}),"real network click restarts controller collection");
        controller.window()->showMinimized(); check(wait([&]{return state(controller.latestSnapshot())=="inactive";}),"minimize reaches worker network reset");
        controller.window()->showNormal(); check(wait([&]{return state(controller.latestSnapshot())=="unavailable";}),"restore reaches worker observation restart");
        controller.window()->close(); check(wait([&]{return state(controller.latestSnapshot())=="inactive";}),"close to tray stops network collection");
        controller.stop();
    }
    const QString evidence=qEnvironmentVariable("DEVSTATS_TEST_EVIDENCE");
    if(!evidence.isEmpty()) { check(QDir().mkpath(evidence),"create explicit evidence directory"); QFile f(evidence+"/network-controller-result.json"); check(f.open(QIODevice::WriteOnly),"open result JSON");
        f.write(QJsonDocument(QJsonObject{{"result","PASS"},{"checks",checks},{"ordinaryUid",int(geteuid())},{"fixtureProcRoot",true},{"transitions",transitions}}).toJson()); }
    std::printf("PASS network controller: %d checks; current-module visibility, no extra timer, unsupported fakeproc, leave/minimize/close reset and sleep/wake lifecycle\n",checks);
}
