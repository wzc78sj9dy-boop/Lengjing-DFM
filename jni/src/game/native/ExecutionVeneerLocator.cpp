#if 0

#include "game/native/ExecutionVeneerLocator.h"

#include <array>
#include <cstring>
#include <limits>

namespace lengjing::game::native {
namespace {

constexpr std::size_t kInstructionSize = 4;
constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kScanPageCount = 4;
constexpr std::uint32_t kLdrXLiteralMask = UINT32_C(0xff000000);
constexpr std::uint32_t kLdrXLiteralValue = UINT32_C(0x58000000);
constexpr std::uint32_t kBrRegisterMask = UINT32_C(0xfffffc1f);
constexpr std::uint32_t kBrRegisterValue = UINT32_C(0xd61f0000);
constexpr std::uint64_t kLow56Mask = UINT64_C(0x00ffffffffffffff);

std::uint32_t DecodeLittleEndian32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t DecodeLittleEndian64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return value;
}

bool AddAddress(std::uintptr_t base,
                std::uintptr_t offset,
                std::uintptr_t& result) noexcept {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base) {
        return false;
    }
    result = base + offset;
    return true;
}

bool AddSignedDisplacement(std::uintptr_t base,
                           std::int64_t displacement,
                           std::uintptr_t& result) noexcept {
    if (displacement >= 0) {
        return AddAddress(
            base, static_cast<std::uintptr_t>(displacement), result);
    }

    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(-(displacement + 1)) + 1U;
    if (magnitude > base) return false;
    result = base - static_cast<std::uintptr_t>(magnitude);
    return true;
}

bool DecodeLiteralAddress(std::uintptr_t instructionAddress,
                          std::uint32_t instruction,
                          std::uintptr_t& literalAddress) noexcept {
    if ((instruction & kLdrXLiteralMask) != kLdrXLiteralValue) {
        return false;
    }

    const std::uint32_t encodedImmediate =
        (instruction >> 5U) & UINT32_C(0x7ffff);
    const std::int64_t signedImmediate =
        (encodedImmediate & UINT32_C(0x40000)) != 0
        ? static_cast<std::int64_t>(encodedImmediate) -
            static_cast<std::int64_t>(UINT32_C(0x80000))
        : static_cast<std::int64_t>(encodedImmediate);
    return AddSignedDisplacement(
        instructionAddress, signedImmediate * 4, literalAddress);
}

bool DecodeVeneer(std::uintptr_t veneerAddress,
                   std::uint32_t ldrInstruction,
                   std::uint32_t brInstruction,
                   std::uintptr_t& literalAddress) noexcept {
    if ((brInstruction & kBrRegisterMask) != kBrRegisterValue ||
        (ldrInstruction & UINT32_C(0x1f)) !=
            ((brInstruction >> 5U) & UINT32_C(0x1f))) {
        return false;
    }
    return DecodeLiteralAddress(
        veneerAddress, ldrInstruction, literalAddress);
}

bool ReadLiteral(const ExecutionVeneerReadMemory& readMemory,
                 std::uintptr_t literalAddress,
                 std::uint64_t& literal) {
    std::array<std::uint8_t, sizeof(std::uint64_t)> bytes{};
    if (!readMemory(
            literalAddress, bytes.data(), bytes.size())) {
        return false;
    }
    literal = DecodeLittleEndian64(bytes.data());
    return true;
}

bool ReadFirstTarget(const ExecutionVeneerReadMemory& readMemory,
                     std::uintptr_t firstVeneer,
                     std::uintptr_t& firstTarget) {
    std::array<std::uint8_t, kInstructionSize * 2> instructions{};
    if (!readMemory(
            firstVeneer, instructions.data(), instructions.size())) {
        return false;
    }

    std::uintptr_t literalAddress = 0;
    if (!DecodeVeneer(
            firstVeneer,
            DecodeLittleEndian32(instructions.data()),
            DecodeLittleEndian32(
                instructions.data() + kInstructionSize),
            literalAddress)) {
        return false;
    }

    std::uint64_t literal = 0;
    if (!ReadLiteral(readMemory, literalAddress, literal)) {
        return false;
    }
    const std::uint64_t lowTarget = literal & kLow56Mask;
    if (lowTarget >
        std::numeric_limits<std::uintptr_t>::max()) {
        return false;
    }
    firstTarget = static_cast<std::uintptr_t>(lowTarget);
    return firstTarget != 0;
}

bool ReadCandidateBranch(
    const ExecutionVeneerReadMemory& readMemory,
    const std::array<std::uint8_t, kPageSize>& page,
    std::uintptr_t candidateAddress,
    std::size_t pageOffset,
    std::uint32_t& branchInstruction) {
    if (pageOffset <= kPageSize - kInstructionSize * 2) {
        branchInstruction = DecodeLittleEndian32(
            page.data() + pageOffset + kInstructionSize);
        return true;
    }

    std::uintptr_t branchAddress = 0;
    if (!AddAddress(
            candidateAddress, kInstructionSize, branchAddress)) {
        return false;
    }
    std::array<std::uint8_t, kInstructionSize> bytes{};
    if (!readMemory(branchAddress, bytes.data(), bytes.size())) {
        return false;
    }
    branchInstruction = DecodeLittleEndian32(bytes.data());
    return true;
}

bool ReadCandidateLiteral(
    const ExecutionVeneerReadMemory& readMemory,
    const std::array<std::uint8_t, kPageSize>& page,
    std::uintptr_t pageAddress,
    std::uintptr_t literalAddress,
    std::uint64_t& literal) {
    if (literalAddress >= pageAddress) {
        const std::uintptr_t offset = literalAddress - pageAddress;
        if (offset <= kPageSize - sizeof(std::uint64_t)) {
            literal = DecodeLittleEndian64(
                page.data() + static_cast<std::size_t>(offset));
            return true;
        }
    }
    return ReadLiteral(readMemory, literalAddress, literal);
}

}  // namespace

bool LocateSecondExecutionVeneer(
    std::uintptr_t moduleBase,
    std::uintptr_t firstVeneerRva,
    const ExecutionVeneerReadMemory& readMemory,
    std::uintptr_t& secondVeneerAddress) {
    secondVeneerAddress = 0;
    if (!readMemory || (moduleBase & 3U) != 0 ||
        (firstVeneerRva & 3U) != 0) {
        return false;
    }

    std::uintptr_t firstVeneer = 0;
    std::uintptr_t expectedSecondTarget = 0;
    if (!AddAddress(moduleBase, firstVeneerRva, firstVeneer) ||
        (firstVeneer & 3U) != 0 ||
        !AddAddress(
            firstVeneer, 4U * kInstructionSize, expectedSecondTarget)) {
        return false;
    }

    std::uintptr_t firstTarget = 0;
    if (!ReadFirstTarget(readMemory, firstVeneer, firstTarget) ||
        (firstTarget & 3U) != 0) {
        return false;
    }

    const std::uintptr_t scanBase =
        firstTarget & ~static_cast<std::uintptr_t>(kPageSize - 1U);
    std::uintptr_t scanEnd = 0;
    if (!AddAddress(
            scanBase,
            static_cast<std::uintptr_t>(
                kPageSize * kScanPageCount),
            scanEnd)) {
        return false;
    }

    std::uintptr_t match = 0;
    std::size_t matchCount = 0;
    std::array<std::uint8_t, kPageSize> page{};
    for (std::size_t pageIndex = 0;
         pageIndex < kScanPageCount; ++pageIndex) {
        std::uintptr_t pageAddress = 0;
        if (!AddAddress(
                scanBase,
                static_cast<std::uintptr_t>(pageIndex * kPageSize),
                pageAddress) ||
            !readMemory(pageAddress, page.data(), page.size())) {
            return false;
        }

        for (std::size_t pageOffset = 0;
             pageOffset < kPageSize;
             pageOffset += kInstructionSize) {
            std::uintptr_t candidateAddress = 0;
            if (!AddAddress(
                    pageAddress,
                    static_cast<std::uintptr_t>(pageOffset),
                    candidateAddress) ||
                candidateAddress >= scanEnd) {
                return false;
            }

            const std::uint32_t ldrInstruction =
                DecodeLittleEndian32(page.data() + pageOffset);
            if ((ldrInstruction & kLdrXLiteralMask) !=
                kLdrXLiteralValue) {
                continue;
            }

            std::uint32_t brInstruction = 0;
            if (!ReadCandidateBranch(
                    readMemory,
                    page,
                    candidateAddress,
                    pageOffset,
                    brInstruction)) {
                continue;
            }

            std::uintptr_t literalAddress = 0;
            if (!DecodeVeneer(
                    candidateAddress,
                    ldrInstruction,
                    brInstruction,
                    literalAddress)) {
                continue;
            }

            std::uint64_t literal = 0;
            if (!ReadCandidateLiteral(
                    readMemory,
                    page,
                    pageAddress,
                    literalAddress,
                    literal)) {
                continue;
            }
            if ((literal & kLow56Mask) != expectedSecondTarget) {
                continue;
            }

            match = candidateAddress;
            ++matchCount;
            if (matchCount > 1) return false;
        }
    }

    if (matchCount != 1) return false;
    secondVeneerAddress = match;
    return true;
}

}  // namespace lengjing::game::native

#endif
