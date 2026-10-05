#pragma once
// efivarfs implementation of IUefiVars (/sys/firmware/efi/efivars).
#include "platform/IUefiVars.h"

namespace bootroll {

class UefiVarsLinux : public IUefiVars {
public:
    bool ensureReady(std::string* error) override;
    bool read(const std::string& name, std::vector<uint8_t>* data,
              uint32_t* attrs, std::string* error) override;
    bool write(const std::string& name, const std::vector<uint8_t>& data,
               uint32_t attrs, std::string* error) override;
    bool remove(const std::string& name, std::string* error) override;
    uint32_t lastErrorCode() const override { return m_lastCode; }

private:
    uint32_t m_lastCode = 0;
};

} // namespace bootroll
