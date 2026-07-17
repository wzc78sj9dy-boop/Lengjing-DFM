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
constexpr std::size_t kMaximumScriptOutput = 32U * 1024U;
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
    std::string output;
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

std::string ErrnoMessage(int value) {
    const char* text = std::strerror(value);
    return text != nullptr ? std::string(text) : std::string("unknown error");
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

int RunChcon(const char* context, const std::string& path) {
    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
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

void AppendOutputTail(std::string& output,
                      const char* data,
                      std::size_t size) {
    if (size >= kMaximumScriptOutput) {
        output.assign(
            data + size - kMaximumScriptOutput,
            kMaximumScriptOutput);
        return;
    }
    if (output.size() + size > kMaximumScriptOutput) {
        output.erase(
            0,
            output.size() + size - kMaximumScriptOutput);
    }
    output.append(data, size);
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
            AppendOutputTail(
                result.output,
                buffer,
                static_cast<std::size_t>(count));
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

std::string OutputTail(std::string output) {
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' ||
            output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    constexpr std::size_t kMaximumErrorOutput = 1024;
    if (output.size() > kMaximumErrorOutput) {
        output.erase(0, output.size() - kMaximumErrorOutput);
    }
    return output;
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
        error = "需要 root 权限加载内核驱动";
        return false;
    }

    const std::string release = CurrentKernelRelease();
    if (release.empty()) {
        error = "无法读取设备内核版本";
        return false;
    }

    const KernelModuleVariant* selected =
        FindKernelModuleVariant(release);
    if (selected == nullptr) {
        error = "没有可用的内核驱动脚本: " + release;
        return false;
    }

    const EmbeddedScript script = FindEmbeddedScript(selected->module);
    if (!script.IsValid()) {
        error = "内置内核驱动脚本无效: " +
            std::string(selected->kernelVersion);
        return false;
    }

    const std::string workDirectory = ResolveWorkDirectory();
    if (workDirectory.empty()) {
        error = "无法创建内核驱动工作目录";
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
        error = "写入内核驱动脚本失败: " + ErrnoMessage(writeError);
        return false;
    }
    ApplyScriptContext(scriptPath);
    const ScriptRunResult run = RunScript(scriptPath);
    static_cast<void>(unlink(scriptPath.c_str()));

    if (!run.started) {
        error = "无法启动内核驱动脚本: " +
            ErrnoMessage(run.systemError);
        return false;
    }
    if (run.exitCode != 0) {
        error = "内核驱动脚本执行失败 (" + release + " -> " +
            std::string(selected->kernelVersion) + ", exit=" +
            std::to_string(run.exitCode) + ")";
        const std::string output = OutputTail(run.output);
        if (!output.empty()) error += ": " + output;
        return false;
    }
    if (!ProbeKernelDriver()) {
        error = "内核驱动脚本执行完成但接口探测失败: " + release;
        return false;
    }
    return true;
}

}  // namespace lengjing::game::native
