#include "game/native/KernelModuleLoader.h"

#include "game/native/KernelModuleCatalog.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lengjing::game::native {
namespace {

extern "C" {
extern const unsigned char _binary_kernel_script_510_pixel_start[];
extern const unsigned char _binary_kernel_script_510_pixel_end[];
extern const unsigned char _binary_kernel_script_510_start[];
extern const unsigned char _binary_kernel_script_510_end[];
extern const unsigned char _binary_kernel_script_515_pixel_start[];
extern const unsigned char _binary_kernel_script_515_pixel_end[];
extern const unsigned char _binary_kernel_script_515_start[];
extern const unsigned char _binary_kernel_script_515_end[];
extern const unsigned char _binary_kernel_script_61_start[];
extern const unsigned char _binary_kernel_script_61_end[];
extern const unsigned char _binary_kernel_script_66_start[];
extern const unsigned char _binary_kernel_script_66_end[];
extern const unsigned char _binary_kernel_script_612_start[];
extern const unsigned char _binary_kernel_script_612_end[];
}

constexpr unsigned long kDriverHandshakeA = 0x7A3B9C4DUL;
constexpr unsigned long kDriverHandshakeB = 0x2F8E1D6BUL;
constexpr std::string_view kPrimaryWorkDirectory = "/data/adb/lengjing";
constexpr std::string_view kFallbackWorkDirectory =
    "/data/local/tmp/lengjing";

struct EmbeddedScript final {
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;

    std::size_t Size() const noexcept {
        const std::uintptr_t first =
            reinterpret_cast<std::uintptr_t>(begin);
        const std::uintptr_t last =
            reinterpret_cast<std::uintptr_t>(end);
        return first != 0 && last >= first
            ? static_cast<std::size_t>(last - first)
            : 0;
    }

    bool Contains(std::string_view marker) const {
        const std::size_t size = Size();
        if (marker.empty() || marker.size() > size) return false;
        return std::search(
                   begin,
                   begin + size,
                   marker.begin(),
                   marker.end()) != begin + size;
    }

    bool IsValid() const {
        constexpr std::string_view kHeader = "#!/bin/sh";
        const std::size_t size = Size();
        return size > 64 && size >= kHeader.size() &&
            std::memcmp(begin, kHeader.data(), kHeader.size()) == 0 &&
            Contains("base64 -d") && Contains("insmod");
    }
};

struct ScriptRunResult final {
    bool started = false;
    int exitCode = -1;
    int systemError = 0;
};

EmbeddedScript FindEmbeddedScript(KernelModuleId module) noexcept {
    switch (module) {
        case KernelModuleId::Kernel510245:
            return {
                _binary_kernel_script_510_pixel_start,
                _binary_kernel_script_510_pixel_end};
        case KernelModuleId::Kernel510252:
            return {
                _binary_kernel_script_510_start,
                _binary_kernel_script_510_end};
        case KernelModuleId::Kernel515202:
            return {
                _binary_kernel_script_515_pixel_start,
                _binary_kernel_script_515_pixel_end};
        case KernelModuleId::Kernel515202Android13:
            return {
                _binary_kernel_script_515_start,
                _binary_kernel_script_515_end};
        case KernelModuleId::Kernel61166:
            return {
                _binary_kernel_script_61_start,
                _binary_kernel_script_61_end};
        case KernelModuleId::Kernel66127:
            return {
                _binary_kernel_script_66_start,
                _binary_kernel_script_66_end};
        case KernelModuleId::Kernel61276:
            return {
                _binary_kernel_script_612_start,
                _binary_kernel_script_612_end};
    }
    return {};
}

bool ProbeKernelDriver() noexcept {
    int descriptor = -1;
    syscall(
        SYS_reboot,
        kDriverHandshakeA,
        kDriverHandshakeB,
        0UL,
        &descriptor);
    if (descriptor < 0) return false;
    close(descriptor);
    return true;
}

std::string CurrentKernelRelease() {
    utsname info{};
    if (uname(&info) != 0) return {};
    return info.release;
}

bool EnsureDirectory(std::string_view path) {
    const std::string mutablePath(path);
    if (mkdir(mutablePath.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat info {};
    if (stat(mutablePath.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return false;
    }
    static_cast<void>(chmod(mutablePath.c_str(), 0700));
    return true;
}

std::string ResolveWorkDirectory() {
    if (EnsureDirectory(kPrimaryWorkDirectory)) {
        return std::string(kPrimaryWorkDirectory);
    }
    if (EnsureDirectory(kFallbackWorkDirectory)) {
        return std::string(kFallbackWorkDirectory);
    }
    return {};
}

bool WriteAll(int descriptor,
              const unsigned char* data,
              std::size_t size) {
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t result = write(
            descriptor,
            data + completed,
            size - completed);
        if (result > 0) {
            completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WriteScriptFile(const std::string& path,
                     const EmbeddedScript& script,
                     int& writeError) {
    writeError = 0;
    const int descriptor = open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0700);
    if (descriptor < 0) {
        writeError = errno;
        return false;
    }
    bool success = WriteAll(descriptor, script.begin, script.Size());
    if (success && fsync(descriptor) != 0) success = false;
    if (success && fchmod(descriptor, 0700) != 0) success = false;
    static_cast<void>(fchown(descriptor, 0, 0));
    if (!success) writeError = errno;
    if (close(descriptor) != 0 && success) {
        success = false;
        writeError = errno;
    }
    if (!success) static_cast<void>(unlink(path.c_str()));
    return success;
}

int WaitForChild(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

void SilenceChildOutput() {
    const int descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) return;
    static_cast<void>(dup2(descriptor, STDOUT_FILENO));
    static_cast<void>(dup2(descriptor, STDERR_FILENO));
    close(descriptor);
}

int RunChcon(const char* context, const std::string& path) {
    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        SilenceChildOutput();
        execl(
            "/system/bin/chcon",
            "chcon",
            context,
            path.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    return WaitForChild(child);
}

void ApplyScriptContext(const std::string& path) {
    if (RunChcon("u:object_r:system_file:s0", path) == 0) return;
    static_cast<void>(RunChcon("u:object_r:magisk_file:s0", path));
}

ScriptRunResult RunScript(const std::string& path) {
    ScriptRunResult result{};
    int outputPipe[2]{};
    if (pipe(outputPipe) != 0) {
        result.systemError = errno;
        return result;
    }

    const pid_t child = fork();
    if (child < 0) {
        result.systemError = errno;
        close(outputPipe[0]);
        close(outputPipe[1]);
        return result;
    }
    if (child == 0) {
        close(outputPipe[0]);
        static_cast<void>(dup2(outputPipe[1], STDOUT_FILENO));
        static_cast<void>(dup2(outputPipe[1], STDERR_FILENO));
        close(outputPipe[1]);
        execl(
            "/system/bin/sh",
            "sh",
            path.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }

    result.started = true;
    close(outputPipe[1]);
    char buffer[4096]{};
    while (true) {
        const ssize_t count = read(outputPipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) result.systemError = errno;
        break;
    }
    close(outputPipe[0]);
    result.exitCode = WaitForChild(child);
    if (result.exitCode < 0 && result.systemError == 0) {
        result.systemError = errno;
    }
    return result;
}

std::mutex& DriverInstallMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

bool EnsureKernelDriverReady(std::string& error) {
    std::lock_guard<std::mutex> lock(DriverInstallMutex());
    error.clear();
    if (ProbeKernelDriver()) return true;

    if (geteuid() != 0) {
        error = "driver_error: privilege_required";
        return false;
    }

    const std::string release = CurrentKernelRelease();
    if (release.empty()) {
        error = "driver_error: platform_query_failed";
        return false;
    }

    const KernelModuleVariant* selected =
        FindKernelModuleVariant(release);
    if (selected == nullptr) {
        error = "driver_error: platform_unsupported";
        return false;
    }

    const EmbeddedScript script = FindEmbeddedScript(selected->module);
    if (!script.IsValid()) {
        error = "driver_error: payload_invalid";
        return false;
    }

    const std::string workDirectory = ResolveWorkDirectory();
    if (workDirectory.empty()) {
        error = "driver_error: workspace_failed";
        return false;
    }

    char fileName[96]{};
    std::snprintf(
        fileName,
        sizeof(fileName),
        "%s/kernel_driver_%d.sh",
        workDirectory.c_str(),
        static_cast<int>(getpid()));
    const std::string scriptPath(fileName);

    int writeError = 0;
    if (!WriteScriptFile(scriptPath, script, writeError)) {
        error = "driver_error: payload_write_failed";
        return false;
    }
    ApplyScriptContext(scriptPath);
    const ScriptRunResult run = RunScript(scriptPath);
    static_cast<void>(unlink(scriptPath.c_str()));

    if (!run.started) {
        error = "driver_error: payload_launch_failed";
        return false;
    }
    if (run.exitCode != 0) {
        error = "driver_error: payload_execution_failed";
        return false;
    }
    if (!ProbeKernelDriver()) {
        error = "driver_error: endpoint_unavailable";
        return false;
    }
    return true;
}

}  // namespace lengjing::game::native
