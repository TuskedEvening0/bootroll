#include "platform/win/VolumeWin.h"
#include "platform/win/WinUtil.h"

#include <iterator>
#include <winioctl.h>

namespace bootroll {

namespace {

// The EFI System Partition GUID ("c12a7328-f81f-11d2-ba4b-00a0c93e0093" partition type).
const GUID GPT_ESP_TYPE = { 0xc12a7328, 0xf81f, 0x11d2, { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b } };

bool getDeviceNumber(HANDLE h, DWORD* diskNumber, DWORD* partitionNumber)
{
    STORAGE_DEVICE_NUMBER sdn = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                         &sdn, sizeof(sdn), &bytes, nullptr)) {
        return false;
    }
    *diskNumber = sdn.DeviceNumber;
    *partitionNumber = sdn.PartitionNumber;
    return true;
}

bool getGptTypeGuid(HANDLE h, GUID* out)
{
    PARTITION_INFORMATION_EX pie = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0,
                         &pie, sizeof(pie), &bytes, nullptr)) {
        return false;
    }
    if (pie.PartitionStyle != PARTITION_STYLE_GPT) {
        return false;
    }
    *out = pie.Gpt.PartitionType;
    return true;
}

} // namespace

std::vector<VolumeInfo> enumerateVolumesWin()
{
    std::vector<VolumeInfo> result;

    wchar_t volumeName[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(volumeName, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) {
        return result;
    }

    do {
        size_t len = wcslen(volumeName);
        if (len > 0 && volumeName[len - 1] == L'\\') {
            volumeName[len - 1] = L'\0'; // "\\?\Volume{guid}\" -> without trailing backslash
        }

        VolumeInfo v;

        // Drive letter(s): take the first.
        wchar_t paths[MAX_PATH * 2] = {};
        DWORD plen = 0;
        if (GetVolumePathNamesForVolumeNameW(volumeName, paths, (DWORD)std::size(paths), &plen) && paths[0]) {
            std::wstring letter(paths);
            if (letter.size() >= 2 && letter[1] == L':') {
                v.driveLetter = utf8FromWide(letter.substr(0, 2));
            }
        }

        // Open with query-attributes-only access: no elevation required.
        HANDLE h = CreateFileW(volumeName, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        DWORD diskNumber = 0, partNumber = 0;
        if (getDeviceNumber(h, &diskNumber, &partNumber)) {
            v.diskNumber = diskNumber;
            v.partitionNumber = partNumber;
        }

        GUID typeGuid = {};
        if (getGptTypeGuid(h, &typeGuid) && IsEqualGUID(typeGuid, GPT_ESP_TYPE)) {
            v.isEsp = true;
        }
        CloseHandle(h);

        // Filesystem + label + sizes (only works for mounted/lettered or mounted volumes).
        wchar_t fsName[64] = {}, volLabel[MAX_PATH + 1] = {};
        ULARGE_INTEGER total = {}, freeB = {};
        std::wstring volWithSlash(volumeName);
        volWithSlash += L"\\";
        if (GetVolumeInformationW(volWithSlash.c_str(), volLabel, MAX_PATH + 1,
                                  nullptr, nullptr, nullptr, fsName, 64)) {
            v.fsName = utf8FromWide(fsName);
            v.label = utf8FromWide(volLabel);
        }
        GetDiskFreeSpaceExW(volWithSlash.c_str(), nullptr, &total, &freeB);
        v.totalBytes = total.QuadPart;
        v.freeBytes = freeB.QuadPart;

        result.push_back(std::move(v));
    } while (FindNextVolumeW(find, volumeName, MAX_PATH));

    FindVolumeClose(find);
    return result;
}

} // namespace bootroll
