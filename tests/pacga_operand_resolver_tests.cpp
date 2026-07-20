#include "game/native/PacgaOperandResolver.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using lengjing::game::native::PacgaOperands;
using lengjing::game::native::ResolvePacgaOperandsFromImmediateBlock;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw std::runtime_error(                                           \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +      \
                ": check failed: " #condition);                               \
        }                                                                       \
    } while (false)

void TestFirstRuntimeShape() {
    constexpr std::array<std::uint32_t, 6> instructions{
        UINT32_C(0x5284B8E8),
        UINT32_C(0x52980169),
        UINT32_C(0x528D7BAA),
        UINT32_C(0x72A824C8),
        UINT32_C(0x72B76F49),
        UINT32_C(0x9AC93108),
    };
    PacgaOperands operands{};
    CHECK(ResolvePacgaOperandsFromImmediateBlock(
        instructions.data(), instructions.size(), 5, operands));
    CHECK(operands.data == UINT64_C(0x412625C7));
    CHECK(operands.modifier == UINT64_C(0xBB7AC00B));
}

void TestRegeneratedRuntimeShape() {
    constexpr std::array<std::uint32_t, 5> instructions{
        UINT32_C(0x52897068),
        UINT32_C(0x529BBDE9),
        UINT32_C(0x72B00AC8),
        UINT32_C(0x72A28969),
        UINT32_C(0x9AC93108),
    };
    PacgaOperands operands{};
    CHECK(ResolvePacgaOperandsFromImmediateBlock(
        instructions.data(), instructions.size(), 4, operands));
    CHECK(operands.data == UINT64_C(0x80564B83));
    CHECK(operands.modifier == UINT64_C(0x144BDDEF));
}

void TestInterleavedUnrelatedRegisterMove() {
    constexpr std::array<std::uint32_t, 6> instructions{
        UINT32_C(0x52897068),
        UINT32_C(0x529BBDE9),
        UINT32_C(0x72B00AC8),
        UINT32_C(0xAA1903FD),
        UINT32_C(0x72A28969),
        UINT32_C(0x9AC93108),
    };
    PacgaOperands operands{};
    CHECK(ResolvePacgaOperandsFromImmediateBlock(
        instructions.data(), instructions.size(), 5, operands));
    CHECK(operands.data == UINT64_C(0x80564B83));
    CHECK(operands.modifier == UINT64_C(0x144BDDEF));
}

void TestRejectsUnknownSourceOverwrite() {
    constexpr std::array<std::uint32_t, 4> instructions{
        UINT32_C(0x52800028),
        UINT32_C(0x52800049),
        UINT32_C(0xAA0003E8),
        UINT32_C(0x9AC93108),
    };
    PacgaOperands operands{};
    CHECK(!ResolvePacgaOperandsFromImmediateBlock(
        instructions.data(), instructions.size(), 3, operands));
}

void TestRejectsNonImmediateInputs() {
    constexpr std::array<std::uint32_t, 3> instructions{
        UINT32_C(0xAA0003E8),
        UINT32_C(0xAA0103E9),
        UINT32_C(0x9AC93108),
    };
    PacgaOperands operands{1, 2};
    CHECK(!ResolvePacgaOperandsFromImmediateBlock(
        instructions.data(), instructions.size(), 2, operands));
    CHECK(operands.data == 0);
    CHECK(operands.modifier == 0);
}

void TestRejectsDifferentInstruction() {
    constexpr std::array<std::uint32_t, 3> instructions{
        UINT32_C(0x52800008),
        UINT32_C(0x52800009),
        UINT32_C(0xD503201F),
    };
    PacgaOperands operands{};
    CHECK(!ResolvePacgaOperandsFromImmediateBlock(
        instructions.data(), instructions.size(), 2, operands));
}

}  // namespace

int main() {
    try {
        TestFirstRuntimeShape();
        TestRegeneratedRuntimeShape();
        TestInterleavedUnrelatedRegisterMove();
        TestRejectsUnknownSourceOverwrite();
        TestRejectsNonImmediateInputs();
        TestRejectsDifferentInstruction();
        std::cout << "pacga operand resolver tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
