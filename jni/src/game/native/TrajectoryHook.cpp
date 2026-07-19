#include "game/native/TrajectoryHook.h"

#include "game/native/MemoryTransport.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace lengjing::game::native {
namespace {

constexpr std::size_t kStubSize = 0xC0;
constexpr std::size_t kJumpSize = 0x10;
constexpr std::size_t kCommandSize = sizeof(aim::TrackingCommand);
constexpr std::uint8_t kMaximumInstallAttempts = 3;
constexpr std::uintptr_t kCommandRva = 0x1D15B700ULL;
constexpr std::uintptr_t kFirstStubRva = 0x1D157800ULL;
constexpr std::uintptr_t kSecondStubRva = 0x1D1578C0ULL;
constexpr std::uintptr_t kFirstPatchRva = 0x0A98C134ULL;
constexpr std::uintptr_t kSecondPatchRva = 0x0A995EBCULL;

using StubImage = std::array<std::uint8_t, kStubSize>;
using JumpImage = std::array<std::uint8_t, kJumpSize>;

static_assert(kCommandSize == 12);
static_assert(offsetof(aim::TrackingCommand, pitch) == 0);
static_assert(offsetof(aim::TrackingCommand, yaw) == 4);
static_assert(offsetof(aim::TrackingCommand, flag) == 8);

constexpr std::array<std::uint8_t, kJumpSize> kFirstOriginal{
    0xE8, 0xA3, 0x02, 0xB9, 0xE8, 0x87, 0x41, 0xF9,
    0x08, 0x41, 0x17, 0x91, 0x00, 0x31, 0x00, 0x91,
};

constexpr std::array<std::uint8_t, kJumpSize> kSecondOriginal{
    0xFD, 0x7B, 0x03, 0xA9, 0xFD, 0xC3, 0x00, 0x91,
    0xFF, 0x83, 0x11, 0xD1, 0xF6, 0x13, 0x09, 0x91,
};

constexpr StubImage kFirstStubTemplate{
    0xE8, 0xA3, 0x02, 0xB9, 0xE8, 0x87, 0x41, 0xF9, 0x08, 0x41, 0x17, 0x91, 0x00, 0x31, 0x00, 0x91,
    0xE1, 0x0B, 0xBF, 0xA9, 0xE3, 0x13, 0xBF, 0xA9, 0xE5, 0x1B, 0xBF, 0xA9, 0xE7, 0x23, 0xBF, 0xA9,
    0xE9, 0x2B, 0xBF, 0xA9, 0xEB, 0x33, 0xBF, 0xA9, 0xED, 0x3B, 0xBF, 0xA9, 0xEF, 0x43, 0xBF, 0xA9,
    0xF2, 0x4F, 0xBF, 0xA9, 0xF4, 0x57, 0xBF, 0xA9, 0xF6, 0x5F, 0xBF, 0xA9, 0xF8, 0x67, 0xBF, 0xA9,
    0xFA, 0x6F, 0xBF, 0xA9, 0xFC, 0x77, 0xBF, 0xA9, 0xFE, 0x03, 0xBF, 0xA9, 0x30, 0x03, 0x00, 0x10,
    0x10, 0x02, 0x40, 0xF9, 0x0B, 0x22, 0x40, 0x39, 0x8B, 0x00, 0x00, 0x34, 0x09, 0x02, 0x40, 0xF9,
    0xEA, 0x07, 0x40, 0xF9, 0x49, 0x01, 0x00, 0xF9, 0xFE, 0x03, 0xC1, 0xA8, 0xFC, 0x77, 0xC1, 0xA8,
    0xFA, 0x6F, 0xC1, 0xA8, 0xF8, 0x67, 0xC1, 0xA8, 0xF6, 0x5F, 0xC1, 0xA8, 0xF4, 0x57, 0xC1, 0xA8,
    0xF2, 0x4F, 0xC1, 0xA8, 0xEF, 0x43, 0xC1, 0xA8, 0xED, 0x3B, 0xC1, 0xA8, 0xEB, 0x33, 0xC1, 0xA8,
    0xE9, 0x2B, 0xC1, 0xA8, 0xE7, 0x23, 0xC1, 0xA8, 0xE5, 0x1B, 0xC1, 0xA8, 0xE3, 0x13, 0xC1, 0xA8,
    0xE1, 0x0B, 0xC1, 0xA8, 0xB1, 0x00, 0x00, 0x10, 0x31, 0x02, 0x40, 0xF9, 0x20, 0x02, 0x1F, 0xD6,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
};

constexpr StubImage kSecondStubTemplate{
    0xFD, 0x7B, 0x03, 0xA9, 0xFD, 0xC3, 0x00, 0x91, 0xFF, 0x83, 0x11, 0xD1, 0xF6, 0x13, 0x09, 0x91,
    0xE1, 0x0B, 0xBF, 0xA9, 0xE3, 0x13, 0xBF, 0xA9, 0xE5, 0x1B, 0xBF, 0xA9, 0xE7, 0x23, 0xBF, 0xA9,
    0xE9, 0x2B, 0xBF, 0xA9, 0xEB, 0x33, 0xBF, 0xA9, 0xED, 0x3B, 0xBF, 0xA9, 0xEF, 0x43, 0xBF, 0xA9,
    0xF2, 0x4F, 0xBF, 0xA9, 0xF4, 0x57, 0xBF, 0xA9, 0xF6, 0x5F, 0xBF, 0xA9, 0xF8, 0x67, 0xBF, 0xA9,
    0xFA, 0x6F, 0xBF, 0xA9, 0xFC, 0x77, 0xBF, 0xA9, 0xFE, 0x03, 0xBF, 0xA9, 0x30, 0x03, 0x00, 0x10,
    0x10, 0x02, 0x40, 0xF9, 0x0B, 0x22, 0x40, 0x39, 0x6B, 0x00, 0x00, 0x34, 0x09, 0x02, 0x40, 0xF9,
    0xA9, 0x0B, 0x00, 0xF9, 0xFE, 0x03, 0xC1, 0xA8, 0xFC, 0x77, 0xC1, 0xA8, 0xFA, 0x6F, 0xC1, 0xA8,
    0xF8, 0x67, 0xC1, 0xA8, 0xF6, 0x5F, 0xC1, 0xA8, 0xF4, 0x57, 0xC1, 0xA8, 0xF2, 0x4F, 0xC1, 0xA8,
    0xEF, 0x43, 0xC1, 0xA8, 0xED, 0x3B, 0xC1, 0xA8, 0xEB, 0x33, 0xC1, 0xA8, 0xE9, 0x2B, 0xC1, 0xA8,
    0xE7, 0x23, 0xC1, 0xA8, 0xE5, 0x1B, 0xC1, 0xA8, 0xE3, 0x13, 0xC1, 0xA8, 0xE1, 0x0B, 0xC1, 0xA8,
    0xD1, 0x00, 0x00, 0x10, 0x31, 0x02, 0x40, 0xF9, 0x20, 0x02, 0x1F, 0xD6, 0x1F, 0x20, 0x03, 0xD5,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
};

bool IsNumericName(const char* name) {
    if (name == nullptr || *name == '\0') return false;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

bool ReadTaskState(pid_t processId, pid_t threadId, char& state) {
    state = '\0';
    char path[96]{};
    std::snprintf(
        path,
        sizeof(path),
        "/proc/%d/task/%d/status",
        processId,
        threadId);
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) return false;
    char line[256]{};
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "State:", 6) != 0) continue;
        const char* cursor = line + 6;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        state = *cursor;
        break;
    }
    std::fclose(file);
    return state != '\0';
}

bool StateCannotExecuteUserCode(char state) {
    return state == 'T' || state == 't' || state == 'D' ||
        state == 'Z' || state == 'X' || state == 'x';
}

bool AllThreadsStopped(pid_t processId) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/task", processId);
    DIR* directory = opendir(path);
    if (directory == nullptr) return false;
    bool found = false;
    bool stopped = true;
    while (dirent* entry = readdir(directory)) {
        if (!IsNumericName(entry->d_name)) continue;
        const pid_t threadId = static_cast<pid_t>(
            std::strtol(entry->d_name, nullptr, 10));
        if (threadId <= 0) continue;
        char state = '\0';
        if (!ReadTaskState(processId, threadId, state) ||
            !StateCannotExecuteUserCode(state)) {
            stopped = false;
            break;
        }
        found = true;
    }
    closedir(directory);
    return found && stopped;
}

class ProcessStopGuard final {
public:
    explicit ProcessStopGuard(pid_t processId) : processId_(processId) {}

    ~ProcessStopGuard() {
        if (resumeRequired_) {
            static_cast<void>(kill(processId_, SIGCONT));
        }
    }

    bool Stop() {
        if (processId_ <= 0) return false;
        if (AllThreadsStopped(processId_)) {
            stopped_ = true;
            return true;
        }
        if (kill(processId_, SIGSTOP) != 0) return false;
        resumeRequired_ = true;
        constexpr int kMaximumPolls = 500;
        for (int poll = 0; poll < kMaximumPolls; ++poll) {
            if (AllThreadsStopped(processId_)) {
                stopped_ = true;
                return true;
            }
            usleep(1000);
        }
        return false;
    }

    bool Stopped() const noexcept {
        return stopped_;
    }

private:
    pid_t processId_ = -1;
    bool resumeRequired_ = false;
    bool stopped_ = false;
};

bool AddRva(std::uintptr_t base,
            std::uintptr_t rva,
            std::uintptr_t& address) {
    if (base == 0 ||
        rva > std::numeric_limits<std::uintptr_t>::max() - base) {
        address = 0;
        return false;
    }
    address = base + rva;
    return address >= 0x10000000ULL && address < 0x10000000000ULL;
}

template <std::size_t Size>
bool ReadImage(MemoryTransport& memory,
               std::uintptr_t address,
               std::array<std::uint8_t, Size>& image) {
    image.fill(0);
    return memory.Read(address, image.data(), image.size());
}

bool WriteAndVerify(MemoryTransport& memory,
                    std::uintptr_t address,
                    const void* data,
                    std::size_t size) {
    if (data == nullptr || size == 0 || size > kStubSize ||
        !memory.Write(address, data, size)) {
        return false;
    }
    std::array<std::uint8_t, kStubSize> observed{};
    return memory.Read(address, observed.data(), size) &&
        std::memcmp(observed.data(), data, size) == 0;
}

template <std::size_t Size>
bool IsZero(const std::array<std::uint8_t, Size>& image) {
    for (const std::uint8_t value : image) {
        if (value != 0) return false;
    }
    return true;
}

void StoreWord(JumpImage& image,
               std::size_t offset,
               std::uint32_t word) {
    image[offset] = static_cast<std::uint8_t>(word);
    image[offset + 1] = static_cast<std::uint8_t>(word >> 8U);
    image[offset + 2] = static_cast<std::uint8_t>(word >> 16U);
    image[offset + 3] = static_cast<std::uint8_t>(word >> 24U);
}

JumpImage AssembleJump(std::uintptr_t destination) {
    JumpImage image{};
    const auto low = static_cast<std::uint32_t>(destination & 0xFFFFU);
    const auto middle =
        static_cast<std::uint32_t>((destination >> 16U) & 0xFFFFU);
    const auto high =
        static_cast<std::uint32_t>((destination >> 32U) & 0xFFFFU);
    StoreWord(image, 0, 0xD2800000U | (low << 5U) | 16U);
    StoreWord(image, 4, 0xF2A00000U | (middle << 5U) | 16U);
    StoreWord(image, 8, 0xF2C00000U | (high << 5U) | 16U);
    StoreWord(image, 12, 0xD61F0200U);
    return image;
}

StubImage PrepareStub(const StubImage& source,
                      std::uintptr_t commandAddress,
                      std::uintptr_t continuationAddress) {
    StubImage image = source;
    std::memcpy(image.data() + 0xB0, &commandAddress, sizeof(commandAddress));
    std::memcpy(
        image.data() + 0xB8,
        &continuationAddress,
        sizeof(continuationAddress));
    return image;
}

bool RestorePatchIfNeeded(MemoryTransport& memory,
                          std::uintptr_t address,
                          const JumpImage& jump,
                          const JumpImage& original) {
    JumpImage current{};
    if (!ReadImage(memory, address, current)) return false;
    if (current == original) return true;
    if (current != jump) return false;
    constexpr int kMaximumAttempts = 3;
    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        if (WriteAndVerify(
                memory, address, original.data(), original.size())) {
            return true;
        }
    }
    return false;
}

bool ForceRestorePatch(MemoryTransport& memory,
                       std::uintptr_t address,
                       const JumpImage& original) {
    constexpr int kMaximumAttempts = 3;
    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        if (WriteAndVerify(
                memory, address, original.data(), original.size())) {
            return true;
        }
    }
    return false;
}

std::chrono::steady_clock::duration InstallRetryDelay(
    std::uint8_t completedAttempts) {
    switch (completedAttempts) {
        case 1:
            return std::chrono::milliseconds(500);
        case 2:
            return std::chrono::seconds(2);
        default:
            return std::chrono::steady_clock::duration::max();
    }
}

}  // namespace

TrajectoryHook::~TrajectoryHook() {
    static_cast<void>(Shutdown());
}

bool TrajectoryHook::EnsureInstalled(
    MemoryTransport& memory,
    pid_t processId,
    std::uintptr_t moduleBase) noexcept {
    if (installed_ && memory_ == &memory && processId_ == processId &&
        moduleBase_ == moduleBase) {
        return true;
    }
    const bool sameContext = memory_ == &memory &&
        processId_ == processId && moduleBase_ == moduleBase;
    if (!sameContext && memory_ != nullptr && !Shutdown()) {
        return false;
    }

    if (!sameContext) {
        memory_ = &memory;
        processId_ = processId;
        moduleBase_ = moduleBase;
    }
    if (processId_ <= 0 ||
        !AddRva(moduleBase_, kCommandRva, commandAddress_) ||
        !AddRva(moduleBase_, kFirstStubRva, firstStubAddress_) ||
        !AddRva(moduleBase_, kSecondStubRva, secondStubAddress_) ||
        !AddRva(moduleBase_, kFirstPatchRva, firstPatchAddress_) ||
        !AddRva(moduleBase_, kSecondPatchRva, secondPatchAddress_)) {
        permanentInstallFailure_ = true;
        return false;
    }
    if (permanentInstallFailure_ ||
        (installAttempts_ >= kMaximumInstallAttempts &&
         !cleanupRequired_)) {
        return false;
    }
    if (!memory_->IsOpen() || !memory_->CanWrite()) return false;
    if (!memory_->UsesKernelBackend()) {
        permanentInstallFailure_ = true;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < retryAfter_) return false;

    if (installAttempts_ < kMaximumInstallAttempts) {
        ++installAttempts_;
    }
    const InstallResult result = Install();
    if (result == InstallResult::Installed) {
        installed_ = true;
        retryAfter_ = {};
        return true;
    }
    if (result == InstallResult::PermanentFailure) {
        permanentInstallFailure_ = true;
        return false;
    }
    if (cleanupRequired_) {
        retryAfter_ = now + InstallRetryDelay(
            installAttempts_ <= 1 ? 1 : 2);
    } else if (installAttempts_ < kMaximumInstallAttempts) {
        retryAfter_ = now + InstallRetryDelay(installAttempts_);
    }
    return false;
}

TrajectoryHook::InstallResult TrajectoryHook::Install() noexcept {
    if (memory_ == nullptr || !memory_->CanWrite() ||
        !memory_->UsesKernelBackend()) {
        return InstallResult::RetryableFailure;
    }

    const StubImage firstStub = PrepareStub(
        kFirstStubTemplate,
        commandAddress_,
        firstPatchAddress_ + kJumpSize);
    const StubImage secondStub = PrepareStub(
        kSecondStubTemplate,
        commandAddress_,
        secondPatchAddress_ + kJumpSize);
    const JumpImage firstJump = AssembleJump(firstStubAddress_);
    const JumpImage secondJump = AssembleJump(secondStubAddress_);

    JumpImage firstPatchCurrent{};
    JumpImage secondPatchCurrent{};
    StubImage firstStubCurrent{};
    StubImage secondStubCurrent{};
    const bool firstPatchRead =
        ReadImage(*memory_, firstPatchAddress_, firstPatchCurrent);
    if (firstPatchRead && firstPatchCurrent == firstJump) {
        cleanupRequired_ = true;
    }
    const bool secondPatchRead =
        ReadImage(*memory_, secondPatchAddress_, secondPatchCurrent);
    if (secondPatchRead && secondPatchCurrent == secondJump) {
        cleanupRequired_ = true;
    }
    if (!firstPatchRead || !secondPatchRead ||
        !ReadImage(*memory_, firstStubAddress_, firstStubCurrent) ||
        !ReadImage(*memory_, secondStubAddress_, secondStubCurrent)) {
        return InstallResult::RetryableFailure;
    }

    const bool firstStubKnown = IsZero(firstStubCurrent) ||
        firstStubCurrent == firstStub;
    const bool secondStubKnown = IsZero(secondStubCurrent) ||
        secondStubCurrent == secondStub;
    const bool firstPatchKnown = firstPatchCurrent == kFirstOriginal ||
        firstPatchCurrent == firstJump;
    const bool secondPatchKnown = secondPatchCurrent == kSecondOriginal ||
        secondPatchCurrent == secondJump;
    if (!firstStubKnown || !secondStubKnown ||
        !firstPatchKnown || !secondPatchKnown) {
        return InstallResult::PermanentFailure;
    }

    cleanupRequired_ = cleanupRequired_ ||
        firstPatchCurrent == firstJump ||
        secondPatchCurrent == secondJump;

    if (!ClearCommandBuffer()) return InstallResult::RetryableFailure;

    if (firstPatchCurrent == firstJump &&
        secondPatchCurrent == secondJump &&
        firstStubCurrent == firstStub &&
        secondStubCurrent == secondStub) {
        return InstallResult::Installed;
    }

    ProcessStopGuard stop(processId_);
    if (!stop.Stop() || !stop.Stopped()) {
        return InstallResult::RetryableFailure;
    }

    bool success = RestorePatchIfNeeded(
        *memory_, firstPatchAddress_, firstJump, kFirstOriginal);
    success = RestorePatchIfNeeded(
        *memory_, secondPatchAddress_, secondJump, kSecondOriginal) &&
        success;
    success = ClearCommandBuffer() && success;
    success = success && WriteAndVerify(
        *memory_, firstStubAddress_, firstStub.data(), firstStub.size());
    success = success && WriteAndVerify(
        *memory_, secondStubAddress_, secondStub.data(), secondStub.size());
    if (success) cleanupRequired_ = true;
    success = success && WriteAndVerify(
        *memory_, firstPatchAddress_, firstJump.data(), firstJump.size());
    success = success && WriteAndVerify(
        *memory_, secondPatchAddress_, secondJump.data(), secondJump.size());
    if (success) return InstallResult::Installed;

    static_cast<void>(ForceRestorePatch(
        *memory_, firstPatchAddress_, kFirstOriginal));
    static_cast<void>(ForceRestorePatch(
        *memory_, secondPatchAddress_, kSecondOriginal));
    JumpImage restoredFirst{};
    JumpImage restoredSecond{};
    const bool restored =
        ReadImage(*memory_, firstPatchAddress_, restoredFirst) &&
        ReadImage(*memory_, secondPatchAddress_, restoredSecond) &&
        restoredFirst == kFirstOriginal &&
        restoredSecond == kSecondOriginal;
    if (restored) {
        cleanupRequired_ = false;
        const StubImage zeros{};
        static_cast<void>(WriteAndVerify(
            *memory_, firstStubAddress_, zeros.data(), zeros.size()));
        static_cast<void>(WriteAndVerify(
            *memory_, secondStubAddress_, zeros.data(), zeros.size()));
        static_cast<void>(ClearCommandBuffer());
    }
    return restored
        ? InstallResult::RetryableFailure
        : InstallResult::PermanentFailure;
}

bool TrajectoryHook::ClearCommandBuffer() noexcept {
    if (memory_ == nullptr || !memory_->IsOpen() ||
        !memory_->CanWrite() || commandAddress_ == 0) {
        return false;
    }

    constexpr int kMaximumClearAttempts = 3;
    const std::int32_t disabled = 0;
    const aim::TrackingCommand emptyCommand{};
    for (int attempt = 0; attempt < kMaximumClearAttempts; ++attempt) {
        if (!WriteAndVerify(
                *memory_,
                commandAddress_ + offsetof(aim::TrackingCommand, flag),
                &disabled,
                sizeof(disabled))) {
            continue;
        }
        if (WriteAndVerify(
                *memory_,
                commandAddress_,
                &emptyCommand,
                kCommandSize)) {
            return true;
        }
    }
    return false;
}

bool TrajectoryHook::Publish(
    const aim::TrackingCommand& command) noexcept {
    if (!installed_ || memory_ == nullptr || command.flag == 0 ||
        !std::isfinite(command.pitch) || !std::isfinite(command.yaw)) {
        return false;
    }
    const std::int32_t disabled = 0;
    if (!WriteAndVerify(
            *memory_,
            commandAddress_ + offsetof(aim::TrackingCommand, flag),
            &disabled,
            sizeof(disabled))) {
        return false;
    }

    std::uint64_t angles = 0;
    static_assert(sizeof(angles) == sizeof(float) * 2);
    std::memcpy(&angles, &command.pitch, sizeof(angles));
    if (!WriteAndVerify(
            *memory_, commandAddress_, &angles, sizeof(angles))) {
        return false;
    }
    const std::int32_t enabled = 1;
    if (!WriteAndVerify(
            *memory_,
            commandAddress_ + offsetof(aim::TrackingCommand, flag),
            &enabled,
            sizeof(enabled))) {
        static_cast<void>(Disable());
        return false;
    }
    return true;
}

bool TrajectoryHook::Disable() noexcept {
    if (!cleanupRequired_) return true;
    return ClearCommandBuffer();
}

bool TrajectoryHook::Shutdown() noexcept {
    if (memory_ == nullptr) {
        ResetLocal();
        return true;
    }
    if (!cleanupRequired_ || !memory_->IsOpen() ||
        !IsProcessAlive(processId_)) {
        ResetLocal();
        return true;
    }

    bool success = ClearCommandBuffer();
    const JumpImage firstJump = AssembleJump(firstStubAddress_);
    const JumpImage secondJump = AssembleJump(secondStubAddress_);
    ProcessStopGuard stop(processId_);
    if (!stop.Stop() || !stop.Stopped()) {
        return false;
    }

    success = RestorePatchIfNeeded(
        *memory_, firstPatchAddress_, firstJump, kFirstOriginal) && success;
    success = RestorePatchIfNeeded(
        *memory_, secondPatchAddress_, secondJump, kSecondOriginal) && success;

    JumpImage firstCurrent{};
    JumpImage secondCurrent{};
    const bool restored =
        ReadImage(*memory_, firstPatchAddress_, firstCurrent) &&
        ReadImage(*memory_, secondPatchAddress_, secondCurrent) &&
        firstCurrent == kFirstOriginal &&
        secondCurrent == kSecondOriginal;
    if (!restored) {
        return false;
    }

    const StubImage zeros{};
    success = WriteAndVerify(
        *memory_, firstStubAddress_, zeros.data(), zeros.size()) && success;
    success = WriteAndVerify(
        *memory_, secondStubAddress_, zeros.data(), zeros.size()) && success;
    success = ClearCommandBuffer() && success;
    if (!success) {
        return false;
    }
    ResetLocal();
    return true;
}

bool TrajectoryHook::Installed() const noexcept {
    return installed_;
}

void TrajectoryHook::ResetLocal() noexcept {
    memory_ = nullptr;
    processId_ = -1;
    moduleBase_ = 0;
    commandAddress_ = 0;
    firstStubAddress_ = 0;
    secondStubAddress_ = 0;
    firstPatchAddress_ = 0;
    secondPatchAddress_ = 0;
    retryAfter_ = {};
    installAttempts_ = 0;
    permanentInstallFailure_ = false;
    cleanupRequired_ = false;
    installed_ = false;
}

}  // namespace lengjing::game::native
