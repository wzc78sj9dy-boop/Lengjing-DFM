#pragma once

#include "game/native/CoordinateExecutionRuntime.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lengjing::game::native {

inline constexpr std::uint8_t kCoordinatePredecodeRawCategory = 124;
inline constexpr std::uint8_t kCoordinatePredecodeAdvanceCategory = 125;
inline constexpr std::uint8_t kCoordinatePredecodeMaximumCategory = 126;
inline constexpr std::uint32_t kCoordinateJitMaximumBlockInstructions = 64;

#pragma pack(push, 1)
struct CoordinatePredecodedInstruction {
    std::uint8_t category = kCoordinatePredecodeRawCategory;
    std::uint8_t registers[3]{};
    std::uint32_t instruction = 0;
    std::uint64_t operands[4]{};
};
#pragma pack(pop)

static_assert(sizeof(CoordinatePredecodedInstruction) == 40);
static_assert(offsetof(CoordinatePredecodedInstruction, instruction) == 4);

struct CoordinateJitBlock {
    std::uint32_t startIndex = 0;
    std::uint32_t instructionCount = 0;
    bool endsInControlFlow = false;
};

enum class CoordinateBackendDispatch : std::uint8_t {
    Dynamic,
    Predecoded,
    CompiledBlock,
    Advance,
};

struct CoordinateBackendSlice {
    CoordinateBackendDispatch dispatch = CoordinateBackendDispatch::Dynamic;
    std::uint32_t instructionCount = 1;
    std::uint8_t category = kCoordinatePredecodeRawCategory;
};

bool IsCoordinateExecutionIgnorableInstruction(
    std::uint32_t instruction) noexcept;

bool IsCoordinateExecutionControlFlowInstruction(
    std::uint32_t instruction) noexcept;

CoordinatePredecodedInstruction PredecodeCoordinateInstruction(
    std::uint32_t instruction) noexcept;

class CoordinateExecutionBackendCache {
public:
    bool BuildPredecode(std::uint64_t codeBase,
                        const std::uint8_t* code,
                        std::size_t codeSize);

    bool BuildJit(std::uint64_t codeBase,
                  const std::uint8_t* code,
                  std::size_t codeSize);

    void Reset() noexcept;

    bool PredecodeReady() const noexcept { return predecodeReady_; }
    bool JitReady() const noexcept { return jitReady_; }
    std::uint64_t CodeBase() const noexcept { return codeBase_; }
    std::size_t CodeSize() const noexcept { return codeSize_; }

    const CoordinatePredecodedInstruction* FindPredecoded(
        std::uint64_t pc) const noexcept;

    const CoordinateJitBlock* FindJitBlock(
        std::uint64_t pc) const noexcept;

    std::uint32_t PredecodedRunLength(
        std::uint64_t pc,
        std::uint32_t maximum =
            kCoordinateJitMaximumBlockInstructions) const noexcept;

    const std::vector<CoordinatePredecodedInstruction>& Records()
        const noexcept {
        return records_;
    }

    const std::vector<CoordinateJitBlock>& Blocks() const noexcept {
        return blocks_;
    }

private:
    bool BindCode(std::uint64_t codeBase, std::size_t codeSize);

    std::uint64_t codeBase_ = 0;
    std::size_t codeSize_ = 0;
    bool predecodeReady_ = false;
    bool jitReady_ = false;
    std::vector<CoordinatePredecodedInstruction> records_;
    std::vector<CoordinateJitBlock> blocks_;
    std::vector<std::int32_t> blockByInstruction_;
};

CoordinateBackendSlice SelectCoordinateBackendSlice(
    CoordinateExecutionMode mode,
    const CoordinateExecutionBackendCache& cache,
    std::uint64_t pc) noexcept;

}  // namespace lengjing::game::native
