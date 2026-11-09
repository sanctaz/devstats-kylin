#pragma once

#include <QString>

// Per-user data directory: DEVSTATS_DATA_DIR, otherwise $XDG_DATA_HOME/devstats.
QString devStatsDataRoot();
// Call after Qt application construction: the private runtime's variables must
// not leak into terminals or desktop applications started from DevStats.
void restoreDevStatsLaunchEnvironment();
QString devStatsLauncherPath();
