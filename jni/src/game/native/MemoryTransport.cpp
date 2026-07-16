#include "game/native/MemoryTransport.h"

#include "game/native/KernelModuleLoader.h"
#include "paradise/paradise_api.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace lengjing::game::native {
namespace {

constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;
constexpr int kModeCount = 2;

enum class Mode : int {
    PureC = 0,
    Kernel = 1,
};

bool IsNumericName(const char* name) {
    if (name == nullptr || *name == '\0') return false;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

std::string ReadFirstToken(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string value;
    std::getline(input, value, '\0');
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

bool IsRemoteRangeValid(std::uintptr_t address, std::size_t size) {
    return address >= kMinimumRemoteAddress &&
        address < kMaximumRemoteAddress &&
        size > 0 &&
        size <= kMaximumRemoteAddress - address;
}

bool ReadDescriptorExact(
    int descriptor,
    std::uintptr_t address,
    void* destination,
    std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t result = pread64(
            descriptor,
            bytes + completed,
            size - completed,
            static_cast<off64_t>(address + completed));
        if (result > 0) {
            completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::string MutableName(std::string_view value) {
    return std::string(value.begin(), value.end());
}

}  // namespace

struct MemoryTransport::Impl {
    Mode mode = Mode::PureC;
    pid_t processId = -1;
    int descriptor = -1;
    paradise_driver* kernel = nullptr;
    bool open = false;
    std::mutex readMutex;

    ~Impl() {
        Close();
    }

    void Close() noexcept {
        std::lock_guard<std::mutex> lock(readMutex);
        if (descriptor >= 0) close(descriptor);
        descriptor = -1;
        delete kernel;
        kernel = nullptr;
        processId = -1;
        open = false;
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) {
        std::lock_guard<std::mutex> lock(readMutex);
        if (!open || destination == nullptr ||
            !IsRemoteRangeValid(address, size)) {
            return false;
        }

        if (mode == Mode::PureC) {
            return descriptor >= 0 &&
                ReadDescriptorExact(descriptor, address, destination, size);
        }
        return kernel != nullptr &&
            kernel->read_fast(address, destination, size);
    }

    std::uintptr_t ModuleBase(std::string_view moduleName) {
        std::lock_guard<std::mutex> lock(readMutex);
        if (!open || mode != Mode::Kernel || kernel == nullptr) return 0;
        const std::string name = MutableName(moduleName);
        return kernel->get_module_base(name.c_str());
    }
};

MemoryTransport::MemoryTransport() : impl_(std::make_unique<Impl>()) {}

MemoryTransport::~MemoryTransport() = default;

bool MemoryTransport::Open(int modeIndex,
                           pid_t processId,
                           std::string_view,
                           std::string& error) {
    Close();
    if (modeIndex < 0 || modeIndex >= kModeCount || processId <= 0) {
        error = "内存读取模式参数无效";
        return false;
    }

    impl_->mode = static_cast<Mode>(modeIndex);
    impl_->processId = processId;
    if (impl_->mode == Mode::PureC) {
        char path[64]{};
        std::snprintf(path, sizeof(path), "/proc/%d/mem", processId);
        impl_->descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (impl_->descriptor < 0) {
            error = "无法打开目标进程内存";
            impl_->Close();
            return false;
        }
    } else {
        if (!EnsureKernelDriverReady(error)) {
            impl_->Close();
            return false;
        }
        impl_->kernel = new (std::nothrow) paradise_driver();
        if (impl_->kernel == nullptr) {
            error = "无法创建内核读取接口";
            impl_->Close();
            return false;
        }
        impl_->kernel->initialize(processId);
    }

    impl_->open = true;
    error.clear();
    return true;
}

void MemoryTransport::Close() noexcept {
    if (impl_ != nullptr) impl_->Close();
}

bool MemoryTransport::Read(
    std::uintptr_t address,
    void* destination,
    std::size_t size) {
    return impl_ != nullptr && impl_->Read(address, destination, size);
}

std::uintptr_t MemoryTransport::ModuleBase(std::string_view moduleName) {
    return impl_ != nullptr && impl_->open
        ? impl_->ModuleBase(moduleName)
        : 0;
}

bool MemoryTransport::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->open;
}

pid_t FindProcessId(std::string_view processName) {
    if (processName.empty()) return -1;
    DIR* directory = opendir("/proc");
    if (directory == nullptr) return -1;

    pid_t result = -1;
    while (dirent* entry = readdir(directory)) {
        if (!IsNumericName(entry->d_name)) continue;
        const pid_t candidate =
            static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        if (candidate <= 0) continue;
        const std::string prefix = std::string("/proc/") + entry->d_name;
        std::string value = ReadFirstToken(prefix + "/cmdline");
        if (value.empty()) value = ReadFirstToken(prefix + "/comm");
        if (value == processName) {
            result = candidate;
            break;
        }
    }
    closedir(directory);
    return result;
}

bool IsProcessAlive(pid_t processId) {
    if (processId <= 0) return false;
    if (kill(processId, 0) == 0 || errno == EPERM) return true;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d", processId);
    struct stat info {};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

std::uintptr_t FindMappedModuleBase(
    pid_t processId,
    std::string_view moduleName) {
    if (processId <= 0 || moduleName.empty()) return 0;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", processId);
    std::ifstream input(path);
    if (!input) return 0;

    std::string line;
    while (std::getline(input, line)) {
        if (line.find(moduleName) == std::string::npos) continue;
        const std::size_t separator = line.find('-');
        if (separator == std::string::npos) continue;
        const std::string start = line.substr(0, separator);
        char* end = nullptr;
        const unsigned long long value =
            std::strtoull(start.c_str(), &end, 16);
        if (end != start.c_str() &&
            value >= kMinimumRemoteAddress &&
            value < kMaximumRemoteAddress) {
            return static_cast<std::uintptr_t>(value);
        }
    }
    return 0;
}

}  // namespace lengjing::game::native
