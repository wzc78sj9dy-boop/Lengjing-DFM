#pragma once

#include "game/native/ThreadExecutionContextProvider.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace lengjing::game::native {

namespace thread_context_device_abi {

constexpr std::uint32_t kReadTpidrEl0Operation = 0x81BU;
constexpr std::uint32_t kReadPacKeysOperation = 0x81CU;
constexpr std::uint32_t kInstallApgaKeyOperation = 0x81DU;
constexpr std::size_t kLargeRequestCount = 0x9400U;
constexpr std::size_t kSmallRequestCount = 0x400U;

struct Envelope {
    std::uint32_t operation;
    std::uint32_t reserved;
    void* payload;
};

struct TpidrPayload {
    std::int32_t threadId;
    std::uint32_t reserved;
    std::uint64_t value;
};

struct PacKeysPayload {
    std::int32_t threadId;
    std::uint32_t reserved;
    ThreadPacKeys keys;
};

static_assert(sizeof(void*) == 8, "thread context device requires 64-bit");
static_assert(sizeof(Envelope) == 16, "bad device envelope layout");
static_assert(offsetof(Envelope, payload) == 8, "bad payload pointer offset");
static_assert(sizeof(TpidrPayload) == 16, "bad TPIDR payload layout");
static_assert(offsetof(TpidrPayload, value) == 8, "bad TPIDR output offset");
static_assert(sizeof(ExecutionPacKey128) == 16, "bad PAC key layout");
static_assert(sizeof(ThreadPacKeys) == 80, "bad PAC keys layout");
static_assert(sizeof(PacKeysPayload) == 88, "bad PAC payload layout");
static_assert(offsetof(PacKeysPayload, keys) == 8, "bad PAC keys offset");
static_assert(offsetof(PacKeysPayload, keys.apga) == 72,
              "bad APGA offset");

enum class WriteSuccessPolicy : std::uint8_t {
    Unspecified,
    ExactZero,
    ExactRequestCount,
    ZeroOrRequestCount,
    AnyNonNegative,
};

struct Profile {
    int fileDescriptor = -1;
    std::size_t requestCount = 0;
    WriteSuccessPolicy successPolicy = WriteSuccessPolicy::Unspecified;

    constexpr bool IsConfigured() const noexcept {
        return fileDescriptor >= 0 &&
            (requestCount == kLargeRequestCount ||
             requestCount == kSmallRequestCount) &&
            successPolicy != WriteSuccessPolicy::Unspecified;
    }
};

}  // namespace thread_context_device_abi

class ThreadContextDeviceTransport final
    : public ThreadExecutionContextReader {
public:
    using WriteInvoker = std::int64_t (*)(int fileDescriptor,
                                          const void* buffer,
                                          std::size_t count);

    explicit ThreadContextDeviceTransport(
        thread_context_device_abi::Profile profile,
        WriteInvoker invoker = nullptr) noexcept;

    ThreadContextReaderCapabilities Capabilities() const noexcept override;
    int ReadTpidrEl0(std::int32_t threadId,
                     std::uint64_t& value) override;
    int ReadPacKeys(std::int32_t threadId, ThreadPacKeys& keys) override;
    int InstallApgaKey(const ExecutionPacKey128& key);

    void UpdateFileDescriptor(int fileDescriptor) noexcept;
    thread_context_device_abi::Profile Configuration() const noexcept;

private:
    static std::int64_t InvokeWrite(int fileDescriptor,
                                    const void* buffer,
                                    std::size_t count);
    int Submit(std::uint32_t operation, void* payload);

    mutable std::mutex mutex_;
    thread_context_device_abi::Profile profile_;
    WriteInvoker invoker_;
};

}  // namespace lengjing::game::native
