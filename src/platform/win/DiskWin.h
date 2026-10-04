#pragma once
// Win32 implementation of IDiskAccess (\\.\PhysicalDriveN + IOCTLs).
#include "core/disk/IDiskAccess.h"

namespace bootroll {

class DiskWin : public IDiskAccess {
public:
    std::vector<DiskInfo> discoverDisks() override;
    void fillDiskDetails(DiskInfo& disk) override;
    void readSectors(uint32_t diskNumber, uint64_t byteOffset,
                     void* buffer, size_t bytes) override;
    void writeSectors(uint32_t diskNumber, uint64_t byteOffset,
                      const void* buffer, size_t bytes) override;
    void flush(uint32_t diskNumber) override;
};

} // namespace bootroll
