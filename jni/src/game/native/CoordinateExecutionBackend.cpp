#include "game/native/CoordinateExecutionBackend.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace lengjing::game::native {
namespace {

constexpr bool IsTextByte(std::uint8_t value) noexcept {
    return value == 0 || (value >= 0x20U && value <= 0x7EU);
}

constexpr std::int64_t SignExtend(std::uint64_t value,
                                  unsigned bits) noexcept {
    const std::uint64_t sign = UINT64_C(1) << (bits - 1U);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

bool DecodeImmediateCategory(std::uint32_t instruction,
                             std::uint8_t& category) noexcept {
    const std::uint32_t kind = (instruction >> 23U) & 7U;
    const bool wide = (instruction & UINT32_C(0x80000000)) != 0;
    const std::uint32_t operation = (instruction >> 29U) & 3U;
    switch (kind) {
        case 2:
        case 3:
            category = static_cast<std::uint8_t>(
                (wide ? 1U : 5U) + ((instruction >> 29U) & 3U));
            return true;
        case 4:
            category = static_cast<std::uint8_t>(
                (wide ? 17U : 21U) + operation);
            return true;
        case 5: {
            const std::uint32_t form = (instruction >> 21U) & 3U;
            if (!wide && form >= 2U) return false;
            if (operation == 1U) return false;
            if (wide) {
                category = static_cast<std::uint8_t>(
                    operation == 0U ? 35U : operation == 2U ? 33U : 34U);
            } else {
                category = static_cast<std::uint8_t>(
                    operation == 0U ? 38U : operation == 2U ? 36U : 37U);
            }
            return true;
        }
        case 6:
            if (!wide) {
                category = 116;
            } else if (operation == 2U) {
                category = 108;
            } else if (operation == 0U) {
                category = 109;
            } else {
                category = 116;
            }
            return true;
        default:
            if (kind > 1U) {
                category = 116;
            } else {
                category = wide ? 112 : 113;
            }
            return true;
    }
}

bool DecodeBranchCategory(std::uint32_t instruction,
                          std::uint8_t& category) noexcept {
    if ((instruction & UINT32_C(0xFC000000)) == UINT32_C(0x14000000)) {
        category = 63;
    } else if ((instruction & UINT32_C(0xFC000000)) ==
               UINT32_C(0x94000000)) {
        category = 64;
    } else if ((instruction & UINT32_C(0xFF000010)) ==
               UINT32_C(0x54000000)) {
        category = 65;
    } else if ((instruction & UINT32_C(0x7F000000)) ==
               UINT32_C(0x34000000)) {
        category = (instruction & UINT32_C(0x80000000)) != 0 ? 66 : 68;
    } else if ((instruction & UINT32_C(0x7F000000)) ==
               UINT32_C(0x35000000)) {
        category = (instruction & UINT32_C(0x80000000)) != 0 ? 67 : 69;
    } else if ((instruction & UINT32_C(0x7F000000)) ==
               UINT32_C(0x36000000)) {
        category = 70;
    } else if ((instruction & UINT32_C(0x7F000000)) ==
               UINT32_C(0x37000000)) {
        category = 71;
    } else if ((instruction & UINT32_C(0xFFFFFC1F)) ==
               UINT32_C(0xD61F0000)) {
        category = 72;
    } else if ((instruction & UINT32_C(0xFFFFFC1F)) ==
               UINT32_C(0xD63F0000)) {
        category = 73;
    } else if ((instruction & UINT32_C(0xFFFFFC1F)) ==
                   UINT32_C(0xD65F0000) ||
               instruction == UINT32_C(0xD65F0BFF) ||
               instruction == UINT32_C(0xD65F0FFF)) {
        category = 74;
    } else if ((instruction & UINT32_C(0xFFE0001F)) ==
               UINT32_C(0xD4000001)) {
        category = 75;
    } else if ((instruction & UINT32_C(0xFFE0001F)) ==
               UINT32_C(0xD4200000)) {
        category = 76;
    } else {
        return false;
    }
    return true;
}

bool DecodeLoadStoreCategory(std::uint32_t instruction,
                             std::uint8_t& category) noexcept {
    const std::uint32_t high = instruction >> 28U;
    if ((instruction & UINT32_C(0x04000000)) == 0 &&
        (instruction & UINT32_C(0x10000000)) == 0 &&
        (high == 2U || high == 3U || high == 10U || high == 11U)) {
        category = 120;
        return true;
    }
    if ((instruction & UINT32_C(0x3F000000)) ==
        UINT32_C(0x18000000)) {
        if ((instruction & UINT32_C(0x40000000)) == 0) return false;
        category = 62;
        return true;
    }
    if ((instruction & UINT32_C(0x04000000)) != 0) {
        category = 118;
    } else if ((instruction & UINT32_C(0x3B200C00)) ==
               UINT32_C(0x38200800)) {
        category = 119;
    } else if ((instruction & UINT32_C(0x3B000000)) ==
               UINT32_C(0x39000000)) {
        category = 118;
    } else if ((instruction & UINT32_C(0x3B200C00)) ==
                   UINT32_C(0x38000C00) ||
               (instruction & UINT32_C(0x3B200C00)) ==
                   UINT32_C(0x38000400)) {
        category = 118;
    } else if ((instruction & UINT32_C(0x3F000000)) ==
               UINT32_C(0x08000000)) {
        category = 114;
    } else {
        category = 118;
    }
    return true;
}

bool DecodeRegisterCategory(std::uint32_t instruction,
                            std::uint8_t& category) noexcept {
    if ((instruction & UINT32_C(0x7FE0E000)) ==
        UINT32_C(0x5AC00000)) {
        const std::uint32_t operation = (instruction >> 10U) & 0x3FU;
        const bool wide = (instruction & UINT32_C(0x80000000)) != 0;
        if (operation == 4U) {
            category = wide ? 97 : 98;
        } else if (operation == 0U && wide) {
            category = 99;
        } else if (operation == 3U && wide) {
            category = 100;
        } else {
            category = kCoordinatePredecodeRawCategory;
        }
        return true;
    }
    if ((instruction & UINT32_C(0x7FE0FC00)) ==
            UINT32_C(0x1A000000) ||
        (instruction & UINT32_C(0x7FE0FC00)) ==
            UINT32_C(0x5A000000) ||
        (instruction & UINT32_C(0x7FE0FC00)) ==
            UINT32_C(0x3A000000) ||
        (instruction & UINT32_C(0x7FE0FC00)) ==
            UINT32_C(0x7A000000)) {
        category = 117;
        return true;
    }

    const bool operationGroup =
        (instruction & UINT32_C(0x10000000)) != 0;
    const std::uint32_t operation = (instruction >> 21U) & 0xFU;
    const bool wide = (instruction & UINT32_C(0x80000000)) != 0;
    if (!operationGroup && (operation & 8U) == 0) {
        const std::uint32_t variant = (instruction >> 29U) & 3U;
        category = static_cast<std::uint8_t>(
            (wide ? 25U : 29U) + variant);
        return true;
    }
    if (!operationGroup && (operation & 9U) == 8U) {
        category = static_cast<std::uint8_t>(
            wide ? ((instruction & UINT32_C(0x20000000)) != 0 ? 11 : 9)
                 : ((instruction & UINT32_C(0x20000000)) != 0 ? 15 : 13));
        if ((instruction & UINT32_C(0x40000000)) != 0) ++category;
        return true;
    }
    if (operationGroup && operation == 4U) {
        const std::uint32_t selector =
            ((wide ? 0U : 4U) |
             ((instruction & UINT32_C(0x40000000)) != 0 ? 2U : 0U) |
             ((instruction & UINT32_C(0x400)) != 0 ? 1U : 0U));
        category = static_cast<std::uint8_t>(77U + selector);
        return true;
    }
    if (operationGroup && (operation & 8U) == 8U) {
        category = (operation & 7U) == 0 ? 85 : 123;
        return true;
    }
    if (operationGroup && operation == 6U) {
        category = wide ? 91 : 117;
        return true;
    }
    if (operationGroup && (operation == 2U || operation == 3U)) {
        category = 121;
        return true;
    }
    category = 122;
    return true;
}

bool DecodeCategory(std::uint32_t instruction,
                    std::uint8_t& category) noexcept {
    if (instruction == UINT32_C(0xD503201F) ||
        instruction == UINT32_C(0xD5033F9F) ||
        instruction == UINT32_C(0xD5033BBF) ||
        instruction == UINT32_C(0xD5033FBF) ||
        (instruction & UINT32_C(0xFFFFF01F)) ==
            UINT32_C(0xD503201F)) {
        category = 0;
        return true;
    }
    if ((instruction & UINT32_C(0xFFF00000)) ==
        UINT32_C(0xD5300000)) {
        category = 106;
        return true;
    }
    if ((instruction & UINT32_C(0xFFF00000)) ==
        UINT32_C(0xD5100000)) {
        const bool recognized =
            (instruction & UINT32_C(0x00080000)) != 0 &&
            ((instruction >> 16U) & 7U) == 3U &&
            ((instruction >> 12U) & 0xFU) == 4U &&
            ((instruction >> 8U) & 0xFU) == 2U &&
            ((instruction >> 5U) & 7U) == 0U;
        category = recognized ? 107 : 116;
        return true;
    }

    const std::uint32_t group = (instruction >> 25U) & 0xFU;
    if ((group & 0xEU) == 8U) {
        return DecodeImmediateCategory(instruction, category);
    }
    if ((group & 0xEU) == 0xAU) {
        return DecodeBranchCategory(instruction, category);
    }
    if ((instruction & UINT32_C(0xBF9F0000)) ==
            UINT32_C(0x0C000000) ||
        (instruction & UINT32_C(0xBF800000)) ==
            UINT32_C(0x0C800000) ||
        (instruction & UINT32_C(0xBF9F0000)) ==
            UINT32_C(0x0D000000) ||
        (instruction & UINT32_C(0xBF800000)) ==
            UINT32_C(0x0D800000)) {
        category = kCoordinatePredecodeRawCategory;
        return true;
    }
    if ((group & 5U) == 4U) {
        return DecodeLoadStoreCategory(instruction, category);
    }
    if ((instruction & UINT32_C(0x7FE0E000)) ==
            UINT32_C(0x5AC00000) ||
        (group & 7U) == 5U) {
        return DecodeRegisterCategory(instruction, category);
    }
    if ((instruction & UINT32_C(0x5E000000)) ==
            UINT32_C(0x1E000000) ||
        (instruction & UINT32_C(0x1E000000)) ==
            UINT32_C(0x0E000000)) {
        category = kCoordinatePredecodeRawCategory;
        return true;
    }
    category = 117;
    return true;
}

bool DecodeDirectTarget(std::uint32_t instruction,
                        std::size_t offset,
                        std::size_t codeSize,
                        std::size_t& target) noexcept {
    std::int64_t displacement = 0;
    if ((instruction >> 26U) == 5U || (instruction >> 26U) == 37U) {
        displacement = SignExtend(instruction & UINT32_C(0x03FFFFFF), 26) * 4;
    } else if ((instruction & UINT32_C(0xFF000010)) ==
                   UINT32_C(0x54000000) ||
               (instruction & UINT32_C(0x7F000000)) ==
                   UINT32_C(0x34000000) ||
               (instruction & UINT32_C(0x7F000000)) ==
                   UINT32_C(0x35000000)) {
        displacement = SignExtend((instruction >> 5U) & 0x7FFFFU, 19) * 4;
    } else if ((instruction & UINT32_C(0x7F000000)) ==
                   UINT32_C(0x36000000) ||
               (instruction & UINT32_C(0x7F000000)) ==
                   UINT32_C(0x37000000)) {
        displacement = SignExtend((instruction >> 5U) & 0x3FFFU, 14) * 4;
    } else {
        return false;
    }

    const std::int64_t base = static_cast<std::int64_t>(offset);
    if ((displacement < 0 && base < -displacement) ||
        (displacement > 0 &&
         base > std::numeric_limits<std::int64_t>::max() - displacement)) {
        return false;
    }
    const std::int64_t result = base + displacement;
    if (result < 0 || static_cast<std::uint64_t>(result) + 4U > codeSize) {
        return false;
    }
    target = static_cast<std::size_t>(result);
    return true;
}

}  // namespace

bool IsCoordinateExecutionIgnorableInstruction(
    std::uint32_t instruction) noexcept {
    if (instruction == 0 || instruction == UINT32_MAX ||
        instruction == UINT32_C(0xB2B1B0AF) ||
        (instruction >> 16U) == 0) {
        return true;
    }

    const bool text =
        IsTextByte(static_cast<std::uint8_t>(instruction)) &&
        IsTextByte(static_cast<std::uint8_t>(instruction >> 8U)) &&
        IsTextByte(static_cast<std::uint8_t>(instruction >> 16U)) &&
        IsTextByte(static_cast<std::uint8_t>(instruction >> 24U));
    const std::uint32_t group = (instruction >> 25U) & 0xFU;
    const bool recognized =
        (group & 5U) == 4U ||
        (instruction & UINT32_C(0x7C000000)) == UINT32_C(0x14000000) ||
        (instruction & UINT32_C(0xFF000010)) == UINT32_C(0x54000000) ||
        (instruction & UINT32_C(0x7E000000)) == UINT32_C(0x34000000) ||
        (instruction & UINT32_C(0x7E000000)) == UINT32_C(0x36000000) ||
        (instruction & UINT32_C(0xFFFFFC1F)) == UINT32_C(0xD61F0000) ||
        (instruction & UINT32_C(0xFFFFFC1F)) == UINT32_C(0xD65F0000) ||
        (instruction & UINT32_C(0x5E000000)) == UINT32_C(0x1E000000) ||
        (instruction & UINT32_C(0x1E000000)) == UINT32_C(0x0E000000);
    if (text && !recognized) return true;

    return (group & 0xEU) != 8U && (group & 0xEU) != 0xAU &&
        (group & 5U) != 4U && (group & 7U) != 5U &&
        (instruction & UINT32_C(0x5E000000)) != UINT32_C(0x1E000000) &&
        (instruction & UINT32_C(0xBFF8FC00)) != UINT32_C(0x0F00E400) &&
        (instruction & UINT32_C(0xBFF8FC00)) != UINT32_C(0x2F00E400) &&
        (instruction & UINT32_C(0xBFF8FC00)) != UINT32_C(0x6F00E400) &&
        (instruction & UINT32_C(0x9F20FC00)) != UINT32_C(0x0E201C00) &&
        (instruction & UINT32_C(0x9F20FC00)) != UINT32_C(0x0E209C00) &&
        (instruction & UINT32_C(0x9F20FC00)) != UINT32_C(0x0E203400) &&
        (instruction & UINT32_C(0x9F20FC00)) != UINT32_C(0x0E206400) &&
        (instruction & UINT32_C(0x9F20FC00)) != UINT32_C(0x0E206C00) &&
        (instruction & UINT32_C(0x9F20FC00)) != UINT32_C(0x0E208C00) &&
        (instruction & UINT32_C(0xBF00FC00)) != UINT32_C(0x0E200400) &&
        (instruction & UINT32_C(0x1F20FC00)) != UINT32_C(0x0E208400) &&
        ((instruction & UINT32_C(0x9F800000)) != UINT32_C(0x0F000000) ||
         (instruction & UINT32_C(0x9FF80000)) == UINT32_C(0x0F000000)) &&
        (((instruction >> 12U) & 0x1FU) != 0x1BU ||
         ((instruction & UINT32_C(0xBF20FC00)) != UINT32_C(0x0E200800) &&
          (instruction & UINT32_C(0xBF3FFC00)) != UINT32_C(0x0E31B800))) &&
        (instruction & UINT32_C(0x9FE08400)) != UINT32_C(0x0E000400) &&
        (instruction & UINT32_C(0xFFE0FC00)) != UINT32_C(0x4E080400) &&
        (instruction & UINT32_C(0xBFE0FC00)) != UINT32_C(0x0E003C00) &&
        (instruction & UINT32_C(0x7FE0FC00)) != UINT32_C(0x5E000400) &&
        (instruction & UINT32_C(0xBF208C00)) != UINT32_C(0x0E000000) &&
        (instruction & UINT32_C(0xBFE08400)) != UINT32_C(0x2E000000) &&
        (instruction & UINT32_C(0xFF3E0C00)) != UINT32_C(0x5E280800) &&
        (instruction & UINT32_C(0xFF3E0C00)) != UINT32_C(0x5E000000) &&
        (instruction & UINT32_C(0xFFE0FC00)) != UINT32_C(0x5E004000) &&
        (instruction & UINT32_C(0xFFE0FC00)) != UINT32_C(0x5E004800) &&
        (instruction & UINT32_C(0xFF00FC00)) != UINT32_C(0x5E000000) &&
        (instruction & UINT32_C(0xBF20FC00)) != UINT32_C(0x0E20E000) &&
        (instruction & UINT32_C(0xBF9F2000)) != UINT32_C(0x0D400000) &&
        (instruction & UINT32_C(0xBF3FFC00)) != UINT32_C(0x0E202800) &&
        (instruction & UINT32_C(0xBF3FFC00)) != UINT32_C(0x0E206800) &&
        (instruction & UINT32_C(0xBF20FC00)) != UINT32_C(0x0E201000) &&
        (instruction & UINT32_C(0xBF20FC00)) != UINT32_C(0x0E203000) &&
        (instruction & UINT32_C(0xBF3FFC00)) != UINT32_C(0x0E205800) &&
        (instruction & UINT32_C(0xBF3FFC00)) != UINT32_C(0x2E205800) &&
        (instruction & UINT32_C(0xBF20FC00)) != UINT32_C(0x2E200800) &&
        (instruction & UINT32_C(0xBF208C00)) != UINT32_C(0x0E000800) &&
        ((instruction & UINT32_C(0x9F200000)) != UINT32_C(0x0E200000) ||
         (instruction & UINT32_C(0x400)) != 0) &&
        (instruction & UINT32_C(0x9FF80000)) != UINT32_C(0x0F000000) &&
        (instruction >> 12U) != UINT32_C(0xD50B7);
}

bool IsCoordinateExecutionControlFlowInstruction(
    std::uint32_t instruction) noexcept {
    return (instruction >> 26U) == 5U || (instruction >> 26U) == 37U ||
        (instruction & UINT32_C(0xFF000010)) == UINT32_C(0x54000000) ||
        (instruction & UINT32_C(0x7F000000)) == UINT32_C(0x34000000) ||
        (instruction & UINT32_C(0x7F000000)) == UINT32_C(0x35000000) ||
        (instruction & UINT32_C(0x7F000000)) == UINT32_C(0x36000000) ||
        (instruction & UINT32_C(0x7F000000)) == UINT32_C(0x37000000) ||
        (instruction & UINT32_C(0xFFFFFC1F)) == UINT32_C(0xD61F0000) ||
        (instruction & UINT32_C(0xFFFFFC1F)) == UINT32_C(0xD63F0000) ||
        (instruction & UINT32_C(0xFFFFFC1F)) == UINT32_C(0xD65F0000);
}

CoordinatePredecodedInstruction PredecodeCoordinateInstruction(
    std::uint32_t instruction) noexcept {
    CoordinatePredecodedInstruction decoded{};
    decoded.instruction = instruction;
    decoded.registers[0] = static_cast<std::uint8_t>(instruction & 0x1FU);
    decoded.registers[1] =
        static_cast<std::uint8_t>((instruction >> 5U) & 0x1FU);
    decoded.registers[2] =
        static_cast<std::uint8_t>((instruction >> 16U) & 0x1FU);
    decoded.operands[0] = (instruction >> 10U) & 0xFFFU;
    decoded.operands[1] = (instruction >> 12U) & 0xFFFFU;
    decoded.operands[2] = (instruction >> 5U) & 0x7FFFFU;
    decoded.operands[3] = instruction & 0x03FFFFFFU;

    if (!DecodeCategory(instruction, decoded.category)) {
        decoded.category = IsCoordinateExecutionIgnorableInstruction(
                               instruction)
            ? kCoordinatePredecodeAdvanceCategory
            : kCoordinatePredecodeRawCategory;
    }
    return decoded;
}

bool CoordinateExecutionBackendCache::BindCode(std::uint64_t codeBase,
                                                std::size_t codeSize) {
    if (codeBase == 0 || codeSize < sizeof(std::uint32_t)) return false;
    if (codeBase_ != codeBase || codeSize_ != codeSize) {
        Reset();
        codeBase_ = codeBase;
        codeSize_ = codeSize;
    }
    return true;
}

bool CoordinateExecutionBackendCache::BuildPredecode(
    std::uint64_t codeBase,
    const std::uint8_t* code,
    std::size_t codeSize) {
    predecodeReady_ = false;
    records_.clear();
    if (code == nullptr || !BindCode(codeBase, codeSize)) return false;

    const std::size_t count = codeSize / sizeof(std::uint32_t);
    try {
        records_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            std::uint32_t instruction = 0;
            std::memcpy(
                &instruction, code + index * sizeof(instruction),
                sizeof(instruction));
            records_.push_back(PredecodeCoordinateInstruction(instruction));
        }
    } catch (...) {
        records_.clear();
        return false;
    }
    predecodeReady_ = records_.size() == count;
    return predecodeReady_;
}

bool CoordinateExecutionBackendCache::BuildJit(std::uint64_t codeBase,
                                                const std::uint8_t* code,
                                                std::size_t codeSize) {
    jitReady_ = false;
    blocks_.clear();
    blockByInstruction_.clear();
    if (code == nullptr || !BindCode(codeBase, codeSize)) return false;

    const std::size_t count = codeSize / sizeof(std::uint32_t);
    try {
        std::vector<std::uint32_t> instructions(count);
        std::memcpy(
            instructions.data(), code, count * sizeof(std::uint32_t));
        std::vector<bool> starts(count, false);
        starts[0] = true;
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint32_t instruction = instructions[index];
            if (!IsCoordinateExecutionControlFlowInstruction(instruction)) {
                continue;
            }
            std::size_t target = 0;
            const bool hasInRangeTarget = DecodeDirectTarget(
                    instruction, index * sizeof(std::uint32_t), codeSize,
                    target);
            if (hasInRangeTarget) {
                starts[target / sizeof(std::uint32_t)] = true;
            }
            const bool unconditionalOrIndirect =
                (instruction >> 26U) == 5U ||
                (instruction >> 26U) == 37U ||
                (instruction & UINT32_C(0xFFFFFC1F)) ==
                    UINT32_C(0xD61F0000) ||
                (instruction & UINT32_C(0xFFFFFC1F)) ==
                    UINT32_C(0xD63F0000) ||
                (instruction & UINT32_C(0xFFFFFC1F)) ==
                    UINT32_C(0xD65F0000);
            if ((unconditionalOrIndirect || hasInRangeTarget) &&
                index + 1 < count) {
                starts[index + 1] = true;
            }
        }

        blockByInstruction_.assign(count, -1);
        for (std::size_t start = 0; start < count; ++start) {
            if (!starts[start]) continue;
            CoordinateJitBlock block{};
            block.startIndex = static_cast<std::uint32_t>(start);
            for (std::size_t index = start;
                 index < count &&
                 block.instructionCount <
                     kCoordinateJitMaximumBlockInstructions;
                 ++index) {
                if (index != start && starts[index]) break;
                ++block.instructionCount;
                if (IsCoordinateExecutionControlFlowInstruction(
                        instructions[index])) {
                    block.endsInControlFlow = true;
                    break;
                }
            }
            if (block.instructionCount == 0) continue;
            const std::int32_t blockIndex =
                static_cast<std::int32_t>(blocks_.size());
            blockByInstruction_[start] = blockIndex;
            blocks_.push_back(block);
        }
    } catch (...) {
        blocks_.clear();
        blockByInstruction_.clear();
        return false;
    }
    jitReady_ = !blocks_.empty();
    return jitReady_;
}

void CoordinateExecutionBackendCache::Reset() noexcept {
    codeBase_ = 0;
    codeSize_ = 0;
    predecodeReady_ = false;
    jitReady_ = false;
    records_.clear();
    blocks_.clear();
    blockByInstruction_.clear();
}

const CoordinatePredecodedInstruction*
CoordinateExecutionBackendCache::FindPredecoded(
    std::uint64_t pc) const noexcept {
    if (!predecodeReady_ || pc < codeBase_) return nullptr;
    const std::uint64_t offset = pc - codeBase_;
    if ((offset & 3U) != 0 || offset / 4U >= records_.size()) return nullptr;
    return &records_[static_cast<std::size_t>(offset / 4U)];
}

const CoordinateJitBlock* CoordinateExecutionBackendCache::FindJitBlock(
    std::uint64_t pc) const noexcept {
    if (!jitReady_ || pc < codeBase_) return nullptr;
    const std::uint64_t offset = pc - codeBase_;
    if ((offset & 3U) != 0 || offset / 4U >= blockByInstruction_.size()) {
        return nullptr;
    }
    const std::int32_t block =
        blockByInstruction_[static_cast<std::size_t>(offset / 4U)];
    if (block < 0 || static_cast<std::size_t>(block) >= blocks_.size()) {
        return nullptr;
    }
    return &blocks_[static_cast<std::size_t>(block)];
}

std::uint32_t CoordinateExecutionBackendCache::PredecodedRunLength(
    std::uint64_t pc,
    std::uint32_t maximum) const noexcept {
    if (!predecodeReady_ || maximum == 0 || pc < codeBase_) return 0;
    const std::uint64_t offset = pc - codeBase_;
    if ((offset & 3U) != 0 || offset / 4U >= records_.size()) return 0;

    const std::size_t first = static_cast<std::size_t>(offset / 4U);
    std::uint32_t count = 0;
    while (count < maximum && first + count < records_.size()) {
        const CoordinatePredecodedInstruction& record =
            records_[first + count];
        if (record.category == kCoordinatePredecodeRawCategory ||
            record.category == kCoordinatePredecodeAdvanceCategory ||
            record.category > kCoordinatePredecodeMaximumCategory) {
            break;
        }
        ++count;
        if (IsCoordinateExecutionControlFlowInstruction(record.instruction)) {
            break;
        }
    }
    return count;
}

CoordinateBackendSlice SelectCoordinateBackendSlice(
    CoordinateExecutionMode mode,
    const CoordinateExecutionBackendCache& cache,
    std::uint64_t pc) noexcept {
    CoordinateBackendSlice slice{};
    if (mode == CoordinateExecutionMode::Predecode) {
        const CoordinatePredecodedInstruction* decoded =
            cache.FindPredecoded(pc);
        if (decoded == nullptr ||
            decoded->category == kCoordinatePredecodeRawCategory ||
            decoded->category > kCoordinatePredecodeMaximumCategory) {
            return slice;
        }
        slice.category = decoded->category;
        slice.dispatch =
            decoded->category == kCoordinatePredecodeAdvanceCategory
            ? CoordinateBackendDispatch::Advance
            : CoordinateBackendDispatch::Predecoded;
        if (slice.dispatch == CoordinateBackendDispatch::Predecoded) {
            slice.instructionCount = cache.PredecodedRunLength(pc);
            if (slice.instructionCount == 0) {
                slice.dispatch = CoordinateBackendDispatch::Dynamic;
                slice.instructionCount = 1;
            }
        }
        return slice;
    }
    if (mode == CoordinateExecutionMode::Jit) {
        const CoordinateJitBlock* block = cache.FindJitBlock(pc);
        if (block == nullptr) return slice;
        slice.dispatch = CoordinateBackendDispatch::CompiledBlock;
        slice.instructionCount = block->instructionCount;
        slice.category = 0;
        return slice;
    }
    return slice;
}

}  // namespace lengjing::game::native
