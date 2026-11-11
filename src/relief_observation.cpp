#include "relief_observation.h"
#include "i18n.h"
#include <QSet>
#include <cmath>

namespace {
bool valid(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0; }
QJsonValue available(const QJsonObject &s) {
    const auto m=s.value("memory").toObject();
    const auto total=m.value("totalBytes"), a=m.value("availableBytes");
    return valid(total) && total.toDouble()>0 && valid(a) && a.toDouble()<=total.toDouble() ? a : QJsonValue(QJsonValue::Null);
}
bool same(const QJsonObject &a,const QJsonObject &b) {
    return valid(a.value("pid")) && valid(a.value("startTicks")) && a.value("pid")==b.value("pid") &&
        a.value("startTicks")==b.value("startTicks") && !a.value("executablePath").toString().isEmpty() &&
        a.value("executablePath")==b.value("executablePath");
}
QJsonArray selected(const QJsonObject &s,const QJsonArray &ids) {
    QJsonArray result;
    for(const auto &v:s.value("processes").toArray()) {
        const auto p=v.toObject();
        for(const auto &id:ids) if(same(p,id.toObject())) {
            result.append(QJsonObject{{"pid",p.value("pid")},{"startTicks",p.value("startTicks")},
                {"executablePath",p.value("executablePath")},{"memoryBytes",p.value("memoryBytes")}}); break;
        }
    }
    return result;
}
}
QJsonObject DevStatsRelief::baseline(const QJsonObject &snapshot,const QJsonArray &identities) {
    return {{"timestamp",snapshot.value("timestamp")},{"memory",snapshot.value("memory")},
        {"processes",selected(snapshot,identities)},{"processesFresh",snapshot.value("processesFresh")},
        {"processSampleAgeSeconds",snapshot.value("processSampleAgeSeconds")}};
}
QJsonObject DevStatsRelief::outcome(const QJsonObject &before,const QJsonObject &after,
                                 const QJsonArray &identities,QJsonObject operation) {
    const auto b=available(before), a=available(after);
    operation["beforeAvailableBytes"]=b; operation["afterAvailableBytes"]=a;
    operation["availableDeltaBytes"]=valid(a)&&valid(b)?QJsonValue(a.toDouble()-b.toDouble()):QJsonValue(QJsonValue::Null);
    operation["timestamp"]=after.value("timestamp");
    double beforeSum=0, afterSum=0; int beforeKnown=0, afterKnown=0, missing=0, unknown=0;
    QSet<QString> seen;
    for(const auto &value:identities) {
        const auto id=value.toObject();
        const QString key=QString::number(id.value("pid").toDouble(),'f',0)+':'+QString::number(id.value("startTicks").toDouble(),'f',0);
        if(seen.contains(key)) continue;
        seen.insert(key);
        bool found=false;
        for(const auto &v:before.value("processes").toArray()) { const auto p=v.toObject();
            if(same(p,id)&&valid(p.value("memoryBytes"))) { beforeSum+=p.value("memoryBytes").toDouble(); ++beforeKnown; break; }
        }
        for(const auto &v:after.value("processes").toArray()) { const auto p=v.toObject();
            if(!same(p,id)) continue;
            found=true;
            if(valid(p.value("memoryBytes"))) {afterSum+=p.value("memoryBytes").toDouble();++afterKnown;} else ++unknown;
            break;
        }
        if(!found)++missing;
    }
    const bool fresh=after.value("processesFresh").toBool() && valid(after.value("processSampleAgeSeconds")) && after.value("processSampleAgeSeconds").toDouble()<=5;
    const bool beforeRecent=valid(before.value("processSampleAgeSeconds")) && before.value("processSampleAgeSeconds").toDouble()<=5;
    operation["selectedBeforeRssBytes"]=beforeRecent&&!seen.isEmpty()&&beforeKnown==seen.size()&&std::isfinite(beforeSum)?QJsonValue(beforeSum):QJsonValue(QJsonValue::Null);
    operation["remainingKnownRssBytes"]=fresh&&afterKnown>0&&std::isfinite(afterSum)?QJsonValue(afterSum):QJsonValue(QJsonValue::Null);
    operation["beforeProcessSampleAgeSeconds"]=before.value("processSampleAgeSeconds");
    operation["missingProcessCount"]=missing;operation["unknownProcessCount"]=unknown;
    operation["processObservationFresh"]=fresh;
    operation["measurementNote"]=T("可用内存变化同时受其他活动影响；RSS含共享页，进程离开可读列表或采样未知都不能当成精确释放量。");
    return operation;
}
