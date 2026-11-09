#include "../src/controller.h"
#include "../src/monitor_window.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <functional>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok,const char *what) { if(!ok) {std::fprintf(stderr,"FAIL diagnostic worker: %s\n",what);std::exit(1);} }
void write(const QString &path,const QByteArray &bytes) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()),"fixture directory"); QFile file(path);
    check(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"fixture open"); check(file.write(bytes)==bytes.size(),"fixture write");
}
bool waitFor(const std::function<bool()> &condition) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 5000) QTest::qWait(50);
    return condition();
}
QByteArray processStat(quint64 ticks) {
    QList<QByteArray> fields; for(int i=3;i<=24;++i)fields.append("0");
    fields[0]="R";fields[1]="1";fields[11]=QByteArray::number(ticks);fields[19]="123456";fields[21]="2048";
    return "424242 (Fixture app) "+fields.join(' ')+'\n';
}
}
int main(int argc,char **argv) {
    QTemporaryDir isolated;check(isolated.isValid(),"isolated environment");
    qputenv("XDG_CONFIG_HOME",(isolated.path()+"/config").toUtf8());
    qputenv("XDG_DATA_HOME",(isolated.path()+"/share").toUtf8());qputenv("DEVSTATS_DATA_DIR",(isolated.path()+"/data").toUtf8());
    
    const QString proc=isolated.path()+"/proc",sys=isolated.path()+"/sys";
    qputenv("DEVSTATS_PROC_ROOT",proc.toUtf8());qputenv("DEVSTATS_SYS_ROOT",sys.toUtf8());
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    qRegisterMetaType<QJsonObject>();qRegisterMetaType<QJsonArray>();
    QDir().mkpath(sys+"/class/power_supply");write(proc+"/uptime","120.0 100.0\n");
    write(proc+"/vmstat","pswpin 0\npswpout 0\npgmajfault 0\noom_kill 0\n");
    quint64 user=100,idle=900,ticks=1;bool pressure=false;
    auto update=[&] {
        user+=pressure?95:10;idle+=pressure?5:90;ticks+=pressure?9:1;
        write(proc+"/stat","cpu "+QByteArray::number(user)+" 0 0 "+QByteArray::number(idle)+" 0 0 0 0\ncpu0 "+QByteArray::number(user)+" 0 0 "+QByteArray::number(idle)+" 0 0 0 0\nprocs_running "+(pressure?QByteArray("2"):QByteArray("0"))+"\nprocs_blocked 0\n");
        write(proc+"/loadavg",pressure?"2.00 2.00 1.00 2/8 4\n":"0.10 0.10 0.10 1/8 4\n");
        write(proc+"/meminfo",pressure?"MemTotal: 8192000 kB\nMemAvailable: 102400 kB\nSwapTotal: 1024000 kB\nSwapFree: 1024000 kB\n":"MemTotal: 8192000 kB\nMemAvailable: 6144000 kB\nSwapTotal: 1024000 kB\nSwapFree: 1024000 kB\n");
        for(const auto &kind:{"cpu","memory","io"}) {
            const bool high=pressure && QByteArray(kind)=="memory";
            write(proc+"/pressure/"+kind, high ? "some avg10=12.0 avg60=3.0 avg300=1.0 total=1\nfull avg10=2.0 avg60=1.0 avg300=0.1 total=1\n" : "some avg10=0.0 avg60=0.0 avg300=0.0 total=0\nfull avg10=0.0 avg60=0.0 avg300=0.0 total=0\n");
        }
        write(proc+"/424242/stat",processStat(ticks));write(proc+"/424242/cmdline",QByteArray("fixture\0",8));
        write(proc+"/424242/io","read_bytes: 0\nwrite_bytes: 0\n");
    };
    update();QTimer fixture;fixture.setInterval(100);QObject::connect(&fixture,&QTimer::timeout,update);fixture.start();
    MonitorWorker worker(isolated.path()+"/data",{},false);
    QJsonObject latest;QJsonArray runSnapshots;int snapshots=0,actions=0;
    QObject::connect(&worker,&MonitorWorker::snapshotReady,[&](const QJsonObject &s) {
        latest=s;++snapshots;
        if(s.value("diagnosticRun").toObject().value("state")=="running")runSnapshots.append(s.value("diagnosticRun"));
    });
    QObject::connect(&worker,&MonitorWorker::actionFinished,[&](const QString &){++actions;});
    worker.start();QTest::qWait(4500);check(!latest.value("processesFresh").toBool(),"idle warmup resumes 10-second process policy");
    const auto timer=worker.findChildren<QTimer*>(QString(),Qt::FindDirectChildrenOnly);
    check(timer.size()==1 && timer.first()->interval()==2000,"one existing worker sampling timer");
    worker.runDiagnostics();const auto started=latest.value("diagnosticRun").toObject();
    check(started.value("state")=="running" && started.value("sampleCount").toInt()==1,"explicit start samples and publishes immediately");
    check(timer.first()->timerType()==Qt::PreciseTimer,"run uses existing timer precisely");
    worker.runDiagnostics();check(latest.value("diagnosticRun").toObject()==started,"duplicate start neither resamples nor resets run");
    pressure=true;update();QTest::qWait(10300);
    auto result=latest.value("diagnosticRun").toObject();
    const auto completed=result;
    check(result.value("state")=="completed" && result.value("sampleCount").toInt()==6,"real worker completes 10-second window");
    check(result.value("progress").toInt()==100 && !result.value("finishedAt").isNull(),"completion progress and wall time");
    check(timer.first()->timerType()==Qt::CoarseTimer,"completion restores previous timer strategy");
    bool sustained=false;for(const auto &f:result.value("findings").toArray())sustained|=f.toObject().value("persistence")=="sustained";
    check(sustained,"real sampler diagnosis aggregated as sustained evidence");
    check(actions==0 && latest.value("memoryOptimization").toObject().isEmpty(),"observation never invokes automatic trim or action feedback");
    check(QFileInfo::exists(proc+"/424242/stat"),"fixture process untouched");
    pressure=false;update();QTest::qWait(2200);check(!latest.value("processesFresh").toBool(),"completion restores idle process cadence");
    check(latest.value("diagnosticRun").toObject()==completed,"completed diagnostic result reused unchanged in later snapshots");
    worker.runDiagnostics();QTest::qWait(2100);worker.cancelDiagnostics();result=latest.value("diagnosticRun").toObject();
    check(result.value("state")=="cancelled" && result.value("sampleCount").toInt()>=2 && result.value("progress").toInt()<100,"cancel preserves bounded partial result");
    check(timer.first()->timerType()==Qt::CoarseTimer,"cancel restores timer strategy");
    QTest::qWait(2200);check(!latest.value("processesFresh").toBool(),"cancel restores idle process cadence");
    check(latest.value("diagnosticRun").toObject()==result,"cancelled diagnostic result reused unchanged in later snapshots");
    worker.runDiagnostics();check(latest.value("diagnosticRun").toObject().value("sampleCount").toInt()==1,"rerun replaces last result");
    worker.setSleeping(true);check(latest.value("diagnosticRun").toObject().value("state")=="interrupted","sleep interrupts active run");
    worker.runDiagnostics();check(latest.value("diagnosticRun").toObject().value("state")=="interrupted","start during sleep ignored");
    const int asleep=snapshots;QTest::qWait(250);check(snapshots==asleep,"sleep stops worker sampling");
    worker.setSleeping(false);check(latest.value("diagnosticRun").toObject().value("state")=="interrupted","wake does not restart observation");
    worker.runDiagnostics();worker.stop();check(latest.value("diagnosticRun").toObject().value("state")=="interrupted","stop interrupts active run");
    const QString evidence=qEnvironmentVariable("DEVSTATS_TEST_EVIDENCE");
    if(!evidence.isEmpty()) {QDir().mkpath(evidence);write(evidence+"/worker-run.json",QJsonDocument(QJsonObject{{"completed",completed},{"cancelled",result},{"actions",actions},{"progressSamples",runSnapshots}}).toJson());}
    {
        AppController controller;controller.start(false);controller.showModule("diagnostics");
        check(waitFor([&] { return !controller.latestSnapshot().isEmpty(); }),"controller worker startup");
        controller.window()->diagnosticRunRequested(true);
        check(waitFor([&] { return controller.latestSnapshot().value("diagnosticRun").toObject().value("state")=="running"; }),"window start signal reaches worker");
        controller.window()->diagnosticRunRequested(false);
        check(waitFor([&] { return controller.latestSnapshot().value("diagnosticRun").toObject().value("state")=="cancelled"; }),"window cancel signal reaches worker");controller.stop();
    }
    fixture.stop();std::puts("PASS diagnostic worker: real sampler/diagnosis, progress/completion, repeated start, rerun/cancel/sleep/stop, timer/process cadence restoration, cached terminal snapshots, no automatic actions, window signal wiring");
}
