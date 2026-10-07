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
#include <set>
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

std::vector<FontCandidate> PlatformLinux::candidateFonts()
{
    std::vector<FontCandidate> out;

    // Files(+faces) that genuinely cover Simplified Chinese per fontconfig.
    // fc-match alone is a FUZZY matcher: with no zh font installed it happily
    // returns DejaVu, which ImGui would load and render as tofu boxes (and
    // the embedded-font fallback would never trigger). Verifying the pick
    // against fc-list :lang=zh-cn keeps that path honest: no zh coverage ->
    // no candidate -> embedded NotoSansSC subset.
    std::set<std::pair<std::string, int>> zh;
    {
        std::string list;
        int code = 1;
        if (runProcessCapture(
                {"fc-list", ":lang=zh-cn", "--format", "%{file}|%{index}\n"},
                &list, &code) &&
            code == 0) {
            size_t pos = 0;
            while (pos < list.size()) {
                size_t eol = list.find('\n', pos);
                if (eol == std::string::npos) {
                    eol = list.size();
                }
                const std::string entry = list.substr(pos, eol - pos);
                pos = eol + 1;
                const size_t bar = entry.rfind('|');
                if (bar == std::string::npos || entry.empty()) {
                    continue;
                }
                std::string f = entry.substr(0, bar);
                const int idx = std::atoi(entry.c_str() + bar + 1);
                if (!f.empty()) {
                    zh.emplace(std::move(f), idx);
                }
            }
        }
    }

    // Resolve the SC face dynamically: fc-match returns the file AND the face
    // index within TTC collections. The upstream Noto Sans CJK ttc face order
    // is JP, KR, SC, TC, HK, Mono... (verified via fc-scan), so ImGui's
    // default face 0 would render Japanese glyph shapes for zh_CN text.
    if (!zh.empty()) {
        const char* patterns[] = {
            "Noto Sans CJK SC:lang=zh-cn", // preferred family (exact face)
            ":lang=zh-cn",                 // any zh-capable font (WQY, ...)
        };
        for (const char* pattern : patterns) {
            std::string line;
            int code = 1;
            if (runProcessCapture(
                    {"fc-match", "-f", "%{file}|%{index}", pattern}, &line,
                    &code) &&
                code == 0) {
                const size_t bar = line.rfind('|');
                if (bar != std::string::npos) {
                    std::string f = line.substr(0, bar);
                    const int idx = std::atoi(line.c_str() + bar + 1);
                    if (!f.empty() && zh.count({f, idx}) > 0 &&
                        fileExists(f)) {
                        out.push_back({std::move(f), idx < 0 ? 0 : idx});
                        break;
                    }
                }
            }
        }
    }

    // Static fallback when fontconfig is unavailable (bare containers).
    // kNotoCjkScFace: SC face within upstream Noto Sans CJK ttc collections.
    constexpr int kNotoCjkScFace = 2;
    static const struct {
        const char* path;
        int face;
    } kStatic[] = {
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", kNotoCjkScFace},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", kNotoCjkScFace},
        {"/usr/share/fonts/noto-cjk/NotoSansCJKsc-Regular.otf", 0},
        {"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", kNotoCjkScFace},
        {"/usr/share/fonts/noto-cjk/NotoSansSC-Regular.otf", 0},
        // WenQuanYi
        {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
        {"/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", 0},
        {"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 0},
        {"/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc", 0},
        // Droid Sans Fallback
        {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 0},
        {"/usr/share/fonts/droid/DroidSansFallbackFull.ttf", 0},
    };
    for (const auto& k : kStatic) {
        if (fileExists(k.path)) {
            out.push_back({k.path, k.face});
        }
    }
    return out;
}

bool PlatformLinux::isElevated()
{
    return geteuid() == 0;
}

namespace {

// True when `name` resolves to an executable file on PATH.
bool executableInPath(const char* name)
{
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }
    const std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t colon = p.find(':', start);
        const std::string dir =
            p.substr(start, colon == std::string::npos ? std::string::npos
                                                       : colon - start);
        const std::string candidate = dir + "/" + name;
        struct stat st = {};
        if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }
    return false;
}

// Classify a fast pkexec exit for the UI. Known categories are English msgids
// (translatable via i18n passthrough when absent from the catalog); unknown
// ones surface the raw stderr. The "not owned by root" category is the
// portable-build case on older polkit (Ubuntu 22.04 floor ships 0.105): its
// pkexec refuses any program that is not root-owned, which a tar.gz extract
// or an AppImage-style user path always is.
std::string classifyElevateFailure(int code, std::string stderrText)
{
    while (!stderrText.empty() &&
           (stderrText.back() == '\n' || stderrText.back() == '\r')) {
        stderrText.pop_back();
    }
    if (stderrText.find("not owned by root") != std::string::npos) {
        return "pkexec refused the executable: it is not owned by root. Install bootroll system-wide (deb/rpm/arch), move it to a root-owned path, or start it manually with sudo.";
    }
    if (stderrText.find("authentication agent") != std::string::npos) {
        return "No polkit authentication agent is running in this session; pkexec cannot ask for the password.";
    }
    if (stderrText.find("Not authorized") != std::string::npos) {
        return stderrText; // dismissed / wrong password: declined semantics
    }
    if (!stderrText.empty()) {
        return stderrText;
    }
    return "pkexec exited with status " + std::to_string(code) + ".";
}

} // namespace

bool PlatformLinux::restartElevated(const std::string& args)
{
    m_lastElevateError.clear();

    // pkexec [env VARS...] /full/path/to/bootroll <args...>. pkexec stays
    // alive for the lifetime of the elevated child, so "declined" is detected
    // via a short exit; a still-running pkexec counts as launched.
    const std::string exe = exePath();
    if (exe.empty()) {
        m_lastElevateError =
            "Cannot resolve the executable path (/proc/self/exe).";
        return false;
    }
    if (!executableInPath("pkexec")) {
        m_lastElevateError = "pkexec is not installed (policykit-1 / polkit package).";
        return false;
    }

    // pkexec resets the child environment to a minimal set (old polkit on the
    // 22.04 floor in particular): without DISPLAY / WAYLAND_DISPLAY the
    // elevated instance dies before opening a window and the restart looks
    // like a silent no-op. Re-export the display/session variables via
    // `pkexec env`; only the ones actually present in this process are sent.
    std::vector<std::string> argvStrings = {"pkexec", "env"};
    for (const char* name : {"DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY",
                             "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
                             "XDG_SESSION_TYPE"}) {
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') {
            argvStrings.push_back(std::string(name) + "=" + v);
        }
    }
    argvStrings.push_back(exe);
    {
        std::istringstream in(args);
        std::string token;
        while (in >> token) {
            argvStrings.push_back(token);
        }
    }

    // Child stderr is captured for failure diagnostics (previously it went to
    // the app's stderr and was lost for GUI runs). O_NONBLOCK + drain inside
    // the wait loop keeps the child from blocking on a full pipe.
    int errPipe[2] = {};
    if (pipe(errPipe) != 0) {
        m_lastElevateError = "pipe() failed.";
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(errPipe[0]);
        close(errPipe[1]);
        m_lastElevateError = "fork() failed.";
        return false;
    }
    if (pid == 0) {
        close(errPipe[0]);
        dup2(errPipe[1], STDERR_FILENO);
        close(errPipe[1]);
        std::vector<char*> argv;
        argv.reserve(argvStrings.size() + 1);
        for (const std::string& a : argvStrings) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127); // exec failed (pkexec missing is pre-checked; belt+braces)
    }
    close(errPipe[1]);
    fcntl(errPipe[0], F_SETFL, fcntl(errPipe[0], F_GETFL) | O_NONBLOCK);

    std::string childErr;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    bool launched = false;
    int exitCode = -1;
    for (;;) {
        char buf[512];
        for (;;) {
            const ssize_t n = read(errPipe[0], buf, sizeof(buf));
            if (n > 0) {
                childErr.append(buf, size_t(n));
            } else {
                break; // EAGAIN / EOF
            }
        }
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (r < 0) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            launched = true; // authenticated and running (or a slow password)
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(errPipe[0]);

    if (launched) {
        return true;
    }
    if (exitCode == 0) {
        return true; // pkexec reports success
    }
    m_lastElevateError = classifyElevateFailure(exitCode, childErr);
    return false;
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
