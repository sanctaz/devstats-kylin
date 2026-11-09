#include "../src/monitor_window.h"
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QtTest/QTest>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
QJsonArray geometry;
void check(bool ok, const char *message) {
    ++checks; if (ok) return;
    std::fprintf(stderr, "FAIL network UI: %s\n", message); std::exit(1);
}
void settle() { QApplication::sendPostedEvents(); QApplication::processEvents(); }
const QJsonValue unknown(QJsonValue::Null);
QJsonObject process(int pid, const QString &name, QJsonValue down, QJsonValue up, bool shared = false) {
    return {{"pid", pid}, {"startTicks", pid + 10000}, {"name", name}, {"downloadBytesPerSecond", down},
        {"uploadBytesPerSecond", up}, {"connectionCount", 1}, {"shared", shared}};
}
QJsonObject app(const QString &id, const QString &name, QJsonValue down, QJsonValue up, const QJsonArray &processes, bool partial = false) {
    return {{"identity", id}, {"name", name}, {"downloadBytesPerSecond", down}, {"uploadBytesPerSecond", up},
        {"processCount", processes.size()}, {"connectionCount", 2}, {"partial", partial}, {"processes", processes}};
}
QJsonObject fixture(const QString &state = "partial", bool large = false) {
    QJsonArray apps{
        app("browser", QStringLiteral("浏览器 · 两个共享连接子进程"), 8.*1024*1024, 128.*1024,
            {process(910001,"Browser main",unknown,unknown,true),process(910002,"Browser helper",unknown,unknown,true)}),
        app("uploader", QStringLiteral("云盘同步 · Sync"), 64.*1024, 6.*1024*1024, {process(920001,"Sync",64.*1024,6.*1024*1024)}),
        app("idle", QStringLiteral("空闲应用"), 0., 0., {process(930001,"Idle",0.,0.)}),
        app("unknown", QStringLiteral("部分可观测应用"), unknown, unknown, {process(940001,"Unreadable",unknown,unknown)},true),
        app("long", QStringLiteral("长名称应用与浏览器扩展辅助进程_abcdefghijklmnopqrstuvwxyz_0123456789_abcdefghijklmnopqrstuvwxyz"), 512.*1024, 1024., {process(950001,"Long helper",512.*1024,1024.)},true)};
    if (large) {
        for (int i=0;i<54;++i) apps.append(app(QStringLiteral("filler-%1").arg(i),QStringLiteral("后台网络应用 %1").arg(i),i+1.,i+2.,{process(960000+i,"Worker",i+1.,i+2.)}));
        apps.append(app("upload-candidate","Upload only candidate",.125,16.*1024*1024,{process(970001,"Upload only worker",.125,16.*1024*1024)}));
    }
    const double now=QDateTime::currentMSecsSinceEpoch()/1000.;
    return {{"timestamp",now},{"version","0.7.10"},
        {"network",QJsonObject{{"interface","wlan0"},{"isUp",true},{"uploadBytesPerSecond",9.*1024*1024},{"downloadBytesPerSecond",16.*1024*1024}}},
        {"networkApplications",QJsonObject{{"state",state},{"sampledAt",now},{"monotonicSeconds",10.},{"intervalSeconds",2.},
            {"applications",state=="starting" || state=="unavailable" || state=="inactive" ? QJsonArray{}:apps},
            {"coverage",QJsonArray{QStringLiteral("仅统计当前用户可见 TCP；UDP / QUIC 不支持，短连接可能遗漏；部分进程不可读。")}},
            {"scannedProcessCount",42},{"unattributedConnectionCount",2},{"limited",false}}}};
}
QTreeWidgetItem *row(QTreeWidget *tree, const QString &name) {
    for(int i=0;i<tree->topLevelItemCount();++i) if(tree->topLevelItem(i)->text(0).contains(name)) return tree->topLevelItem(i);
    return nullptr;
}
int visibleRows(QTreeWidget *tree) {
    int rows=0; for(int i=0;i<tree->topLevelItemCount();++i) if(!tree->topLevelItem(i)->isHidden()) ++rows; return rows;
}
QTreeWidgetItem *firstVisible(QTreeWidget *tree) {
    for(int i=0;i<tree->topLevelItemCount();++i) if(!tree->topLevelItem(i)->isHidden()) return tree->topLevelItem(i); return nullptr;
}
void selectModule(MonitorWindow &window, int index) {
    auto *nav=window.findChild<QListWidget*>("navigation"); check(nav && nav->count()==12,"nine modules under three sections");
    QTest::mouseClick(nav->viewport(),Qt::LeftButton,Qt::NoModifier,nav->visualItemRect(nav->item(index)).center()); settle();
}
void interaction(MonitorWindow &window) {
    auto *tree=window.findChild<QTreeWidget*>("networkApplications");
    auto *sort=window.findChild<QComboBox*>("networkApplicationSort");
    auto *search=window.findChild<QLineEdit*>("networkApplicationSearch");
    auto *status=window.findChild<QLabel*>("networkApplicationStatus");
    auto *coverage=window.findChild<QLabel*>("networkApplicationCoverage");
    auto *toggle=window.findChild<QAbstractButton*>("networkApplicationCoverageToggle");
    check(tree && sort && search && status && coverage && toggle,"all application-network controls exposed by stable object names");
    check(tree->columnCount()==4 && tree->headerItem()->text(2).contains(QStringLiteral("上传")) && tree->headerItem()->text(3).contains(QStringLiteral("下载")),"table keeps upload before download");
    check(sort->currentData().toString()=="downloadBytesPerSecond","download ranking is default");
    window.setSnapshot(fixture()); settle();
    check(tree->topLevelItemCount()==5 && tree->topLevelItem(0)->text(0).contains(QStringLiteral("浏览器")),"default ranks known application downloads descending");
    auto *browser=row(tree,QStringLiteral("浏览器")), *idle=row(tree,QStringLiteral("空闲")), *missing=row(tree,QStringLiteral("部分可观测"));
    check(browser && browser->childCount()==2 && browser->text(1).contains("2"),"one application groups its two PIDs without duplicate top rows");
    check(browser->child(0)->text(1).contains("910001") && browser->child(1)->text(1).contains("910002"),"expanded rows expose individual PIDs");
    check(browser->child(0)->text(3).contains(QString::fromUtf8("—")),"shared sockets do not invent per-PID download rates");
    check(idle && idle->text(3).contains("0 B") && missing && missing->text(3).contains(QString::fromUtf8("—")),"measured zero remains distinct from missing rate");
    check(status->text().contains(QStringLiteral("部分")),"partial sample is explicit");
    QTest::mouseClick(toggle,Qt::LeftButton); settle();
    check(coverage->isVisible() && coverage->text().contains("TCP") && coverage->text().contains("UDP") && coverage->text().contains("QUIC"),"expandable coverage states TCP and UDP/QUIC limits");
    QTest::mouseClick(toggle,Qt::LeftButton); settle(); check(!coverage->isVisible(),"coverage details can collapse");
    sort->setFocus(); QTest::keyClick(sort,Qt::Key_End); settle();
    check(sort->currentData().toString()=="uploadBytesPerSecond" && tree->topLevelItem(0)->text(0).contains(QStringLiteral("云盘")),"keyboard sort interaction ranks uploads");
    search->setFocus(); QTest::keyClicks(search,"910002"); settle();
    check(visibleRows(tree)==1 && firstVisible(tree)->text(0).contains(QStringLiteral("浏览器")),"PID search retains owning application");
    QTest::keyClick(search,Qt::Key_A,Qt::ControlModifier); QTest::keyClicks(search,"Sync"); settle();
    check(visibleRows(tree)==1 && firstVisible(tree)->text(0).contains(QStringLiteral("云盘")),"application-name search retains owning application");
    search->clear(); sort->setCurrentIndex(sort->findData("downloadBytesPerSecond")); window.setSnapshot(fixture("partial",true)); settle();
    check(tree->topLevelItemCount()==60 && visibleRows(tree)==30,"sixty bounded download/upload candidates expose only thirty ranked rows");
    check(row(tree,"Upload only candidate") && row(tree,"Upload only candidate")->isHidden(),"low-download upload candidate is retained outside default visible thirty");
    browser=row(tree,QStringLiteral("浏览器")); browser->setExpanded(true); tree->setCurrentItem(browser->child(1)); browser->child(1)->setSelected(true);
    tree->verticalScrollBar()->setValue(2); const int scroll=tree->verticalScrollBar()->value();
    window.setSnapshot(fixture("partial",true)); settle(); browser=row(tree,QStringLiteral("浏览器"));
    check(browser->isExpanded() && browser->child(1)->isSelected(),"refresh preserves expanded application and selected PID identity");
    check(tree->verticalScrollBar()->value()==scroll,"refresh preserves table scroll position");
    sort->setFocus(); QTest::keyClick(sort,Qt::Key_End); settle();
    check(firstVisible(tree)->text(0)=="Upload only candidate" && visibleRows(tree)==30,"upload ordering considers candidates outside initial download thirty");
    sort->setCurrentIndex(sort->findData("downloadBytesPerSecond")); search->setFocus(); QTest::keyClicks(search,"Upload only"); settle();
    check(visibleRows(tree)==1 && firstVisible(tree)->text(0)=="Upload only candidate","search can find a candidate outside default visible thirty");
    auto *leader=window.findChild<QLabel*>("networkApplicationLeader");
    check(leader && leader->text().contains("Upload only candidate"),"leader follows matching search candidates");
    search->clear(); QTest::keyClicks(search,"does-not-exist"); settle();
    check(visibleRows(tree)==0 && leader->text().contains(QStringLiteral("没有匹配")),"unmatched search does not advertise an unrelated leader");
    search->clear(); settle();
    auto onlyUnknown=fixture(); auto unknownObservation=onlyUnknown.value("networkApplications").toObject();
    unknownObservation["applications"]=QJsonArray{app("unmeasured","Unmeasured",unknown,unknown,{process(980001,"Unmeasured",unknown,unknown)},true)};
    onlyUnknown["networkApplications"]=unknownObservation; window.setSnapshot(onlyUnknown); settle();
    check(leader->text().contains(QStringLiteral("暂未取得")) && !leader->text().contains("0 B"),"unknown-only observations do not imply zero activity");
    for(const QString &state:{QStringLiteral("starting"),QStringLiteral("ready"),QStringLiteral("unavailable"),QStringLiteral("inactive")}) {
        window.setSnapshot(fixture(state)); settle(); check(!status->text().isEmpty(),"every network observation state has explicit user feedback");
        if(state=="starting") check(status->text().contains(QStringLiteral("基线")),"fresh observation explains baseline collection");
        if(state=="unavailable") check(tree->topLevelItemCount()==0 && (status->text().contains(QStringLiteral("不支持")) || status->text().contains(QStringLiteral("不可用")) || status->text().contains(QStringLiteral("无法"))),"unsupported samples clear old rates and explain unavailability");
        if(state=="inactive") check(tree->topLevelItemCount()==0,"inactive observation never retains a stale ranking");
    }
}
void visibility(MonitorWindow &window) {
    QSignalSpy requests(&window,&MonitorWindow::networkObservationRequested);
    selectModule(window,1); check(!requests.isEmpty() && !requests.last()[0].toBool(),"leaving network page stops observation");
    selectModule(window,5); check(requests.last()[0].toBool(),"real navigation into visible network page requests observation");
    window.showMinimized(); settle(); check(!requests.last()[0].toBool(),"minimized network page stops observation");
    window.showNormal(); settle(); check(requests.last()[0].toBool(),"restored network page starts fresh observation");
    window.close(); settle(); check(!requests.last()[0].toBool(),"closing main window stops network observation");
    window.show(); settle(); check(requests.last()[0].toBool(),"reopened network page requests observation");
}
void layout(MonitorWindow &window, const QString &evidence) {
    window.setSnapshot(fixture("partial",true));
    auto *tree=window.findChild<QTreeWidget*>("networkApplications");
    auto *upload=window.findChild<QLabel*>("networkUpload"), *download=window.findChild<QLabel*>("networkDownload");
    check(upload && download && upload->mapTo(&window,QPoint()).y()<download->mapTo(&window,QPoint()).y(),"overall interface upload remains above download");
    for(int mode:{1,2}) for(QSize size:{QSize(1040,820),QSize(800,620)}) {
        window.setAppearance(mode); window.resize(size); settle();
        check(window.size()==size,"window supports both requested logical dimensions");
        auto *page=window.findChild<QWidget*>("networkPage"); auto *area=qobject_cast<QScrollArea*>(page->parentWidget()->parentWidget());
        check(area && area->horizontalScrollBar()->maximum()==0,"network cards fit page width without horizontal scrolling");
        check(tree->horizontalScrollBar()->maximum()==0 && tree->columnWidth(2)>=75 && tree->columnWidth(3)>=75,"both rate columns remain readable without horizontal clipping");
        for(int column:{2,3}) {
            int required=0; for(int i=0;i<tree->topLevelItemCount();++i) required=qMax(required,QFontMetrics(tree->font()).horizontalAdvance(tree->topLevelItem(i)->text(column))+12);
            check(tree->columnWidth(column)>=required,"complete measured rate and unit fit their columns, including partial lower-bound prefix");
        }
        check(tree->header()->sectionPosition(2)+tree->columnWidth(2)<=tree->viewport()->width()+2 && tree->header()->sectionPosition(3)+tree->columnWidth(3)<=tree->viewport()->width()+2,"upload and download columns are within viewport");
        auto *longRow=row(tree,QStringLiteral("长名称")); check(longRow && longRow->toolTip(0).contains(QStringLiteral("长名称")),"long application name remains available in tooltip");
        geometry.append(QJsonObject{{"mode",mode},{"width",size.width()},{"height",size.height()},{"dpr",window.devicePixelRatioF()},
            {"pageWidth",page->width()},{"treeViewportWidth",tree->viewport()->width()},{"pageHorizontalMaximum",area->horizontalScrollBar()->maximum()},{"treeHorizontalMaximum",tree->horizontalScrollBar()->maximum()}});
        if(!evidence.isEmpty()) {
            area->ensureWidgetVisible(tree,0,12); settle();
            check(window.grab().save(evidence+QStringLiteral("/network-%1-%2x%3-dpr%4.png").arg(mode).arg(size.width()).arg(size.height()).arg(qRound(window.devicePixelRatioF()))),"save bounded visual evidence");
        }
    }
}
}
int main(int argc,char **argv) {
    QApplication application(argc,argv); MonitorWindow window; window.setSelectedModule("network"); window.show(); settle();
    const QString evidence=qEnvironmentVariable("DEVSTATS_TEST_EVIDENCE"); if(!evidence.isEmpty()) check(QDir().mkpath(evidence),"create evidence directory");
    interaction(window); visibility(window); layout(window,evidence);
    if(!evidence.isEmpty()) { QFile f(evidence+"/network-ui-result.json"); check(f.open(QIODevice::WriteOnly),"open evidence JSON");
        f.write(QJsonDocument(QJsonObject{{"result","PASS"},{"checks",checks},{"geometry",geometry}}).toJson()); }
    std::printf("PASS network UI: %d checks; ranking, shared PID details, unknown/zero, sort/search, lifecycle, refresh retention and responsive themes\n",checks);
}
