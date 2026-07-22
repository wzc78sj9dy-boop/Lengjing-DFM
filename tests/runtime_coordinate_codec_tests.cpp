#include "game/native/RuntimeCoordinateCodec.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <unordered_map>

namespace {

using lengjing::game::native::CoordinatePoolCodeFingerprint;
using lengjing::game::native::RuntimeCoordinateCodec;
using lengjing::game::native::RuntimeCoordinateCodecDiagnostic;
using lengjing::game::native::RuntimeCoordinateCodecError;
using lengjing::game::native::RuntimeCoordinateCodecLayout;
using lengjing::game::native::RuntimeCoordinateCodecStage;

constexpr std::uintptr_t kModule = UINT64_C(0x100000000);
constexpr std::uintptr_t kTrampoline = UINT64_C(0x200000000);
constexpr std::uintptr_t kCallback = UINT64_C(0x300000000);
constexpr std::uintptr_t kContext = UINT64_C(0x400000000);
constexpr std::uintptr_t kLink = UINT64_C(0x410000000);
constexpr std::uintptr_t kSlot0 = UINT64_C(0x420000000);
constexpr std::uintptr_t kSlot1 = UINT64_C(0x430000000);
constexpr std::uintptr_t kConfig = UINT64_C(0x500000000);
constexpr std::uintptr_t kObject = UINT64_C(0x600000000);
constexpr std::uintptr_t kOwner = UINT64_C(0x610000000);
constexpr std::uintptr_t kIndexArray = UINT64_C(0x620000000);
constexpr std::uintptr_t kAuxiliary = UINT64_C(0x630000000);
constexpr std::uintptr_t kRecords = UINT64_C(0x640000000);
constexpr std::uint64_t kRawX0 = UINT64_C(0xB400007CF4B077A0);
constexpr std::uint64_t kStateInput = UINT64_C(0xBF54D9DA033ED36F);
constexpr std::uintptr_t kState = UINT64_C(0x7F2404E000);
constexpr std::uint32_t kCodecSeed = UINT32_C(0x13579BDF);
constexpr std::uint32_t kTableSeed = UINT32_C(0x2468ACE0);
constexpr std::uint32_t kCapacity = 8;
constexpr std::uint32_t kTargetPhysicalIndex = 5;
constexpr std::uint32_t kDecoyPhysicalIndex = 2;
constexpr std::size_t kRecordSize = 0x568;

class Memory final {
public:
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        PutBytes(address, &value, sizeof(value));
    }

    void PutBytes(std::uintptr_t address,
                  const void* source,
                  std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(source);
        for (std::size_t index = 0; index < size; ++index) {
            bytes_[address + index] = bytes[index];
        }
    }

    void MakeRecordUnstable(std::uintptr_t address) {
        unstableRecord_ = address;
        unstableRecordReads_ = 0;
    }

    bool OrdinaryFieldRead() const noexcept {
        return ordinaryFieldRead_;
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) {
        auto* bytes = static_cast<std::uint8_t*>(destination);
        for (std::size_t index = 0; index < size; ++index) {
            const auto found = bytes_.find(address + index);
            if (found == bytes_.end()) return false;
            bytes[index] = found->second;
        }
        const std::uintptr_t end = address + size;
        if (address < kObject + 0x240 && end > kObject + 0x210) {
            ordinaryFieldRead_ = true;
        }
        if (address == unstableRecord_ && size == kRecordSize) {
            ++unstableRecordReads_;
            if ((unstableRecordReads_ & 1U) == 0) {
                bytes[0] ^= 1U;
            }
        }
        return true;
    }

private:
    std::unordered_map<std::uintptr_t, std::uint8_t> bytes_;
    std::uintptr_t unstableRecord_ = 0;
    unsigned unstableRecordReads_ = 0;
    bool ordinaryFieldRead_ = false;
};

template <typename T>
void Store(std::array<std::uint8_t, kRecordSize>& record,
           std::size_t offset,
           const T& value) {
    std::memcpy(record.data() + offset, &value, sizeof(value));
}

RuntimeCoordinateCodecLayout BuildLayout() {
    std::array<std::uint8_t, 0xA0> zeros{};
    RuntimeCoordinateCodecLayout layout{};
    layout.wrapperFingerprint =
        CoordinatePoolCodeFingerprint(zeros.data(), 0xA0);
    layout.mainFingerprint =
        CoordinatePoolCodeFingerprint(zeros.data(), 0x40);
    layout.stateFingerprint =
        CoordinatePoolCodeFingerprint(zeros.data(), 0x58);
    layout.codecFingerprint =
        CoordinatePoolCodeFingerprint(zeros.data(), 0x78);
    return layout;
}

struct FixtureOptions {
    bool active = true;
    bool wrongFieldKey = false;
    bool wrongOwnerKey = false;
    bool invalidCoordinate = false;
    std::uint32_t globalRing = 13;
    std::uint64_t ringSalt = 3;
};

std::uintptr_t RecordAddress(std::uint32_t physicalIndex) {
    return kRecords +
        static_cast<std::uintptr_t>(physicalIndex) * kRecordSize;
}

void PutFixture(Memory& memory,
                const RuntimeCoordinateCodecLayout& layout,
                const FixtureOptions& options = {}) {
    const std::uintptr_t hook = kModule + layout.hookRva;
    std::array<std::uint32_t, 11> hookCode{
        UINT32_C(0xD503201F), UINT32_C(0xD503201F),
        UINT32_C(0xD503201F), UINT32_C(0xD503201F),
        UINT32_C(0xD503201F), UINT32_C(0xD503201F),
        UINT32_C(0xBD016800), UINT32_C(0xBD016C01),
        UINT32_C(0xBD017002), UINT32_C(0x58000050),
        UINT32_C(0xD61F0200),
    };
    memory.PutBytes(hook, hookCode.data(), sizeof(hookCode));
    memory.Put(hook + 0x2C, kTrampoline);

    std::array<std::uint8_t, 0xA8> trampoline{};
    const auto putInstruction = [&trampoline](std::size_t offset,
                                               std::uint32_t instruction) {
        std::memcpy(
            trampoline.data() + offset,
            &instruction,
            sizeof(instruction));
    };
    putInstruction(0, UINT32_C(0xA93E77FE));
    putInstruction(4, UINT32_C(0xA93D77FC));
    putInstruction(0x8C, UINT32_C(0x58FFFB60));
    putInstruction(0x90, UINT32_C(0x910003E1));
    putInstruction(0x94, UINT32_C(0x100000BE));
    putInstruction(0x98, UINT32_C(0x58000050));
    putInstruction(0x9C, UINT32_C(0xD61F0200));
    std::memcpy(
        trampoline.data() + 0xA0,
        &kCallback,
        sizeof(kCallback));
    memory.Put(kTrampoline - 8, kContext);
    memory.PutBytes(kTrampoline, trampoline.data(), trampoline.size());

    std::array<std::uint8_t, 0xA0> zeros{};
    memory.PutBytes(kCallback, zeros.data(), 0xA0);
    memory.PutBytes(kCallback + 0x26F4, zeros.data(), 0x40);
    memory.PutBytes(kCallback + 0xB7A4, zeros.data(), 0x58);
    memory.PutBytes(kCallback + 0x5324, zeros.data(), 0x78);

    memory.Put(kContext + 0x10, kLink);
    memory.Put(kLink + 0x10, kSlot0);
    memory.Put(kLink + 0x28, kSlot1);
    memory.Put(kSlot0, kRawX0);
    const std::uint64_t one = 1;
    memory.Put(kSlot1, one);
    memory.Put(
        RuntimeCoordinateCodec::StripPointer(kRawX0) + 0x30,
        kStateInput);

    memory.Put(kState + 0x2740, kConfig);
    const std::array<std::uint32_t, 4> parameters{
        layout.divisor,
        layout.mask,
        0,
        layout.zBias,
    };
    memory.Put(kConfig + 0x210, parameters);

    memory.Put(kState + 0x00, kIndexArray);
    memory.Put(kState + 0xAB0, kAuxiliary);
    memory.Put(kState + 0xC20, kCodecSeed);
    memory.Put(kState + 0xC38, kCapacity);
    const std::uint32_t count = 2;
    memory.Put(kState + 0x1498, count);
    memory.Put(kState + 0x186C, kTableSeed);
    memory.Put(kState + 0x450, options.globalRing);
    const std::uint32_t recordsSeed = kTableSeed;
    const std::uint64_t tableSalt =
        RuntimeCoordinateCodec::EncodeRecordValue(kRecords, recordsSeed);
    memory.Put(kState + 0x2588, tableSalt);

    memory.Put(kObject + 0xE8, kOwner);
    const RuntimeCoordinateCodec::Coordinate publicValue{
        10.0f,
        20.0f,
        30.0f,
    };
    memory.Put(kObject + 0x210, publicValue);

    std::array<std::uint8_t, kRecordSize> target{};
    const std::uintptr_t targetField =
        kObject + (options.wrongFieldKey ? 0x168 : 0x210);
    const std::uint64_t targetFieldKey =
        RuntimeCoordinateCodec::EncodeRecordValue(
            targetField, kCodecSeed);
    const std::uint64_t targetObjectKey =
        RuntimeCoordinateCodec::EncodeRecordValue(kObject, kCodecSeed);
    const std::uint64_t targetOwnerKey =
        RuntimeCoordinateCodec::EncodeRecordValue(
            options.wrongOwnerKey ? kOwner + 4 : kOwner,
            kCodecSeed);
    const std::uint64_t encodedRingSalt =
        RuntimeCoordinateCodec::EncodeRecordValue(
            options.ringSalt, kCodecSeed);
    Store(target, 0x10, targetFieldKey);
    Store(target, 0x530, targetObjectKey);
    target[0x53D] = options.active ? 0 : 1;
    Store(target, 0x540, encodedRingSalt);
    Store(target, 0x548, targetOwnerKey);
    const std::uint32_t ringSlot = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(options.globalRing) +
         options.ringSalt) %
        14U);
    const RuntimeCoordinateCodec::Coordinate expected =
        options.invalidCoordinate
        ? RuntimeCoordinateCodec::Coordinate{}
        : RuntimeCoordinateCodec::Coordinate{
              8174.84375f,
              -17462.125f,
              714.033936f,
          };
    Store(
        target,
        0x28 + static_cast<std::size_t>(ringSlot) * 0x30,
        expected);
    memory.PutBytes(
        RecordAddress(kTargetPhysicalIndex),
        target.data(),
        target.size());

    std::array<std::uint8_t, kRecordSize> decoy{};
    const std::uint64_t decoyFieldKey =
        RuntimeCoordinateCodec::EncodeRecordValue(
            kObject + 0x168, kCodecSeed);
    Store(decoy, 0x10, decoyFieldKey);
    memory.PutBytes(
        RecordAddress(kDecoyPhysicalIndex),
        decoy.data(),
        decoy.size());

    const std::array<std::uint32_t, 2> indexes =
        targetFieldKey < decoyFieldKey
        ? std::array<std::uint32_t, 2>{
              kTargetPhysicalIndex,
              kDecoyPhysicalIndex,
          }
        : std::array<std::uint32_t, 2>{
              kDecoyPhysicalIndex,
              kTargetPhysicalIndex,
          };
    memory.Put(kIndexArray, indexes);
}

template <typename Read>
bool Refresh(RuntimeCoordinateCodec& codec, Read& read) {
    return codec.Refresh(
        kModule,
        read,
        [](std::uintptr_t) { return true; },
        [](std::uintptr_t) { return true; });
}

void TestAddressAndCodecPrimitives() {
    REQUIRE(RuntimeCoordinateCodec::StripPointer(kRawX0) ==
        UINT64_C(0x7CF4B077A0));
    REQUIRE(RuntimeCoordinateCodec::DecodeStateAddress(
        kRawX0, kStateInput) == kState);

    const std::uint64_t plain = UINT64_C(0x7CB9B3E020);
    const std::uint64_t encoded =
        RuntimeCoordinateCodec::EncodeRecordValue(plain, kCodecSeed);
    REQUIRE(RuntimeCoordinateCodec::DecodeRecordValue(
        encoded, kCodecSeed) == plain);

    std::uintptr_t literal = 0;
    REQUIRE(RuntimeCoordinateCodec::DecodeLdrLiteralAddress(
        UINT32_C(0x58000050), UINT64_C(0x1000), literal));
    REQUIRE(literal == UINT64_C(0x1008));
    REQUIRE(!RuntimeCoordinateCodec::DecodeLdrLiteralAddress(
        UINT32_C(0xD503201F), UINT64_C(0x1000), literal));
}

void TestRefreshAndExactRingDecode() {
    const RuntimeCoordinateCodecLayout layout = BuildLayout();
    Memory memory;
    PutFixture(memory, layout);
    auto read = [&memory](std::uintptr_t address,
                          void* destination,
                          std::size_t size) {
        return memory.Read(address, destination, size);
    };
    RuntimeCoordinateCodec codec(layout);
    REQUIRE(Refresh(codec, read));
    REQUIRE(codec.Ready());
    const RuntimeCoordinateCodecDiagnostic ready = codec.Diagnostic();
    REQUIRE(ready.error == RuntimeCoordinateCodecError::None);
    REQUIRE(ready.state == kState);
    REQUIRE(ready.indexArray == kIndexArray);
    REQUIRE(ready.records == kRecords);
    REQUIRE(ready.count == 2);
    REQUIRE(ready.capacity == kCapacity);

    RuntimeCoordinateCodec::Coordinate coordinate{};
    RuntimeCoordinateCodecDiagnostic diagnostic{};
    REQUIRE(codec.Decode(
        kObject, kOwner, coordinate, diagnostic, read));
    REQUIRE(coordinate.x == 8174.84375f);
    REQUIRE(coordinate.y == -17462.125f);
    REQUIRE(coordinate.z == 714.033936f);
    REQUIRE(diagnostic.error == RuntimeCoordinateCodecError::None);
    REQUIRE(diagnostic.stage == RuntimeCoordinateCodecStage::RingDecoded);
    REQUIRE(diagnostic.object == kObject);
    REQUIRE(diagnostic.owner == kOwner);
    REQUIRE(diagnostic.physicalIndex == kTargetPhysicalIndex);
    REQUIRE(diagnostic.ringSlot == 2);
    REQUIRE(!memory.OrdinaryFieldRead());
}

void TestStrictIdentityFailures() {
    const RuntimeCoordinateCodecLayout layout = BuildLayout();

    {
        Memory memory;
        PutFixture(memory, layout);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner + 4, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::OwnerMismatch);
    }

    {
        Memory memory;
        FixtureOptions options{};
        options.wrongOwnerKey = true;
        PutFixture(memory, layout, options);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::RecordKeyMismatch);
    }

    {
        Memory memory;
        FixtureOptions options{};
        options.wrongFieldKey = true;
        PutFixture(memory, layout, options);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::RecordNotFound);
        REQUIRE(!memory.OrdinaryFieldRead());
    }

    {
        Memory memory;
        FixtureOptions options{};
        options.active = false;
        PutFixture(memory, layout, options);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::RecordInactive);
    }
}

void TestSnapshotAndBoundsFailures() {
    const RuntimeCoordinateCodecLayout layout = BuildLayout();

    {
        Memory memory;
        PutFixture(memory, layout);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        memory.MakeRecordUnstable(RecordAddress(kTargetPhysicalIndex));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::RecordUnstable);
    }

    {
        Memory memory;
        PutFixture(memory, layout);
        const std::uint32_t count = 1;
        const std::uint32_t invalidPhysical = kCapacity;
        memory.Put(kState + 0x1498, count);
        memory.Put(kIndexArray, invalidPhysical);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::PhysicalIndexInvalid);
    }

    {
        Memory memory;
        FixtureOptions options{};
        options.invalidCoordinate = true;
        PutFixture(memory, layout, options);
        auto read = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
            return memory.Read(address, destination, size);
        };
        RuntimeCoordinateCodec codec(layout);
        REQUIRE(Refresh(codec, read));
        RuntimeCoordinateCodec::Coordinate coordinate{};
        RuntimeCoordinateCodecDiagnostic diagnostic{};
        REQUIRE(!codec.Decode(
            kObject, kOwner, coordinate, diagnostic, read));
        REQUIRE(diagnostic.error ==
            RuntimeCoordinateCodecError::OutputInvalid);
    }
}

void TestFingerprintFailure() {
    const RuntimeCoordinateCodecLayout layout = BuildLayout();
    Memory memory;
    PutFixture(memory, layout);
    const std::uint8_t changed = 1;
    memory.Put(kCallback + 0x26F4, changed);
    auto read = [&memory](std::uintptr_t address,
                          void* destination,
                          std::size_t size) {
        return memory.Read(address, destination, size);
    };
    RuntimeCoordinateCodec codec(layout);
    REQUIRE(!Refresh(codec, read));
    const RuntimeCoordinateCodecDiagnostic diagnostic = codec.Diagnostic();
    REQUIRE(diagnostic.error ==
        RuntimeCoordinateCodecError::CallbackFingerprintMismatch);
    REQUIRE(diagnostic.fingerprintWindow == 1);
    REQUIRE(diagnostic.expectedFingerprint !=
        diagnostic.observedFingerprint);
}

}  // namespace

int main() {
    try {
        TestAddressAndCodecPrimitives();
        TestRefreshAndExactRingDecode();
        TestStrictIdentityFailures();
        TestSnapshotAndBoundsFailures();
        TestFingerprintFailure();
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
}
