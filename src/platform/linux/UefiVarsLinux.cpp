#include "platform/linux/UefiVarsLinux.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bootroll {

namespace {

// EFI_GLOBAL_VARIABLE: vendor GUID of Boot####/BootOrder/BootNext/Timeout.
// UEFI spec 2.x "Global Variables": 8BE4DF61-93CA-11D2-AA0D-00E098032B8C.
// Copied verbatim from the IUefiVars.h header comment (LESSONS.md #5 - do not
// retype GUIDs from memory).
constexpr const char* kGlobalGuid = "8be4df61-93ca-11d2-aa0d-00e098032b8c";

constexpr const char* kEfiVarsDir = "/sys/firmware/efi/efivars";
constexpr size_t kMaxVarSize = 1024 * 1024; // firmware sanity cap

std::string varPath(const std::string& name)
{
    return std::string(kEfiVarsDir) + "/" + name + "-" + kGlobalGuid;
}

std::string makeError(const char* action, const std::string& name)
{
    // errno passthrough: ENOENT(2) matches ERROR_FILE_NOT_FOUND and the UI's
    // isUefiVarAbsent() discrimination (PLATFORM_SEAMS.md #4).
    std::string s(action);
    if (!name.empty()) {
        s += " '" + name + "'";
    }
    s += ": ";
    s += std::strerror(errno);
    s += " (error " + std::to_string(errno) + ")";
    return s;
}

} // namespace

bool UefiVarsLinux::ensureReady(std::string* error)
{
    struct stat st = {};
    if (stat(kEfiVarsDir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        m_lastCode = errno ? errno : ENOENT;
        *error = "efivarfs is not available (BIOS boot, or not mounted): " +
                 std::string(kEfiVarsDir);
        return false;
    }
    // Readability probe: efivarfs files exist per variable; try to open one.
    // Non-root usually can read; EACCES means privileges are missing.
    DIR* dir = opendir(kEfiVarsDir); // NOLINT(*) - readdir probe below
    if (!dir) {
        m_lastCode = errno;
        *error = "Cannot open " + std::string(kEfiVarsDir) + ": " +
                 std::strerror(errno);
        return false;
    }
    errno = 0;
    const dirent* e = nullptr;
    while ((e = readdir(dir)) != nullptr) {
        if (e->d_name[0] == '.') {
            continue;
        }
        break; // any variable file is enough for the probe
    }
    const int listErr = errno;
    closedir(dir);
    if (!e) {
        if (listErr != 0) {
            m_lastCode = listErr;
            *error = "Cannot list " + std::string(kEfiVarsDir) + ": " +
                     std::strerror(listErr);
            return false;
        }
        return true; // empty efivarfs: valid, just no variables
    }
    const int fd = open(varPath(e->d_name).c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            m_lastCode = errno;
            *error = "Root privilege is required to read UEFI variables.";
            return false;
        }
        // Odd per-variable failures (IO errors) are deferred to read().
    } else {
        close(fd);
    }
    m_lastCode = 0;
    return true;
}

bool UefiVarsLinux::read(const std::string& name, std::vector<uint8_t>* data,
                         uint32_t* attrs, std::string* error)
{
    const int fd = open(varPath(name).c_str(), O_RDONLY);
    if (fd < 0) {
        m_lastCode = errno;
        *error = makeError("Cannot read UEFI variable", name);
        return false;
    }

    // efivarfs layout: first 4 bytes = attributes (uint32 LE), then payload.
    unsigned char header[4] = {};
    size_t got = 0;
    while (got < sizeof(header)) {
        const ssize_t n = ::read(fd, header + got, sizeof(header) - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_lastCode = errno;
            *error = makeError("Cannot read UEFI variable", name);
            close(fd);
            return false;
        }
        if (n == 0) {
            break; // truncated/empty variable
        }
        got += size_t(n);
    }
    if (got < sizeof(header)) {
        m_lastCode = EIO;
        *error = makeError("UEFI variable is truncated", name);
        close(fd);
        return false;
    }

    std::vector<uint8_t> payload;
    unsigned char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_lastCode = errno;
            *error = makeError("Cannot read UEFI variable", name);
            close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        payload.insert(payload.end(), buf, buf + n);
        if (payload.size() > kMaxVarSize) {
            m_lastCode = EFBIG;
            *error = makeError("UEFI variable is too large", name);
            close(fd);
            return false;
        }
    }
    close(fd);

    *attrs = uint32_t(header[0]) | uint32_t(header[1]) << 8 |
             uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
    *data = std::move(payload);
    m_lastCode = 0;
    return true;
}

bool UefiVarsLinux::write(const std::string& name, const std::vector<uint8_t>& data,
                          uint32_t attrs, std::string* error)
{
    // efivarfs refuses to change an existing variable's size through an open
    // fd: the correct sequence is unlink() first, then create + write
    // (PLATFORM_SEAMS.md #4; O_TRUNC alone is not reliable).
    const std::string path = varPath(name);
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        m_lastCode = errno;
        *error = makeError("Cannot replace UEFI variable", name);
        return false;
    }

    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        m_lastCode = errno;
        *error = makeError("Cannot create UEFI variable", name);
        return false;
    }

    unsigned char header[4] = {
        static_cast<unsigned char>(attrs & 0xFF),
        static_cast<unsigned char>((attrs >> 8) & 0xFF),
        static_cast<unsigned char>((attrs >> 16) & 0xFF),
        static_cast<unsigned char>((attrs >> 24) & 0xFF),
    };
    bool ok = true;
    size_t written = 0;
    while (written < sizeof(header)) {
        const ssize_t n = ::write(fd, header + written, sizeof(header) - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            m_lastCode = errno;
            *error = makeError("Cannot write UEFI variable", name);
            ok = false;
            break;
        }
        written += size_t(n);
    }
    if (ok && !data.empty()) {
        size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_lastCode = errno;
                *error = makeError("Cannot write UEFI variable", name);
                ok = false;
                break;
            }
            off += size_t(n);
        }
    }
    close(fd);
    if (!ok) {
        return false;
    }
    m_lastCode = 0;
    return true;
}

bool UefiVarsLinux::remove(const std::string& name, std::string* error)
{
    if (unlink(varPath(name).c_str()) != 0) {
        m_lastCode = errno;
        *error = makeError("Cannot delete UEFI variable", name);
        return false;
    }
    m_lastCode = 0;
    return true;
}

} // namespace bootroll
