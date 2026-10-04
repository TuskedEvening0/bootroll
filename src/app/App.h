#pragma once
// App orchestration: fonts/theme/i18n/settings + page routing. Portable ImGui code.
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/bcd/BcdStore.h"
#include "core/disk/DiskInfo.h"
#include "core/disk/IDiskAccess.h"
#include "platform/IPlatform.h"

struct ImFont; // ImGui forward declaration (imgui.h defines ::ImFont)

namespace bootroll {

class App {
public:
    enum class Page {
        Main,
        Bcd,       // M1/M2
        Mbr,       // M3
        Pbr,       // M3
        Grub4dos,  // M4
        Partition, // M4
        Sector,    // M5
        Uefi,      // M7
        About,
    };

    // platform must outlive the App.
    bool init(IPlatform* platform, const char* version);
    void shutdown();
    void drawFrame();

    void onResize(int width, int height);
    void requestExit() { m_exitRequested = true; }
    bool exitRequested() const { return m_exitRequested; }

    // --- UI helpers shared with screens ---
    void gotoPage(Page p) { m_page = p; }
    Page page() const { return m_page; }

    const std::vector<DiskInfo>& disks() const { return m_diskList; }
    int selectedDisk() const { return m_selectedDisk; }
    void selectDisk(int index);
    // Background disk enumeration in two phases (fast stubs + one detail
    // thread per disk), so a misbehaving device never blocks startup, the UI
    // thread, or the other disks.
    void refreshDisks();   // kick off the workers (no-op while one is in flight)
    void pumpDiskEnum();   // UI thread: incrementally pick up finished probes
    const DiskInfo* currentDisk() const;

    // BCD page state (M1). Owned here; screens are stateless.
    struct BcdEditState {
        BcdStore store;
        std::string path;   // current file ("" for a fresh store)
        std::string error;  // last operation error, "" when none
        bool dirty = false;

        // Professional mode selection (M2).
        std::string profObjGuid; // "" = first object
        uint32_t profElemId = 0; // 0 = none selected
    };
    BcdEditState& bcdEdit() { return m_bcdEdit; }

    IPlatform* platform() const { return m_platform; }
    IDiskAccess* diskAccess() const { return m_diskAccess.get(); }

    // Log severity tags in bootroll.log: [II] info / [WW] warning / [EE] error.
    enum class LogLevel { Info, Warn, Error };
    void log(const std::string& line, LogLevel level = LogLevel::Info);

    // Fixed-pitch font for hex grids (sector editor); may equal the main font.
    ImFont* hexFont() const { return m_hexFont; }

    // Current DPI scale (96 dpi = 1.0x). setDpiScale rebuilds style + fonts.
    float dpiScale() const { return m_dpiScale; }
    void setDpiScale(float scale);

private:
    void applyTheme(float scale);
    void loadFonts(float scale);
    void drawTopBar();
    void drawPage();
    void drawPlaceholder(Page p, const char* title, const char* milestone);

    IPlatform* m_platform = nullptr;
    // shared_ptr: the enumeration worker can outlive the App (stalled device);
    // the worker keeps its own copies of these alive.
    std::shared_ptr<IDiskAccess> m_diskAccess;
    std::vector<DiskInfo> m_diskList;
    int m_selectedDisk = -1;

    // Shared state for the two-phase disk enumeration workers. Phase 1
    // discovers disk stubs (fast, no media I/O); phase 2 runs one thread per
    // disk for the detail queries, so a single misbehaving device stalls only
    // its own probe. Results are harvested incrementally by pumpDiskEnum().
    struct DiskEnumState {
        struct ProbeResult {
            DiskInfo disk;
            std::string error; // non-empty: detail probe failed (stub still harvested)
            double elapsedMs = 0.0;
            bool stub = false;  // discovery placeholder, details still coming
            bool first = false; // first stub of a refresh: replaces the whole list
        };
        struct Probe {
            uint32_t number = 0;
            std::chrono::steady_clock::time_point started{};
            bool finished = false;
            bool warned = false; // stall warning already logged
        };

        std::mutex mutex;
        uint64_t generation = 0;            // bumped per refresh; stale workers drop results
        std::vector<ProbeResult> completed; // filled by workers, drained by pumpDiskEnum()
        std::string error;                  // discovery-phase error
        std::vector<Probe> probes;          // one entry per disk being detailed
        bool discoveryDone = false;         // probes vector is final
        bool discoveryWarned = false;       // "still discovering" warning logged
        std::chrono::steady_clock::time_point startedAt{};
        bool running = false;               // enumeration in flight (cleared at summary)
    };
    std::shared_ptr<DiskEnumState> m_diskEnum = std::make_shared<DiskEnumState>();

    Page m_page = Page::Main;
    BcdEditState m_bcdEdit;
    bool m_exitRequested = false;
    std::string m_version;
    std::string m_fontUsed;
    ImFont* m_hexFont = nullptr;
    float m_dpiScale = 1.0f;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
    bool m_showDemo = false;
};

} // namespace bootroll
