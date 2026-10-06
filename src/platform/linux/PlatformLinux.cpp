#include "platform/linux/PlatformLinux.h"
#include "platform/linux/DiskAccessLinux.h"
#include "platform/linux/VolumeLinux.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bootroll {

namespace {

constexpr const char* kAppIniName = "bootroll.ini";
constexpr const char* kAppLogName = "bootroll.log";

std::string exePath()
{
    char buf[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

std::string dirOf(const std::string& path)
{
    const size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

bool fileExists(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

// Split "desc|*.bin|all|*.*" into (desc, pattern) pairs.
struct FilterPair {
    std::string name;
    std::string pattern;
};
std::vector<FilterPair> parseFilter(const std::string& filter)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= filter.size()) {
        const size_t bar = filter.find('|', start);
        if (bar == std::string::npos) {
            parts.push_back(filter.substr(start));
            break;
        }
        parts.push_back(filter.substr(start, bar - start));
        start = bar + 1;
    }
    std::vector<FilterPair> out;
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        out.push_back({parts[i], parts[i + 1]});
    }
    return out;
}

// Fork/exec a command, capture stdout, wait. Returns false when the spawn
// itself failed. exitCode 127 = child could not exec (e.g. zenity missing).
bool runProcessCapture(const std::vector<std::string>& args,
                       std::string* out, int* exitCode)
{
    int fds[2] = {};
    if (pipe(fds) != 0) {
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    }

    close(fds[1]);
    out->clear();
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        out->append(buf, size_t(n));
    }
    close(fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }
    *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    // Trim the trailing newline zenity appends to the selection.
    while (!out->empty() && ((*out)[out->size() - 1] == '\n' ||
                             (*out)[out->size() - 1] == '\r')) {
        out->pop_back();
    }
    return true;
}

std::string zenityFileDialog(bool save, const std::string& title,
                             const std::string& filter,
                             const std::string& defaultExt)
{
    std::vector<std::string> args = {"zenity", "--file-selection", "--title", title};
    if (save) {
        args.push_back("--save");
        args.push_back("--confirm-overwrite");
    }
    for (const FilterPair& f : parseFilter(filter)) {
        if (!f.name.empty() && !f.pattern.empty()) {
            args.push_back("--file-filter");
            args.push_back(f.name + " | " + f.pattern);
        }
    }

    std::string out;
    int code = 1;
    if (!runProcessCapture(args, &out, &code)) {
        return {};
    }
    if (code != 0 || out.empty()) {
        return {}; // cancelled / unavailable
    }
    if (save && !defaultExt.empty() &&
        out.find('.') == std::string::npos) {
        out += "." + defaultExt; // GetSaveFileName lpstrDefExt semantics
    }
    return out;
}

} // namespace

std::unique_ptr<IDiskAccess> PlatformLinux::createDiskAccess()
{
    return std::make_unique<DiskAccessLinux>();
}

std::vector<VolumeInfo> PlatformLinux::enumerateVolumes()
{
    // Pure file reads (mountinfo/sysfs/statvfs) - safe on the UI thread.
    return enumerateVolumesLinux();
}

bool PlatformLinux::readVolumeFirstSector(const std::string& driveLetter,
                                          std::vector<uint8_t>* out,
                                          std::string* error)
{
    out->clear();
    // driveLetter holds a mount point on Linux. stat() resolves the mounted
    // filesystem's block device without probing any disk.
    struct stat st = {};
    if (::stat(driveLetter.c_str(), &st) != 0) {
        *error = "Cannot stat " + driveLetter + ": " + std::strerror(errno);
        return false;
    }
    char devNumber[32];
    std::snprintf(devNumber, sizeof(devNumber), "%u:%u",
                  static_cast<unsigned>(major(st.st_dev)),
                  static_cast<unsigned>(minor(st.st_dev)));
    const std::string node = devNodeForDevNumber(devNumber);
    if (node.empty()) {
        *error = "Cannot resolve the block device behind " + driveLetter;
        return false;
    }
    const int fd = open(node.c_str(), O_RDONLY);
    if (fd < 0) {
        *error = "Cannot open " + node + ": " + std::strerror(errno);
        return false;
    }
    out->resize(512);
    size_t got = 0;
    int readErr = EIO;
    while (got < 512) {
        const ssize_t n = pread(fd, out->data() + got, 512 - got, off_t(got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            readErr = errno;
            break;
        }
        if (n == 0) {
            break; // unexpected EOF
        }
        got += size_t(n);
    }
    close(fd);
    if (got != 512) {
        out->clear();
        *error = "Read failed on " + node + ": " + std::strerror(readErr);
        return false;
    }
    return true;
}

std::string PlatformLinux::openFileDialog(const std::string& title,
                                          const std::string& filter)
{
    return zenityFileDialog(false, title, filter, {});
}

std::string PlatformLinux::saveFileDialog(const std::string& title,
                                          const std::string& filter,
                                          const std::string& defaultExt)
{
    return zenityFileDialog(true, title, filter, defaultExt);
}

bool PlatformLinux::confirmDialog(const std::string& title, const std::string& text)
{
    // Blocking native dialog above the main window - the same tradeoff the
    // Win32 MessageBoxW makes (LESSONS.md #3 allows blocking for dialogs).
    // Exit code 0 = confirmed, 1 = cancelled, 127 = zenity missing (fail
    // closed: no confirm, no destructive action).
    std::string out;
    int code = 1;
    if (!runProcessCapture({"zenity", "--question", "--title", title,
                            "--text", text, "--width", "460"},
                           &out, &code)) {
        return false;
    }
    return code == 0;
}

FirmwareType PlatformLinux::firmwareType()
{
    // chroot/containers follow the same check (PLATFORM_SEAMS.md #2).
    struct stat st = {};
    return stat("/sys/firmware/efi", &st) == 0 && S_ISDIR(st.st_mode)
               ? FirmwareType::Uefi
               : FirmwareType::Bios;
}

float PlatformLinux::dpiScale() const
{
    // Pre-window default: the real per-monitor content scale is followed by
    // the main loop via GlfwWindow::dpiScale() (glfwGetWindowContentScale,
    // Wayland compositor scale / X11 Xft.dpi), which rebuilds style+fonts.
    return 1.0f;
}

std::string PlatformLinux::systemBcdPath()
{
    // Probe vfat mount points for the Microsoft BCD (letter-less ESP probing
    // - the Linux equivalent of probing every volume on Windows).
    for (const MountEntry& m : parseMountInfo()) {
        if (m.fsType != "vfat") {
            continue;
        }
        const std::string path = m.mountPoint + "/EFI/Microsoft/Boot/BCD";
        if (fileExists(path)) {
            return path;
        }
    }
    return {};
}

std::vector<std::string> PlatformLinux::candidateFontPaths()
{
    static const char* kCandidates[] = {
        // Noto Sans CJK SC (Debian/Arch package layouts)
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        // WenQuanYi
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
        // Droid Sans Fallback
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/droid/DroidSansFallbackFull.ttf",
    };
    std::vector<std::string> out;
    for (const char* p : kCandidates) {
        if (fileExists(p)) {
            out.push_back(p);
        }
    }
    return out;
}

bool PlatformLinux::isElevated()
{
    return geteuid() == 0;
}

bool PlatformLinux::restartElevated(const std::string& args)
{
    // pkexec /full/path/to/bootroll <args...>. pkexec stays alive for the
    // lifetime of the elevated child, so "declined" is detected via a short
    // exit (exit 126/127); a still-running pkexec counts as launched.
    const std::string exe = exePath();
    if (exe.empty()) {
        return false;
    }
    std::vector<std::string> argvStrings = {"pkexec", exe};
    {
        std::istringstream in(args);
        std::string token;
        while (in >> token) {
            argvStrings.push_back(token);
        }
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(argvStrings.size() + 1);
        for (const std::string& a : argvStrings) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    for (;;) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            return code == 0; // 126/127: dismissed or not authorized
        }
        if (r < 0) {
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return true; // authenticated and running (or a slow password)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

const char* PlatformLinux::elevateActionMsgId() const
{
    return "Restart as root";
}

const char* PlatformLinux::elevateDeclinedMsgId() const
{
    return "Restart as root was declined or failed.";
}

std::string PlatformLinux::exeDir() const
{
    return dirOf(exePath());
}

std::string PlatformLinux::iniPath()
{
    // Single-file portability: settings/log live next to the executable
    // (same semantics as the Windows build; no XDG).
    return exeDir() + "/" + kAppIniName;
}

std::string PlatformLinux::logPath()
{
    return exeDir() + "/" + kAppLogName;
}

std::unique_ptr<IPlatform> createPlatform()
{
    return std::make_unique<PlatformLinux>();
}

} // namespace bootroll
