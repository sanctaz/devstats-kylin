#include "../src/monitor_window.h"
#include "../src/controller.h"
#include "../src/process_network_sampler.h"
#include <QApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>
namespace {
int checks=0;
void check(bool value,const char *message){++checks;if(value)return;std::fprintf(stderr,"FAIL network cache: %s\n",message);std::exit(1);}
void settle(){QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QApplication::processEvents();}
QJsonObject ready(double rate=4096.){
    QJsonObject child{{"pid",123},{"startTicks",10},{"name","Alpha helper"},{"connectionCount",1},{"downloadBytesPerSecond",rate},{"uploadBytesPerSecond",1024.}};
    QJsonObject app{{"identity","app-alpha"},{"name","Alpha"},{"processCount",1},{"connectionCount",1},{"downloadBytesPerSecond",rate},{"uploadBytesPerSecond",1024.},{"processes",QJsonArray{child}}};
    return {{"state","ready"},{"sampledAt",1770000000.},{"observing",true},{"applications",QJsonArray{app}}};
}
QJsonObject published(const QJsonObject &current,const QJsonObject &previous={}){
    QJsonObject result{{"timestamp",1770000000.},{"networkApplications",current},{"network",QJsonObject{}}};
    if(!previous.isEmpty())result["networkApplicationsPrevious"]=previous;return result;
}
}
class MonitorWorkerTest {
public:
    static void sleepTransitions(const QJsonObject &measured) {
        QTemporaryDir root;check(root.isValid(),"worker isolated root");
        const auto oldProc=qgetenv("DEVSTATS_PROC_ROOT"),oldSys=qgetenv("DEVSTATS_SYS_ROOT");
        const bool hadProc=qEnvironmentVariableIsSet("DEVSTATS_PROC_ROOT"),hadSys=qEnvironmentVariableIsSet("DEVSTATS_SYS_ROOT");
        qputenv("DEVSTATS_PROC_ROOT",(root.path()+"/proc").toUtf8());qputenv("DEVSTATS_SYS_ROOT",(root.path()+"/sys").toUtf8());
        MonitorWorker worker(root.path()+"/data",{});QJsonObject seen;
        QObject::connect(&worker,&MonitorWorker::snapshotReady,[&](const QJsonObject &value){seen=value;});worker.start();
        worker.cachedNetwork_=measured;worker.updateNetworkPresentation();worker.networkObservation_=true;
        check(!worker.latest_.contains("networkApplicationsPrevious"),"live worker publication omits duplicate previous payload");
        worker.setSleeping(true);check(seen.value("networkApplications").toObject().value("state").toString()=="inactive"&&seen.value("networkApplicationsPrevious").toObject()==measured,"actual sleep inactive emission preserves dated measured cache");
        worker.setSleeping(false);check(seen.value("networkApplicationsPrevious").toObject()==measured,"actual wake retains cache when fake proc cannot observe TCP");
        worker.cachedNetwork_=QJsonObject{{"state","starting"},{"observing",true},{"applications",QJsonArray{}}};worker.updateNetworkPresentation();
        check(worker.latest_.value("networkApplicationsPrevious").toObject()==measured,"wake starting publication retains exactly one last measurement");
        auto fresh=measured;fresh["sampledAt"]=1770000002.;worker.cachedNetwork_=fresh;worker.updateNetworkPresentation();
        check(!worker.latest_.contains("networkApplicationsPrevious")&&worker.lastMeasuredNetwork_==fresh,"fresh wake measurement takes over and drops previous publication");worker.stop();
        if(hadProc)qputenv("DEVSTATS_PROC_ROOT",oldProc);else qunsetenv("DEVSTATS_PROC_ROOT");if(hadSys)qputenv("DEVSTATS_SYS_ROOT",oldSys);else qunsetenv("DEVSTATS_SYS_ROOT");
    }
};

int main(int argc,char **argv){
    QApplication application(argc,argv);application.setQuitOnLastWindowClosed(false);
    auto measured=ready();MonitorWorkerTest::sleepTransitions(measured);QJsonObject cached=ProcessNetworkSampler::retainedObservation(measured,{});check(cached==measured,"ready bare observation retained");
    check(ProcessNetworkSampler::retainedObservation({{"state","starting"}},{}).isEmpty(),"unknown startup is never a measured cache");
    const int bytes=QJsonDocument(cached).toJson(QJsonDocument::Compact).size();
    for(int i=0;i<60;i++){
        for(const auto &state:{"inactive","starting","unavailable"})cached=ProcessNetworkSampler::retainedObservation({{"state",state},{"previous",cached}},cached);
        auto fresh=ready();fresh["previous"]=cached;fresh["networkApplicationsPrevious"]=cached;cached=ProcessNetworkSampler::retainedObservation(fresh,cached);
        check(!cached.contains("previous")&&!cached.contains("networkApplicationsPrevious")&&QJsonDocument(cached).toJson(QJsonDocument::Compact).size()==bytes,"many hide/start/ready cycles retain exactly one constant-size observation");
    }
    auto unknown=ready();unknown["state"]="partial";auto apps=unknown["applications"].toArray();auto app=apps[0].toObject();app["downloadBytesPerSecond"]=QJsonValue(QJsonValue::Null);app["uploadBytesPerSecond"]=QJsonValue(QJsonValue::Null);apps[0]=app;unknown["applications"]=apps;
    check(ProcessNetworkSampler::retainedObservation(unknown,cached)==cached,"partial without confirmed rates cannot replace cache");
    auto partial=ready(0.);partial["state"]="partial";check(ProcessNetworkSampler::retainedObservation(partial,cached)==partial,"confirmed partial zero is a valid measurement");
    auto *window=new MonitorWindow;QPointer<MonitorWindow> owner=window;window->setSelectedModule("network");window->setSnapshot(published(measured));window->show();settle();
    auto *tree=window->findChild<QTreeWidget*>("networkApplications");auto *status=window->findChild<QLabel*>("networkApplicationStatus");
    check(tree->topLevelItemCount()==1,"live result renders application");auto *row=tree->topLevelItem(0);auto *child=row->child(0);row->setExpanded(true);tree->setCurrentItem(child);
    auto changed=ready(8192.);window->setSnapshot(published(changed));settle();
    check(tree->topLevelItem(0)==row&&row->child(0)==child,"rate updates reuse application and process objects");check(row->isExpanded()&&tree->currentItem()==child,"reuse preserves expanded and selected child");
    for(const auto &state:{"inactive","starting","unavailable"}){
        window->setSnapshot(published({{"state",state},{"applications",QJsonArray{}}},cached));settle();
        check(tree->topLevelItemCount()==1&&status->text().contains(QStringLiteral("上次采样"))&&status->text().contains("2026-")&&status->text().contains(QStringLiteral("非实时")),"same-window reopen/start/unavailable clearly labels previous dated rates");
        if(QString(state)=="starting")check(status->text().contains(QStringLiteral("正在更新")),"startup explicitly updates historical result");
        if(QString(state)=="unavailable")check(status->text().contains(QStringLiteral("当前不可用")),"unavailable cannot masquerade as live data");
    }
    window->close();settle();check(!owner,"old window released");window=new MonitorWindow;window->setSelectedModule("network");window->setSnapshot(published({{"state","starting"}},cached));window->show();settle();
    tree=window->findChild<QTreeWidget*>("networkApplications");status=window->findChild<QLabel*>("networkApplicationStatus");check(tree->topLevelItemCount()==1&&status->text().contains(QStringLiteral("上次采样")),"recreated window restores worker-level previous observation");
    auto empty=ready();empty["applications"]=QJsonArray{};cached=ProcessNetworkSampler::retainedObservation(empty,cached);window->setSnapshot(published(empty));settle();check(tree->topLevelItemCount()==0&&!status->text().contains(QStringLiteral("非实时")),"legitimate fresh ready empty clears historical ranking");
    window->setSnapshot(published({{"state","starting"}},cached));settle();check(tree->topLevelItemCount()==0,"empty measured cache cannot resurrect old rows");window->close();settle();
    std::printf("PASS network cache: %d checks; bounded bare cache, history/live labels, reused identity rows, same/recreated-window reopen and fresh empty takeover\n",checks);
}
