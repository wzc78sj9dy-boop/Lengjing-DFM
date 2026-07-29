#include "game/native/Arm64Pacga.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using lengjing::game::native::Arm64PacgaKey;
using lengjing::game::native::ComputeArm64Pacga;

struct TestVector {
    std::uint64_t data;
    std::uint64_t modifier;
    Arm64PacgaKey key;
    std::uint64_t expected;
};

constexpr std::array<TestVector, 5> kVectors{{
    {0, 0, {0, 0}, UINT64_C(0x76243B9500000000)},
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

}  // namespace

int main() {
    for (const TestVector& vector : kVectors) {
        if (ComputeArm64Pacga(
                vector.data,
                vector.modifier,
                vector.key) != vector.expected) {
            std::cerr << "PACGA vector mismatch\n";
            return 1;
        }
    }
    std::cout << "PACGA vectors passed\n";
    return 0;
}
