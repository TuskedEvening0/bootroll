#include "platform/linux/DiskAccessLinux.h"
#include "core/disk/PartitionTable.h"
#include "platform/linux/SysFs.h"
#include "platform/linux/VolumeLinux.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

namespace bootroll {

namespace {

constexpr size_t kSector = 512;

std::string errnoText()
{
    const int e = errno; // capture once: strerror/formatting may touch errno
    return std::string(std::strerror(e)) + " (errno " + std::to_string(e) + ")";
}

// Open a block device self-contained; throws DiskAccessError on failure.
int openDevNode(const std::string& path, bool write)
{
    const int fd = open(path.c_str(), write ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        std::string hint;
        if (errno == EACCES) {
            hint = " (root privileges required - use the elevate button)";
        } else if (errno == ENOENT) {
            hint = " (device is gone)";
        }
        throw DiskAccessError("Cannot open " + path + ": " + errnoText() + hint);
    }
    return fd;
}

void readFully(int fd, uint64_t byteOffset, void* buffer, size_t bytes,
               const std::string& what)
{
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = pread(fd, out + done, bytes - done,
                                off_t(byteOffset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw DiskAccessError("Read failed on " + what + ": " + errnoText());
        }
        if (n == 0) {
            throw DiskAccessError("Unexpected end of device " + what);
        }
        done += size_t(n);
    }
}

void writeFully(int fd, uint64_t byteOffset, const void* buffer, size_t bytes,
                const std::string& what)
{
    auto* in = static_cast<const uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = pwrite(fd, in + done, bytes - done,
                                 off_t(byteOffset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw DiskAccessError("Write failed on " + what + ": " + errnoText());
        }
        done += size_t(n);
    }
}

bool isHiddenMbrType(uint8_t t)
{
    switch (t) {
    case 0x11: case 0x12: case 0x14: case 0x16: case 0x17:
    case 0x18: case 0x1B: case 0x1C: case 0x1E:
        return true; // hidden FAT/NTFS/HPFS variants
    default:
        return false;
    }
}

void addMbrPartition(DiskInfo& disk, uint32_t number, uint64_t firstLba,
                     uint32_t countLba, uint8_t type, bool active)
{
    PartitionInfo pi;
    pi.number = number;
    pi.style = PartitionStyle::Mbr;
    pi.offsetBytes = firstLba * uint64_t(disk.sectorSize);
    pi.sizeBytes = uint64_t(countLba) * uint64_t(disk.sectorSize);
    pi.typeCode = type;
    pi.bootIndicator = active;
    pi.hidden = isHiddenMbrType(type);
    disk.partitions.push_back(std::move(pi));
}

// GPT parse with primary + backup header tolerance (M8_PLAN §3). fd stays
// open; sectorSize decides all offset math.
void parseGptLayout(int fd, const std::string& what, DiskInfo& disk,
                    uint64_t totalBytes)
{
    const uint32_t ss = disk.sectorSize;

    auto readSector = [&](uint64_t lba, uint8_t* out) {
        readFully(fd, lba * ss, out, ss, what);
    };

    std::vector<uint8_t> header(ss);
    readSector(1, header.data());
    GptHeader h = parseGptHeader(header.data(), header.size());
    if (!h.valid && totalBytes >= 2 * ss) {
        // Corrupt primary header: try the backup header at the last LBA.
        const uint64_t lastLba = (totalBytes / ss) - 1;
        readSector(lastLba, header.data());
        h = parseGptHeader(header.data(), header.size());
    }
    if (!h.valid) {
        return; // not a GPT disk
    }
    if (h.entryCount > 128 || h.entrySize < 128 || h.entrySize > 1024) {
        throw DiskAccessError("GPT entry geometry looks bogus on " + what);
    }

    const size_t arrayBytes = size_t(h.entryCount) * h.entrySize;
    std::vector<uint8_t> array((arrayBytes + ss - 1) / ss * ss);
    readFully(fd, h.entryArrayLba * ss, array.data(), arrayBytes, what);
    std::vector<GptEntry> entries = parseGptEntries(h, array.data(), array.size());
    if (entries.empty()) {
        return; // entry array CRC failed (both header copies checked above)
    }

    disk.style = PartitionStyle::Gpt;
    for (const GptEntry& e : entries) {
        if (e.lastLba < e.firstLba) {
            continue;
        }
        PartitionInfo pi;
        pi.number = uint32_t(disk.partitions.size()) + 1;
        pi.style = PartitionStyle::Gpt;
        pi.offsetBytes = e.firstLba * ss;
        pi.sizeBytes = (e.lastLba - e.firstLba + 1) * ss;
        pi.gptTypeGuid = e.typeGuid;
        pi.gptPartGuid = e.partGuid;
        pi.gptName = e.name;
        disk.partitions.push_back(std::move(pi));
    }
}

} // namespace

std::vector<DiskInfo> DiskAccessLinux::discoverDisks()
{
    std::vector<DiskInfo> result;

    // Phase 1: pure sysfs. Never opens /dev, never touches media.
    const std::vector<std::string> names = sysfs::scanBlockDevNames();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const std::string& name : names) {
        uint32_t number;
        const auto known = m_nameToNumber.find(name);
        if (known != m_nameToNumber.end()) {
            number = known->second; // stable across refreshes / hotplug
        } else {
            number = m_nextNumber++;
            m_nameToNumber[name] = number;
            m_numberToName[number] = name;
        }

        DiskInfo d;
        d.number = number;
        d.model = sysfs::modelOfBlockDev(name);
        result.push_back(std::move(d));
    }
    std::sort(result.begin(), result.end(),
              [](const DiskInfo& a, const DiskInfo& b) { return a.number < b.number; });
    return result;
}

void DiskAccessLinux::fillDiskDetails(DiskInfo& disk)
{
    // Worker thread, one disk per thread: everything here is local to *this
    // call* - no shared handles, no shared mutable state (the dev-name map is
    // read-only in this phase or lazily assigned under the mutex).
    std::string devNode;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const auto it = m_numberToName.find(disk.number);
        if (it == m_numberToName.end()) {
            throw DiskAccessError("Unknown disk number " +
                                  std::to_string(disk.number));
        }
        devNode = sysfs::devNodePath(it->second);
        const std::string blockName = it->second;

        // Cheap sysfs facts that do not need the device node.
        disk.totalBytes = sysfs::diskSizeBytes(blockName);
        disk.sectorSize = sysfs::logicalBlockSize(blockName);
        disk.busType = sysfs::busTypeOfBlockDev(blockName);
        if (disk.model.empty()) {
            disk.model = sysfs::modelOfBlockDev(blockName);
        }
        disk.isVhd = (disk.busType == "Virtual") ||
                     disk.model.find("Virtual") != std::string::npos ||
                     disk.model.find("VHD") != std::string::npos;
    }

    const int fd = openDevNode(devNode, false);
    try {
        if (disk.sectorSize == 0) {
            disk.sectorSize = 512;
        }
        // BLKGETSIZE64 is the authoritative size when sysfs disagrees/missing.
        if (disk.totalBytes == 0) {
            uint64_t size = 0;
            if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
                disk.totalBytes = size;
            }
        }

        // Sector 0 decides MBR vs GPT.
        std::vector<uint8_t> sector0(kSector);
        readFully(fd, 0, sector0.data(), sector0.size(), devNode);
        const MbrTable mbr = parseMbr(sector0.data(), sector0.size());

        std::vector<uint8_t> sector1(kSector);
        GptHeader probe = {};
        if (disk.totalBytes >= 2 * uint64_t(disk.sectorSize)) {
            readFully(fd, uint64_t(disk.sectorSize), sector1.data(), kSector, devNode);
            probe = parseGptHeader(sector1.data(), sector1.size());
        }

        if (probe.valid ||
            (mbr.bootSignature && !mbr.entries.empty() &&
             mbr.entries[0].type == 0xEE)) {
            parseGptLayout(fd, devNode, disk, disk.totalBytes);
        } else if (mbr.bootSignature) {
            disk.style = PartitionStyle::Mbr;
            // Primary partitions keep their table slot number (kernel: sda1
            // = slot 1); the extended container itself gets no node; logical
            // partitions always number from 5.
            uint32_t logicalNo = 5;
            for (int slot = 0; slot < int(mbr.entries.size()); ++slot) {
                const MbrEntry& e = mbr.entries[size_t(slot)];
                if (e.empty) {
                    continue;
                }
                if (e.type == 0x05 || e.type == 0x0F) {
                    // Extended partition: walk the EBR chain for logicals.
                    const uint64_t extBase = e.beginLba;
                    uint64_t ebrLba = e.beginLba;
                    for (int guard = 0; guard < 128; ++guard) {
                        std::vector<uint8_t> ebr(kSector);
                        readFully(fd, ebrLba * uint64_t(disk.sectorSize),
                                  ebr.data(), ebr.size(), devNode);
                        const MbrTable t = parseMbr(ebr.data(), ebr.size());
                        if (!t.bootSignature) {
                            break;
                        }
                        const MbrEntry& data = t.entries[0];
                        if (!data.empty && data.type != 0x05 &&
                            data.type != 0x0F) {
                            addMbrPartition(disk, logicalNo++,
                                            extBase + data.beginLba,
                                            data.sectorCount, data.type, false);
                        }
                        const MbrEntry& link = t.entries[1];
                        if (link.empty ||
                            (link.type != 0x05 && link.type != 0x0F)) {
                            break;
                        }
                        ebrLba = extBase + link.beginLba;
                    }
                } else {
                    addMbrPartition(disk, uint32_t(slot) + 1, e.beginLba,
                                    e.sectorCount, e.type, e.active);
                }
            }
        }
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);

    // Mounted-volume attachment: pure file reads (mountinfo/sysfs), safe on
    // this worker thread (mirrors DiskWin::attachVolumeData).
    attachVolumeDataLinux(disk);
}

std::string DiskAccessLinux::devNodeForNumber(uint32_t diskNumber)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_numberToName.find(diskNumber);
    if (it != m_numberToName.end()) {
        return sysfs::devNodePath(it->second);
    }
    // Unknown number: refuse rather than guess a device (never write to the
    // wrong node).
    throw DiskAccessError("Unknown disk number " + std::to_string(diskNumber));
}

void DiskAccessLinux::readSectors(uint32_t diskNumber, uint64_t byteOffset,
                                  void* buffer, size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    if (byteOffset % kSector != 0) {
        throw DiskAccessError("Sector read offset must be 512-byte aligned");
    }
    const std::string devNode = devNodeForNumber(diskNumber);
    const int fd = openDevNode(devNode, false);
    try {
        readFully(fd, byteOffset, buffer, bytes, devNode);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
}

void DiskAccessLinux::writeSectors(uint32_t diskNumber, uint64_t byteOffset,
                                   const void* buffer, size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    if (byteOffset % kSector != 0) {
        throw DiskAccessError("Sector write offset must be 512-byte aligned");
    }
    const std::string devNode = devNodeForNumber(diskNumber);
    const int fd = openDevNode(devNode, true); // needs root
    try {
        writeFully(fd, byteOffset, buffer, bytes, devNode);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
}

void DiskAccessLinux::flush(uint32_t diskNumber)
{
    const std::string devNode = devNodeForNumber(diskNumber);
    const int fd = openDevNode(devNode, true);
    const int rc = fsync(fd);
    const int err = errno;
    close(fd);
    if (rc != 0) {
        errno = err;
        throw DiskAccessError("Flush failed on " + devNode + ": " + errnoText());
    }
}

} // namespace bootroll
