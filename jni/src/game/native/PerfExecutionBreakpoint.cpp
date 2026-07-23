#include "game/native/PerfExecutionBreakpoint.h"

#include "game/native/MemoryTransport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <new>

#if defined(__linux__) && defined(__aarch64__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace perf_execution_breakpoint_internal {
namespace {

constexpr std::uint64_t kPerfSampleRegsAbi64 = 2;

template <typename Value>
bool ReadValue(const std::uint8_t*& cursor,
               std::size_t& remaining,
               Value& value) noexcept {
    if (remaining < sizeof(Value)) return false;
    std::memcpy(&value, cursor, sizeof(Value));
    cursor += sizeof(Value);
    remaining -= sizeof(Value);
    return true;
}

}  // namespace

bool ParseArm64SamplePayload(
    const std::uint8_t* payload,
    std::size_t payloadSize,
    std::uint64_t sampleType,
    std::uint64_t registerMask,
    ParsedSample& sample) noexcept {
    sample = {};
    if (payload == nullptr ||
        sampleType != kConfiguredSampleType ||
        (registerMask & kArm64RegisterMask) != kArm64RegisterMask) {
        return false;
    }

    std::size_t registerCount = 0;
    for (unsigned int index = 0; index < 64; ++index) {
        if ((registerMask & (UINT64_C(1) << index)) != 0) {
            ++registerCount;
        }
    }
    constexpr std::size_t kFixedPayloadSize =
        sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t) +
        sizeof(std::uint64_t);
    if (registerCount >
            (std::numeric_limits<std::size_t>::max() -
             kFixedPayloadSize) /
                sizeof(std::uint64_t) ||
        payloadSize !=
            kFixedPayloadSize +
                registerCount * sizeof(std::uint64_t)) {
        return false;
    }

    const std::uint8_t* cursor = payload;
    std::size_t remaining = payloadSize;
    std::uint32_t processId = 0;
    std::uint32_t threadId = 0;
    std::uint64_t abi = 0;
    if (!ReadValue(cursor, remaining, sample.ip) ||
        !ReadValue(cursor, remaining, processId) ||
        !ReadValue(cursor, remaining, threadId) ||
        !ReadValue(cursor, remaining, abi) ||
        abi != kPerfSampleRegsAbi64 ||
        processId == 0 || threadId == 0 ||
        processId >
            static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max()) ||
        threadId >
            static_cast<std::uint32_t>(
                std::numeric_limits<std::int32_t>::max())) {
        return false;
    }

    bool foundX0 = false;
    bool foundX23 = false;
    bool foundSp = false;
    bool foundPc = false;
    for (unsigned int index = 0; index < 64; ++index) {
        if ((registerMask & (UINT64_C(1) << index)) == 0) continue;
        std::uint64_t value = 0;
        if (!ReadValue(cursor, remaining, value)) return false;
        switch (index) {
            case kArm64RegisterX0:
                sample.x0 = value;
                foundX0 = true;
                break;
            case kArm64RegisterX23:
                sample.x23 = value;
                foundX23 = true;
                break;
            case kArm64RegisterSp:
                sample.sp = value;
                foundSp = true;
                break;
            case kArm64RegisterPc:
                sample.pc = value;
                foundPc = true;
                break;
            default:
                break;
        }
    }
    if (remaining != 0 || !foundX0 || !foundX23 || !foundSp ||
        !foundPc) {
        sample = {};
        return false;
    }
    sample.processId = static_cast<std::int32_t>(processId);
    sample.threadId = static_cast<std::int32_t>(threadId);
    return true;
}

bool IsTargetArm64Sample(const ParsedSample& sample,
                         std::int32_t processId,
                         std::int32_t threadId,
                         std::uintptr_t address) noexcept {
    return processId > 0 && threadId > 0 && address != 0 &&
        sample.processId == processId &&
        sample.threadId == threadId &&
        sample.ip == address &&
        sample.pc == address;
}

}  // namespace perf_execution_breakpoint_internal

#if defined(__linux__) && defined(__aarch64__)
namespace {

constexpr std::size_t kPerfDataPageCount = 2;
constexpr auto kThreadDiscoveryInterval = std::chrono::milliseconds(250);

bool IsNumericName(const char* name) noexcept {
    if (name == nullptr || *name == '\0') return false;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

std::uint64_t SaturatingAdd(std::uint64_t left,
                            std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

int PerfEventOpen(perf_event_attr* attributes, pid_t threadId) noexcept {
    int descriptor = -1;
    do {
        errno = 0;
        descriptor = static_cast<int>(syscall(
            __NR_perf_event_open,
            attributes,
            threadId,
            -1,
            -1,
            PERF_FLAG_FD_CLOEXEC));
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

}  // namespace

struct PerfExecutionBreakpoint::Impl {
    struct ThreadEvent {
        pid_t threadId = -1;
        int descriptor = -1;
        void* mapping = MAP_FAILED;
        std::size_t mappingSize = 0;
        std::size_t dataOffset = 0;
        std::size_t dataSize = 0;
        std::uint64_t hitCount = 0;
        std::uint64_t lostCount = 0;
    };

    struct SavedRecord {
        ExecutionBreakpointRecord record{};
        std::uint64_t updateSequence = 0;
    };

    pid_t processId = -1;
    std::uintptr_t address = 0;
    std::size_t pageSize = 0;
    std::map<pid_t, std::unique_ptr<ThreadEvent>> events;
    std::map<pid_t, SavedRecord> records;
    std::uint64_t updateSequence = 0;
    std::chrono::steady_clock::time_point nextDiscovery{};
    bool configured = false;

    ~Impl() {
        Remove();
    }

    static void CloseEvent(ThreadEvent& event) noexcept {
        if (event.descriptor >= 0) {
            static_cast<void>(
                ioctl(event.descriptor, PERF_EVENT_IOC_DISABLE, 0));
        }
        if (event.mapping != MAP_FAILED && event.mapping != nullptr &&
            event.mappingSize != 0) {
            static_cast<void>(
                munmap(event.mapping, event.mappingSize));
        }
        if (event.descriptor >= 0) {
            static_cast<void>(close(event.descriptor));
        }
        event = {};
    }

    int OpenEvent(pid_t threadId,
                  std::unique_ptr<ThreadEvent>& opened) noexcept {
        opened.reset();
        perf_event_attr attributes{};
        attributes.type = PERF_TYPE_BREAKPOINT;
        attributes.size = sizeof(attributes);
        attributes.sample_period = 1;
        attributes.sample_type =
            perf_execution_breakpoint_internal::kConfiguredSampleType;
        attributes.sample_regs_user =
            perf_execution_breakpoint_internal::kArm64RegisterMask;
        attributes.wakeup_events = 1;
        attributes.bp_type = HW_BREAKPOINT_X;
        attributes.bp_addr = static_cast<std::uint64_t>(address);
        attributes.bp_len = HW_BREAKPOINT_LEN_4;
        attributes.disabled = 1;
        attributes.exclude_kernel = 1;
        attributes.exclude_hv = 1;

        const int descriptor = PerfEventOpen(&attributes, threadId);
        if (descriptor < 0) {
            return errno != 0 ? -errno : -EIO;
        }

        auto event = std::unique_ptr<ThreadEvent>(
            new (std::nothrow) ThreadEvent());
        if (event == nullptr) {
            static_cast<void>(close(descriptor));
            return -ENOMEM;
        }
        event->threadId = threadId;
        event->descriptor = descriptor;
        const auto saved = records.find(threadId);
        if (saved != records.end()) {
            event->hitCount = saved->second.record.hitCount;
        }
        event->mappingSize =
            pageSize * (1 + kPerfDataPageCount);
        event->mapping = mmap(
            nullptr,
            event->mappingSize,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            descriptor,
            0);
        if (event->mapping == MAP_FAILED) {
            const int status = errno != 0 ? -errno : -EIO;
            CloseEvent(*event);
            return status;
        }

        auto* metadata =
            static_cast<perf_event_mmap_page*>(event->mapping);
        event->dataOffset = metadata->data_offset != 0
            ? static_cast<std::size_t>(metadata->data_offset)
            : pageSize;
        event->dataSize = metadata->data_size != 0
            ? static_cast<std::size_t>(metadata->data_size)
            : pageSize * kPerfDataPageCount;
        if (event->dataOffset < pageSize ||
            event->dataOffset > event->mappingSize ||
            event->dataSize == 0 ||
            event->dataSize > event->mappingSize - event->dataOffset ||
            (event->dataSize & (event->dataSize - 1)) != 0) {
            CloseEvent(*event);
            return -EPROTO;
        }

        if (ioctl(descriptor, PERF_EVENT_IOC_RESET, 0) != 0 ||
            ioctl(descriptor, PERF_EVENT_IOC_ENABLE, 0) != 0) {
            const int status = errno != 0 ? -errno : -EIO;
            CloseEvent(*event);
            return status;
        }
        opened = std::move(event);
        return 0;
    }

    bool EnumerateThreads(std::set<pid_t>& threadIds) noexcept {
        threadIds.clear();
        const std::string taskPath =
            "/proc/" + std::to_string(processId) + "/task";
        DIR* directory = opendir(taskPath.c_str());
        if (directory == nullptr) return false;
        errno = 0;
        while (dirent* entry = readdir(directory)) {
            if (!IsNumericName(entry->d_name)) continue;
            char* end = nullptr;
            errno = 0;
            const long value = std::strtol(entry->d_name, &end, 10);
            if (errno == 0 && end != entry->d_name &&
                *end == '\0' && value > 0 &&
                value <= std::numeric_limits<pid_t>::max()) {
                threadIds.insert(static_cast<pid_t>(value));
            }
        }
        const int readStatus = errno;
        const int closeStatus = closedir(directory);
        return readStatus == 0 && closeStatus == 0 &&
            !threadIds.empty();
    }

    bool DiscoverThreads(bool force) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < nextDiscovery) return true;
        nextDiscovery = now + kThreadDiscoveryInterval;

        std::set<pid_t> threadIds;
        if (!EnumerateThreads(threadIds)) return false;

        for (auto iterator = events.begin();
             iterator != events.end();) {
            if (threadIds.find(iterator->first) != threadIds.end()) {
                ++iterator;
                continue;
            }
            CloseEvent(*iterator->second);
            iterator = events.erase(iterator);
        }

        bool complete = true;
        for (const pid_t threadId : threadIds) {
            if (events.find(threadId) != events.end()) continue;
            std::unique_ptr<ThreadEvent> event;
            const int status = OpenEvent(threadId, event);
            if (status == 0 && event != nullptr) {
                events.emplace(threadId, std::move(event));
                continue;
            }
            if (status != -ESRCH && status != -ENOENT) {
                complete = false;
            }
        }
        return complete && !events.empty();
    }

    static bool CopyRingBytes(const ThreadEvent& event,
                              std::uint64_t position,
                              void* destination,
                              std::size_t size) noexcept {
        if (destination == nullptr || size > event.dataSize) return false;
        const auto* data =
            static_cast<const std::uint8_t*>(event.mapping) +
            event.dataOffset;
        const std::size_t offset =
            static_cast<std::size_t>(position & (event.dataSize - 1));
        const std::size_t first =
            std::min(size, event.dataSize - offset);
        std::memcpy(destination, data + offset, first);
        if (first != size) {
            std::memcpy(
                static_cast<std::uint8_t*>(destination) + first,
                data,
                size - first);
        }
        return true;
    }

    void SaveSample(
        ThreadEvent& event,
        const perf_execution_breakpoint_internal::ParsedSample& sample)
        noexcept {
        event.hitCount = SaturatingAdd(event.hitCount, 1);
        auto iterator = records.find(event.threadId);
        if (iterator == records.end() &&
            records.size() >= kExecutionBreakpointRecordLimit) {
            auto oldest = records.begin();
            for (auto candidate = records.begin();
                 candidate != records.end(); ++candidate) {
                if (candidate->second.updateSequence <
                    oldest->second.updateSequence) {
                    oldest = candidate;
                }
            }
            records.erase(oldest);
        }
        SavedRecord& saved = records[event.threadId];
        saved.record = {
            event.threadId,
            event.hitCount,
            static_cast<std::uintptr_t>(sample.pc),
            static_cast<std::uintptr_t>(sample.sp),
            static_cast<std::uintptr_t>(sample.x0),
            static_cast<std::uintptr_t>(sample.x23),
        };
        saved.updateSequence = SaturatingAdd(updateSequence, 1);
        updateSequence = saved.updateSequence;
    }

    bool DrainEvent(ThreadEvent& event) noexcept {
        if (event.mapping == MAP_FAILED || event.mapping == nullptr ||
            event.dataSize == 0) {
            return false;
        }
        auto* metadata =
            static_cast<perf_event_mmap_page*>(event.mapping);
        const std::uint64_t head =
            __atomic_load_n(&metadata->data_head, __ATOMIC_ACQUIRE);
        std::uint64_t tail =
            __atomic_load_n(&metadata->data_tail, __ATOMIC_RELAXED);
        if (head < tail || head - tail > event.dataSize) {
            __atomic_store_n(
                &metadata->data_tail, head, __ATOMIC_RELEASE);
            return false;
        }

        bool valid = true;
        std::uint64_t cursor = tail;
        while (cursor < head) {
            const std::uint64_t available = head - cursor;
            perf_event_header header{};
            if (available < sizeof(header) ||
                !CopyRingBytes(
                    event, cursor, &header, sizeof(header)) ||
                header.size < sizeof(header) ||
                header.size > event.dataSize ||
                header.size > available) {
                valid = false;
                cursor = head;
                break;
            }

            const std::size_t payloadSize =
                header.size - sizeof(header);
            if (header.type == PERF_RECORD_SAMPLE) {
                constexpr std::size_t kMaximumPayloadSize = 512;
                if (payloadSize > kMaximumPayloadSize) {
                    valid = false;
                    cursor = head;
                    break;
                }
                std::array<std::uint8_t, kMaximumPayloadSize> payload{};
                if (!CopyRingBytes(
                        event,
                        cursor + sizeof(header),
                        payload.data(),
                        payloadSize)) {
                    valid = false;
                    cursor = head;
                    break;
                }
                perf_execution_breakpoint_internal::ParsedSample sample{};
                if (!perf_execution_breakpoint_internal::
                        ParseArm64SamplePayload(
                            payload.data(),
                            payloadSize,
                            perf_execution_breakpoint_internal::
                                kConfiguredSampleType,
                            perf_execution_breakpoint_internal::
                                kArm64RegisterMask,
                        sample) ||
                    !perf_execution_breakpoint_internal::
                        IsTargetArm64Sample(
                            sample,
                            processId,
                            event.threadId,
                            address)) {
                    valid = false;
                    cursor = head;
                    break;
                }
                SaveSample(event, sample);
            } else if (header.type == PERF_RECORD_LOST) {
                struct LostPayload {
                    std::uint64_t id;
                    std::uint64_t lost;
                } lost{};
                if (payloadSize != sizeof(lost) ||
                    !CopyRingBytes(
                        event,
                        cursor + sizeof(header),
                        &lost,
                        sizeof(lost))) {
                    valid = false;
                    cursor = head;
                    break;
                }
                event.lostCount =
                    SaturatingAdd(event.lostCount, lost.lost);
                event.hitCount =
                    SaturatingAdd(event.hitCount, lost.lost);
            }
            cursor += header.size;
        }
        __atomic_store_n(
            &metadata->data_tail, cursor, __ATOMIC_RELEASE);
        return valid;
    }

    bool Configure(pid_t targetProcessId,
                   std::uintptr_t targetAddress) noexcept {
        if (targetProcessId <= 0 || targetAddress == 0 ||
            (targetAddress & 3U) != 0) {
            return false;
        }
        if (configured && processId == targetProcessId &&
            address == targetAddress) {
            return DiscoverThreads(true);
        }
        Remove();
        const long nativePageSize = sysconf(_SC_PAGESIZE);
        if (nativePageSize <= 0 ||
            static_cast<unsigned long>(nativePageSize) >
                std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        pageSize = static_cast<std::size_t>(nativePageSize);
        processId = targetProcessId;
        address = targetAddress;
        configured = true;
        if (!DiscoverThreads(true)) {
            Remove();
            return false;
        }
        return true;
    }

    bool ReadRecords(ExecutionBreakpointRecord* output,
                     std::size_t capacity,
                     std::size_t& recordsRead,
                     std::uintptr_t& hitAddress,
                     std::size_t& totalRecords) noexcept {
        recordsRead = 0;
        hitAddress = 0;
        totalRecords = 0;
        if (!configured || output == nullptr || capacity == 0) {
            return false;
        }

        bool valid = true;
        for (auto& entry : events) {
            if (!DrainEvent(*entry.second)) valid = false;
        }
        if (!DiscoverThreads(false)) valid = false;
        if (!valid) return false;

        totalRecords = std::min(
            records.size(), kExecutionBreakpointRecordLimit);
        const std::size_t requested =
            std::min(capacity, totalRecords);
        for (const auto& entry : records) {
            if (recordsRead == requested) break;
            output[recordsRead++] = entry.second.record;
        }
        hitAddress = address;
        return true;
    }

    bool Remove() noexcept {
        for (auto& entry : events) {
            CloseEvent(*entry.second);
        }
        events.clear();
        records.clear();
        processId = -1;
        address = 0;
        pageSize = 0;
        updateSequence = 0;
        nextDiscovery = {};
        configured = false;
        return true;
    }
};

#else

struct PerfExecutionBreakpoint::Impl {
    bool configured = false;
};

#endif

PerfExecutionBreakpoint::PerfExecutionBreakpoint()
    : impl_(std::make_unique<Impl>()) {}

PerfExecutionBreakpoint::~PerfExecutionBreakpoint() = default;

bool PerfExecutionBreakpoint::IsSupported() noexcept {
#if defined(__linux__) && defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

bool PerfExecutionBreakpoint::Configure(
    pid_t processId,
    std::uintptr_t address) noexcept {
#if defined(__linux__) && defined(__aarch64__)
    try {
        return impl_ != nullptr &&
            impl_->Configure(processId, address);
    } catch (...) {
        return false;
    }
#else
    static_cast<void>(processId);
    static_cast<void>(address);
    return false;
#endif
}

bool PerfExecutionBreakpoint::ReadRecords(
    ExecutionBreakpointRecord* records,
    std::size_t capacity,
    std::size_t& recordsRead,
    std::uintptr_t& hitAddress,
    std::size_t& totalRecords) noexcept {
#if defined(__linux__) && defined(__aarch64__)
    try {
        return impl_ != nullptr &&
            impl_->ReadRecords(
                records,
                capacity,
                recordsRead,
                hitAddress,
                totalRecords);
    } catch (...) {
        recordsRead = 0;
        hitAddress = 0;
        totalRecords = 0;
        return false;
    }
#else
    static_cast<void>(records);
    static_cast<void>(capacity);
    recordsRead = 0;
    hitAddress = 0;
    totalRecords = 0;
    return false;
#endif
}

bool PerfExecutionBreakpoint::Remove() noexcept {
#if defined(__linux__) && defined(__aarch64__)
    try {
        return impl_ == nullptr || impl_->Remove();
    } catch (...) {
        return false;
    }
#else
    if (impl_ != nullptr) impl_->configured = false;
    return true;
#endif
}

bool PerfExecutionBreakpoint::IsConfigured() const noexcept {
#if defined(__linux__) && defined(__aarch64__)
    return impl_ != nullptr && impl_->configured;
#else
    return false;
#endif
}

}  // namespace lengjing::game::native
