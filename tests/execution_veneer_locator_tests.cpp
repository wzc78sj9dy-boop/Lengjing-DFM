#if 0

#include "game/native/ExecutionVeneerLocator.h"
#include "test_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr std::uintptr_t kModuleBase = UINT64_C(0x7000000000);
constexpr std::uintptr_t kFirstVeneerRva = UINT64_C(0x1000);
constexpr std::uintptr_t kScanBase = UINT64_C(0x7100000000);
constexpr std::size_t kPageSize = 4096;

struct ReadRequest {
    std::uintptr_t address = 0;
    std::size_t size = 0;
};

struct MemoryRegion {
    std::uintptr_t base = 0;
    std::vector<std::uint8_t> bytes;
};

class FakeMemory final {
public:
    void AddRegion(std::uintptr_t base, std::size_t size) {
        REQUIRE(size != 0);
        REQUIRE(base <=
                std::numeric_limits<std::uintptr_t>::max() - size);
        regions_.push_back(MemoryRegion{
            base, std::vector<std::uint8_t>(size)});
    }

    bool Read(std::uintptr_t address,
              void* destination,
              std::size_t size) {
        reads.push_back(ReadRequest{address, size});
        if (destination == nullptr && size != 0) return false;

        auto* output = static_cast<std::uint8_t*>(destination);
        std::size_t copied = 0;
        while (copied < size) {
            if (address >
                std::numeric_limits<std::uintptr_t>::max() - copied) {
                return false;
            }
            const std::uintptr_t cursor = address + copied;
            MemoryRegion* region = FindRegion(cursor);
            if (region == nullptr) return false;
            const std::size_t regionOffset =
                static_cast<std::size_t>(cursor - region->base);
            const std::size_t chunk = std::min(
                size - copied, region->bytes.size() - regionOffset);
            std::memcpy(
                output + copied, region->bytes.data() + regionOffset, chunk);
            copied += chunk;
        }
        return true;
    }

    void Write32(std::uintptr_t address, std::uint32_t value) {
        std::uint8_t bytes[4]{};
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            bytes[index] = static_cast<std::uint8_t>(
                value >> (index * 8U));
        }
        Write(address, bytes, sizeof(bytes));
    }

    void Write64(std::uintptr_t address, std::uint64_t value) {
        std::uint8_t bytes[8]{};
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            bytes[index] = static_cast<std::uint8_t>(
                value >> (index * 8U));
        }
        Write(address, bytes, sizeof(bytes));
    }

    std::vector<ReadRequest> reads;

private:
    MemoryRegion* FindRegion(std::uintptr_t address) {
        for (auto& region : regions_) {
            if (address >= region.base &&
                address - region.base < region.bytes.size()) {
                return &region;
            }
        }
        return nullptr;
    }

    void Write(std::uintptr_t address,
               const void* source,
               std::size_t size) {
        const auto* input = static_cast<const std::uint8_t*>(source);
        std::size_t copied = 0;
        while (copied < size) {
            REQUIRE(address <=
                    std::numeric_limits<std::uintptr_t>::max() - copied);
            const std::uintptr_t cursor = address + copied;
            MemoryRegion* region = FindRegion(cursor);
            REQUIRE(region != nullptr);
            const std::size_t regionOffset =
                static_cast<std::size_t>(cursor - region->base);
            const std::size_t chunk = std::min(
                size - copied, region->bytes.size() - regionOffset);
            std::memcpy(
                region->bytes.data() + regionOffset,
                input + copied,
                chunk);
            copied += chunk;
        }
    }

    std::vector<MemoryRegion> regions_;
};

std::uint32_t EncodeLiteralLoad(
    std::uint32_t opcode,
    std::uint32_t targetRegister,
    std::uintptr_t instructionAddress,
    std::uintptr_t literalAddress) {
    REQUIRE(targetRegister < 32);
    std::int64_t displacement = 0;
    if (literalAddress >= instructionAddress) {
        const std::uintptr_t difference =
            literalAddress - instructionAddress;
        REQUIRE(difference <=
                static_cast<std::uintptr_t>(
                    std::numeric_limits<std::int64_t>::max()));
        displacement = static_cast<std::int64_t>(difference);
    } else {
        const std::uintptr_t difference =
            instructionAddress - literalAddress;
        REQUIRE(difference <=
                static_cast<std::uintptr_t>(
                    std::numeric_limits<std::int64_t>::max()));
        displacement = -static_cast<std::int64_t>(difference);
    }
    REQUIRE((displacement & 3) == 0);

    const std::int64_t immediate = displacement / 4;
    REQUIRE(
        immediate >= -static_cast<std::int64_t>(UINT64_C(0x40000)));
    REQUIRE(
        immediate <= static_cast<std::int64_t>(UINT64_C(0x3ffff)));
    const std::uint32_t encodedImmediate =
        static_cast<std::uint32_t>(immediate) & UINT32_C(0x7ffff);
    return opcode | (encodedImmediate << 5U) | targetRegister;
}

std::uint32_t EncodeLdrX(std::uint32_t targetRegister,
                         std::uintptr_t instructionAddress,
                         std::uintptr_t literalAddress) {
    return EncodeLiteralLoad(
        UINT32_C(0x58000000),
        targetRegister,
        instructionAddress,
        literalAddress);
}

std::uint32_t EncodeLdrW(std::uint32_t targetRegister,
                         std::uintptr_t instructionAddress,
                         std::uintptr_t literalAddress) {
    return EncodeLiteralLoad(
        UINT32_C(0x18000000),
        targetRegister,
        instructionAddress,
        literalAddress);
}

std::uint32_t EncodeBr(std::uint32_t sourceRegister) {
    REQUIRE(sourceRegister < 32);
    return UINT32_C(0xd61f0000) | (sourceRegister << 5U);
}

struct LocatorFixture {
    LocatorFixture() {
        memory.AddRegion(kModuleBase, 3 * kPageSize);
        memory.AddRegion(kScanBase, 5 * kPageSize);
        WriteFirstVeneer(kScanBase + UINT64_C(0x234));
    }

    void WriteFirstVeneer(std::uintptr_t firstTarget) {
        const std::uintptr_t literalAddress =
            firstVeneer + UINT64_C(0x100);
        memory.Write32(
            firstVeneer,
            EncodeLdrX(16, firstVeneer, literalAddress));
        memory.Write32(firstVeneer + 4, EncodeBr(16));
        memory.Write64(
            literalAddress,
            UINT64_C(0xab00000000000000) |
                static_cast<std::uint64_t>(firstTarget));
    }

    void WriteSecondVeneer(
        std::uintptr_t veneerAddress,
        std::uintptr_t literalAddress,
        std::uint32_t ldrRegister = 9,
        std::uint32_t brRegister = 9,
        bool useWRegisterLoad = false) {
        memory.Write32(
            veneerAddress,
            useWRegisterLoad
                ? EncodeLdrW(
                    ldrRegister, veneerAddress, literalAddress)
                : EncodeLdrX(
                    ldrRegister, veneerAddress, literalAddress));
        memory.Write32(veneerAddress + 4, EncodeBr(brRegister));
        memory.Write64(
            literalAddress,
            UINT64_C(0xcd00000000000000) |
                static_cast<std::uint64_t>(firstVeneer + 0x10));
    }

    lengjing::game::native::ExecutionVeneerReadMemory Reader() {
        return [this](std::uintptr_t address,
                      void* destination,
                      std::size_t size) {
            return memory.Read(address, destination, size);
        };
    }

    FakeMemory memory;
    const std::uintptr_t firstVeneer =
        kModuleBase + kFirstVeneerRva;
};

void TestTaggedLiteralsAndExactVeneerAddress() {
    LocatorFixture fixture;
    const std::uintptr_t secondVeneer = kScanBase + 0x80;
    fixture.WriteSecondVeneer(
        secondVeneer, kScanBase + 0x200);

    std::uintptr_t located = 0;
    REQUIRE(lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == secondVeneer);
    REQUIRE(located != secondVeneer + 0x20);
}

void TestNegativeImm19() {
    LocatorFixture fixture;
    const std::uintptr_t secondVeneer = kScanBase + 0x380;
    fixture.WriteSecondVeneer(
        secondVeneer, kScanBase + 0x40);

    std::uintptr_t located = 0;
    REQUIRE(lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == secondVeneer);
}

void TestRegisterMismatchIsRejected() {
    LocatorFixture fixture;
    fixture.WriteSecondVeneer(
        kScanBase + 0x80, kScanBase + 0x200, 9, 10);

    std::uintptr_t located = 1;
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == 0);
}

void TestWLiteralLoadsAreRejected() {
    LocatorFixture fixture;
    fixture.WriteSecondVeneer(
        kScanBase + 0x80, kScanBase + 0x200, 9, 9, true);

    std::uintptr_t located = 1;
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == 0);

    LocatorFixture firstFixture;
    const std::uintptr_t firstLiteral =
        firstFixture.firstVeneer + UINT64_C(0x100);
    firstFixture.memory.Write32(
        firstFixture.firstVeneer,
        EncodeLdrW(16, firstFixture.firstVeneer, firstLiteral));
    firstFixture.WriteSecondVeneer(
        kScanBase + 0x80, kScanBase + 0x200);
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        firstFixture.Reader(),
        located));
    REQUIRE(located == 0);
}

void TestOverflowAndAlignmentFailClosed() {
    bool readerCalled = false;
    const lengjing::game::native::ExecutionVeneerReadMemory reader =
        [&readerCalled](std::uintptr_t, void*, std::size_t) {
            readerCalled = true;
            return false;
        };

    std::uintptr_t located = 1;
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        std::numeric_limits<std::uintptr_t>::max() - 3,
        4,
        reader,
        located));
    REQUIRE(located == 0);
    REQUIRE(!readerCalled);

    located = 1;
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        2,
        reader,
        located));
    REQUIRE(located == 0);
    REQUIRE(!readerCalled);

    LocatorFixture fixture;
    fixture.WriteFirstVeneer(kScanBase + 2);
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == 0);
}

void TestZeroAndMultipleMatchesFail() {
    LocatorFixture fixture;
    std::uintptr_t located = 1;
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == 0);

    LocatorFixture multipleFixture;
    multipleFixture.WriteSecondVeneer(
        kScanBase + 0x80, kScanBase + 0x200);
    multipleFixture.WriteSecondVeneer(
        kScanBase + 0x480, kScanBase + 0x600);
    REQUIRE(!lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        multipleFixture.Reader(),
        located));
    REQUIRE(located == 0);
}

void TestCrossPageLiteralUsesCallback() {
    LocatorFixture fixture;
    const std::uintptr_t secondVeneer = kScanBase + 0x100;
    const std::uintptr_t literalAddress =
        kScanBase + kPageSize - 4;
    fixture.WriteSecondVeneer(secondVeneer, literalAddress);

    std::uintptr_t located = 0;
    REQUIRE(lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == secondVeneer);
    REQUIRE(std::any_of(
        fixture.memory.reads.begin(),
        fixture.memory.reads.end(),
        [literalAddress](const ReadRequest& request) {
            return request.address == literalAddress &&
                request.size == sizeof(std::uint64_t);
        }));
}

void TestUnreadableDecoyLiteralIsSkipped() {
    LocatorFixture fixture;
    const std::uintptr_t decoy = kScanBase + 0x20;
    fixture.memory.Write32(
        decoy,
        EncodeLdrX(8, decoy, kScanBase - kPageSize));
    fixture.memory.Write32(decoy + 4, EncodeBr(8));
    const std::uintptr_t secondVeneer = kScanBase + 0x180;
    fixture.WriteSecondVeneer(
        secondVeneer, kScanBase + 0x300);

    std::uintptr_t located = 0;
    REQUIRE(lengjing::game::native::LocateSecondExecutionVeneer(
        kModuleBase,
        kFirstVeneerRva,
        fixture.Reader(),
        located));
    REQUIRE(located == secondVeneer);
}

}  // namespace

void RunExecutionVeneerLocatorTests() {
    TestTaggedLiteralsAndExactVeneerAddress();
    TestNegativeImm19();
    TestRegisterMismatchIsRejected();
    TestWLiteralLoadsAreRejected();
    TestOverflowAndAlignmentFailClosed();
    TestZeroAndMultipleMatchesFail();
    TestCrossPageLiteralUsesCallback();
    TestUnreadableDecoyLiteralIsSkipped();
}

#endif
