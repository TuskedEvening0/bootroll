#pragma once
// Platform-agnostic raw disk access interface.
// Core/UI code depends only on this; the platform layer provides implementations
// (Win32: \\.\PhysicalDriveN + IOCTLs; Linux M7: /dev/sdX or udisks2).
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/disk/DiskInfo.h"

namespace bootroll {

class DiskAccessError : public std::runtime_error {
public:
    explicit DiskAccessError(const std::string& what) : std::runtime_error(what) {}
};

class IDiskAccess {
public:
    virtual ~IDiskAccess() = default;

    // Phase 1 of enumeration (fast, no media-touching I/O): discover the
    // disks present. Returns stubs with number/model filled only; details are
    // filled per disk by fillDiskDetails() so one misbehaving device cannot
    // stall discovery of the others.
    virtual std::vector<DiskInfo> discoverDisks() = 0;

    // Phase 2 of enumeration: fill one disk's details (bus type, size, sector
    // size, partition layout, mounted volumes). Runs on its own worker thread
    // and may block indefinitely on a misbehaving device. Throws
    // DiskAccessError when the disk cannot be queried at all.
    virtual void fillDiskDetails(DiskInfo& disk) = 0;

    // Raw sector I/O. byteOffset must be a multiple of the disk sector size.
    // May require elevation on Windows (writing). Throws DiskAccessError on failure.
    virtual void readSectors(uint32_t diskNumber, uint64_t byteOffset,
                             void* buffer, size_t bytes) = 0;
    virtual void writeSectors(uint32_t diskNumber, uint64_t byteOffset,
                              const void* buffer, size_t bytes) = 0;
    virtual void flush(uint32_t diskNumber) = 0;
};

} // namespace bootroll
