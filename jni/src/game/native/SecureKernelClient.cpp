#include "game/native/SecureKernelClient.h"

#include "game/native/SecureKernelAssetPolicy.h"
#include "game/native/SecureKernelCommandAbi.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lengjing::game::native {
namespace {

using namespace secure_kernel_abi;

extern "C" {
extern const unsigned char _binary_compute_510_ko_start[];
extern const unsigned char _binary_compute_510_ko_end[];
extern const unsigned char _binary_compute_515_ko_start[];
extern const unsigned char _binary_compute_515_ko_end[];
extern const unsigned char _binary_compute_61_ko_start[];
extern const unsigned char _binary_compute_61_ko_end[];
extern const unsigned char _binary_compute_66_ko_start[];
extern const unsigned char _binary_compute_66_ko_end[];
extern const unsigned char _binary_compute_612_ko_start[];
extern const unsigned char _binary_compute_612_ko_end[];
}

constexpr std::string_view kPrimaryWorkDirectory = "/data/adb/lengjing";
constexpr std::string_view kFallbackWorkDirectory =
    "/data/local/tmp/lengjing";

struct EmbeddedModule final {
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;

    std::size_t Size() const noexcept {
        const auto first = reinterpret_cast<std::uintptr_t>(begin);
        const auto last = reinterpret_cast<std::uintptr_t>(end);
        return first != 0 && last >= first
            ? static_cast<std::size_t>(last - first)
            : 0;
    }

    bool IsValid() const noexcept {
        static constexpr std::array<unsigned char, 4> kElfMagic{
            0x7F, 'E', 'L', 'F'};
        return Size() > 0x800 &&
            std::memcmp(begin, kElfMagic.data(), kElfMagic.size()) == 0;
    }
};

EmbeddedModule SelectEmbeddedModule(std::string_view release) noexcept {
    switch (SelectSecureKernelAsset(release)) {
        case SecureKernelAsset::Kernel510:
            return {
                _binary_compute_510_ko_start,
                _binary_compute_510_ko_end};
        case SecureKernelAsset::Kernel515:
            return {
                _binary_compute_515_ko_start,
                _binary_compute_515_ko_end};
        case SecureKernelAsset::Kernel61:
            return {
                _binary_compute_61_ko_start,
                _binary_compute_61_ko_end};
        case SecureKernelAsset::Kernel66:
            return {
                _binary_compute_66_ko_start,
                _binary_compute_66_ko_end};
        case SecureKernelAsset::Kernel612:
            return {
                _binary_compute_612_ko_start,
                _binary_compute_612_ko_end};
        case SecureKernelAsset::None:
        default:
            return {};
    }
}

bool EnsureDirectory(std::string_view path) noexcept {
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
              std::size_t size) noexcept {
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t written = write(
            descriptor,
            data + completed,
            size - completed);
        if (written > 0) {
            completed += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool WriteModuleFile(const std::string& path,
                     const EmbeddedModule& module) noexcept {
    const int descriptor = open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600);
    if (descriptor < 0) return false;
    bool success = WriteAll(descriptor, module.begin, module.Size());
    if (success && fsync(descriptor) != 0) success = false;
    if (success && fchmod(descriptor, 0600) != 0) success = false;
    static_cast<void>(fchown(descriptor, 0, 0));
    if (close(descriptor) != 0) success = false;
    if (!success) static_cast<void>(unlink(path.c_str()));
    return success;
}

int WaitForChild(pid_t child) noexcept {
    int childStatus = 0;
    while (waitpid(child, &childStatus, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(childStatus)) return WEXITSTATUS(childStatus);
    if (WIFSIGNALED(childStatus)) return 128 + WTERMSIG(childStatus);
    return -1;
}

void SilenceChildOutput() noexcept {
    const int descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) return;
    static_cast<void>(dup2(descriptor, STDOUT_FILENO));
    static_cast<void>(dup2(descriptor, STDERR_FILENO));
    close(descriptor);
}

int RunModuleLoader(const std::string& path) noexcept {
    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        SilenceChildOutput();
        execl(
            "/system/bin/insmod",
            "insmod",
            path.c_str(),
            static_cast<char*>(nullptr));
        execl(
            "/system/bin/toybox",
            "toybox",
            "insmod",
            path.c_str(),
            static_cast<char*>(nullptr));
        execlp(
            "insmod",
            "insmod",
            path.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    return WaitForChild(child);
}

int ProbeDescriptorWithSyscall(long number) noexcept {
    int descriptor = -1;
    static_cast<void>(syscall(
        number,
        kHandshakeA,
        kHandshakeB,
        0ULL,
        &descriptor));
    return descriptor;
}

int ProbeDescriptor() noexcept {
    constexpr std::array<long, 5> kInitialSyscalls{
        18, 38, 42, 163, 164};
    for (const long number : kInitialSyscalls) {
        const int descriptor = ProbeDescriptorWithSyscall(number);
        if (descriptor >= 0) return descriptor;
    }
    for (long number = 244; number <= 259; ++number) {
        const int descriptor = ProbeDescriptorWithSyscall(number);
        if (descriptor >= 0) return descriptor;
    }
    for (long number = 295; number <= 423; ++number) {
        const int descriptor = ProbeDescriptorWithSyscall(number);
        if (descriptor >= 0) return descriptor;
    }
    return ProbeDescriptorWithSyscall(142);
}

std::mutex& InstallMutex() {
    static std::mutex mutex;
    return mutex;
}

bool InstallEmbeddedModule() noexcept {
    if (geteuid() != 0) return false;
    utsname systemInfo{};
    if (uname(&systemInfo) != 0) return false;
    const EmbeddedModule module =
        SelectEmbeddedModule(systemInfo.release);
    if (!module.IsValid()) return false;

    const std::string directory = ResolveWorkDirectory();
    if (directory.empty()) return false;
    const std::string path =
        directory + "/compute_" + std::to_string(getpid()) + ".ko";
    if (!WriteModuleFile(path, module)) return false;
    const int loadStatus = RunModuleLoader(path);
    static_cast<void>(unlink(path.c_str()));
    return loadStatus == 0;
}

template <typename Request>
int RawIoctl(int descriptor,
             std::uint32_t command,
             Request& request) noexcept {
    errno = 0;
    const int result = ioctl(
        descriptor,
        static_cast<unsigned long>(command),
        &request);
    if (result >= 0) return result;
    return -(errno != 0 ? errno : EIO);
}

int RawIoctlNoArgument(
    int descriptor,
    std::uint32_t command) noexcept {
    errno = 0;
    const int result = ioctl(
        descriptor,
        static_cast<unsigned long>(command),
        nullptr);
    if (result >= 0) return result;
    return -(errno != 0 ? errno : EIO);
}

template <std::size_t Capacity>
bool CopyName(std::string_view value, char (&destination)[Capacity]) noexcept {
    static_assert(Capacity > 1);
    if (value.empty()) return false;
    const std::size_t count = std::min(value.size(), Capacity - 1);
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
    return true;
}

}  // namespace

SecureKernelClient::~SecureKernelClient() {
    Close();
}

bool SecureKernelClient::EnsureConnected() noexcept {
    if (descriptor_ >= 0) return true;
    std::lock_guard<std::mutex> lock(InstallMutex());
    descriptor_ = ProbeDescriptor();
    if (descriptor_ >= 0) return true;
    if (!installAttempted_) {
        installAttempted_ = true;
        static_cast<void>(InstallEmbeddedModule());
    }
    descriptor_ = ProbeDescriptor();
    return descriptor_ >= 0;
}

bool SecureKernelClient::Read(pid_t processId,
                              std::uintptr_t address,
                              void* destination,
                              std::size_t size,
                              int& status) noexcept {
    status = -EINVAL;
    if (processId < 1 || address == 0 || destination == nullptr ||
        size == 0 || size > 0x400000) {
        return false;
    }
    if (!EnsureConnected()) return false;
    MemoryRequest request{};
    request.processId = processId;
    request.remoteAddress = address;
    request.size = size;
    request.localAddress =
        reinterpret_cast<std::uintptr_t>(destination);
    status = RawIoctl(descriptor_, kReadMemoryCommand, request);
    return status == 0;
}

std::uintptr_t SecureKernelClient::ModuleBase(
    pid_t processId,
    std::string_view moduleName,
    std::size_t& moduleSize,
    int& status) noexcept {
    moduleSize = 0;
    status = -EINVAL;
    if (processId < 1 || moduleName.empty()) return 0;
    if (!EnsureConnected()) return 0;
    ModuleRequest request{};
    request.processId = processId;
    if (!CopyName(moduleName, request.moduleName)) return 0;
    status = RawIoctl(descriptor_, kGetModuleCommand, request);
    if (status < 0 || request.base == 0) return 0;
    moduleSize = static_cast<std::size_t>(request.size);
    return static_cast<std::uintptr_t>(request.base);
}

bool SecureKernelClient::ExecuteComputation(
    pid_t processId,
    std::uint64_t data,
    std::uint64_t modifier,
    std::uint64_t& result,
    int& status) noexcept {
    result = 0;
    status = -EINVAL;
    if (processId < 1 || !EnsureConnected()) return false;
    ComputationRequest request{};
    request.processId = processId;
    request.data = data;
    request.modifier = modifier;
    status = RawIoctl(
        descriptor_,
        kExecuteComputationCommand,
        request);
    const bool accepted = request.supported != 0 && status >= 0;
    if (accepted) result = request.result;
    return accepted;
}

bool SecureKernelClient::QueryNamedThreadTls(
    pid_t processId,
    std::string_view threadName,
    pid_t& threadId,
    std::uintptr_t& tls,
    int& status) noexcept {
    threadId = -1;
    tls = 0;
    status = -EINVAL;
    if (processId < 1 || threadName.empty() || !EnsureConnected()) {
        return false;
    }

    ThreadTidRequest threadRequest{};
    threadRequest.processId = processId;
    threadRequest.threadId = 0;
    if (!CopyName(threadName, threadRequest.threadName)) return false;
    status = RawIoctl(
        descriptor_,
        kGetThreadTidCommand,
        threadRequest);
    if (status < 0 || threadRequest.threadId < 1) return false;

    ThreadTlsRequest tlsRequest{};
    tlsRequest.threadId = threadRequest.threadId;
    status = RawIoctl(
        descriptor_,
        kGetThreadTlsCommand,
        tlsRequest);
    if (status < 0 || tlsRequest.tls == 0) return false;

    threadId = threadRequest.threadId;
    tls = static_cast<std::uintptr_t>(
        NormalizeRemoteAddress(tlsRequest.tls));
    return tls != 0;
}

bool SecureKernelClient::QueryThreadStack(
    pid_t threadId,
    std::uintptr_t expectedTls,
    std::uintptr_t& low,
    std::uintptr_t& high,
    int& status) noexcept {
    low = 0;
    high = 0;
    status = -EINVAL;
    if (threadId < 1 || expectedTls == 0 || !EnsureConnected()) {
        return false;
    }

    ThreadStackRequest stackRequest{};
    stackRequest.threadId = threadId;
    status = RawIoctl(
        descriptor_,
        kGetThreadStackCommand,
        stackRequest);
    if (status < 0 ||
        !IsValidStackRange(stackRequest, expectedTls)) {
        return false;
    }

    low = static_cast<std::uintptr_t>(
        NormalizeRemoteAddress(stackRequest.low));
    high = static_cast<std::uintptr_t>(
        NormalizeRemoteAddress(stackRequest.high));
    return true;
}

bool SecureKernelClient::CaptureExecutionState(
    pid_t threadId,
    const SecureExecutionCapturePlan& plan,
    SecureExecutionCaptureResult& result,
    int& status) noexcept {
    using namespace std::chrono_literals;

    result = {};
    result.fpsr = plan.fallbackFpsr;
    result.fpcr = plan.fallbackFpcr;
    status = -EINVAL;
    if (threadId < 1 || plan.entryPc == 0 ||
        (plan.entryPc & 3U) != 0 || !EnsureConnected()) {
        return !plan.metadataValid;
    }

    ExecutionCapabilities capabilities{};
    status = RawIoctl(
        descriptor_,
        kGetExecutionCapabilitiesCommand,
        capabilities);
    if (status < 0 ||
        capabilities.breakpointCount == 0 ||
        capabilities.eventCount == 0 ||
        capabilities.supported == 0) {
        return !plan.metadataValid;
    }

    const bool triple =
        plan.metadataValid &&
        capabilities.breakpointCount > 2 &&
        capabilities.eventCount > 2 &&
        plan.storedRegister <= 30 &&
        plan.loadedRegister <= 30 &&
        plan.callPc != 0 &&
        plan.postLoadPc != 0 &&
        ((plan.callPc | plan.postLoadPc) & 3U) == 0;

    InstallExecutionBreakpointsRequest install{};
    install.threadId = static_cast<std::uint32_t>(threadId);
    install.count = triple ? 3U : 1U;
    install.breakpoints[0] = {
        4,
        4,
        static_cast<std::uint64_t>(plan.entryPc),
    };
    if (triple) {
        install.breakpoints[1] = {
            4,
            4,
            static_cast<std::uint64_t>(plan.callPc),
        };
        install.breakpoints[2] = {
            4,
            4,
            static_cast<std::uint64_t>(plan.postLoadPc),
        };
    }
    status = RawIoctl(
        descriptor_,
        kInstallExecutionBreakpointsCommand,
        install);
    if (status != 0) return !plan.metadataValid;

    std::unique_ptr<ExecutionPollBuffer> buffer(
        new (std::nothrow) ExecutionPollBuffer{});
    if (buffer == nullptr) {
        status = -ENOMEM;
        static_cast<void>(RawIoctlNoArgument(
            descriptor_, kRemoveExecutionBreakpointsCommand));
        return !plan.metadataValid;
    }

    std::array<std::uint64_t, 3> aggregate{};
    std::array<std::uint64_t, 3> eventCounter{};
    bool storedSeen = false;
    bool loadedSeen = false;
    const auto deadline =
        std::chrono::steady_clock::now() + 25ms;

    while (true) {
        std::memset(buffer.get(), 0, sizeof(*buffer));
        PollExecutionBreakpointsRequest poll{
            reinterpret_cast<std::uintptr_t>(buffer.get()),
            sizeof(*buffer),
            0,
        };
        status = RawIoctl(
            descriptor_,
            kPollExecutionBreakpointsCommand,
            poll);
        if (status != 0) break;

        const std::uint32_t slotCount =
            std::min(buffer->slotCount, install.count);
        if (slotCount != 0) {
            aggregate[0] += buffer->slots[0].aggregate;
            for (std::uint32_t slotIndex = 1;
                 slotIndex < slotCount;
                 ++slotIndex) {
                aggregate[1] +=
                    buffer->slots[slotIndex].aggregate;
            }
        }
        if (buffer->threadId ==
            static_cast<std::uint32_t>(threadId)) {
            for (std::uint32_t slotIndex = 0;
                 slotIndex < slotCount;
                 ++slotIndex) {
                const ExecutionEventSlot& slot =
                    buffer->slots[slotIndex];
                const ExecutionBreakpointSpec& spec =
                    install.breakpoints[slotIndex];
                if (slot.type != spec.type ||
                    slot.length != spec.length ||
                    slot.address != spec.address) {
                    continue;
                }
                const std::uint32_t eventCount =
                    std::min(slot.count, UINT32_C(64));
                for (std::uint32_t eventIndex = 0;
                     eventIndex < eventCount;
                     ++eventIndex) {
                    const ExecutionEvent& event =
                        slot.events[eventIndex];
                    if (event.slotTag != slotIndex ||
                        event.kind != 4 ||
                        event.pc0 != slot.address ||
                        event.pc1 != slot.address ||
                        event.pc2 != slot.address) {
                        continue;
                    }
                    eventCounter[slotIndex] += event.counter;
                    if (slotIndex == 0) {
                        result.entryCaptured = true;
                        result.fpsr = event.fpsr;
                        result.fpcr = event.fpcr;
                    } else if (
                        slotIndex == 1 &&
                        plan.storedRegister <= 30) {
                        result.storedValue =
                            static_cast<std::uint32_t>(
                                event.x[plan.storedRegister]);
                        storedSeen = result.storedValue != 0;
                    } else if (
                        slotIndex == 2 &&
                        plan.loadedRegister <= 30) {
                        result.loadedValue =
                            event.x[plan.loadedRegister];
                        loadedSeen = result.loadedValue != 0;
                    }
                }
            }
        }

        if (result.entryCaptured &&
            (!triple || (storedSeen && loadedSeen))) {
            break;
        }
        if ((buffer->flags & 2U) != 0 ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    const int removeStatus = RawIoctlNoArgument(
        descriptor_, kRemoveExecutionBreakpointsCommand);
    if (removeStatus != 0) status = removeStatus;

    result.entryCaptured =
        removeStatus == 0 &&
        result.entryCaptured &&
        eventCounter[0] != 0 &&
        aggregate[0] == 0;
    if (!result.entryCaptured) {
        result.fpsr = plan.fallbackFpsr;
        result.fpcr = plan.fallbackFpcr;
    }
    result.metadataCaptured =
        removeStatus == 0 &&
        triple &&
        storedSeen &&
        loadedSeen &&
        eventCounter[1] + eventCounter[2] != 0 &&
        aggregate[1] == 0;
    return !plan.metadataValid || result.metadataCaptured;
}

void SecureKernelClient::Close() noexcept {
    if (descriptor_ >= 0) {
        close(descriptor_);
        descriptor_ = -1;
    }
}

}  // namespace lengjing::game::native
