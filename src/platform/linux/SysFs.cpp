#include "platform/linux/SysFs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bootroll {
namespace sysfs {

std::string readFileTrimmed(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    // Trim trailing whitespace (sysfs attributes are newline-terminated; the
    // SCSI/NVMe model strings are space-padded).
    while (!data.empty() &&
           std::isspace(static_cast<unsigned char>(data.back()))) {
        data.pop_back();
    }
    return data;
}

bool fileExists(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
}

std::vector<std::string> scanBlockDevNames()
{
    std::vector<std::string> names;
    DIR* dir = opendir("/sys/block");
    if (!dir) {
        return names;
    }
    while (const dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        // Virtual / ephemeral / RAID layers are out of scope for bootroll.
        // loop* stays visible: a losetup-attached image/VHD is the Linux
        // equivalent of Windows attaching a VHD (sector editor target).
        if (name.rfind("ram", 0) == 0 || name.rfind("zram", 0) == 0 ||
            name.rfind("rom", 0) == 0 || name.rfind("fd", 0) == 0 ||
            name.rfind("sr", 0) == 0 || name.rfind("md", 0) == 0 ||
            name.rfind("dm-", 0) == 0) {
            continue;
        }
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

std::string devNodePath(const std::string& blockName)
{
    return "/dev/" + blockName;
}

std::string modelOfBlockDev(const std::string& blockName)
{
    // loop device: show the backing image/VHD file name (that is what the
    // user attached - mirrors "MS Virtual Disk" on Windows).
    if (blockName.rfind("loop", 0) == 0) {
        const std::string backing =
            readFileTrimmed("/sys/block/" + blockName + "/loop/backing_file");
        if (!backing.empty()) {
            const size_t slash = backing.find_last_of('/');
            return slash == std::string::npos ? backing : backing.substr(slash + 1);
        }
        return {};
    }
    // SCSI/SATA/USB: device/model; NVMe: device/model too; MMC: device/name.
    std::string model =
        readFileTrimmed("/sys/block/" + blockName + "/device/model");
    if (model.empty()) {
        model = readFileTrimmed("/sys/block/" + blockName + "/device/name");
    }
    return model;
}

std::string busTypeOfBlockDev(const std::string& blockName)
{
    // sysfs never reports the transport directly for sd*; the device symlink
    // path reveals it (ataN for SATA, usbN for USB, sas for SAS, ...).
    if (blockName.rfind("nvme", 0) == 0) {
        return "NVMe";
    }
    if (blockName.rfind("vd", 0) == 0 || blockName.rfind("loop", 0) == 0) {
        return "Virtual"; // virtio / attached image (isVhd detection contract)
    }
    if (blockName.rfind("mmcblk", 0) == 0) {
        return "MMC";
    }
    if (blockName.rfind("hd", 0) == 0) {
        return "ATA";
    }
    if (blockName.rfind("sd", 0) == 0) {
        char link[1024] = {};
        const std::string devLink = "/sys/block/" + blockName + "/device";
        const ssize_t n = readlink(devLink.c_str(), link, sizeof(link) - 1);
        if (n > 0) {
            link[n] = '\0';
            const std::string path = link;
            if (path.find("/usb") != std::string::npos) {
                return "USB";
            }
            if (path.find("/ata") != std::string::npos) {
                return "SATA";
            }
            if (path.find("/sas") != std::string::npos) {
                return "SCSI";
            }
        }
        return "SCSI";
    }
    return "Unknown";
}

uint64_t diskSizeBytes(const std::string& blockName)
{
    // /sys/block/<name>/size counts 512-byte sectors on every device type.
    const std::string sectors = readFileTrimmed("/sys/block/" + blockName + "/size");
    if (sectors.empty()) {
        return 0;
    }
    unsigned long long n = 0;
    if (std::sscanf(sectors.c_str(), "%llu", &n) != 1) {
        return 0;
    }
    return uint64_t(n) * 512;
}

uint32_t logicalBlockSize(const std::string& blockName)
{
    const std::string v = readFileTrimmed("/sys/block/" + blockName +
                                          "/queue/logical_block_size");
    if (v.empty()) {
        return 512;
    }
    unsigned n = 0;
    if (std::sscanf(v.c_str(), "%u", &n) != 1 || n == 0) {
        return 512;
    }
    return n;
}

std::map<uint32_t, std::string> partitionDirsOfDisk(const std::string& blockName)
{
    std::map<uint32_t, std::string> out;
    const std::string base = "/sys/block/" + blockName;
    DIR* dir = opendir(base.c_str());
    if (!dir) {
        return out;
    }
    while (const dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == ".." || name == blockName) {
            continue;
        }
        const std::string partFile = base + "/" + name + "/partition";
        if (!fileExists(partFile)) {
            continue;
        }
        const std::string number = readFileTrimmed(partFile);
        unsigned n = 0;
        if (std::sscanf(number.c_str(), "%u", &n) == 1 && n > 0) {
            out[n] = name;
        }
    }
    closedir(dir);
    return out;
}

std::string devNumberOfDir(const std::string& sysfsDir)
{
    return readFileTrimmed(sysfsDir + "/dev");
}

std::string blockDirNameOfDevNumber(const std::string& majorMinor)
{
    char buf[1024] = {};
    const std::string link = "/sys/dev/block/" + majorMinor;
    const ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    const std::string target = buf;
    const size_t slash = target.find_last_of('/');
    return slash == std::string::npos ? target : target.substr(slash + 1);
}

} // namespace sysfs
} // namespace bootroll
