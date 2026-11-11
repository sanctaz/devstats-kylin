#pragma once
#include <QString>

// Interface text is written in Chinese where it is used. In English it is looked up
// in assets/i18n-en.json, which scripts/check-i18n.py keeps complete: every text
// passed to T(), Tc() or Tn() has an entry there, and no entry is unused.
QString T(const char *chinese);
// The same Chinese words can need different English: "持续" heads a column of
// durations and also says that a problem lasted. `context` names the use; the catalog
// holds the text under "@context@text". Chinese shows the text unchanged.
QString Tc(const char *context, const char *chinese);
// Text about a count. Its English entry may hold two forms as "one|many", and the
// form for `count` is returned. A %1 in the text is left for the caller's arg().
QString Tn(const char *chinese, qint64 count);

namespace DevStatsLanguage {
// What the user chose in the settings. The numbers are what is stored.
enum Choice { FollowSystem = 0, Chinese = 1, English = 2 };
// Makes `choice` the language of every text looked up from now on. Text that is
// already on screen stays as it is, so the caller rebuilds what it shows.
void apply(int choice);
bool isEnglish();
// Whether the desktop session is in Chinese. Any other language gets English.
bool systemIsChinese();
// "zh-CN" or "en", for the language attribute of exported pages.
QString tag();
}
