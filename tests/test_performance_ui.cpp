#include "../src/controller.h"
#include "../src/monitor_window.h"
#include "../src/charts.h"
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QImage>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest/QTest>
#include <cstdio>
#include <cstdlib>

namespace {
int checks=0;
void check(bool value,const char *message){++checks;if(value)return;std::fprintf(stderr,"FAIL performance UI: %s\n",message);std::exit(1);}
QJsonObject snapshot(QString name="Alpha",int start=10){
    QJsonObject p{{"pid",123},{"startTicks",start},{"name",name},{"executablePath","/fixture/app"},{"cpuPercent",12.5},{"memoryBytes",1048576}};
    return {{"timestamp",1770000000.},{"processesFresh",true},{"processSampleAgeSeconds",0.},
        {"cpu",QJsonObject{{"usage",.25},{"user",.15},{"system",.1},{"coreCount",2},{"perCore",QJsonArray{.2,.3}}}},
        {"memory",QJsonObject{{"fraction",.5},{"totalBytes",2147483648.},{"usedBytes",1073741824.},{"availableBytes",1073741824.}}},
        {"processes",QJsonArray{p}},{"groups",QJsonArray{}}};
}
void drain(){QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QApplication::processEvents();}
}

int main(int argc,char **argv){
    QTemporaryDir root;check(root.isValid(),"isolated root");
    qputenv("XDG_CONFIG_HOME",(root.path()+"/config").toUtf8());qputenv("XDG_DATA_HOME",(root.path()+"/share").toUtf8());qputenv("DEVSTATS_DATA_DIR",(root.path()+"/data").toUtf8());
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);qRegisterMetaType<QJsonArray>();qRegisterMetaType<QJsonObject>();
    {
        MonitorWindow window;auto value=snapshot();window.setSnapshot(value);window.show();drain();
        check(window.property("appearanceMode").toInt()==1&&window.palette().color(QPalette::Window).lightness()<128,"default dark palette retained without redundant theme repolish");
        check(window.grab().toImage().pixelColor(3,3).lightness()<128,"default dark style actually paints dark");
        for (int appearance : {1,2,1}) {
            window.setAppearance(appearance);drain();bool correct=true;
            for (auto *widget : window.findChildren<QWidget*>()) if (dynamic_cast<HistoryChart*>(widget)) {
                correct &= (widget->palette().color(QPalette::Window).lightness()<128)==(appearance==1);
                correct &= (widget->palette().color(QPalette::WindowText).lightness()>128)==(appearance==1);
            }
            check(correct,"all custom histories use explicit default/dark/light palettes including pressure charts");
        }
        auto *cpu=window.findChild<QTreeWidget*>("cpuProcesses"),*memory=window.findChild<QTreeWidget*>("memoryProcesses");
        check(cpu&&memory,"all module controls retained");check(cpu->topLevelItemCount()==0&&memory->topLevelItemCount()==0,"hidden process pages do no table work");
        window.setSelectedModule("cpu");check(cpu->topLevelItemCount()==1&&cpu->topLevelItem(0)->text(0)=="Alpha","switch renders cached latest CPU snapshot");
        auto *identity=cpu->topLevelItem(0);value["battery"]=QJsonObject{{"state","charging"}};window.setSnapshot(value);
        check(cpu->topLevelItem(0)==identity,"unrelated snapshot change preserves CPU rows");
        value=snapshot("Renamed");window.setSnapshot(value);check(cpu->topLevelItem(0)==identity&&identity->text(0)=="Renamed","same timestamp with changed name updates reused row");
        value=snapshot("Replacement",11);window.setSnapshot(value);check(cpu->topLevelItem(0)->data(0,Qt::UserRole).toString()=="123:11","PID reuse gets distinct identity row");
        window.setSelectedModule("memory");check(memory->topLevelItem(0)->text(0)=="Replacement","memory page uses latest name and identity");
        auto later=snapshot("Newest",12);window.setSnapshot(later);check(cpu->topLevelItem(0)->text(0)=="Replacement","CPU stays untouched while memory visible");
        window.setSelectedModule("cpu");check(cpu->topLevelItem(0)->text(0)=="Newest","returning page catches latest hidden changes");
        window.setSelectedModule("overview");QJsonObject histories{{"cpu",QJsonArray{QJsonObject{{"timestamp",1770000000.},{"value",.4}}}}};window.setHistories(HistoryFeed(histories,1769999900,1770000000));
        auto charts=window.findChildren<HistoryChart*>("cpuHistory");check(charts.size()==1,"CPU page chart exists");
        window.setSelectedModule("cpu");check(charts.first()->accessibleDescription().contains("40%"),"hidden chart catches cached history on switch");
        for (const auto &module : {"overview", "cpu", "memory", "disk", "network", "battery", "diagnostics", "automation", "projects"}) {
            window.setSelectedModule(module); drain(); check(window.selectedModule() == module, "all nine modules remain navigable with current-page rendering");
        }
        window.setSelectedModule("ollama");check(window.selectedModule()=="projects","removed modules are not navigable");
        window.setSelectedModule("builds");check(window.selectedModule()=="projects","removed modules are not navigable");
        window.setSelectedModule("projects");window.setSelectedModule("cpu");window.setPreferences(false,0,{"/fixture/manual-project"},{},true);window.setSelectedModule("projects");
        check(window.findChild<QTreeWidget*>("projectTree")->topLevelItem(0)->text(0)=="manual-project","manual project change while hidden invalidates its cached page input");
    }
    {
        AppController controller;controller.start(false);QTest::qWait(250);controller.showModule("memory");drain();QPointer<MonitorWindow> first=controller.window();
        check(first&&first->isVisible(),"controller opens real window");auto *ttl=controller.findChild<QTimer*>("windowReleaseTimer");check(ttl&&ttl->interval()==30000,"cache has explicit 30 second bound");
        first->close();drain();check(first&&!first->isVisible()&&ttl->isActive(),"close hides and starts bounded cache");controller.showModule("cpu");check(controller.window()==first&&first->isVisible()&&!ttl->isActive(),"reopen within TTL reuses same window");
        first->showMinimized();drain();ttl->stop();QMetaObject::invokeMethod(ttl,"timeout",Qt::DirectConnection);check(first,"minimized window is retained");
        first->close();drain();check(ttl->isActive(),"close after minimized TTL restarts bounded cache even when presentation was already false");controller.showModule("memory");
        first->showMinimized();drain();ttl->stop();QMetaObject::invokeMethod(ttl,"timeout",Qt::DirectConnection);first->hide();drain();check(ttl->isActive(),"hide after minimized TTL restarts bounded cache");first->showNormal();drain();
        controller.showModule("memory");first->findChild<QPushButton*>("memoryReliefFromMemory")->click();drain();
        QDialog *relief=nullptr;for(auto *dialog:first->findChildren<QDialog*>())if(dialog->isVisible())relief=dialog;check(relief,"real memory relief interaction opened");
        QPointer<QMessageBox> confirmation=new QMessageBox(QMessageBox::Warning,"Fixture confirmation","No action is performed",QMessageBox::Cancel,relief);confirmation->show();first->hide();drain();
        check(relief->isVisible()&&confirmation->isVisible(),"owned top-level interactions remain visible with hidden owner");
        QTest::qWait(31000);check(first&&controller.window()==first&&relief->isVisible()&&confirmation->isVisible(),"real TTL expiry never destroys visible relief or nested confirmation");
        confirmation->hide();delete confirmation.data();relief->hide();drain();QMetaObject::invokeMethod(ttl,"timeout",Qt::DirectConnection);drain();check(!first&&!controller.window(),"after interactions finish hidden cache can be released");
        controller.showModule("network");QPointer<MonitorWindow> rebuilt=controller.window();check(rebuilt&&rebuilt->isVisible(),"TTL release permits clean rebuild");rebuilt->close();controller.stop();check(!ttl->isActive(),"quit stops release timer");
    }
    std::printf("PASS performance UI: %d checks; visible-page data, same-timestamp changes, row identity, cached history and real TTL/dialog lifecycle\n",checks);
}
