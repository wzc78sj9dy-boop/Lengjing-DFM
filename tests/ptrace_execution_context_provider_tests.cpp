#include "game/native/PtraceExecutionContextProvider.h"

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

using lengjing::game::native::PacgaOracleInstruction;
using lengjing::game::native::PacgaOracleReader;
using lengjing::game::native::PtraceExecutionContextProvider;

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
            ("lengjing-ptrace-context-" + std::to_string(stamp));
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

private:
    std::filesystem::path root_;
};

struct ReaderValue {
    int status = 0;
    std::uint64_t tpidrEl0 = 0;
    std::uint64_t result = 0;
};

class FakeReader final : public PacgaOracleReader {
public:
    std::unordered_map<std::int32_t, ReaderValue> values;
    std::vector<std::int32_t> attempts;
    PacgaOracleInstruction lastInstruction{};

    int Read(std::int32_t threadId,
             const PacgaOracleInstruction& instruction,
             std::uint64_t& tpidrEl0,
             std::uint64_t& result) override {
        attempts.push_back(threadId);
        lastInstruction = instruction;
        const auto item = values.find(threadId);
        if (item == values.end()) return -ESRCH;
        if (item->second.status != 0) return item->second.status;
        tpidrEl0 = item->second.tpidrEl0;
        result = item->second.result;
        return 0;
    }
};

void TestCandidateRotationAndCache() {
    TempProcTree proc;
    constexpr std::int32_t processId = 1400;
    proc.WriteThread(processId, 1401, "GameThread", 10);
    proc.WriteThread(processId, 1402, "GameThread", 20);
    proc.WriteThread(processId, 1403, "GameThreadExtra", 30);

    FakeReader reader;
    reader.values[1401] = {-EPERM, 0, 0};
    reader.values[1402] = {
        0,
        UINT64_C(0x772984DA80),
        UINT64_C(0x3579425300000000),
    };
    PtraceExecutionContextProvider provider(
        processId,
        reader,
        "GameThread",
        proc.Root().string());

    const PacgaOracleInstruction first{
        UINT64_C(0x767F196CC4),
        UINT64_C(0x412625C7),
        UINT64_C(0xBB7AC00B),
    };
    auto refresh = provider.Refresh(first);
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1402);
    CHECK(refresh.snapshot.threadStartTimeTicks == 20);
    CHECK(refresh.snapshot.tpidrEl0 == UINT64_C(0x772984DA80));
    CHECK(refresh.snapshot.result == UINT64_C(0x3579425300000000));
    CHECK(refresh.snapshot.generation == 1);
    CHECK(reader.attempts == std::vector<std::int32_t>({1401, 1402}));
    CHECK(reader.lastInstruction == first);

    reader.attempts.clear();
    refresh = provider.Refresh(first);
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.generation == 1);
    CHECK(reader.attempts.empty());

    const PacgaOracleInstruction second{
        UINT64_C(0x767F196CC8),
        first.data,
        first.modifier,
    };
    reader.attempts.clear();
    refresh = provider.Refresh(second);
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1402);
    CHECK(refresh.snapshot.generation == 2);
    CHECK(reader.attempts == std::vector<std::int32_t>({1402}));

    CHECK(provider.RejectCurrent());
    reader.values[1401] = {
        0,
        UINT64_C(0x7729886A80),
        UINT64_C(0x3579425300000000),
    };
    reader.attempts.clear();
    refresh = provider.Refresh(second);
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1401);
    CHECK(refresh.snapshot.generation == 3);
    CHECK(reader.attempts == std::vector<std::int32_t>({1401}));

    CHECK(provider.RejectCurrent());
    reader.attempts.clear();
    refresh = provider.Refresh(second);
    CHECK(refresh.HasContext());
    CHECK(refresh.snapshot.threadId == 1401);
    CHECK(refresh.snapshot.generation == 4);
    CHECK(reader.attempts == std::vector<std::int32_t>({1401}));
}

void TestInputValidation() {
    TempProcTree proc;
    FakeReader reader;
    PtraceExecutionContextProvider provider(
        1500,
        reader,
        "GameThread",
        proc.Root().string());
    const auto refresh = provider.Refresh({});
    CHECK(!refresh.HasContext());
    CHECK(refresh.status == -EINVAL);
    CHECK(!provider.RejectCurrent());
}

void TestFailureBackoff() {
    TempProcTree proc;
    constexpr std::int32_t processId = 1600;
    proc.WriteThread(processId, 1601, "GameThread", 10);

    FakeReader reader;
    reader.values[1601] = {-EPERM, 0, 0};
    PtraceExecutionContextProvider provider(
        processId,
        reader,
        "GameThread",
        proc.Root().string());
    const PacgaOracleInstruction instruction{
        UINT64_C(0x767F196CC4),
        UINT64_C(0x412625C7),
        UINT64_C(0xBB7AC00B),
    };
    auto refresh = provider.Refresh(instruction);
    CHECK(!refresh.HasContext());
    CHECK(refresh.status == -EPERM);
    CHECK(reader.attempts == std::vector<std::int32_t>({1601}));

    reader.attempts.clear();
    refresh = provider.Refresh(instruction);
    CHECK(!refresh.HasContext());
    CHECK(refresh.status == -EPERM);
    CHECK(reader.attempts.empty());
}

}  // namespace

int main() {
    try {
        TestCandidateRotationAndCache();
        TestInputValidation();
        TestFailureBackoff();
        std::cout << "ptrace execution context provider tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
