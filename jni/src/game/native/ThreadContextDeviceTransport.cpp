#include "game/native/ThreadContextDeviceTransport.h"

#include <cerrno>

#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace {

bool IsPositiveId(std::int32_t value) {
    return value > 0;
}

bool IsSuccessfulWrite(
    std::int64_t result,
    const thread_context_device_abi::Profile& profile) {
    using thread_context_device_abi::WriteSuccessPolicy;
    switch (profile.successPolicy) {
        case WriteSuccessPolicy::Unspecified:
            return false;
        case WriteSuccessPolicy::ExactZero:
            return result == 0;
        case WriteSuccessPolicy::ExactRequestCount:
            return result == static_cast<std::int64_t>(profile.requestCount);
        case WriteSuccessPolicy::ZeroOrRequestCount:
            return result == 0 ||
                result == static_cast<std::int64_t>(profile.requestCount);
        case WriteSuccessPolicy::AnyNonNegative:
            return result >= 0;
    }
    return false;
}

bool IsNegotiationFailure(int result) {
    return result == -EINVAL || result == -EPROTO ||
        result == -EMSGSIZE || result == -E2BIG ||
        result == -ENOTTY;
}

std::size_t AlternateRequestCount(std::size_t requestCount) {
    using namespace thread_context_device_abi;
    return requestCount == kLargeRequestCount
        ? kSmallRequestCount
        : kLargeRequestCount;
}

}  // namespace

ThreadContextDeviceTransport::ThreadContextDeviceTransport(
    thread_context_device_abi::Profile profile,
    WriteInvoker invoker) noexcept
    : profile_(profile),
      preferredRequestCount_(profile.requestCount),
      negotiationComplete_(
          profile.requestCountMode ==
          thread_context_device_abi::RequestCountMode::Fixed),
      invoker_(invoker != nullptr ? invoker : &InvokeWrite) {}

ThreadContextReaderCapabilities
ThreadContextDeviceTransport::Capabilities() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool configured = profile_.IsConfigured();
    return {configured, configured, configured};
}

int ThreadContextDeviceTransport::ReadTpidrEl0(
    std::int32_t threadId,
    std::uint64_t& value) {
    if (!IsPositiveId(threadId)) return -EINVAL;
    thread_context_device_abi::TpidrPayload payload{
        threadId,
        0,
        0,
    };
    const int result = Submit(
        thread_context_device_abi::kReadTpidrEl0Operation,
        &payload);
    if (result != 0) return result;
    if (payload.threadId != threadId) return -EPROTO;
    value = payload.value;
    return 0;
}

int ThreadContextDeviceTransport::ReadPacKeys(
    std::int32_t threadId,
    ThreadPacKeys& keys) {
    if (!IsPositiveId(threadId)) return -EINVAL;
    thread_context_device_abi::PacKeysPayload payload{};
    payload.threadId = threadId;
    const int result = Submit(
        thread_context_device_abi::kReadPacKeysOperation,
        &payload);
    if (result != 0) return result;
    if (payload.threadId != threadId) return -EPROTO;
    keys = payload.keys;
    return 0;
}

int ThreadContextDeviceTransport::InstallApgaKey(
    const ExecutionPacKey128& key) {
    ExecutionPacKey128 payload = key;
    return Submit(
        thread_context_device_abi::kInstallApgaKeyOperation,
        &payload);
}

void ThreadContextDeviceTransport::UpdateFileDescriptor(
    int fileDescriptor) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    profile_.fileDescriptor = fileDescriptor;
    if (profile_.requestCountMode ==
        thread_context_device_abi::RequestCountMode::Negotiate) {
        profile_.requestCount = preferredRequestCount_;
        negotiationComplete_ = false;
    }
}

thread_context_device_abi::Profile
ThreadContextDeviceTransport::Configuration() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return profile_;
}

std::int64_t ThreadContextDeviceTransport::InvokeWrite(
    int fileDescriptor,
    const void* buffer,
    std::size_t count) {
#if defined(__ANDROID__) || defined(__linux__)
    return static_cast<std::int64_t>(::write(fileDescriptor, buffer, count));
#else
    (void)fileDescriptor;
    (void)buffer;
    (void)count;
    errno = ENOSYS;
    return -1;
#endif
}

int ThreadContextDeviceTransport::SubmitWithRequestCount(
    const thread_context_device_abi::Envelope& envelope,
    std::size_t requestCount) {
    auto attempt = profile_;
    attempt.requestCount = requestCount;
    errno = 0;
    const std::int64_t result = invoker_(
        profile_.fileDescriptor,
        &envelope,
        requestCount);
    if (result < 0) return errno != 0 ? -errno : -EIO;
    return IsSuccessfulWrite(result, attempt) ? 0 : -EPROTO;
}

int ThreadContextDeviceTransport::Submit(
    std::uint32_t operation,
    void* payload) {
    if (operation == 0 || payload == nullptr) return -EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!profile_.IsConfigured()) return -ENOTSUP;

    thread_context_device_abi::Envelope envelope{
        operation,
        0,
        payload,
    };
    const int primaryResult = SubmitWithRequestCount(
        envelope,
        profile_.requestCount);
    if (primaryResult == 0) {
        negotiationComplete_ = true;
        return 0;
    }
    if (profile_.requestCountMode !=
            thread_context_device_abi::RequestCountMode::Negotiate ||
        negotiationComplete_ || !IsNegotiationFailure(primaryResult)) {
        return primaryResult;
    }

    const std::size_t alternateRequestCount =
        AlternateRequestCount(profile_.requestCount);
    const int alternateResult = SubmitWithRequestCount(
        envelope,
        alternateRequestCount);
    if (alternateResult == 0) {
        profile_.requestCount = alternateRequestCount;
        negotiationComplete_ = true;
    }
    return alternateResult;
}

}  // namespace lengjing::game::native
