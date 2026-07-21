#include "game/native/PacgaOperandResolver.h"

#include <array>
#include <limits>

namespace lengjing::game::native {
namespace {

constexpr std::uint32_t kMoveWideMask = UINT32_C(0x7F800000);
constexpr std::uint32_t kMoveNot = UINT32_C(0x12800000);
constexpr std::uint32_t kMoveZero = UINT32_C(0x52800000);
constexpr std::uint32_t kMoveKeep = UINT32_C(0x72800000);
constexpr std::uint32_t kRegisterMoveMask = UINT32_C(0x7FE0FFE0);
constexpr std::uint32_t kRegisterMove = UINT32_C(0x2A0003E0);
constexpr std::uint32_t kPacgaMask = UINT32_C(0xFFE0FC00);
constexpr std::uint32_t kPacgaOpcode = UINT32_C(0x9AC03000);
constexpr std::uint32_t kNop = UINT32_C(0xD503201F);
constexpr std::size_t kMaximumImmediateBlockInstructions = 16;

struct ConstantRegister {
    std::uint64_t value = 0;
    bool known = false;
};

bool IsMoveWide(std::uint32_t instruction) noexcept {
    const std::uint32_t opcode = instruction & kMoveWideMask;
    return opcode == kMoveNot || opcode == kMoveZero ||
        opcode == kMoveKeep;
}

bool IsRegisterMove(std::uint32_t instruction) noexcept {
    return (instruction & kRegisterMoveMask) == kRegisterMove;
}

bool IsImmediateBlockInstruction(std::uint32_t instruction) noexcept {
    return IsMoveWide(instruction) || IsRegisterMove(instruction) ||
        instruction == kNop;
}

void ApplyMoveWide(std::uint32_t instruction,
                   ConstantRegister& state) noexcept {
    const std::uint32_t opcode = instruction & kMoveWideMask;
    const bool is64Bit = (instruction >> 31U) != 0;
    const std::uint32_t lane = (instruction >> 21U) & 3U;
    if ((!is64Bit && lane > 1U) ||
        (opcode != kMoveNot && opcode != kMoveZero &&
         opcode != kMoveKeep)) {
        state = {};
        return;
    }

    const std::uint32_t shift = lane * 16U;
    const std::uint64_t immediate =
        static_cast<std::uint64_t>((instruction >> 5U) & 0xFFFFU) << shift;
    const std::uint64_t widthMask = is64Bit
        ? std::numeric_limits<std::uint64_t>::max()
        : UINT64_C(0xFFFFFFFF);
    const std::uint64_t laneMask = UINT64_C(0xFFFF) << shift;

    if (opcode == kMoveZero) {
        state.value = immediate & widthMask;
        state.known = true;
    } else if (opcode == kMoveNot) {
        state.value = (~immediate) & widthMask;
        state.known = true;
    } else if (state.known) {
        state.value = ((state.value & ~laneMask) | immediate) & widthMask;
    }
}

}  // namespace

bool ResolvePacgaOperandsFromImmediateBlock(
    const std::uint32_t* instructions,
    std::size_t instructionCount,
    std::size_t pacgaIndex,
    PacgaOperands& operands) noexcept {
    operands = {};
    if (instructions == nullptr || pacgaIndex >= instructionCount) {
        return false;
    }

    const std::uint32_t pacga = instructions[pacgaIndex];
    if ((pacga & kPacgaMask) != kPacgaOpcode) return false;
    const std::uint32_t dataRegister = (pacga >> 5U) & 0x1FU;
    const std::uint32_t modifierRegister = (pacga >> 16U) & 0x1FU;
    if (dataRegister == 31U || modifierRegister == 31U) return false;

    std::size_t begin = pacgaIndex;
    while (begin > 0 &&
           pacgaIndex - begin < kMaximumImmediateBlockInstructions &&
           IsImmediateBlockInstruction(instructions[begin - 1])) {
        --begin;
    }
    if (begin == pacgaIndex) return false;

    std::array<ConstantRegister, 32> registers{};
    for (std::size_t index = begin; index < pacgaIndex; ++index) {
        const std::uint32_t instruction = instructions[index];
        if (IsMoveWide(instruction)) {
            ApplyMoveWide(instruction, registers[instruction & 0x1FU]);
        } else if (IsRegisterMove(instruction)) {
            const std::uint32_t destination = instruction & 0x1FU;
            if (destination == 31U) continue;
            const std::uint32_t source = (instruction >> 16U) & 0x1FU;
            registers[destination] = source == 31U
                ? ConstantRegister{0, true}
                : registers[source];
            if ((instruction >> 31U) == 0 && registers[destination].known) {
                registers[destination].value &= UINT64_C(0xFFFFFFFF);
            }
        }
    }
    if (!registers[dataRegister].known ||
        !registers[modifierRegister].known) {
        return false;
    }

    operands.data = registers[dataRegister].value;
    operands.modifier = registers[modifierRegister].value;
    return true;
}

}  // namespace lengjing::game::native
