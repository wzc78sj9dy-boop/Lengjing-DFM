#include "game/native/CoordinateExecutionBackend.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

namespace game = lengjing::game::native;

constexpr std::uint64_t kCodeBase = UINT64_C(0x0000007100000000);

template <std::size_t Size>
std::array<std::uint8_t, Size * sizeof(std::uint32_t)> Encode(
    const std::array<std::uint32_t, Size>& instructions) {
    std::array<std::uint8_t, Size * sizeof(std::uint32_t)> bytes{};
    std::memcpy(bytes.data(), instructions.data(), bytes.size());
    return bytes;
}

template <typename Value>
Value ReadRecordValue(const game::CoordinatePredecodedInstruction& record,
                      std::size_t offset) {
    Value value{};
    std::memcpy(
        &value,
        reinterpret_cast<const std::uint8_t*>(&record) + offset,
        sizeof(value));
    return value;
}

void TestRecordLayoutAndCategories() {
    REQUIRE(sizeof(game::CoordinatePredecodedInstruction) == 40);
    REQUIRE(offsetof(game::CoordinatePredecodedInstruction, instruction) == 4);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xD503201F))
                .category == 0);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x14000002))
                .category == 63);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xD53BD040))
                .category == 106);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xF9400020))
                .category == 39);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xF9000020))
                .category == 46);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xA9400440))
                .category == 120);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xE9400440))
                .category == 56);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xE9000440))
                .category == 57);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xF8408C20))
                .category == 52);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0xF8408420))
                .category == 53);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x88097D6A))
                .category == 114);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x9AC02000))
                .category == 93);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x9B000000))
                .category == 85);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x9B008000))
                .category == 86);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x1200FC00))
                .category == game::kCoordinatePredecodeRawCategory);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x0B008000))
                .category == game::kCoordinatePredecodeRawCategory);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x1E202800))
                .category == game::kCoordinatePredecodeRawCategory);
    REQUIRE(game::PredecodeCoordinateInstruction(UINT32_C(0x55434241))
                .category == game::kCoordinatePredecodeAdvanceCategory);
}

void TestIgnorableInstructionPolicy() {
    REQUIRE(game::IsCoordinateExecutionIgnorableInstruction(0));
    REQUIRE(game::IsCoordinateExecutionIgnorableInstruction(UINT32_MAX));
    REQUIRE(game::IsCoordinateExecutionIgnorableInstruction(
        UINT32_C(0xB2B1B0AF)));
    REQUIRE(game::IsCoordinateExecutionIgnorableInstruction(
        UINT32_C(0x55434241)));
    REQUIRE(!game::IsCoordinateExecutionIgnorableInstruction(
        UINT32_C(0x14000002)));
}

void TestRecordOperands() {
    const auto add = game::PredecodeCoordinateInstruction(
        UINT32_C(0x91001420));
    REQUIRE(add.category == 1);
    REQUIRE(add.registers[0] == 0);
    REQUIRE(add.registers[1] == 1);
    REQUIRE(ReadRecordValue<std::uint32_t>(add, 8) == 5);
    REQUIRE(ReadRecordValue<std::uint8_t>(add, 28) == 0);

    const auto logical = game::PredecodeCoordinateInstruction(
        UINT32_C(0x92401C20));
    REQUIRE(logical.category == 17);
    REQUIRE(ReadRecordValue<std::uint64_t>(logical, 8) == UINT64_C(0xFF));
    REQUIRE(ReadRecordValue<std::uint64_t>(logical, 16) == UINT64_C(0xFF));

    const auto wideLogical = game::PredecodeCoordinateInstruction(
        UINT32_C(0x9240DC00));
    REQUIRE(wideLogical.category == 17);
    REQUIRE(ReadRecordValue<std::uint64_t>(wideLogical, 8) ==
            UINT64_C(0xFFFFFFFF));
    REQUIRE(ReadRecordValue<std::uint64_t>(wideLogical, 16) ==
            UINT64_C(0x00FFFFFFFFFFFFFF));

    const auto branch = game::PredecodeCoordinateInstruction(
        UINT32_C(0x14000002));
    REQUIRE(ReadRecordValue<std::int32_t>(branch, 24) == 8);

    const auto pair = game::PredecodeCoordinateInstruction(
        UINT32_C(0xE9400440));
    REQUIRE(pair.registers[0] == 0);
    REQUIRE(pair.registers[1] == 2);
    REQUIRE(pair.registers[2] == 1);
    REQUIRE(ReadRecordValue<std::int32_t>(pair, 24) == 0);

    const auto exclusive = game::PredecodeCoordinateInstruction(
        UINT32_C(0x88097D6A));
    REQUIRE(exclusive.registers[0] == 9);
    REQUIRE(exclusive.registers[1] == 0);
    REQUIRE(exclusive.registers[2] == 0);

    const auto raw = game::PredecodeCoordinateInstruction(
        UINT32_C(0x1E202800));
    REQUIRE(raw.operands[0] == 0);
    REQUIRE(raw.operands[1] == 0);
    REQUIRE(raw.operands[2] == 0);
    REQUIRE(raw.operands[3] == 0);
}

void TestPredecodeCacheAndFallback() {
    const auto bytes = Encode(std::array<std::uint32_t, 3>{
        UINT32_C(0xD503201F),
        UINT32_C(0x1E202800),
        UINT32_C(0x55434241),
    });
    game::CoordinateExecutionBackendCache cache;
    REQUIRE(cache.BuildPredecode(kCodeBase, bytes.data(), bytes.size()));
    REQUIRE(cache.PredecodeReady());
    REQUIRE(cache.Records().size() == 3);

    const auto decoded = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase);
    REQUIRE(decoded.dispatch == game::CoordinateBackendDispatch::Predecoded);
    REQUIRE(decoded.category == 0);
    REQUIRE(decoded.instructionCount == 1);

    const auto fallback = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase + 4);
    REQUIRE(fallback.dispatch == game::CoordinateBackendDispatch::Dynamic);

    const auto advance = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase + 8);
    REQUIRE(advance.dispatch == game::CoordinateBackendDispatch::Advance);

    const auto outside = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase + 12);
    REQUIRE(outside.dispatch == game::CoordinateBackendDispatch::Dynamic);
}

void TestJitControlFlowAndBlocks() {
    const auto bytes = Encode(std::array<std::uint32_t, 4>{
        UINT32_C(0x14000002),
        UINT32_C(0xD503201F),
        UINT32_C(0xD65F03C0),
        UINT32_C(0xD503201F),
    });
    game::CoordinateExecutionBackendCache cache;
    REQUIRE(cache.BuildJit(kCodeBase, bytes.data(), bytes.size()));
    REQUIRE(cache.JitReady());
    REQUIRE(cache.Blocks().size() == 4);
    REQUIRE(cache.Blocks()[0].instructionCount == 1);
    REQUIRE(cache.Blocks()[0].endsInControlFlow);
    REQUIRE(cache.Blocks()[1].instructionCount == 1);
    REQUIRE(!cache.Blocks()[1].endsInControlFlow);

    const auto block = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Jit, cache, kCodeBase + 8);
    REQUIRE(block.dispatch == game::CoordinateBackendDispatch::CompiledBlock);
    REQUIRE(block.instructionCount == 1);

    const auto fallthrough = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Jit, cache, kCodeBase + 4);
    REQUIRE(fallthrough.dispatch ==
            game::CoordinateBackendDispatch::CompiledBlock);

    const auto outside = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Jit, cache, kCodeBase + bytes.size());
    REQUIRE(outside.dispatch == game::CoordinateBackendDispatch::Dynamic);
}

void TestPredecodedRunsStopAtControlFlowAndFallback() {
    const auto bytes = Encode(std::array<std::uint32_t, 5>{
        UINT32_C(0xD503201F),
        UINT32_C(0xD503201F),
        UINT32_C(0x14000001),
        UINT32_C(0xD503201F),
        UINT32_C(0x1E202800),
    });
    game::CoordinateExecutionBackendCache cache;
    REQUIRE(cache.BuildPredecode(kCodeBase, bytes.data(), bytes.size()));

    const auto first = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase);
    REQUIRE(first.dispatch == game::CoordinateBackendDispatch::Predecoded);
    REQUIRE(first.instructionCount == 3);

    const auto second = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase + 12);
    REQUIRE(second.dispatch == game::CoordinateBackendDispatch::Predecoded);
    REQUIRE(second.instructionCount == 1);

    const auto fallback = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Predecode, cache, kCodeBase + 16);
    REQUIRE(fallback.dispatch == game::CoordinateBackendDispatch::Dynamic);
    REQUIRE(fallback.instructionCount == 1);
}

void TestMaximumBlockLength() {
    std::array<std::uint32_t, 70> instructions{};
    instructions.fill(UINT32_C(0xD503201F));
    const auto bytes = Encode(instructions);
    game::CoordinateExecutionBackendCache cache;
    REQUIRE(cache.BuildJit(kCodeBase, bytes.data(), bytes.size()));
    REQUIRE(cache.Blocks().size() == 1);
    REQUIRE(cache.Blocks()[0].instructionCount ==
            game::kCoordinateJitMaximumBlockInstructions);
    REQUIRE(!cache.Blocks()[0].endsInControlFlow);
    const auto fallback = game::SelectCoordinateBackendSlice(
        game::CoordinateExecutionMode::Jit,
        cache,
        kCodeBase + game::kCoordinateJitMaximumBlockInstructions * 4U);
    REQUIRE(fallback.dispatch == game::CoordinateBackendDispatch::Dynamic);
}

void TestOutOfRangeControlFlowBoundaries() {
    const auto conditionalBytes = Encode(std::array<std::uint32_t, 2>{
        UINT32_C(0x54000040),
        UINT32_C(0xD503201F),
    });
    game::CoordinateExecutionBackendCache conditional;
    REQUIRE(conditional.BuildJit(
        kCodeBase, conditionalBytes.data(), conditionalBytes.size()));
    REQUIRE(conditional.Blocks().size() == 2);
    REQUIRE(game::SelectCoordinateBackendSlice(
                game::CoordinateExecutionMode::Jit,
                conditional,
                kCodeBase + 4)
                .dispatch ==
            game::CoordinateBackendDispatch::CompiledBlock);

    const auto branchBytes = Encode(std::array<std::uint32_t, 2>{
        UINT32_C(0x17FFFFFF),
        UINT32_C(0xD503201F),
    });
    game::CoordinateExecutionBackendCache branch;
    REQUIRE(branch.BuildJit(
        kCodeBase, branchBytes.data(), branchBytes.size()));
    REQUIRE(branch.Blocks().size() == 2);
    REQUIRE(game::SelectCoordinateBackendSlice(
                game::CoordinateExecutionMode::Jit,
                branch,
                kCodeBase + 4)
                .dispatch ==
            game::CoordinateBackendDispatch::CompiledBlock);
}

}  // namespace

int main() {
    TestRecordLayoutAndCategories();
    TestIgnorableInstructionPolicy();
    TestRecordOperands();
    TestPredecodeCacheAndFallback();
    TestJitControlFlowAndBlocks();
    TestPredecodedRunsStopAtControlFlowAndFallback();
    TestMaximumBlockLength();
    TestOutOfRangeControlFlowBoundaries();
    return 0;
}
