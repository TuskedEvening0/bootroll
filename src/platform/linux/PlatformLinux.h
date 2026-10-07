#pragma once
// Concrete IPlatform for Linux (decision record: GLFW + OpenGL3 rendering,
// zenity subprocess dialogs, pkexec elevation - M8_PLAN §2/§5).
#include "platform/IPlatform.h"
#include "platform/linux/UefiVarsLinux.h"

namespace bootroll {

class PlatformLinux : public IPlatform {
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
    std::vector<FontCandidate> candidateFonts() override;
    bool isElevated() override;
    bool restartElevated(const std::string& args) override;
    std::string lastElevateError() const override { return m_lastElevateError; }
    const char* elevateActionMsgId() const override;
    const char* elevateDeclinedMsgId() const override;
    std::string iniPath() override;
    std::string logPath() override;

private:
    std::string exeDir() const;

    UefiVarsLinux m_uefi;
    std::string m_lastElevateError; // classified reason for the last failure
};

} // namespace bootroll
