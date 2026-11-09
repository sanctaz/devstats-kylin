#include "../src/controller.h"
#include "../src/monitor_window.h"
#include "../src/memory_relief_dialog.h"
#include <QApplication>
#include <QAbstractButton>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest/QTest>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {
int assertions=0;
void check(bool ok,const char *message) { ++assertions; if(!ok) {std::fprintf(stderr,"FAIL relief controller: %s\n",message);std::exit(1);} }
bool waitFor(const std::function<bool()> &test,int ms=12000) {QElapsedTimer t;t.start();while(!test()&&t.elapsed()<ms)QTest::qWait(20);return test();}
MemoryReliefDialog *dialog() {for(auto *w:QApplication::topLevelWidgets())if(auto *d=qobject_cast<MemoryReliefDialog*>(w))return d;return nullptr;}
QJsonObject selectedProcess(const QJsonObject &s,qint64 pid) {for(const auto &v:s.value("processes").toArray())if(v.toObject().value("pid").toDouble()==pid)return v.toObject();return {};}
void selectPID(MemoryReliefDialog *d,qint64 pid) {
    auto *tree=d->findChild<QTreeWidget*>("memoryReliefApplications");check(tree,"application table");bool found=false;
    for(int i=0;i<tree->topLevelItemCount();++i) {auto *row=tree->topLevelItem(i);bool match=false;for(const auto &v:row->data(0,Qt::UserRole+1).toJsonArray())match|=v.toObject().value("pid").toDouble()==pid;row->setCheckState(0,match?Qt::Checked:Qt::Unchecked);found|=match;}
    check(found,"selected owned sibling in real sampler table");
}
void confirmNext(const QString &objectName,bool accept,int *seen) {
    auto *timer=new QTimer(qApp);timer->setInterval(20);QObject::connect(timer,&QTimer::timeout,qApp,[=] {
        auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(!box||box->objectName()!=objectName)return;
        check(box->defaultButton()==box->button(QMessageBox::Cancel),"confirmation defaults to Cancel");
        check(box->textFormat()==Qt::PlainText,"confirmation renders application names as plain text");++*seen;
        if(objectName=="confirmMemoryReliefApplications")check(box->text().contains("2 个已确认窗口"),"confirmation contains exactly two inspected target windows");
        if(accept) {QAbstractButton *button=nullptr;for(auto *b:box->buttons())if(box->buttonRole(b)==QMessageBox::AcceptRole)button=b;check(button,"explicit confirmation accept action");button->click();}
        else box->button(QMessageBox::Cancel)->click();timer->stop();timer->deleteLater();
    });timer->start();
}
}
int main(int argc,char **argv) {
    QTemporaryDir isolated;check(isolated.isValid(),"isolated configuration");
    qputenv("XDG_CONFIG_HOME",(isolated.path()+"/config").toUtf8());qputenv("XDG_DATA_HOME",(isolated.path()+"/share").toUtf8());qputenv("DEVSTATS_DATA_DIR",(isolated.path()+"/data").toUtf8());
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);qRegisterMetaType<QJsonArray>();qRegisterMetaType<QJsonObject>();
    const qint64 pid=qEnvironmentVariableIntValue("DEVSTATS_RELIEF_FIXTURE_PID");check(pid>1,"explicit owned native sibling PID required");
    QJsonObject finalApplications;int confirms=0;
    {
        AppController controller;controller.start(false);controller.showModule("memory");
        check(waitFor([&]{return !selectedProcess(controller.latestSnapshot(),pid).isEmpty();}),"actual sampler sees owned sibling");
        auto *entry=controller.window()->findChild<QPushButton*>("memoryReliefFromMemory");check(entry,"memory page entry");entry->click();check(waitFor([]{return dialog()&&dialog()->isVisible();}),"memory page opens controller dialog");
        dialog()->close();QTest::qWait(50);controller.showModule("diagnostics");
        auto *diagnosticEntry=controller.window()->findChild<QPushButton*>("memoryReliefFromDiagnostics");
        check(diagnosticEntry,"diagnostics page entry");diagnosticEntry->click();
        check(waitFor([]{return dialog()&&dialog()->isVisible();}),"diagnostics bridge opens dialog");dialog()->close();QTest::qWait(50);
        QMenu *menu=nullptr;for(auto *w:QApplication::topLevelWidgets())if(auto *m=qobject_cast<QMenu*>(w))for(auto *a:m->actions())if(a->text()==QStringLiteral("内存减压…")){menu=m;a->trigger();}
        check(menu,"shared native tray menu has a relief action");check(waitFor([]{return dialog()&&dialog()->isVisible();}),"tray action opens controller dialog");
        auto *d=dialog();auto *trim=d->findChild<QPushButton*>("trimMemoryReliefSelf");auto *close=d->findChild<QPushButton*>("closeMemoryReliefApplications");
        selectPID(d,pid);check(close->isEnabled(),"close enabled for selected fresh owned application");confirmNext("confirmMemoryReliefApplications",false,&confirms);close->click();
        check(waitFor([&]{return d->findChild<QLabel*>("memoryReliefOutcome")->text().contains("已取消正常关窗")&&trim->isEnabled();}),"application confirmation Cancel unlocks busy and reports no message sent");
        d->setBusy(true);emit d->closeApplicationsRequested(QJsonArray{QJsonObject{{"pid",99999999},{"startTicks",1},{"executablePath","/does/not/exist"}}},{"stale fixture"});
        check(waitFor([&]{return trim->isEnabled()&&d->findChild<QLabel*>("memoryReliefOutcome")->text().contains("已变化");}),"worker identity refusal unlocks dialog busy");
        selectPID(d,pid);check(waitFor([&]{return close->isEnabled();}),"selection remains usable after refusal");confirmNext("confirmMemoryReliefApplications",true,&confirms);close->click();
        check(waitFor([&]{return d->findChild<QLabel*>("memoryReliefOutcome")->text().contains("正常关闭请求")||controller.latestSnapshot().value("memoryReliefBusy").toBool();}),"accepted normal close enters observation");
        check(waitFor([&]{finalApplications=controller.latestSnapshot().value("memoryReliefOutcome").toObject();return finalApplications.value("action")=="applications"&&finalApplications.value("requestedCount").toInt()==2&&finalApplications.value("processObservationFresh").toBool();},20000),"real worker completes post-close fresh process observation");
        check(finalApplications.value("beforeAvailableBytes").isDouble()&&finalApplications.value("afterAvailableBytes").isDouble(),"before/after system available memory measured");
        check(finalApplications.value("remainingKnownRssBytes").toDouble()>0&&finalApplications.value("missingProcessCount").toInt()==0,"save-prompt fixture remains observable with RSS; no false exit claim");check(trim->isEnabled(),"accepted application operation releases busy");
        controller.stop();
    }
    check(confirms==2,"two actual default-Cancel controller confirmations exercised");
    QJsonObject evidence{{"result","PASS"},{"assertions",assertions},{"ordinaryUid",double(::getuid())},{"applicationOutcome",finalApplications},{"confirmations",confirms}};
    const QString output=qEnvironmentVariable("DEVSTATS_RELIEF_TEST_RESULT");if(!output.isEmpty()){QFile file(output);check(file.open(QIODevice::WriteOnly),"open evidence");evidence["assertions"]=assertions;file.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));}
    std::printf("PASS relief controller: %d assertions; real sampler, memory/diagnostics/tray entry, default-Cancel, refusal/cancel/complete busy, native two-window WM_DELETE observation\n",assertions);
}
