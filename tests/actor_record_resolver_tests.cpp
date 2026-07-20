#include "test_support.h"

#include "game/native/ActorRecordResolver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using lengjing::game::native::ActorAddressRecord;
using lengjing::game::native::ActorArrayDescriptor;
using lengjing::game::native::ActorRecordLayout;
using lengjing::game::native::ActorRecordResolver;

static_assert(sizeof(std::uintptr_t) >= sizeof(std::uint64_t));

constexpr std::uintptr_t kModule = 0x10000000;
constexpr std::uintptr_t kEncrypted = 0x20000000;
constexpr std::uintptr_t kFirst = 0x21000000;
constexpr std::uintptr_t kSecond = 0x22000000;
constexpr std::uintptr_t kThird = 0x23000000;
constexpr std::uintptr_t kContainer = 0x24000000;

class ActorMemory final {
public:
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        std::vector<std::uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        values_[address] = std::move(bytes);
    }

    bool Read(std::uintptr_t address,
              void* destination,
              std::size_t size) const {
        reads_.push_back(address);
        const auto found = values_.find(address);
        if (found == values_.end() || found->second.size() != size) {
            return false;
        }
        std::memcpy(destination, found->second.data(), size);
        return true;
    }

    std::size_t ReadCount() const noexcept { return reads_.size(); }

private:
    std::unordered_map<std::uintptr_t, std::vector<std::uint8_t>> values_;
    mutable std::vector<std::uintptr_t> reads_;
};

void PlaceContainerChain(ActorMemory& memory,
                         const ActorRecordLayout& layout,
                         int missingLink = -1) {
    memory.Put(
        kModule + layout.taggedContainerOffset,
        static_cast<std::uintptr_t>(0xAB00000020000000ULL));
    if (missingLink != 0) memory.Put(kEncrypted - 8, kFirst);
    if (missingLink != 1) memory.Put(kFirst + 16, kSecond);
    if (missingLink != 2) memory.Put(kSecond + 16, kThird);
    if (missingLink != 3) {
        memory.Put(
            kThird,
            static_cast<std::uintptr_t>(0xCD00000024000000ULL));
    }
}

auto ReaderFor(const ActorMemory& memory) {
    return [&memory](std::uintptr_t address,
                     void* destination,
                     std::size_t size) {
        return memory.Read(address, destination, size);
    };
}

auto ValidContainerEntry() {
    return [](std::uintptr_t pointer) { return pointer == kEncrypted; };
}

void TestTaggedContainerResolution() {
    const ActorRecordResolver resolver;
    REQUIRE(
        ActorRecordResolver::UntagPointer(
            static_cast<std::uintptr_t>(0xAB00123456789ABCULL)) ==
        static_cast<std::uintptr_t>(0x00123456789ABCULL));

    ActorMemory memory;
    PlaceContainerChain(memory, resolver.Layout());
    auto read = ReaderFor(memory);
    auto validate = ValidContainerEntry();
    REQUIRE(resolver.ResolveContainer(kModule, read, validate) == kContainer);

    auto reject = [](std::uintptr_t) { return false; };
    REQUIRE(resolver.ResolveContainer(kModule, read, reject) == 0);

    for (int missingLink = 0; missingLink < 4; ++missingLink) {
        ActorMemory incomplete;
        PlaceContainerChain(incomplete, resolver.Layout(), missingLink);
        auto incompleteRead = ReaderFor(incomplete);
        REQUIRE(
            resolver.ResolveContainer(
                kModule, incompleteRead, validate) == 0);
    }

    ActorRecordLayout disabledLayout = resolver.Layout();
    disabledLayout.taggedContainerOffset = 0;
    const ActorRecordResolver disabled(disabledLayout);
    REQUIRE(disabled.ResolveContainer(kModule, read, validate) == 0);
}

void TestSentinelScanBoundaries() {
    const ActorRecordResolver resolver;

    ActorMemory lowerMemory;
    lowerMemory.Put(kContainer, kModule);
    auto lowerRead = ReaderFor(lowerMemory);
    const auto lower =
        resolver.FindArrayStartOffset(kContainer, kModule, lowerRead);
    REQUIRE(lower.has_value());
    REQUIRE(*lower == 80);

    ActorMemory upperMemory;
    upperMemory.Put(kContainer + 0x1F8, kModule);
    auto upperRead = ReaderFor(upperMemory);
    const auto upper =
        resolver.FindArrayStartOffset(kContainer, kModule, upperRead);
    REQUIRE(upper.has_value());
    REQUIRE(*upper == 0x1F8U + 80U);

    ActorMemory outsideMemory;
    outsideMemory.Put(kContainer + 0x200, kModule);
    auto outsideRead = ReaderFor(outsideMemory);
    REQUIRE(
        !resolver.FindArrayStartOffset(
             kContainer, kModule, outsideRead)
             .has_value());
}

void TestProbeComparisonContract() {
    const ActorRecordResolver resolver;
    ActorMemory memory;
    memory.Put(kContainer, kModule);
    std::array<std::uint8_t, 256> block{};
    block[17] = 0x5A;
    memory.Put(kContainer + 80, block);
    auto read = ReaderFor(memory);

    bool zeroFirst = false;
    bool blockSecond = false;
    bool sizeMatched = false;
    const auto oddCompare = [&](const void* left,
                                const void* right,
                                std::size_t size) {
        const auto* first = static_cast<const std::uint8_t*>(left);
        const auto* second = static_cast<const std::uint8_t*>(right);
        zeroFirst = true;
        for (std::size_t index = 0; index < size; ++index) {
            if (first[index] != 0) zeroFirst = false;
        }
        blockSecond = size > 17 && second[17] == 0x5A;
        sizeMatched = size == block.size();
        return 3;
    };
    REQUIRE(
        resolver.ProbeEncryptedArrayWithCompare(
            kContainer, kModule, read, oddCompare) == 3);
    REQUIRE(zeroFirst);
    REQUIRE(blockSecond);
    REQUIRE(sizeMatched);

    const auto evenCompare = [](const void*, const void*, std::size_t) {
        return 2;
    };
    REQUIRE(
        (resolver.ProbeEncryptedArrayWithCompare(
             kContainer, kModule, read, evenCompare) &
         1) == 0);

    ActorMemory unreadableBlock;
    unreadableBlock.Put(kContainer, kModule);
    auto unreadableRead = ReaderFor(unreadableBlock);
    bool secondWasZero = false;
    const auto inspectZero = [&](const void*,
                                 const void* right,
                                 std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(right);
        secondWasZero = true;
        for (std::size_t index = 0; index < size; ++index) {
            if (bytes[index] != 0) secondWasZero = false;
        }
        return 0;
    };
    resolver.ProbeEncryptedArrayWithCompare(
        kContainer, kModule, unreadableRead, inspectZero);
    REQUIRE(secondWasZero);
}

void TestDynamicStrideDiscovery() {
    const ActorRecordResolver resolver;

    ActorMemory firstLayout;
    firstLayout.Put(kContainer + 8, std::uintptr_t{1});
    firstLayout.Put(kContainer + 136 + 16, std::uintptr_t{256});
    auto firstRead = ReaderFor(firstLayout);
    const auto firstStride =
        resolver.DiscoverStride(kContainer, true, firstRead);
    REQUIRE(firstStride.has_value());
    REQUIRE(*firstStride == 136U + 8U);

    ActorMemory upperLayout;
    upperLayout.Put(kContainer + 8, std::uintptr_t{256});
    upperLayout.Put(kContainer + 496 + 16, std::uintptr_t{1});
    auto upperRead = ReaderFor(upperLayout);
    const auto upperStride =
        resolver.DiscoverStride(kContainer, true, upperRead);
    REQUIRE(upperStride.has_value());
    REQUIRE(*upperStride == 496U + 8U);

    ActorMemory invalidLayout;
    invalidLayout.Put(kContainer + 8, std::uintptr_t{2});
    invalidLayout.Put(kContainer + 136 + 16, std::uintptr_t{1});
    auto invalidRead = ReaderFor(invalidLayout);
    REQUIRE(
        !resolver.DiscoverStride(kContainer, true, invalidRead)
             .has_value());

    ActorMemory unused;
    auto unusedRead = ReaderFor(unused);
    const auto plainStride =
        resolver.DiscoverStride(kContainer, false, unusedRead);
    REQUIRE(plainStride.has_value());
    REQUIRE(*plainStride == resolver.Layout().plainRecordStride);
    REQUIRE(unused.ReadCount() == 0);
}

void TestEncryptedAndPlainLocation() {
    const ActorRecordResolver resolver;
    const ActorRecordLayout& layout = resolver.Layout();
    ActorMemory memory;
    PlaceContainerChain(memory, layout);
    memory.Put(kContainer, kModule);
    std::array<std::uint8_t, 256> block{};
    block[0] = 1;
    memory.Put(kContainer + 80, block);
    const std::uintptr_t encryptedData = kContainer + 80;
    memory.Put(encryptedData + 8, std::uintptr_t{1});
    memory.Put(encryptedData + 136 + 16, std::uintptr_t{256});

    constexpr std::uintptr_t plainData = 0x28000000;
    const std::uintptr_t plainHeader = kModule + layout.plainArrayOffset;
    memory.Put(plainHeader, plainData);
    memory.Put(plainHeader + 8, std::int32_t{7});

    auto read = ReaderFor(memory);
    auto validate = ValidContainerEntry();
    const auto odd = [](const void*, const void*, std::size_t) { return 1; };
    const auto encrypted =
        resolver.LocateWithCompare(kModule, read, validate, odd);
    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->data == encryptedData);
    REQUIRE(encrypted->count == layout.encryptedRecordCount);
    REQUIRE(encrypted->stride == 136U + 8U);
    REQUIRE(encrypted->encrypted);

    const auto even = [](const void*, const void*, std::size_t) { return 2; };
    const auto plain =
        resolver.LocateWithCompare(kModule, read, validate, even);
    REQUIRE(plain.has_value());
    REQUIRE(plain->data == plainData);
    REQUIRE(plain->count == 7);
    REQUIRE(plain->stride == layout.plainRecordStride);
    REQUIRE(!plain->encrypted);

    ActorMemory invalidStrideMemory;
    PlaceContainerChain(invalidStrideMemory, layout);
    invalidStrideMemory.Put(kContainer, kModule);
    invalidStrideMemory.Put(kContainer + 80, block);
    invalidStrideMemory.Put(encryptedData + 8, std::uintptr_t{2});
    invalidStrideMemory.Put(plainHeader, plainData);
    invalidStrideMemory.Put(plainHeader + 8, std::int32_t{9});
    auto invalidStrideRead = ReaderFor(invalidStrideMemory);
    const auto invalidStrideFallback = resolver.LocateWithCompare(
        kModule, invalidStrideRead, validate, odd);
    REQUIRE(invalidStrideFallback.has_value());
    REQUIRE(!invalidStrideFallback->encrypted);
    REQUIRE(invalidStrideFallback->data == plainData);
    REQUIRE(invalidStrideFallback->count == 9);

    ActorMemory fallbackMemory;
    fallbackMemory.Put(plainHeader, plainData);
    fallbackMemory.Put(plainHeader + 8, std::int32_t{0});
    auto fallbackRead = ReaderFor(fallbackMemory);
    const auto fallback =
        resolver.Locate(kModule, fallbackRead, validate);
    REQUIRE(fallback.has_value());
    REQUIRE(fallback->count ==
            static_cast<std::uint32_t>(layout.fallbackPlainCount));
    fallbackMemory.Put(
        plainHeader + 8,
        static_cast<std::int32_t>(layout.maximumPlainCount + 1));
    const auto overMaximum =
        resolver.Locate(kModule, fallbackRead, validate);
    REQUIRE(overMaximum.has_value());
    REQUIRE(overMaximum->count ==
            static_cast<std::uint32_t>(layout.fallbackPlainCount));

    ActorMemory missingCountMemory;
    missingCountMemory.Put(plainHeader, plainData);
    auto missingCountRead = ReaderFor(missingCountMemory);
    REQUIRE(
        !resolver.Locate(kModule, missingCountRead, validate).has_value());

    ActorRecordLayout disabledLayout = layout;
    disabledLayout.taggedContainerOffset = 0;
    disabledLayout.plainArrayOffset = 0;
    const ActorRecordResolver disabled(disabledLayout);
    REQUIRE(!disabled.Locate(kModule, read, validate).has_value());
}

void TestRecordReading() {
    const ActorRecordResolver resolver;
    const ActorRecordLayout& layout = resolver.Layout();

    ActorMemory encryptedMemory;
    const ActorArrayDescriptor encryptedArray{
        0x30000000,
        4,
        136U + 8U,
        true,
    };
    const std::uintptr_t encryptedEntry =
        encryptedArray.data + 3U * encryptedArray.stride;
    constexpr std::uintptr_t encryptedActor = 0x31000000;
    constexpr std::uintptr_t encryptedRoot = 0x32000000;
    constexpr std::uintptr_t encryptedMesh = 0x33000000;
    encryptedMemory.Put(encryptedEntry, encryptedActor);
    encryptedMemory.Put(encryptedEntry + 16, encryptedRoot);
    encryptedMemory.Put(encryptedEntry + 32, encryptedMesh);
    auto encryptedRead = ReaderFor(encryptedMemory);
    const auto encryptedRecord =
        resolver.ReadRecord(encryptedArray, 3, encryptedRead);
    REQUIRE(encryptedRecord.has_value());
    REQUIRE(encryptedRecord->actor == encryptedActor);
    REQUIRE(encryptedRecord->root == encryptedRoot);
    REQUIRE(encryptedRecord->mesh == encryptedMesh);

    ActorMemory plainMemory;
    const ActorArrayDescriptor plainArray{0x34000000, 3, 24, false};
    const std::uintptr_t plainEntry =
        plainArray.data + 2U * plainArray.stride;
    constexpr std::uintptr_t plainActor = 0x35000000;
    constexpr std::uintptr_t plainRoot = 0x36000000;
    constexpr std::uintptr_t plainMesh = 0x37000000;
    plainMemory.Put(plainEntry, plainActor);
    plainMemory.Put(plainActor + layout.plainRootOffset, plainRoot);
    plainMemory.Put(plainActor + layout.plainMeshOffset, plainMesh);
    auto plainRead = ReaderFor(plainMemory);
    const auto plainRecord = resolver.ReadRecord(plainArray, 2, plainRead);
    REQUIRE(plainRecord.has_value());
    REQUIRE(plainRecord->actor == plainActor);
    REQUIRE(plainRecord->root == plainRoot);
    REQUIRE(plainRecord->mesh == plainMesh);

    ActorMemory incompleteMemory;
    incompleteMemory.Put(plainEntry, plainActor);
    incompleteMemory.Put(plainActor + layout.plainRootOffset, plainRoot);
    auto incompleteRead = ReaderFor(incompleteMemory);
    const auto incompleteRecord =
        resolver.ReadRecord(plainArray, 2, incompleteRead);
    REQUIRE(incompleteRecord.has_value());
    REQUIRE(incompleteRecord->actor == plainActor);
    REQUIRE(incompleteRecord->root == plainRoot);
    REQUIRE(incompleteRecord->mesh == 0);

    ActorMemory actorOnlyMemory;
    actorOnlyMemory.Put(plainEntry, plainActor);
    auto actorOnlyRead = ReaderFor(actorOnlyMemory);
    const auto actorOnlyRecord =
        resolver.ReadRecord(plainArray, 2, actorOnlyRead);
    REQUIRE(actorOnlyRecord.has_value());
    REQUIRE(actorOnlyRecord->actor == plainActor);
    REQUIRE(actorOnlyRecord->root == 0);
    REQUIRE(actorOnlyRecord->mesh == 0);

    REQUIRE(!resolver.ReadRecord(plainArray, plainArray.count, plainRead)
                 .has_value());
    const ActorArrayDescriptor zeroStride{plainArray.data, 1, 0, false};
    REQUIRE(!resolver.ReadRecord(zeroStride, 0, plainRead).has_value());
    const ActorArrayDescriptor overflow{
        std::numeric_limits<std::uintptr_t>::max() - 10,
        2,
        24,
        true,
    };
    REQUIRE(!resolver.ReadRecord(overflow, 1, plainRead).has_value());
}

}  // namespace

void RunActorRecordResolverTests() {
    TestTaggedContainerResolution();
    TestSentinelScanBoundaries();
    TestProbeComparisonContract();
    TestDynamicStrideDiscovery();
    TestEncryptedAndPlainLocation();
    TestRecordReading();
}

#if defined(ACTOR_RECORD_RESOLVER_STANDALONE)
int main() {
    RunActorRecordResolverTests();
    return 0;
}
#endif
