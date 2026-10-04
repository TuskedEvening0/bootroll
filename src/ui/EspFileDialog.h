#pragma once
// DiskGenius-style simple file browser for FAT volumes. Windows does not mount
// the ESP, so this dialog reads the raw partition through IDiskAccess and
// walks the file system with core/fat/FatVolume (read-only). Rendered as a
// stand-alone top-level ImGui window above the calling page.
#include <string>
#include <vector>

#include "core/fat/FatVolume.h"

namespace bootroll {

class App;

class EspFileDialog {
public:
    // Open the browser for app.disks()[diskIdx].partitions[partIdx].
    void open(App& app, int diskIdx, int partIdx);
    void close() { m_open = false; }
    bool isOpen() const { return m_open; }

    // Draw the dialog. Returns true when the user confirmed and *outPath holds
    // the picked file ('\'-separated, leading '\', volume-relative).
    bool draw(App& app, std::string* outPath);

private:
    struct Choice {
        int diskIdx = -1;
        int partIdx = -1;
        std::string label; // combo caption
    };

    void openVolume(App& app);
    void reload();
    bool ensureChoices(App& app);

    bool m_open = false;
    bool m_popupQueued = false;
    int m_diskIdx = -1;
    int m_partIdx = -1;
    std::vector<Choice> m_choices; // FAT-capable partitions across all disks
    FatVolume m_volume;
    bool m_volumeOk = false;
    std::string m_volumeError;
    std::string m_curDir; // "" = root
    std::vector<FatDirEntry> m_listing;
    int m_selected = -1; // index into m_listing
};

} // namespace bootroll
