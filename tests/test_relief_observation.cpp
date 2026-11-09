#include "../src/relief_observation.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
namespace {
int checks=0;
void check(bool ok,const char *message){++checks;if(!ok){std::fprintf(stderr,"FAIL observation: %s\n",message);std::exit(1);}}
QJsonObject process(int pid,int start,QJsonValue rss,const QString &exe="/fixture/app") {
    return {{"pid",pid},{"startTicks",start},{"executablePath",exe},{"memoryBytes",rss},{"commandLine","private --secret"}};
}
QJsonObject sample(double available,const QJsonArray &p) {
    return {{"timestamp",1234.},{"memory",QJsonObject{{"totalBytes",4096.},{"availableBytes",available}}},
        {"processes",p},{"processesFresh",true},{"processSampleAgeSeconds",0.}};
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const QJsonObject a=process(100,1000,100.),b=process(101,1100,200.);
    const QJsonArray ids{a,b};auto before=sample(2048,ids);
    const auto base=DevStatsRelief::baseline(before,ids);
    check(base.value("processes").toArray().size()==2,"only selected baseline captured");
    check(!QJsonDocument(base).toJson().contains("secret"),"baseline omits command lines");
    auto after=sample(1024,{process(100,1000,50.),process(101,9999,900.)});
    auto result=DevStatsRelief::outcome(base,after,ids,{{"message","close requested"}});
    check(result.value("availableDeltaBytes").toDouble()==-1024,"system availability can fall after request");
    check(result.value("selectedBeforeRssBytes").toDouble()==300,"baseline RSS sum");
    check(result.value("remainingKnownRssBytes").toDouble()==50,"only same full identity counted after");
    check(result.value("missingProcessCount").toInt()==1,"PID reuse does not count as retained application");
    check(!result.contains("freedBytes"),"no fabricated bytes-freed property");
    check(!result.value("measurementNote").toString().isEmpty(),"concurrency and shared RSS caveat explicit");
    after["processesFresh"]=false;
    check(DevStatsRelief::outcome(base,after,ids,{}).value("remainingKnownRssBytes").isNull(),"cached post sample not current target RSS");
    after["processesFresh"]=true;after["processSampleAgeSeconds"]=6.;
    check(DevStatsRelief::outcome(base,after,ids,{}).value("remainingKnownRssBytes").isNull(),"stale post sample unknown");
    auto unknown=sample(1024,{process(100,1000,QJsonValue::Null),process(101,1100,0.)});
    auto partial=DevStatsRelief::outcome(base,unknown,ids,{});
    check(partial.value("unknownProcessCount").toInt()==1,"unknown process metric counted");
    check(partial.value("remainingKnownRssBytes").toDouble(-1)==0,"measured zero distinguished from unknown");
    auto allGone=DevStatsRelief::outcome(base,sample(3000,{}),ids,{});
    check(allGone.value("missingProcessCount").toInt()==2,"absent readable members explicit");
    check(allGone.value("remainingKnownRssBytes").isNull(),"all missing is not proof of zero RSS");
    check(DevStatsRelief::outcome(base,{},ids,{}).value("afterAvailableBytes").isNull(),"awaiting post observation null");
    check(DevStatsRelief::outcome(base,{},ids,{}).value("availableDeltaBytes").isNull(),"unknown delta not zero");
    auto invalid=sample(5000,{});
    check(DevStatsRelief::outcome(base,invalid,{},{}).value("afterAvailableBytes").isNull(),"availability beyond total rejected");
    auto stale=base;stale["processSampleAgeSeconds"]=6.;
    check(DevStatsRelief::outcome(stale,sample(1024,ids),ids,{}).value("selectedBeforeRssBytes").isNull(),"stale before RSS unknown");
    auto duplicate=DevStatsRelief::outcome(base,sample(1024,ids),QJsonArray{a,a,b},{});
    check(duplicate.value("selectedBeforeRssBytes").toDouble()==300,"duplicate selection not double counted");
    auto changedExe=DevStatsRelief::outcome(base,sample(1024,{process(100,1000,900.,"/fixture/other"),b}),ids,{});
    check(changedExe.value("remainingKnownRssBytes").toDouble()==200,"changed executable excluded");
    check(changedExe.value("missingProcessCount").toInt()==1,"changed executable covered as unavailable original");
    auto noTargets=DevStatsRelief::outcome(base,sample(3072,{}),{},{{"action","models"}});
    check(noTargets.value("selectedBeforeRssBytes").isNull()&&noTargets.value("remainingKnownRssBytes").isNull(),"model actions do not fabricate target process RSS");
    check(noTargets.value("availableDeltaBytes").toDouble()==1024,"model action has observational system delta");
    std::printf("PASS relief observation: %d checks, signed system delta, unknown/stale/partial, exact identity, missing PID and no false freed bytes\n",checks);
}
