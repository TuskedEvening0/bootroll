#pragma once
// Linux implementation of IDiskAccess (sysfs discovery + raw /dev/sdX I/O).
//
// Two-phase contract (PLATFORM_SEAMS.md #1):
//   - discoverDisks() reads /sys/block only, never opens a device node.
//   - fillDiskDetails() opens the device, fills details, closes; it runs on
//     its own worker thread per disk and may block indefinitely on a sick
//     device without stalling the others.
// Disk numbers are assigned once per device name and stay stable for the
// lifetime of this object (hotplug-safe across repeated refreshes). The only
// mutable state is that mutex-guarded name<->number string map - no handles
// are ever cached, all I/O opens/reads/closes self-contained.
#include <map>
#include <mutex>

#include "core/disk/IDiskAccess.h"

namespace bootroll {

class DiskAccessLinux : public IDiskAccess {
public:
    std::vector<DiskInfo> discoverDisks() override;
    void fillDiskDetails(DiskInfo& disk) override;
    void readSectors(uint32_t diskNumber, uint64_t byteOffset,
                     void* buffer, size_t bytes) override;
    void writeSectors(uint32_t diskNumber, uint64_t byteOffset,
                      const void* buffer, size_t bytes) override;
    void flush(uint32_t diskNumber) override;

private:
    // Dev node path for a disk number; assigns one lazily for unknown disks.
    std::string devNodeForNumber(uint32_t diskNumber);

    std::mutex m_mutex;
    std::map<uint32_t, std::string> m_numberToName;  // number -> /sys/block name
    std::map<std::string, uint32_t> m_nameToNumber;
    uint32_t m_nextNumber = 0;
};

} // namespace bootroll
