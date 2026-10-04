#pragma once
// UEFI variable access seam. Implementations target the EFI_GLOBAL_VARIABLE
// vendor GUID ({8BE4DF61-93CA-11D2-AA0D-00E098032B8C}) where the firmware
// boot variables (Boot####, BootOrder, BootNext, Timeout) live.
#include <cstdint>
#include <string>
#include <vector>

namespace bootroll {

class IUefiVars {
public:
    virtual ~IUefiVars() = default;

    // Enable the system-environment privilege; must be called (once) before
    // any read/write. Returns false and fills *error on failure.
    virtual bool ensureReady(std::string* error) = 0;

    // Read one variable. On success *data holds the payload and *attrs the
    // UEFI variable attributes. On failure fills *error (this includes the
    // "variable not found" case, which callers treat as absence).
    virtual bool read(const std::string& name, std::vector<uint8_t>* data,
                      uint32_t* attrs, std::string* error) = 0;

    // Create or replace a variable. attrs uses the EFI_VARIABLE_* bit flags
    // (non-volatile | boot-service | runtime = 0x7 for the boot variables).
    virtual bool write(const std::string& name, const std::vector<uint8_t>& data,
                       uint32_t attrs, std::string* error) = 0;

    // Delete a variable.
    virtual bool remove(const std::string& name, std::string* error) = 0;

    // OS error code of the last failed operation (0 after a success). Lets
    // callers tell "variable absent" apart from systemic failures (access
    // denied, privilege not held, ...) that must be reported, not skipped.
    virtual uint32_t lastErrorCode() const = 0;
};

// Win32 codes a firmware variable read yields when the variable simply does
// not exist (vs. a real failure).
inline bool isUefiVarAbsent(uint32_t code)
{
    return code == 2      // ERROR_FILE_NOT_FOUND
        || code == 203    // ERROR_ENVVAR_NOT_FOUND
        || code == 1168;  // ERROR_NOT_FOUND
}

} // namespace bootroll
