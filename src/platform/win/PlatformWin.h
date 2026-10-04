#pragma once
#include "platform/IPlatform.h"
#include "platform/win/UefiWin.h"

namespace bootroll {

// Concrete IPlatform for Windows 10.
class PlatformWin : public IPlatform {
public:
    std::unique_ptr<IDiskAccess> createDiskAccess() override;
    std::vector<VolumeInfo> enumerateVolumes() override;
    bool readVolumeFirstSector(const std::string& driveLetter,
                               std::vector<uint8_t>* out,
                               std::string* error) override;
    std::string openFileDialog(const std::string& title,
                               const std::string& filter) override;
    std::string saveFileDialog(const std::string& title,
                               const std::string& filter,
                               const std::string& defaultExt) override;
    bool confirmDialog(const std::string& title, const std::string& text) override;
    FirmwareType firmwareType() override;
    IUefiVars* uefiVars() override { return &m_uefi; }
    float dpiScale() const override;
    std::string systemBcdPath() override;
    std::vector<std::string> candidateFontPaths() override;
    bool isElevated() override;
    bool restartElevated(const std::string& args) override;
    std::string iniPath() override;
    std::string logPath() override;

private:
    std::string exeDir() const;

    UefiWin m_uefi;
};

} // namespace bootroll
