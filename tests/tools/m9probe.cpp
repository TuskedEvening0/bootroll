// M9 probe: runtime差异验证 (runs inside distro containers).
// 1. candidateFonts(): what the font discovery resolves in THIS environment
//    (fc-match/fc-list present? CJK fonts installed? static fallback?)
// 2. confirmDialog(): must fail-closed (return false) when zenity is absent.
// 3. restartElevated(): must fail-closed (return false) when pkexec is absent.
// Link: g++ -std=c++20 -Isrc probe.cpp src/platform/linux/*.cpp
//       src/core/disk/PartitionTable.cpp -lpthread
#include "platform/IPlatform.h"

#include <cstdio>
#include <memory>

using namespace bootroll;

int main()
{
    std::unique_ptr<IPlatform> plat = createPlatform();
    IPlatform* p = plat.get();

    printf("== candidateFonts ==\n");
    for (const FontCandidate& fc : p->candidateFonts()) {
        printf("  %s [face %d]\n", fc.path.c_str(), fc.faceIndex);
    }

    printf("== fail-closed checks ==\n");
    const bool dlg = p->confirmDialog("probe", "probe");
    printf("  confirmDialog: %s (expect false without zenity)\n",
           dlg ? "TRUE <-- FAIL-CLOSED BROKEN" : "false (ok)");
    const bool elev = p->restartElevated("");
    printf("  restartElevated: %s (expect false without pkexec)\n",
           elev ? "TRUE <-- UNEXPECTED" : "false (ok)");

    int bad = 0;
    if (dlg) bad |= 1;
    if (elev) bad |= 2;
    printf("PROBE_RESULT bad=%d\n", bad);
    return bad;
}
