#include "game/native/Arm64Pacga.h"

#include <array>
#include <cassert>

namespace lengjing::game::native {
namespace {

/*
 * ARM v8.3-PAuth Operations
 *
 * Copyright (c) 2019 Linaro, Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */
std::uint64_t ExtractBits64(std::uint64_t value, int start, int length) {
    assert(start >= 0 && length > 0 && length <= 64 - start);
    return (value >> start) & (~0ULL >> (64 - length));
}

std::uint32_t ExtractBits32(std::uint32_t value, int start, int length) {
    assert(start >= 0 && length > 0 && length <= 32 - start);
    return (value >> start) & (~0U >> (32 - length));
}

std::uint64_t PacCellShuffle(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= ExtractBits64(value, 52, 4);
    result |= ExtractBits64(value, 24, 4) << 4;
    result |= ExtractBits64(value, 44, 4) << 8;
    result |= ExtractBits64(value, 0, 4) << 12;
    result |= ExtractBits64(value, 28, 4) << 16;
    result |= ExtractBits64(value, 48, 4) << 20;
    result |= ExtractBits64(value, 4, 4) << 24;
    result |= ExtractBits64(value, 40, 4) << 28;
    result |= ExtractBits64(value, 32, 4) << 32;
    result |= ExtractBits64(value, 12, 4) << 36;
    result |= ExtractBits64(value, 56, 4) << 40;
    result |= ExtractBits64(value, 20, 4) << 44;
    result |= ExtractBits64(value, 8, 4) << 48;
    result |= ExtractBits64(value, 36, 4) << 52;
    result |= ExtractBits64(value, 16, 4) << 56;
    result |= ExtractBits64(value, 60, 4) << 60;
    return result;
}

std::uint64_t PacCellInverseShuffle(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= ExtractBits64(value, 12, 4);
    result |= ExtractBits64(value, 24, 4) << 4;
    result |= ExtractBits64(value, 48, 4) << 8;
    result |= ExtractBits64(value, 36, 4) << 12;
    result |= ExtractBits64(value, 56, 4) << 16;
    result |= ExtractBits64(value, 44, 4) << 20;
    result |= ExtractBits64(value, 4, 4) << 24;
    result |= ExtractBits64(value, 16, 4) << 28;
    result |= value & (0xFULL << 32);
    result |= ExtractBits64(value, 52, 4) << 36;
    result |= ExtractBits64(value, 28, 4) << 40;
    result |= ExtractBits64(value, 8, 4) << 44;
    result |= ExtractBits64(value, 20, 4) << 48;
    result |= ExtractBits64(value, 0, 4) << 52;
    result |= ExtractBits64(value, 40, 4) << 56;
    result |= value & (0xFULL << 60);
    return result;
}

std::uint64_t PacSubstitute(std::uint64_t value) {
    static constexpr std::array<std::uint8_t, 16> kSubstitution{
        0xB, 0x6, 0x8, 0xF, 0xC, 0x0, 0x9, 0xE,
        0x3, 0x7, 0x4, 0x5, 0xD, 0x2, 0x1, 0xA,
    };
    std::uint64_t result = 0;
    for (int bit = 0; bit < 64; bit += 4) {
        result |= static_cast<std::uint64_t>(
                      kSubstitution[(value >> bit) & 0xF])
            << bit;
    }
    return result;
}

std::uint64_t PacInverseSubstitute(std::uint64_t value) {
    static constexpr std::array<std::uint8_t, 16> kSubstitution{
        0x5, 0xE, 0xD, 0x8, 0xA, 0xB, 0x1, 0x9,
        0x2, 0x6, 0xF, 0x0, 0x4, 0xC, 0x7, 0x3,
    };
    std::uint64_t result = 0;
    for (int bit = 0; bit < 64; bit += 4) {
        result |= static_cast<std::uint64_t>(
                      kSubstitution[(value >> bit) & 0xF])
            << bit;
    }
    return result;
}

int RotatePacCell(int cell, int amount) {
    cell |= cell << 4;
    return static_cast<int>(ExtractBits32(
        static_cast<std::uint32_t>(cell), 4 - amount, 4));
}

std::uint64_t PacMultiply(std::uint64_t value) {
    std::uint64_t result = 0;
    for (int bit = 0; bit < 16; bit += 4) {
        const int i0 = static_cast<int>(ExtractBits64(value, bit, 4));
        const int i4 = static_cast<int>(ExtractBits64(value, bit + 16, 4));
        const int i8 = static_cast<int>(ExtractBits64(value, bit + 32, 4));
        const int ic = static_cast<int>(ExtractBits64(value, bit + 48, 4));
        const int t0 = RotatePacCell(i8, 1) ^ RotatePacCell(i4, 2) ^
            RotatePacCell(i0, 1);
        const int t1 = RotatePacCell(ic, 1) ^ RotatePacCell(i4, 1) ^
            RotatePacCell(i0, 2);
        const int t2 = RotatePacCell(ic, 2) ^ RotatePacCell(i8, 1) ^
            RotatePacCell(i0, 1);
        const int t3 = RotatePacCell(ic, 1) ^ RotatePacCell(i8, 2) ^
            RotatePacCell(i4, 1);
        result |= static_cast<std::uint64_t>(t3) << bit;
        result |= static_cast<std::uint64_t>(t2) << (bit + 16);
        result |= static_cast<std::uint64_t>(t1) << (bit + 32);
        result |= static_cast<std::uint64_t>(t0) << (bit + 48);
    }
    return result;
}

std::uint64_t RotatePacTweakCell(std::uint64_t cell) {
    return (cell >> 1) | (((cell ^ (cell >> 1)) & 1) << 3);
}

std::uint64_t ShufflePacTweak(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= ExtractBits64(value, 16, 4);
    result |= ExtractBits64(value, 20, 4) << 4;
    result |= RotatePacTweakCell(ExtractBits64(value, 24, 4)) << 8;
    result |= ExtractBits64(value, 28, 4) << 12;
    result |= RotatePacTweakCell(ExtractBits64(value, 44, 4)) << 16;
    result |= ExtractBits64(value, 8, 4) << 20;
    result |= ExtractBits64(value, 12, 4) << 24;
    result |= RotatePacTweakCell(ExtractBits64(value, 32, 4)) << 28;
    result |= ExtractBits64(value, 48, 4) << 32;
    result |= ExtractBits64(value, 52, 4) << 36;
    result |= ExtractBits64(value, 56, 4) << 40;
    result |= RotatePacTweakCell(ExtractBits64(value, 60, 4)) << 44;
    result |= RotatePacTweakCell(ExtractBits64(value, 0, 4)) << 48;
    result |= ExtractBits64(value, 4, 4) << 52;
    result |= RotatePacTweakCell(ExtractBits64(value, 40, 4)) << 56;
    result |= RotatePacTweakCell(ExtractBits64(value, 36, 4)) << 60;
    return result;
}

std::uint64_t InverseRotatePacTweakCell(std::uint64_t cell) {
    return ((cell << 1) & 0xF) | ((cell & 1) ^ (cell >> 3));
}

std::uint64_t InverseShufflePacTweak(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 48, 4));
    result |= ExtractBits64(value, 52, 4) << 4;
    result |= ExtractBits64(value, 20, 4) << 8;
    result |= ExtractBits64(value, 24, 4) << 12;
    result |= ExtractBits64(value, 0, 4) << 16;
    result |= ExtractBits64(value, 4, 4) << 20;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 8, 4)) << 24;
    result |= ExtractBits64(value, 12, 4) << 28;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 28, 4)) << 32;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 60, 4)) << 36;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 56, 4)) << 40;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 16, 4)) << 44;
    result |= ExtractBits64(value, 32, 4) << 48;
    result |= ExtractBits64(value, 36, 4) << 52;
    result |= ExtractBits64(value, 40, 4) << 56;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 44, 4)) << 60;
    return result;
}

std::uint64_t ComputePacgaRaw(std::uint64_t data,
                              std::uint64_t modifier,
                              const Arm64PacgaKey& key) {
    static constexpr std::array<std::uint64_t, 5> kRoundConstants{
        0x0000000000000000ULL,
        0x13198A2E03707344ULL,
        0xA4093822299F31D0ULL,
        0x082EFA98EC4E6C89ULL,
        0x452821E638D01377ULL,
    };
    constexpr std::uint64_t kAlpha = 0xC0AC29B7C97C50DDULL;
    const std::uint64_t key0 = key.high;
    const std::uint64_t key1 = key.low;
    const std::uint64_t modifiedKey0 =
        (key0 << 63) | ((key0 >> 1) ^ (key0 >> 63));
    std::uint64_t runningModifier = modifier;
    std::uint64_t workingValue = data ^ key0;

    for (std::size_t round = 0; round <= 4; ++round) {
        workingValue ^= key1 ^ runningModifier;
        workingValue ^= kRoundConstants[round];
        if (round > 0) {
            workingValue = PacCellShuffle(workingValue);
            workingValue = PacMultiply(workingValue);
        }
        workingValue = PacSubstitute(workingValue);
        runningModifier = ShufflePacTweak(runningModifier);
    }

    workingValue ^= modifiedKey0 ^ runningModifier;
    workingValue = PacCellShuffle(workingValue);
    workingValue = PacMultiply(workingValue);
    workingValue = PacSubstitute(workingValue);
    workingValue = PacCellShuffle(workingValue);
    workingValue = PacMultiply(workingValue);
    workingValue ^= key1;
    workingValue = PacCellInverseShuffle(workingValue);
    workingValue = PacInverseSubstitute(workingValue);
    workingValue = PacMultiply(workingValue);
    workingValue = PacCellInverseShuffle(workingValue);
    workingValue ^= key0;
    workingValue ^= runningModifier;

    for (std::size_t round = 0; round <= 4; ++round) {
        workingValue = PacInverseSubstitute(workingValue);
        if (round < 4) {
            workingValue = PacMultiply(workingValue);
            workingValue = PacCellInverseShuffle(workingValue);
        }
        runningModifier = InverseShufflePacTweak(runningModifier);
        workingValue ^= kRoundConstants[4 - round];
        workingValue ^= key1 ^ runningModifier;
        workingValue ^= kAlpha;
    }
    return workingValue ^ modifiedKey0;
}

}  // namespace

std::uint64_t ComputeArm64Pacga(
    std::uint64_t data,
    std::uint64_t modifier,
    const Arm64PacgaKey& key) noexcept {
    return FormatArm64PacgaResult(ComputePacgaRaw(data, modifier, key));
}

}  // namespace lengjing::game::native
