#include "game/native/ThreadExecutionContextProvider.h"
#include "game/native/ThreadContextDeviceTransport.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using lengjing::game::native::ExecutionPacKey128;
using lengjing::game::native::ProcTaskThreadLocator;
using lengjing::game::native::TaskThreadIdentity;
using lengjing::game::native::ThreadContextDeviceTransport;
using lengjing::game::native::ThreadContextReaderCapabilities;
using lengjing::game::native::ThreadExecutionContextEvent;
using lengjing::game::native::ThreadExecutionContextProvider;
using lengjing::game::native::ThreadExecutionContextReader;
using lengjing::game::native::ThreadExecutionContextSnapshot;
using lengjing::game::native::ThreadPacKeys;
namespace device_abi =
    lengjing::game::native::thread_context_device_abi;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw std::runtime_error(                                           \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +      \
                ": check failed: " #condition);                               \
        }                                                                       \
    } while (false)

class TempProcTree final {
public:
    TempProcTree() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        root_ = std::filesystem::temp_directory_path() /
            ("lengjing-thread-context-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);
    }

    ~TempProcTree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path& Root() const noexcept { return root_; }

    void WriteThread(std::int32_t processId,
                     std::int32_t threadId,
                     const std::string& name,
                     std::uint64_t startTimeTicks) {
        const auto directory = root_ /
            std::to_string(processId) /
            "task" /
            std::to_string(threadId);
        std::filesystem::create_directories(directory);
        {
            std::ofstream comm(directory / "comm", std::ios::binary);
            comm << name << '\n';
        }
        {
            std::ofstream stat(directory / "stat", std::ios::binary);
            stat << threadId << " (" << name << ") S";
            for (int field = 4; field <= 21; ++field) stat << " 0";
            stat << ' ' << startTimeTicks << " 0\n";
        }
    }

    void RemoveThread(std::int32_t processId, std::int32_t threadId) {
        std::filesystem::remove_all(
            root_ /
            std::to_string(processId) /
            "task" /
            std::to_string(threadId));
    }

private:
    std::filesystem::path root_;
};

struct FakeValues {
    std::uint64_t tpidrEl0 = 0;
    ExecutionPacKey128 apga{};
};

class FakeReader final : public ThreadExecutionContextReader {
public:
    ThreadContextReaderCapabilities capabilities{true, true, true};
    std::unordered_map<std::int32_t, FakeValues> values;
    int tpidrFailure = 0;
    int pacFailure = 0;
    int transientTpidrFailure = 0;
    int transientPacFailure = 0;
    int tpidrFailuresRemaining = 0;
    int pacFailuresRemaining = 0;
    int tpidrReadCount = 0;
    int pacReadCount = 0;
    std::vector<std::int32_t> tpidrAttempts;
    std::vector<std::int32_t> pacAttempts;
    std::int32_t lastTpidrThreadId = 0;
    std::int32_t lastPacThreadId = 0;

    ThreadContextReaderCapabilities Capabilities() const noexcept override {
        return capabilities;
    }

    int ReadTpidrEl0(std::int32_t threadId,
                     std::uint64_t& value) override {
        ++tpidrReadCount;
        tpidrAttempts.push_back(threadId);
        lastTpidrThreadId = threadId;
        if (tpidrFailuresRemaining > 0) {
            --tpidrFailuresRemaining;
            return transientTpidrFailure;
        }
        if (tpidrFailure != 0) return tpidrFailure;
        const auto item = values.find(threadId);
        if (item == values.end()) return -ESRCH;
        value = item->second.tpidrEl0;
        return 0;
    }

    int ReadPacKeys(
        std::int32_t threadId,
        ThreadPacKeys& keys) override {
        ++pacReadCount;
        pacAttempts.push_back(threadId);
        lastPacThreadId = threadId;
        if (pacFailuresRemaining > 0) {
            --pacFailuresRemaining;
            return transientPacFailure;
        }
        if (pacFailure != 0) return pacFailure;
        const auto item = values.find(threadId);
        if (item == values.end()) return -ESRCH;
        keys = {};
        keys.apga = item->second.apga;
        return 0;
    }
};

struct DeviceCapture {
    int fileDescriptor = -1;
    std::size_t requestCount = 0;
    std::int32_t tpidrSubject = 0;
    std::int32_t pacSubject = 0;
    ExecutionPacKey128 installedKey{};
    std::int64_t writeResult = 0;
    int error = 0;
};

DeviceCapture gDeviceCapture;

std::int64_t FakeDeviceWrite(int fileDescriptor,
                             const void* buffer,
                             std::size_t count) {
    gDeviceCapture.fileDescriptor = fileDescriptor;
    gDeviceCapture.requestCount = count;
    auto* envelope = static_cast<const device_abi::Envelope*>(buffer);
    CHECK(envelope != nullptr);
    CHECK(envelope->reserved == 0);
    if (envelope->operation == device_abi::kReadTpidrEl0Operation) {
        auto* payload = static_cast<device_abi::TpidrPayload*>(
            envelope->payload);
        gDeviceCapture.tpidrSubject = payload->threadId;
        payload->value = UINT64_C(0x7000555500);
    } else if (envelope->operation == device_abi::kReadPacKeysOperation) {
        auto* payload = static_cast<device_abi::PacKeysPayload*>(
            envelope->payload);
        gDeviceCapture.pacSubject = payload->threadId;
        payload->keys.apga = {
            UINT64_C(0x7777),
            UINT64_C(0x8888),
        };
    } else if (envelope->operation ==
               device_abi::kInstallApgaKeyOperation) {
        gDeviceCapture.installedKey =
            *static_cast<const ExecutionPacKey128*>(envelope->payload);
    } else {
        errno = ENOSYS;
        return -1;
    }
    errno = gDeviceCapture.error;
    return gDeviceCapture.writeResult;
}

void TestExactThreadLookup() {
    TempProcTree proc;
    constexpr std::int32_t processId = 400;
    proc.WriteThread(processId, 402, "GameThreadExtra", 20);
    proc.WriteThread(processId, 407, "GameThread", 70);
    proc.WriteThread(processId, 405, "GameThread", 50);
    proc.WriteThread(processId, 401, "RenderThread", 10);

    ProcTaskThreadLocator locator(proc.Root().string());
    TaskThreadIdentity identity{};
    CHECK(locator.FindExact(processId, "GameThread", identity) == 0);
    CHECK(identity.threadId == 405);
    CHECK(identity.startTimeTicks == 50);

    std::vector<TaskThreadIdentity> identities;
    CHECK(locator.FindAllExact(processId, "GameThread", identities) == 0);
    CHECK(identities.size() == 2);
    CHECK(identities[0].threadId == 405);
    CHECK(identities[1].threadId == 407);

    CHECK(locator.FindExact(processId, "GameThread", identity, 407) == 0);
    CHECK(identity.threadId == 407);
    CHECK(identity.startTimeTicks == 70);

    CHECK(locator.FindExact(processId, "gameThread", identity) == -ENOENT);
    CHECK(identity.threadId == 0);
    CHECK(locator.FindExact(0, "GameThread", identity) == -EINVAL);
}

void TestProviderStateMachine() {
    TempProcTree proc;
    constexpr std::int32_t processId = 700;
    proc.WriteThread(processId, 701, "GameThread", 100);

    FakeReader reader;
    reader.values[701] = {
        UINT64_C(0x7000111100),
        {UINT64_C(0x1111), UINT64_C(0x2222)},
    };
    ThreadExecutionContextProvider provider(
        processId,
        reader,
        "GameThread",
        proc.Root().string());

    auto refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Acquired);
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.processId == processId);
    CHECK(refresh.snapshot.threadId == 701);
    CHECK(refresh.snapshot.threadStartTimeTicks == 100);
    CHECK(refresh.snapshot.generation == 1);
    CHECK(reader.lastTpidrThreadId == 701);
    CHECK(reader.lastPacThreadId == 701);

    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Stable);
    CHECK(refresh.snapshot.generation == 1);

    const int tpidrReadsBeforeRetry = reader.tpidrReadCount;
    const int pacReadsBeforeRetry = reader.pacReadCount;
    reader.transientTpidrFailure = -ESRCH;
    reader.tpidrFailuresRemaining = 1;
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Stable);
    CHECK(reader.tpidrReadCount == tpidrReadsBeforeRetry + 2);
    CHECK(reader.pacReadCount == pacReadsBeforeRetry + 1);
    reader.transientTpidrFailure = 0;

    reader.values[701].tpidrEl0 = UINT64_C(0x7000222200);
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::ValuesChanged);
    CHECK(refresh.snapshot.generation == 2);

    proc.WriteThread(processId, 701, "GameThread", 200);
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::ThreadRecreated);
    CHECK(refresh.snapshot.threadId == 701);
    CHECK(refresh.snapshot.threadStartTimeTicks == 200);
    CHECK(refresh.snapshot.generation == 3);

    proc.WriteThread(processId, 699, "GameThread", 300);
    reader.values[699] = {
        UINT64_C(0x7000333300),
        {UINT64_C(0x3333), UINT64_C(0x4444)},
    };
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Stable);
    CHECK(refresh.snapshot.threadId == 701);

    proc.RemoveThread(processId, 701);
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::ThreadRecreated);
    CHECK(refresh.snapshot.threadId == 699);
    CHECK(refresh.snapshot.generation == 4);

    proc.RemoveThread(processId, 699);
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Lost);
    CHECK(refresh.status == -ENOENT);
    CHECK(!refresh.HasContext());
    ThreadExecutionContextSnapshot snapshot{};
    CHECK(!provider.Current(snapshot));

    proc.WriteThread(processId, 702, "GameThread", 400);
    reader.values[702] = {
        UINT64_C(0x7000444400),
        {UINT64_C(0x5555), UINT64_C(0x6666)},
    };
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Acquired);
    CHECK(refresh.snapshot.threadId == 702);
    CHECK(refresh.snapshot.generation == 5);

    reader.pacFailure = -EIO;
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Lost);
    CHECK(refresh.status == -EIO);
    reader.pacFailure = 0;

    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Acquired);
    CHECK(refresh.snapshot.generation == 6);

    reader.values[702].apga = {};
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Lost);
    CHECK(refresh.status == -ENODATA);

    provider.Reset();
    CHECK(!provider.Current(snapshot));
}

void TestCapabilityGate() {
    TempProcTree proc;
    constexpr std::int32_t processId = 900;
    proc.WriteThread(processId, 901, "GameThread", 10);

    FakeReader reader;
    reader.capabilities.threadIdSubject = false;
    ThreadExecutionContextProvider provider(
        processId,
        reader,
        "GameThread",
        proc.Root().string());
    const auto refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Missing);
    CHECK(refresh.status == -ENOTSUP);
    CHECK(reader.lastTpidrThreadId == 0);
    CHECK(reader.lastPacThreadId == 0);
}

void TestMultipleExactCandidates() {
    TempProcTree proc;
    constexpr std::int32_t processId = 1100;
    proc.WriteThread(processId, 1101, "GameThread", 10);
    proc.WriteThread(processId, 1102, "GameThread", 20);
    proc.WriteThread(processId, 1103, "GameThread", 30);

    FakeReader reader;
    reader.values[1102] = {
        0,
        {UINT64_C(0x1111), UINT64_C(0x2222)},
    };
    reader.values[1103] = {
        UINT64_C(0x7000666600),
        {UINT64_C(0x3333), UINT64_C(0x4444)},
    };
    ThreadExecutionContextProvider provider(
        processId,
        reader,
        "GameThread",
        proc.Root().string());

    auto refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Acquired);
    CHECK(refresh.snapshot.threadId == 1103);
    CHECK(reader.tpidrAttempts ==
          std::vector<std::int32_t>({1101, 1102, 1103}));
    CHECK(reader.pacAttempts ==
          std::vector<std::int32_t>({1102, 1103}));

    reader.tpidrAttempts.clear();
    reader.pacAttempts.clear();
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::Stable);
    CHECK(reader.tpidrAttempts == std::vector<std::int32_t>({1103}));
    CHECK(reader.pacAttempts == std::vector<std::int32_t>({1103}));

    proc.WriteThread(processId, 1103, "GameThread", 40);
    reader.tpidrAttempts.clear();
    reader.pacAttempts.clear();
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::ThreadRecreated);
    CHECK(refresh.snapshot.threadId == 1103);
    CHECK(refresh.snapshot.threadStartTimeTicks == 40);
    CHECK(reader.tpidrAttempts ==
          std::vector<std::int32_t>({1101, 1102, 1103}));

    proc.RemoveThread(processId, 1103);
    reader.values[1102].tpidrEl0 = UINT64_C(0x7000777700);
    reader.tpidrAttempts.clear();
    reader.pacAttempts.clear();
    refresh = provider.Refresh();
    CHECK(refresh.event == ThreadExecutionContextEvent::ThreadRecreated);
    CHECK(refresh.snapshot.threadId == 1102);
    CHECK(reader.tpidrAttempts ==
          std::vector<std::int32_t>({1101, 1102}));
    CHECK(reader.pacAttempts == std::vector<std::int32_t>({1102}));
}

void TestDeviceTransportUsesThreadId() {
    TempProcTree proc;
    constexpr std::int32_t processId = 1000;
    constexpr std::int32_t threadId = 1002;
    proc.WriteThread(processId, threadId, "GameThread", 50);

    const device_abi::Profile profile{
        43,
        device_abi::kLargeRequestCount,
        device_abi::WriteSuccessPolicy::ExactZero,
    };
    ThreadContextDeviceTransport transport(profile, &FakeDeviceWrite);
    CHECK(transport.Capabilities().IsUsable());

    gDeviceCapture = {};
    ThreadExecutionContextProvider provider(
        processId,
        transport,
        "GameThread",
        proc.Root().string());
    const auto refresh = provider.Refresh();
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.processId == processId);
    CHECK(refresh.snapshot.threadId == threadId);
    CHECK(refresh.snapshot.apga.low == UINT64_C(0x7777));
    CHECK(refresh.snapshot.apga.high == UINT64_C(0x8888));
    CHECK(gDeviceCapture.fileDescriptor == 43);
    CHECK(gDeviceCapture.requestCount == device_abi::kLargeRequestCount);
    CHECK(gDeviceCapture.tpidrSubject == threadId);
    CHECK(gDeviceCapture.pacSubject == threadId);

    const ExecutionPacKey128 key{
        UINT64_C(0x1234),
        UINT64_C(0x5678),
    };
    CHECK(transport.InstallApgaKey(key) == 0);
    CHECK(gDeviceCapture.installedKey.low == key.low);
    CHECK(gDeviceCapture.installedKey.high == key.high);

    transport.UpdateFileDescriptor(-1);
    CHECK(!transport.Capabilities().IsUsable());
    std::uint64_t value = 0;
    CHECK(transport.ReadTpidrEl0(threadId, value) == -ENOTSUP);
}

void TestDeviceWriteResultPolicies() {
    const device_abi::Profile unspecified{
        44,
        device_abi::kSmallRequestCount,
        device_abi::WriteSuccessPolicy::Unspecified,
    };
    ThreadContextDeviceTransport unconfigured(
        unspecified,
        &FakeDeviceWrite);
    CHECK(!unconfigured.Capabilities().IsUsable());

    gDeviceCapture = {};
    gDeviceCapture.writeResult =
        static_cast<std::int64_t>(device_abi::kSmallRequestCount);
    device_abi::Profile profile{
        44,
        device_abi::kSmallRequestCount,
        device_abi::WriteSuccessPolicy::ExactRequestCount,
    };
    ThreadContextDeviceTransport transport(profile, &FakeDeviceWrite);
    std::uint64_t value = 0;
    CHECK(transport.ReadTpidrEl0(1200, value) == 0);
    CHECK(value == UINT64_C(0x7000555500));

    profile.successPolicy = device_abi::WriteSuccessPolicy::ExactZero;
    ThreadContextDeviceTransport zeroOnly(profile, &FakeDeviceWrite);
    CHECK(zeroOnly.ReadTpidrEl0(1200, value) == -EPROTO);

    gDeviceCapture.writeResult = -1;
    gDeviceCapture.error = EACCES;
    CHECK(transport.ReadTpidrEl0(1200, value) == -EACCES);
}

void TestCandidateRejectionRotation() {
    TempProcTree proc;
    constexpr std::int32_t processId = 1300;
    proc.WriteThread(processId, 1301, "GameThread", 10);
    proc.WriteThread(processId, 1302, "GameThread", 20);

    FakeReader reader;
    reader.values[1301] = {
        UINT64_C(0x7000111100),
        {UINT64_C(0x1111), UINT64_C(0x2222)},
    };
    reader.values[1302] = {
        UINT64_C(0x7000222200),
        {UINT64_C(0x3333), UINT64_C(0x4444)},
    };
    ThreadExecutionContextProvider provider(
        processId,
        reader,
        "GameThread",
        proc.Root().string());

    auto refresh = provider.Refresh();
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1301);
    CHECK(provider.RejectCurrent());
    refresh = provider.Refresh();
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1302);
    CHECK(provider.RejectCurrent());
    refresh = provider.Refresh();
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1301);
}

}  // namespace

int main() {
    try {
        TestExactThreadLookup();
        TestProviderStateMachine();
        TestCapabilityGate();
        TestMultipleExactCandidates();
        TestDeviceTransportUsesThreadId();
        TestDeviceWriteResultPolicies();
        TestCandidateRejectionRotation();
        std::cout << "thread execution context provider tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
