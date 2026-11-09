#include "../src/controller.h"
#include "../src/monitor_window.h"
#include "../src/ui_components.h"
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL incidents integration: %s\n", message); std::exit(1); }
}
void write(const QString &path, const QByteArray &bytes) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
    QFile f(path); check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "open fixture");
    check(f.write(bytes)==bytes.size(), "write fixture");
}
QByteArray processStat(quint64 ticks) {
    QList<QByteArray> values; for (int field=3;field<=24;++field) values.append("0");
    values[0]="R"; values[1]="1"; values[11]=QByteArray::number(ticks);
    values[19]="123456"; values[21]="2048";
    return "424242 (fixture compiler) " + values.join(' ') + '\n';
}
}
int main(int argc, char **argv) {
    QTemporaryDir isolated; check(isolated.isValid(), "isolated home");
    
    qputenv("XDG_CONFIG_HOME",(isolated.path()+"/config").toUtf8());
    qputenv("XDG_DATA_HOME",(isolated.path()+"/share").toUtf8());
    qputenv("DEVSTATS_DATA_DIR",(isolated.path()+"/data").toUtf8());
    
    const QString proc=isolated.path()+"/proc", sys=isolated.path()+"/sys";
    qputenv("DEVSTATS_PROC_ROOT",proc.toUtf8()); qputenv("DEVSTATS_SYS_ROOT",sys.toUtf8());
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>();
    write(proc+"/meminfo","MemTotal: 8192000 kB\nMemAvailable: 6144000 kB\nSwapTotal: 1024000 kB\nSwapFree: 1024000 kB\n");
    write(proc+"/vmstat","pswpin 0\npswpout 0\npgmajfault 0\noom_kill 0\n");
    for (const auto &resource : {"cpu", "memory", "io"})
        write(proc+"/pressure/"+resource,"some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    write(proc+"/uptime","120.0 100.0\n"); QDir().mkpath(sys+"/class/power_supply");
    quint64 user=100,idle=900,ticks=1; bool forcePressure=false;
    QElapsedTimer elapsed; elapsed.start();
    auto update=[&] {
        const double t=elapsed.elapsed()/1000.; const bool high=forcePressure || (t>=4 && t<10);
        user+=high?90:10; idle+=high?0:90; ticks+=high?8:1;
        write(proc+"/stat","cpu "+QByteArray::number(user)+" 0 0 "+QByteArray::number(idle)+" 0 0 0 0\ncpu0 "+QByteArray::number(user)+" 0 0 "+QByteArray::number(idle)+" 0 0 0 0\nprocs_running "+(high?QByteArray("4"):QByteArray("1"))+"\nprocs_blocked 0\n");
        write(proc+"/loadavg",high?"2.00 2.00 1.00 4/8 4\n":"0.10 0.10 0.10 1/8 4\n");
        write(proc+"/424242/stat",processStat(ticks));
        write(proc+"/424242/cmdline",QByteArray("compiler\0",9));
        write(proc+"/424242/io","read_bytes: 0\nwrite_bytes: 0\n");
    };
    update(); QTimer fixture; fixture.setInterval(100); fixture.setTimerType(Qt::PreciseTimer);
    QObject::connect(&fixture,&QTimer::timeout,update); fixture.start();
    QJsonArray records; QJsonObject detail; int snapshots=0, normalCached=0, pressureFresh=0;
    MonitorWorker worker(isolated.path()+"/data",{});
    QObject::connect(&worker,&MonitorWorker::snapshotReady,[&](const QJsonObject &s) {
        ++snapshots; const double t=s.value("monotonicSeconds").toDouble();
        if(t>3 && t<6 && !s.value("processesFresh").toBool()) ++normalCached;
        if(t>7 && t<10.5 && s.value("processesFresh").toBool()) ++pressureFresh;
    });
    QObject::connect(&worker,&MonitorWorker::incidentsReady,[&](const QJsonArray &r,const QString &error) { check(error.isEmpty(),"no persistence error"); records=r; });
    QObject::connect(&worker,&MonitorWorker::incidentDetailReady,[&](const QJsonObject &d) { detail=d; });
    worker.start(); QTest::qWait(18200); worker.requestIncidents();
    check(normalCached>0,"idle process scans remain cached"); check(pressureFresh>0,"pressure temporarily increases process capture cadence");
    check(records.size()==1,"one coalesced pressure incident");
    const QString id=records[0].toObject().value("id").toString(); worker.requestIncident(id);
    check(detail.value("status").toString()=="recovered","recovery persisted through worker pipeline");
    check(!detail.value("witnesses").toArray().isEmpty(),"event contains ranked witnesses");
    check(!detail.value("prelude").toArray().isEmpty(),"event contains bounded prelude");
    bool compiler=false; for(const auto &v:detail.value("witnesses").toArray())
        for(const auto &p:v.toObject().value("topProcesses").toArray()) if(p.toObject().value("pid").toInt()==424242) compiler=true;
    check(compiler,"real sampler PID reaches persisted event");
    worker.setSleeping(true); const int asleep=snapshots; QTest::qWait(300); check(snapshots==asleep,"sleep stops polling");
    worker.setIncidentRecording(false); forcePressure=true; update(); worker.setSleeping(false); QTest::qWait(6500);
    worker.requestIncidents(); check(records.size()==1,"disabled recorder retains history without new captures"); worker.stop(); fixture.stop();
    const QString evidence=qEnvironmentVariable("DEVSTATS_TEST_EVIDENCE");
    if (!evidence.isEmpty()) {
        check(QDir().mkpath(evidence),"create explicit test evidence directory");
        const QString path=evidence+"/fixture-incidents.sqlite3";
        check(!QFile::exists(path),"do not overwrite unrelated evidence");
        check(QFile::copy(isolated.path()+"/data/Incidents/incidents.sqlite3",path),"save actual captured fixture for upgrade preservation test");
        check(QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner),"fixture evidence remains private");
    }
    {
        AppController controller; controller.start(false); controller.showModule("diagnostics"); QTest::qWait(150);
        auto *box=controller.window()->findChild<ToggleSwitch*>("autoRecordIncidents"); check(box && box->isChecked(),"default capture setting enabled");
        box->setChecked(false); QTest::qWait(150); controller.stop();
    }
    {
        AppController controller; controller.start(false); controller.showModule("diagnostics"); QTest::qWait(150);
        auto *box=controller.window()->findChild<ToggleSwitch*>("autoRecordIncidents"); check(box && !box->isChecked(),"disabled setting survives controller restart"); controller.stop();
    }
    std::puts("PASS incidents controller: real sampler→diagnosis→event DB; idle/pressure cadence, coalescing/recovery, ranked PID/prelude, sleep, disabled capture and persisted setting");
}
