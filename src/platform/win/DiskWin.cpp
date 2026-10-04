#include "platform/win/DiskWin.h"
#include "platform/win/WinUtil.h"
#include "platform/win/VolumeWin.h"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <winioctl.h>
#include <objbase.h>

namespace bootroll {

namespace {

std::wstring physicalDrivePath(uint32_t n)
{
    wchar_t buf[64];
    swprintf_s(buf, L"\\\\.\\PhysicalDrive%u", n);
    return buf;
}

// Open a physical drive. access=0 is enough for IOCTL queries (no elevation needed).
HANDLE openDisk(uint32_t n, DWORD access)
{
    HANDLE h = CreateFileW(physicalDrivePath(n).c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw DiskAccessError("Cannot open " + utf8FromWide(physicalDrivePath(n)) +
                              ": " + lastErrorMessage(GetLastError()));
    }
    return h;
}

void queryStorageProperty(HANDLE h, std::string* product, std::string* busType)
{
    char out[4096] = {};
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                         out, sizeof(out), &bytes, nullptr)) {
        return;
    }
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(out);
    if (product && desc->ProductIdOffset && desc->ProductIdOffset < bytes) {
        *product = out + desc->ProductIdOffset; // ASCII product id
    }
    if (busType) {
        switch (desc->BusType) {
        case BusTypeUsb: *busType = "USB"; break;
        case BusTypeSata: *busType = "SATA"; break;
        case BusTypeSas: *busType = "SAS"; break;
        case BusTypeNvme: *busType = "NVMe"; break;
        case BusTypeVirtual: *busType = "Virtual"; break;
        case BusTypeScsi: *busType = "SCSI"; break;
        case BusTypeAta: *busType = "ATA"; break;
        case BusTypeSd: *busType = "SD"; break;
        case BusTypeMmc: *busType = "MMC"; break;
        default: *busType = "Unknown"; break;
        }
    }
}

bool queryGeometry(HANDLE h, uint64_t* totalBytes, uint32_t* sectorSize)
{
    union {
        DISK_GEOMETRY_EX g;
        char buf[512];
    } u = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                         &u, sizeof(u), &bytes, nullptr)) {
        return false;
    }
    *totalBytes = u.g.DiskSize.QuadPart;
    *sectorSize = u.g.Geometry.BytesPerSector;
    return true;
}

void queryLayout(HANDLE h, DiskInfo* out)
{
    union {
        DRIVE_LAYOUT_INFORMATION_EX l;
        char buf[16 * 1024];
    } u = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                         &u, sizeof(u), &bytes, nullptr)) {
        return;
    }

    if (u.l.PartitionStyle == PARTITION_STYLE_GPT) {
        out->style = PartitionStyle::Gpt;
    } else if (u.l.PartitionStyle == PARTITION_STYLE_MBR) {
        out->style = PartitionStyle::Mbr;
    }

    for (DWORD i = 0; i < (DWORD)u.l.PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX& p = u.l.PartitionEntry[i];
        if (p.PartitionNumber == 0 || p.PartitionLength.QuadPart == 0) {
            continue; // unused slots / gaps
        }
        PartitionInfo pi;
        pi.number = p.PartitionNumber;
        pi.offsetBytes = p.StartingOffset.QuadPart;
        pi.sizeBytes = p.PartitionLength.QuadPart;
        if (p.PartitionStyle == PARTITION_STYLE_MBR) {
            pi.style = PartitionStyle::Mbr;
            pi.typeCode = p.Mbr.PartitionType;
            pi.bootIndicator = p.Mbr.BootIndicator != 0;
        } else if (p.PartitionStyle == PARTITION_STYLE_GPT) {
            pi.style = PartitionStyle::Gpt;
            const auto guidToString = [](const GUID& g) {
                wchar_t s[64] = {};
                StringFromGUID2(g, s, 64);
                std::string out = utf8FromWide(s);
                if (out.size() >= 2 && out.front() == '{' && out.back() == '}') {
                    out = out.substr(1, out.size() - 2);
                }
                for (char& c : out) {
                    c = (char)std::tolower((unsigned char)c);
                }
                return out;
            };
            pi.gptTypeGuid = guidToString(p.Gpt.PartitionType);
            pi.gptPartGuid = guidToString(p.Gpt.PartitionId);
            pi.gptName = utf8FromWide(p.Gpt.Name);
        }
        out->partitions.push_back(std::move(pi));
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

void attachVolumeData(DiskInfo& disk, const std::vector<VolumeInfo>& volumes)
{
    for (auto& p : disk.partitions) {
        for (const auto& v : volumes) {
            if (v.diskNumber == disk.number && v.partitionNumber == p.number) {
                p.driveLetter = v.driveLetter;
                p.label = v.label;
                p.fsName = v.fsName;
                break;
            }
        }
    }
}

void sectorIo(uint32_t diskNumber, uint64_t byteOffset, void* buffer, size_t bytes, bool write)
{
    if (bytes == 0) {
        return;
    }
    HANDLE h = openDisk(diskNumber, write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ);

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(byteOffset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        throw DiskAccessError("Seek failed: " + lastErrorMessage(err));
    }

    BOOL ok = FALSE;
    DWORD transferred = 0;
    if (write) {
        ok = WriteFile(h, buffer, (DWORD)bytes, &transferred, nullptr);
    } else {
        ok = ReadFile(h, buffer, (DWORD)bytes, &transferred, nullptr) && transferred == bytes;
    }
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) {
        throw DiskAccessError(write ? "Write failed: " : "Read failed: " + lastErrorMessage(err));
    }
}

} // namespace

std::vector<DiskInfo> DiskWin::discoverDisks()
{
    std::vector<DiskInfo> result;

    const HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr,
                                                  nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_DISK, idx, &ifData); ++idx) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) {
            continue;
        }
        std::vector<char> detailBuf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devData = {};
        devData.cbSize = sizeof(devData);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, nullptr, &devData)) {
            continue;
        }

        // Friendly name / device description (model string).
        wchar_t friendly[256] = {};
        SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_FRIENDLYNAME,
                                          nullptr, (PBYTE)friendly, sizeof(friendly), nullptr);
        if (!friendly[0]) {
            SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_DEVICEDESC,
                                              nullptr, (PBYTE)friendly, sizeof(friendly), nullptr);
        }

        // Open with 0 access: query-only, works without elevation.
        HANDLE h = CreateFileW(detail->DevicePath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        STORAGE_DEVICE_NUMBER sdn = {};
        DWORD bytes = 0;
        if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                             &sdn, sizeof(sdn), &bytes, nullptr)) {
            CloseHandle(h);
            continue;
        }
        CloseHandle(h);

        // Two device interfaces can map to the same PhysicalDriveN; keep one.
        bool duplicate = false;
        for (const auto& d : result) {
            if (d.number == sdn.DeviceNumber) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        DiskInfo d;
        d.number = sdn.DeviceNumber;
        if (friendly[0]) {
            d.model = utf8FromWide(friendly);
        }
        result.push_back(std::move(d));
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    std::sort(result.begin(), result.end(),
              [](const DiskInfo& a, const DiskInfo& b) { return a.number < b.number; });
    return result;
}

void DiskWin::fillDiskDetails(DiskInfo& d)
{
    // Runs on its own worker thread: any of these IOCTLs / volume queries may
    // block for a long time (or forever) on a misbehaving device, and that
    // must only delay this disk, never the others.
    HANDLE h = openDisk(d.number, 0); // query-only; throws DiskAccessError

    std::string product;
    queryStorageProperty(h, &product, &d.busType);
    if (d.model.empty()) {
        d.model = product;
    }

    uint64_t total = 0;
    uint32_t sector = 512;
    if (queryGeometry(h, &total, &sector)) {
        d.totalBytes = total;
        d.sectorSize = sector;
    }
    d.isVhd = (d.busType == "Virtual") ||
              d.model.find("Virtual") != std::string::npos ||
              d.model.find("VHD") != std::string::npos;
    queryLayout(h, &d);
    CloseHandle(h);

    for (auto& p : d.partitions) {
        p.hidden = (p.style == PartitionStyle::Mbr) && isHiddenMbrType(p.typeCode);
    }

    attachVolumeData(d, enumerateVolumesWin());
}

void DiskWin::readSectors(uint32_t diskNumber, uint64_t byteOffset, void* buffer, size_t bytes)
{
    sectorIo(diskNumber, byteOffset, buffer, bytes, false);
}

void DiskWin::writeSectors(uint32_t diskNumber, uint64_t byteOffset, const void* buffer, size_t bytes)
{
    sectorIo(diskNumber, byteOffset, const_cast<void*>(buffer), bytes, true);
}

void DiskWin::flush(uint32_t diskNumber)
{
    HANDLE h = openDisk(diskNumber, GENERIC_READ | GENERIC_WRITE);
    BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        throw DiskAccessError("Flush failed: " + lastErrorMessage(GetLastError()));
    }
}

} // namespace bootroll
