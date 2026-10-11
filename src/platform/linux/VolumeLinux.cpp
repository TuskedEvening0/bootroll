#include "platform/linux/VolumeLinux.h"
#include "platform/linux/SysFs.h"

#include "core/disk/FileSystemProbe.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace bootroll {

namespace {

// Decode mountinfo octal escapes: \040 space, \011 tab, \012 newline, \134 '\'.
std::string decodeOctalEscapes(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            out.push_back(static_cast<char>(
                std::stoi(s.substr(i + 1, 3), nullptr, 8)));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Decode udev-style hex escapes in /dev/disk/by-* entry names (\x20 etc.).
std::string decodeHexEscapes(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            const std::string hex = s.substr(i + 2, 2);
            if (std::all_of(hex.begin(), hex.end(), [](char c) {
                    return std::isxdigit(static_cast<unsigned char>(c));
                })) {
                out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                i += 3;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string upperCopy(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string baseNameOf(const std::string& path)
{
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

// Devname ("nvme0n1p1") -> label map built from /dev/disk/by-label symlinks.
std::map<std::string, std::string> labelMap()
{
    std::map<std::string, std::string> out;
    const char* dirPath = "/dev/disk/by-label";
    DIR* dir = opendir(dirPath);
    if (!dir) {
        return out;
    }
    while (const dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        char buf[PATH_MAX] = {};
        const std::string link = std::string(dirPath) + "/" + name;
        const ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        out[baseNameOf(buf)] = decodeHexEscapes(name);
    }
    closedir(dir);
    return out;
}

// Resolve /sys/dev/block/<major:minor> to its sysfs directory (real path).
bool realSysfsDirOfDev(const std::string& devNumber, std::string* outDir)
{
    char buf[PATH_MAX] = {};
    const std::string link = "/sys/dev/block/" + devNumber;
    if (realpath(link.c_str(), buf) == nullptr) {
        return false;
    }
    *outDir = buf;
    return true;
}

// diskNumber assignment: index into the (deterministic) block device scan.
uint32_t diskNumberOfName(const std::string& blockName)
{
    const std::vector<std::string> names = sysfs::scanBlockDevNames();
    const auto it = std::find(names.begin(), names.end(), blockName);
    return it == names.end() ? 0xFFFFFFFFu : uint32_t(it - names.begin());
}

// Read a partition's boot sector (+ the 64 KiB superblock area for btrfs)
// straight off the already-open whole-disk fd and identify the filesystem.
// Best effort: any read error simply leaves the type unknown.
std::string probePartitionFilesystem(int fd, const PartitionInfo& p)
{
    if (fd < 0 || p.sizeBytes < 512) {
        return {};
    }
    uint8_t boot[4096] = {};
    const size_t want = size_t(std::min<uint64_t>(sizeof(boot), p.sizeBytes));
    size_t got = 0;
    while (got < want) {
        const ssize_t n = pread(fd, boot + got, want - got,
                                off_t(p.offsetBytes + got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {};
        }
        if (n == 0) {
            break; // unexpected EOF
        }
        got += size_t(n);
    }

    std::string name = probeFilesystemName(boot, got);
    if (!name.empty() || got < 512) {
        return name;
    }
    // btrfs keeps its superblock at partition offset 0x10040, far beyond the
    // boot sector - only worth a read when the boot sector said "unknown".
    if (p.sizeBytes >= 0x10048) {
        uint8_t magic[8] = {};
        ssize_t n;
        do {
            n = pread(fd, magic, sizeof(magic), off_t(p.offsetBytes + 0x10040));
        } while (n < 0 && errno == EINTR);
        if (n == ssize_t(sizeof(magic)) && isBtrfsSuperblock(magic, sizeof(magic))) {
            return "BTRFS";
        }
    }
    return {};
}

} // namespace

std::vector<MountEntry> parseMountInfo()
{
    std::vector<MountEntry> result;
    std::ifstream in("/proc/self/mountinfo");
    if (!in) {
        return result;
    }
    std::map<std::string, bool> seen; // devNumber -> already reported
    std::string line;
    while (std::getline(in, line)) {
        // Fields: id parent major:minor root mount-point mount-options
        //         [opts...] - fstype source super-options
        const size_t sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        const size_t dash = sep;
        std::istringstream head(line.substr(0, dash));
        std::string id, parent, devNumber, root, mountPoint, mountOpts;
        head >> id >> parent >> devNumber >> root >> mountPoint >> mountOpts;
        if (!head || devNumber.rfind("0:", 0) == 0) {
            continue; // pseudo-filesystems have no block device behind them
        }
        std::istringstream tail(line.substr(dash + 3));
        std::string fsType, source;
        tail >> fsType >> source;
        if (fsType.empty()) {
            continue;
        }
        if (seen.count(devNumber) != 0) {
            continue; // same device mounted again (bind / btrfs subvolume)
        }
        seen[devNumber] = true;
        MountEntry e;
        e.devNumber = devNumber;
        e.mountPoint = decodeOctalEscapes(mountPoint);
        e.fsType = fsType;
        result.push_back(std::move(e));
    }
    return result;
}

std::vector<VolumeInfo> enumerateVolumesLinux()
{
    std::vector<VolumeInfo> result;
    const std::map<std::string, std::string> labels = labelMap();

    for (const MountEntry& m : parseMountInfo()) {
        VolumeInfo v;
        v.driveLetter = m.mountPoint; // Linux contract: mount point
        // ntfs3 is the kernel's NTFS driver, not a separate filesystem -
        // report the filesystem the way Windows names it (probeFilesystemName
        // agrees, and PbrScreen's kindForFsName keys off "ntfs").
        v.fsName = upperCopy(m.fsType == "ntfs3" ? "ntfs" : m.fsType);

        struct statvfs vfs = {};
        if (statvfs(m.mountPoint.c_str(), &vfs) == 0) {
            v.totalBytes = uint64_t(vfs.f_blocks) * uint64_t(vfs.f_frsize);
            v.freeBytes = uint64_t(vfs.f_bfree) * uint64_t(vfs.f_frsize);
        }

        // Map the volume back to its disk + partition via sysfs (no device I/O).
        std::string dir;
        if (realSysfsDirOfDev(m.devNumber, &dir)) {
            const std::string dirName = baseNameOf(dir);
            if (sysfs::fileExists(dir + "/partition")) {
                const std::string numStr = sysfs::readFileTrimmed(dir + "/partition");
                if (!numStr.empty()) {
                    v.partitionNumber = unsigned(std::stoi(numStr));
                }
                const std::string diskName = baseNameOf(dir.substr(0, dir.find_last_of('/')));
                v.diskNumber = diskNumberOfName(diskName);
                const auto it = labels.find(dirName);
                if (it != labels.end()) {
                    v.label = it->second;
                }
            } else {
                // Filesystem on a whole disk (no partition table).
                v.diskNumber = diskNumberOfName(dirName);
                const auto it = labels.find(dirName);
                if (it != labels.end()) {
                    v.label = it->second;
                }
            }
        }

        // isEsp stays false: the partition type GUID needs a GPT read, which
        // must not happen here (UI-thread, no device I/O). The UI's ESP logic
        // runs off PartitionInfo.gptTypeGuid, filled by DiskAccessLinux's
        // GPT parse on the worker threads instead.
        result.push_back(std::move(v));
    }
    return result;
}

void attachVolumeDataLinux(int diskFd, const std::string& /*what*/, DiskInfo& disk)
{
    const std::vector<MountEntry> mounts = parseMountInfo();
    const std::map<std::string, std::string> labels = labelMap();
    const std::string diskName = [number = disk.number]() {
        const std::vector<std::string> names = sysfs::scanBlockDevNames();
        if (number < names.size()) {
            return names[number];
        }
        return std::string();
    }();
    if (diskName.empty()) {
        return;
    }
    const std::map<uint32_t, std::string> partDirs =
        sysfs::partitionDirsOfDisk(diskName);

    for (auto& p : disk.partitions) {
        const auto dirIt = partDirs.find(p.number);
        if (dirIt == partDirs.end()) {
            continue;
        }
        const std::string devNumber =
            sysfs::devNumberOfDir("/sys/block/" + diskName + "/" + dirIt->second);
        if (devNumber.empty()) {
            continue;
        }
        for (const MountEntry& m : mounts) {
            if (m.devNumber != devNumber) {
                continue;
            }
            p.driveLetter = m.mountPoint;
            // Same ntfs3 -> NTFS normalization as enumerateVolumesLinux.
            p.fsName = upperCopy(m.fsType == "ntfs3" ? "ntfs" : m.fsType);
            break;
        }
        // /dev/disk/by-label exists for unmounted partitions too, so labels
        // are attached regardless of mount state.
        const auto lblIt = labels.find(dirIt->second);
        if (lblIt != labels.end() && p.label.empty()) {
            p.label = lblIt->second;
        }
        // mountinfo only knows mounted filesystems: an unmounted partition
        // (typical ESP, NTFS data disk) keeps fsName empty, and ntfs-3g mounts
        // report the generic "fuseblk". Probe the boot sector off the
        // already-open disk fd for the real type (no new device opened).
        if (p.fsName.empty() || p.fsName == "FUSEBLK") {
            p.fsName = probePartitionFilesystem(diskFd, p);
        }
    }
}

std::string devNodeForDevNumber(const std::string& majorMinor)
{
    std::string dir;
    if (!realSysfsDirOfDev(majorMinor, &dir)) {
        return {};
    }
    return "/dev/" + baseNameOf(dir);
}

} // namespace bootroll
