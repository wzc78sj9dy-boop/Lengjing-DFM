#include "game/native/AlgorithmPositionRuntime.h"
#include "game/native/Arm64Pacga.h"

#include <array>
#include <cstdint>
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

using lengjing::game::native::Arm64PacgaKey;
using lengjing::game::native::ComputeArm64Pacga;
using lengjing::game::native::FormatAlgorithmPacgaResult;
using lengjing::game::native::FormatArm64PacgaResult;

struct TestVector {
    std::uint64_t data;
    std::uint64_t modifier;
    Arm64PacgaKey key;
    std::uint64_t expected;
};

constexpr std::array<TestVector, 5> kTestVectors{{
    {0,
     0,
     {0, 0},
     UINT64_C(0x76243B9500000000)},
    {UINT64_C(0x0123456789ABCDEF),
     UINT64_C(0xFEDCBA9876543210),
     {UINT64_C(0x0011223344556677), UINT64_C(0x8899AABBCCDDEEFF)},
     UINT64_C(0x7220062500000000)},
    {UINT64_MAX,
     UINT64_MAX,
     {UINT64_MAX, UINT64_MAX},
     UINT64_C(0x56B6776D00000000)},
    {UINT64_C(0xDEADBEEFCAFEBABE),
     UINT64_C(0x0F1E2D3C4B5A6978),
     {UINT64_C(0x0123456789ABCDEF), UINT64_C(0xFEDCBA9876543210)},
     UINT64_C(0xBF925B0A00000000)},
    {UINT64_C(0x0000007012345678),
     UINT64_C(0x000000709ABCDEF0),
     {UINT64_C(0x13579BDF2468ACE0), UINT64_C(0x0ECA8642FDB97531)},
     UINT64_C(0x4957D09F00000000)},
}};

void TestFormattingCompatibility() {
    static_assert(FormatArm64PacgaResult(
                      UINT64_C(0x123456789ABCDEF0)) ==
                  UINT64_C(0x1234567800000000));
    static_assert(FormatArm64PacgaResult(UINT64_MAX) ==
                  FormatAlgorithmPacgaResult(UINT64_MAX));
    REQUIRE(FormatArm64PacgaResult(UINT64_C(0x00000000FFFFFFFF)) ==
            FormatAlgorithmPacgaResult(UINT64_C(0x00000000FFFFFFFF)));
}

void TestFixedVectors() {
    for (const TestVector& vector : kTestVectors) {
        const std::uint64_t actual = ComputeArm64Pacga(
            vector.data, vector.modifier, vector.key);
        REQUIRE(actual == vector.expected);
        REQUIRE((actual & UINT64_C(0x00000000FFFFFFFF)) == 0);
    }
}

void TestInputSensitivity() {
    const TestVector& base = kTestVectors[1];
    const std::uint64_t expected = ComputeArm64Pacga(
        base.data, base.modifier, base.key);
    REQUIRE(ComputeArm64Pacga(
                base.data ^ 1U, base.modifier, base.key) != expected);
    REQUIRE(ComputeArm64Pacga(
                base.data, base.modifier ^ 1U, base.key) != expected);
    REQUIRE(ComputeArm64Pacga(
                base.data,
                base.modifier,
                Arm64PacgaKey{base.key.low ^ 1U, base.key.high}) != expected);
    REQUIRE(ComputeArm64Pacga(
                base.data,
                base.modifier,
                Arm64PacgaKey{base.key.low, base.key.high ^ 1U}) != expected);
}

}  // namespace

int main() {
    TestFormattingCompatibility();
    TestFixedVectors();
    TestInputSensitivity();
    return 0;
}
