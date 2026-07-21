#include "ProgramMotionChannel.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#if defined(__ANDROID__) && defined(__aarch64__)
#include <asm/ptrace.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace lengjing::game::aim {
namespace {

struct ProgramMotionCommand {
    std::uint8_t enabled = 0;
    std::array<std::uint8_t, 3> padding{};
    float pitch = 0.0f;
    float yaw = 0.0f;
};

static_assert(sizeof(ProgramMotionCommand) == 12);

#if defined(__ANDROID__) && defined(__aarch64__)

constexpr std::string_view kServiceProcess = "system_server";
constexpr std::string_view kRemoteModulePath = "/data/app/libmotion_input.so";
constexpr std::string_view kRemoteModuleName = "libmotion_input.so";
constexpr std::uintptr_t kCommandPointerOffset = 0x12000;
constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000;

#include "ProgramMotionPayload.inc"

struct MapEntry {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    std::uintptr_t offset = 0;
    char permissions[5]{};
    std::string path;
};

bool IsNumericName(const char* text) {
    if (text == nullptr || *text == '\0') return false;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

pid_t FindProcess(std::string_view name) {
    DIR* directory = opendir("/proc");
    if (directory == nullptr) return -1;

    pid_t result = -1;
    while (dirent* entry = readdir(directory)) {
        if (!IsNumericName(entry->d_name)) continue;
        char path[64]{};
        std::snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) continue;
        char processName[128]{};
        const bool read = std::fgets(processName, sizeof(processName), file) != nullptr;
        std::fclose(file);
        if (!read) continue;
        processName[std::strcspn(processName, "\r\n")] = '\0';
        if (name == processName) {
            result = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
            break;
        }
    }
    closedir(directory);
    return result;
}

bool ParseMapLine(const char* line, MapEntry& output) {
    if (line == nullptr) return false;
    unsigned long start = 0;
    unsigned long end = 0;
    unsigned long offset = 0;
    int pathOffset = 0;
    char permissions[5]{};
    const int parsed = std::sscanf(
        line,
        "%lx-%lx %4s %lx %*s %*s %n",
        &start,
        &end,
        permissions,
        &offset,
        &pathOffset);
    if (parsed != 4 || start >= end) return false;

    output.start = static_cast<std::uintptr_t>(start);
    output.end = static_cast<std::uintptr_t>(end);
    output.offset = static_cast<std::uintptr_t>(offset);
    std::memcpy(output.permissions, permissions, sizeof(permissions));
    output.path.assign(line + std::max(pathOffset, 0));
    while (!output.path.empty() &&
           (output.path.back() == '\n' || output.path.back() == '\r' ||
            output.path.back() == ' ')) {
        output.path.pop_back();
    }
    return true;
}

template <typename Visitor>
bool VisitMaps(pid_t processId, Visitor&& visitor) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", processId);
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return false;
    char line[1024]{};
    bool matched = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        MapEntry entry{};
        if (ParseMapLine(line, entry) && visitor(entry)) {
            matched = true;
            break;
        }
    }
    std::fclose(file);
    return matched;
}

std::uintptr_t FindModuleBase(pid_t processId, std::string_view moduleName) {
    std::uintptr_t base = 0;
    VisitMaps(processId, [&](const MapEntry& entry) {
        if (entry.path.find(moduleName) == std::string::npos ||
            entry.start < entry.offset) {
            return false;
        }
        const std::uintptr_t candidate = entry.start - entry.offset;
        if (base == 0 || candidate < base) base = candidate;
        return false;
    });
    return base;
}

bool IsWritableRange(pid_t processId, std::uintptr_t address, std::size_t size) {
    if (address < kMinimumRemoteAddress || size == 0 ||
        address > UINTPTR_MAX - size) {
        return false;
    }
    return VisitMaps(processId, [&](const MapEntry& entry) {
        return entry.permissions[1] == 'w' &&
            address >= entry.start &&
            address + size <= entry.end;
    });
}

std::string BaseName(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos
        ? path
        : path.substr(slash + 1));
}

bool ReadRemote(pid_t processId,
                std::uintptr_t address,
                void* destination,
                std::size_t size) {
    if (destination == nullptr || size == 0) return false;
    iovec local{destination, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return syscall(
        SYS_process_vm_readv,
        processId,
        &local,
        1UL,
        &remote,
        1UL,
        0UL) == static_cast<ssize_t>(size);
}

bool WriteRemote(pid_t processId,
                 std::uintptr_t address,
                 const void* source,
                 std::size_t size) {
    if (source == nullptr || size == 0) return false;
    iovec local{const_cast<void*>(source), size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return syscall(
        SYS_process_vm_writev,
        processId,
        &local,
        1UL,
        &remote,
        1UL,
        0UL) == static_cast<ssize_t>(size);
}

bool WriteAll(int file, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(file, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool WriteModuleFile() {
    const std::string path(kRemoteModulePath);
    const int file = open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0755);
    if (file < 0) return false;
    const bool written = WriteAll(
        file, kProgramMotionModule, sizeof(kProgramMotionModule));
    if (written) fsync(file);
    close(file);
    if (!written || chmod(path.c_str(), 0755) != 0) {
        unlink(path.c_str());
        return false;
    }
    return true;
}

bool WaitForStop(pid_t processId,
                 std::chrono::milliseconds timeout,
                 int* stopSignal = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = waitpid(processId, &status, WNOHANG | __WALL);
        if (result == processId) {
            if (!WIFSTOPPED(status)) return false;
            if (stopSignal != nullptr) *stopSignal = WSTOPSIG(status);
            return true;
        }
        if (result < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool GetRegisters(pid_t processId, user_pt_regs& registers) {
    iovec vector{&registers, sizeof(registers)};
    return ptrace(
        PTRACE_GETREGSET,
        processId,
        reinterpret_cast<void*>(NT_PRSTATUS),
        &vector) == 0 &&
        vector.iov_len == sizeof(registers);
}

bool SetRegisters(pid_t processId, const user_pt_regs& registers) {
    iovec vector{const_cast<user_pt_regs*>(&registers), sizeof(registers)};
    return ptrace(
        PTRACE_SETREGSET,
        processId,
        reinterpret_cast<void*>(NT_PRSTATUS),
        &vector) == 0;
}

bool PtraceRead(pid_t processId,
                std::uintptr_t address,
                void* destination,
                std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        errno = 0;
        const long word = ptrace(
            PTRACE_PEEKDATA,
            processId,
            reinterpret_cast<void*>(address + offset),
            nullptr);
        if (word == -1 && errno != 0) return false;
        const std::size_t count = std::min(sizeof(word), size - offset);
        std::memcpy(bytes + offset, &word, count);
        offset += count;
    }
    return true;
}

bool PtraceWrite(pid_t processId,
                 std::uintptr_t address,
                 const void* source,
                 std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0;
    while (offset < size) {
        long word = 0;
        const std::size_t count = std::min(sizeof(word), size - offset);
        if (count != sizeof(word) &&
            !PtraceRead(processId, address + offset, &word, sizeof(word))) {
            return false;
        }
        std::memcpy(&word, bytes + offset, count);
        if (ptrace(
                PTRACE_POKEDATA,
                processId,
                reinterpret_cast<void*>(address + offset),
                reinterpret_cast<void*>(word)) != 0) {
            return false;
        }
        offset += count;
    }
    return true;
}

std::uintptr_t RemoteFunctionAddress(pid_t processId,
                                     std::uintptr_t localAddress) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(localAddress), &info) == 0 ||
        info.dli_fbase == nullptr || info.dli_fname == nullptr) {
        return 0;
    }
    const std::uintptr_t localBase =
        reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    if (localAddress < localBase) return 0;
    const std::uintptr_t remoteBase =
        FindModuleBase(processId, BaseName(info.dli_fname));
    return remoteBase == 0 ? 0 : remoteBase + (localAddress - localBase);
}

bool CallRemoteFunction(pid_t processId,
                        std::uintptr_t functionAddress,
                        const std::uintptr_t* parameters,
                        std::size_t parameterCount,
                        const user_pt_regs& baseRegisters,
                        user_pt_regs& resultRegisters,
                        bool& stopped) {
    if (!stopped || functionAddress == 0 || parameterCount > 8 ||
        (parameterCount != 0 && parameters == nullptr)) {
        return false;
    }

    user_pt_regs callRegisters = baseRegisters;
    for (std::size_t index = 0; index < parameterCount; ++index) {
        callRegisters.regs[index] = parameters[index];
    }
    callRegisters.regs[30] = 0;
    callRegisters.pc = functionAddress;
    if (!SetRegisters(processId, callRegisters) ||
        ptrace(PTRACE_CONT, processId, nullptr, nullptr) != 0) {
        return false;
    }

    stopped = false;
    int stopSignal = 0;
    if (!WaitForStop(
            processId, std::chrono::seconds(5), &stopSignal)) {
        return false;
    }
    stopped = true;
    if (stopSignal != SIGSEGV && stopSignal != SIGTRAP) {
        return false;
    }
    return GetRegisters(processId, resultRegisters);
}

bool LoadRemoteModule(pid_t processId) {
    const std::uintptr_t mmapAddress = RemoteFunctionAddress(
        processId, reinterpret_cast<std::uintptr_t>(&mmap));
    const std::uintptr_t munmapAddress = RemoteFunctionAddress(
        processId, reinterpret_cast<std::uintptr_t>(&munmap));
    const std::uintptr_t dlopenAddress = RemoteFunctionAddress(
        processId, reinterpret_cast<std::uintptr_t>(&dlopen));
    if (mmapAddress == 0 || dlopenAddress == 0) return false;
    if (ptrace(PTRACE_ATTACH, processId, nullptr, nullptr) != 0) return false;

    bool stopped = WaitForStop(processId, std::chrono::seconds(3));
    bool originalSaved = false;
    user_pt_regs original{};
    user_pt_regs current{};
    std::uintptr_t scratchAddress = 0;
    bool result = false;

    if (stopped && GetRegisters(processId, original)) {
        originalSaved = true;
        const std::string path(kRemoteModulePath);
        constexpr std::size_t kScratchSize = 4096;
        const std::array<std::uintptr_t, 6> mmapParameters{
            0,
            kScratchSize,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            static_cast<std::uintptr_t>(-1),
            0,
        };
        if (CallRemoteFunction(
                processId,
                mmapAddress,
                mmapParameters.data(),
                mmapParameters.size(),
                original,
                current,
                stopped)) {
            scratchAddress = current.regs[0];
        }
        if (scratchAddress != 0 && scratchAddress != UINTPTR_MAX &&
            IsWritableRange(processId, scratchAddress, kScratchSize) &&
            path.size() + 1 <= kScratchSize &&
            PtraceWrite(
                processId, scratchAddress, path.c_str(), path.size() + 1)) {
            const std::array<std::uintptr_t, 2> dlopenParameters{
                scratchAddress,
                RTLD_NOW | RTLD_GLOBAL,
            };
            if (CallRemoteFunction(
                    processId,
                    dlopenAddress,
                    dlopenParameters.data(),
                    dlopenParameters.size(),
                    original,
                    current,
                    stopped)) {
                result = current.regs[0] != 0;
            }
        }

        if (stopped && scratchAddress != 0 &&
            scratchAddress != UINTPTR_MAX && munmapAddress != 0) {
            const std::array<std::uintptr_t, 2> munmapParameters{
                scratchAddress,
                kScratchSize,
            };
            user_pt_regs ignored{};
            CallRemoteFunction(
                processId,
                munmapAddress,
                munmapParameters.data(),
                munmapParameters.size(),
                original,
                ignored,
                stopped);
        }
    }

    if (!stopped) {
        kill(processId, SIGSTOP);
        stopped = WaitForStop(processId, std::chrono::seconds(2));
    }
    if (stopped) {
        bool restored = true;
        if (originalSaved) restored = SetRegisters(processId, original) && restored;
        restored = ptrace(PTRACE_DETACH, processId, nullptr, nullptr) == 0 &&
            restored;
        result = result && restored;
    } else {
        ptrace(PTRACE_DETACH, processId, nullptr, nullptr);
        kill(processId, SIGCONT);
        result = false;
    }
    return result;
}

bool InjectProgramModule(pid_t processId) {
    if (!WriteModuleFile()) return false;
    const bool loaded = LoadRemoteModule(processId);
    const std::string path(kRemoteModulePath);
    unlink(path.c_str());
    return loaded;
}

#endif

}  // namespace

ProgramMotionChannel::~ProgramMotionChannel() {
    Close();
}

bool ProgramMotionChannel::Start() {
    Close();
#if defined(__ANDROID__) && defined(__aarch64__)
    pid_t processId = FindProcess(kServiceProcess);
    if (processId <= 0) return false;
    if (!Resolve(processId)) {
        if (!InjectProgramModule(processId)) return false;
        for (int attempt = 0; attempt < 20 && !Resolve(processId); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (commandAddress_ == 0) return false;

    processId_ = processId;
    ready_ = true;
    if (!WriteCommand(false, 0.0f, 0.0f)) {
        Close();
        return false;
    }
    ProgramMotionCommand verified{};
    if (!ReadRemote(processId_, commandAddress_, &verified, sizeof(verified)) ||
        verified.enabled != 0 || verified.pitch != 0.0f ||
        verified.yaw != 0.0f) {
        Close();
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool ProgramMotionChannel::Send(float pitch, float yaw) {
    if (!ready_) return false;
    if (!WriteCommand(true, pitch, yaw)) {
        ready_ = false;
        commandAddress_ = 0;
        processId_ = -1;
        return false;
    }
    return true;
}

bool ProgramMotionChannel::Release() noexcept {
    return ready_ && WriteCommand(false, 0.0f, 0.0f);
}

void ProgramMotionChannel::Close() noexcept {
    if (ready_) WriteCommand(false, 0.0f, 0.0f);
    ready_ = false;
    commandAddress_ = 0;
    processId_ = -1;
}

bool ProgramMotionChannel::Resolve(pid_t processId) {
#if defined(__ANDROID__) && defined(__aarch64__)
    const std::uintptr_t moduleBase =
        FindModuleBase(processId, kRemoteModuleName);
    if (moduleBase == 0) return false;
    std::uintptr_t commandAddress = 0;
    if (!ReadRemote(
            processId,
            moduleBase + kCommandPointerOffset,
            &commandAddress,
            sizeof(commandAddress)) ||
        !IsWritableRange(processId, commandAddress, sizeof(ProgramMotionCommand))) {
        return false;
    }
    processId_ = processId;
    commandAddress_ = commandAddress;
    return true;
#else
    static_cast<void>(processId);
    return false;
#endif
}

bool ProgramMotionChannel::WriteCommand(
    bool enabled,
    float pitch,
    float yaw) const {
#if defined(__ANDROID__) && defined(__aarch64__)
    if (processId_ <= 0 || commandAddress_ == 0) return false;
    const ProgramMotionCommand command{
        static_cast<std::uint8_t>(enabled ? 1 : 0),
        {},
        pitch,
        yaw};
    return WriteRemote(
        processId_,
        commandAddress_,
        &command,
        sizeof(command));
#else
    static_cast<void>(enabled);
    static_cast<void>(pitch);
    static_cast<void>(yaw);
    return false;
#endif
}

}  // namespace lengjing::game::aim
