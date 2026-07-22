#include "game/native/AlgorithmCoordinateReader.h"
#include "game/native/AlgorithmCoordinateProbePolicy.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using lengjing::game::native::AlgorithmCoordinate;
using lengjing::game::native::AlgorithmCoordinateDiagnostic;
using lengjing::game::native::AlgorithmCoordinateReadError;
using lengjing::game::native::AlgorithmCoordinateReader;
using lengjing::game::native::AlgorithmCoordinateRecord;

constexpr std::uintptr_t kModule = 0x10000000;
constexpr std::uintptr_t kTable = 0x20000000;
constexpr std::uintptr_t kRecords = 0x30000000;
constexpr std::uintptr_t kActor = 0x40000000;

class Memory final {
public:
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            values_[address + index] = bytes[index];
        }
    }

    void PutRecord(std::uintptr_t address,
                   std::uintptr_t actor,
                   const AlgorithmCoordinate& coordinate) {
        std::array<std::uint8_t, AlgorithmCoordinateReader::kRecordSize>
            bytes{};
        std::memcpy(bytes.data(), &actor, sizeof(actor));
        std::memcpy(
            bytes.data() + AlgorithmCoordinateReader::kCoordinateOffset,
            &coordinate, sizeof(coordinate));
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            values_[address + index] = bytes[index];
        }
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) {
        ++readCount_;
        largestRead_ = std::max(largestRead_, size);
        auto* bytes = static_cast<std::uint8_t*>(destination);
        for (std::size_t index = 0; index < size; ++index) {
            const auto found = values_.find(address + index);
            if (found == values_.end()) return false;
            bytes[index] = found->second;
        }
        return true;
    }

    std::size_t ReadCount() const noexcept {
        return readCount_;
    }

    std::size_t LargestRead() const noexcept {
        return largestRead_;
    }

private:
    std::unordered_map<std::uintptr_t, std::uint8_t> values_;
    std::size_t readCount_ = 0;
    std::size_t largestRead_ = 0;
};

void PutTable(Memory& memory, std::uint32_t count) {
    memory.Put(kModule + AlgorithmCoordinateReader::kRecordTableRva, kTable);
    memory.Put(kTable, kRecords);
    memory.Put(kTable + sizeof(std::uintptr_t), count);
}

bool Read(Memory& memory,
          AlgorithmCoordinate& coordinate,
          std::uintptr_t actor = kActor) {
    auto callback = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
        return memory.Read(address, destination, size);
    };
    return AlgorithmCoordinateReader{}.Read(
        kModule, actor, coordinate, callback);
}

void TestLookupAndStride() {
    REQUIRE(AlgorithmCoordinateReader::kRecordTableRva == 0x1D15B700ULL);
    REQUIRE(AlgorithmCoordinateReader::kRecordStride == 0x20);
    REQUIRE(AlgorithmCoordinateReader::kRecordSize == 0x20);
    REQUIRE(AlgorithmCoordinateReader::kMaximumRecordCount == 5000);
    Memory memory;
    PutTable(memory, 3);
    memory.PutRecord(kRecords, 0x50000000, {1.0f, 2.0f, 3.0f});
    memory.PutRecord(kRecords + 0x20, kActor, {4.0f, 5.0f, 6.0f});
    memory.PutRecord(kRecords + 0x40, 0x50000002, {7.0f, 8.0f, 9.0f});
    AlgorithmCoordinate coordinate{9.0f, 9.0f, 9.0f};
    REQUIRE(Read(memory, coordinate));
    REQUIRE(coordinate.x == 4.0f && coordinate.y == 5.0f &&
        coordinate.z == 6.0f);
    REQUIRE(memory.LargestRead() ==
        3 * AlgorithmCoordinateReader::kRecordStride);
    REQUIRE(memory.ReadCount() == 7);
}

void TestBoundsAndFailure() {
    Memory maximum;
    PutTable(maximum, AlgorithmCoordinateReader::kMaximumRecordCount);
    for (std::uint32_t index = 0;
         index + 1 < AlgorithmCoordinateReader::kMaximumRecordCount;
         ++index) {
        maximum.PutRecord(
            kRecords + index * AlgorithmCoordinateReader::kRecordStride,
            0x50000000ULL + index,
            {1.0f, 2.0f, 3.0f});
    }
    maximum.PutRecord(
        kRecords + (AlgorithmCoordinateReader::kMaximumRecordCount - 1) *
            AlgorithmCoordinateReader::kRecordStride,
        kActor,
        {10.0f, 20.0f, 30.0f});
    AlgorithmCoordinate coordinate{};
    REQUIRE(Read(maximum, coordinate));
    REQUIRE(coordinate.z == 30.0f);
    for (const std::uint32_t count : {0U, 5001U}) {
        Memory invalid;
        PutTable(invalid, count);
        coordinate = {1.0f, 1.0f, 1.0f};
        REQUIRE(!Read(invalid, coordinate));
        REQUIRE(coordinate.x == 0.0f && coordinate.y == 0.0f &&
            coordinate.z == 0.0f);
    }
    Memory missing;
    PutTable(missing, 1);
    missing.PutRecord(kRecords, 0x50000000, {1.0f, 2.0f, 3.0f});
    coordinate = {8.0f, 8.0f, 8.0f};
    REQUIRE(!Read(missing, coordinate));
    REQUIRE(coordinate.x == 0.0f && coordinate.y == 0.0f &&
        coordinate.z == 0.0f);
}

void TestSnapshotAndDiagnostics() {
    Memory memory;
    PutTable(memory, 3);
    memory.PutRecord(kRecords, 0x50000000, {1.0f, 2.0f, 3.0f});
    memory.PutRecord(kRecords + 0x20, kActor, {4.0f, 5.0f, 6.0f});
    memory.PutRecord(kRecords + 0x40, 0x50000002, {});
    auto callback = [&memory](std::uintptr_t address,
                              void* destination,
                              std::size_t size) {
        return memory.Read(address, destination, size);
    };

    std::vector<AlgorithmCoordinateRecord> snapshot;
    AlgorithmCoordinateDiagnostic diagnostic{};
    REQUIRE(AlgorithmCoordinateReader{}.ReadTable(
        kModule, snapshot, diagnostic, callback));
    REQUIRE(snapshot.size() == 3);
    REQUIRE(snapshot[1].actor == kActor);
    REQUIRE(snapshot[1].valid);
    REQUIRE(!snapshot[2].valid);
    REQUIRE(diagnostic.error == AlgorithmCoordinateReadError::None);
    REQUIRE(diagnostic.tableAddress ==
        kModule + AlgorithmCoordinateReader::kRecordTableRva);
    REQUIRE(diagnostic.table == kTable);
    REQUIRE(diagnostic.records == kRecords);
    REQUIRE(diagnostic.count == 3);
    REQUIRE(diagnostic.validCount == 2);

    AlgorithmCoordinate coordinate{};
    REQUIRE(!AlgorithmCoordinateReader{}.Read(
        kModule, 0x60000000, coordinate, diagnostic, callback));
    REQUIRE(diagnostic.error ==
        AlgorithmCoordinateReadError::ActorNotFound);
    REQUIRE(diagnostic.actor == 0x60000000);

    REQUIRE(!AlgorithmCoordinateReader{}.Read(
        kModule, 0x50000002, coordinate, diagnostic, callback));
    REQUIRE(diagnostic.error ==
        AlgorithmCoordinateReadError::CoordinateInvalid);
    REQUIRE(diagnostic.recordIndex == 2);

    Memory unavailable;
    const std::uintptr_t zero = 0;
    unavailable.Put(
        kModule + AlgorithmCoordinateReader::kRecordTableRva, zero);
    auto unavailableRead = [&unavailable](std::uintptr_t address,
                                         void* destination,
                                         std::size_t size) {
        return unavailable.Read(address, destination, size);
    };
    REQUIRE(!AlgorithmCoordinateReader{}.ReadTable(
        kModule, snapshot, diagnostic, unavailableRead));
    REQUIRE(diagnostic.error ==
        AlgorithmCoordinateReadError::TableUnavailable);
}

void TestInvalidValuesAndOverflow() {
    const std::array<AlgorithmCoordinate, 4> invalid{{
        {},
        {0.0f, 0.0f, -90.0f},
        {std::numeric_limits<float>::quiet_NaN(), 1.0f, 2.0f},
        {std::numeric_limits<float>::infinity(), 1.0f, 2.0f},
    }};
    for (const AlgorithmCoordinate& stored : invalid) {
        Memory memory;
        PutTable(memory, 1);
        memory.PutRecord(kRecords, kActor, stored);
        AlgorithmCoordinate coordinate{2.0f, 2.0f, 2.0f};
        REQUIRE(!Read(memory, coordinate));
        REQUIRE(coordinate.x == 0.0f && coordinate.y == 0.0f &&
            coordinate.z == 0.0f);
    }
    AlgorithmCoordinate coordinate{3.0f, 3.0f, 3.0f};
    AlgorithmCoordinateDiagnostic diagnostic{};
    auto noRead = [](std::uintptr_t, void*, std::size_t) { return false; };
    const std::uintptr_t maximum =
        std::numeric_limits<std::uintptr_t>::max();
    REQUIRE(!AlgorithmCoordinateReader{}.Read(
        maximum - AlgorithmCoordinateReader::kRecordTableRva + 1,
        kActor, coordinate, diagnostic, noRead));
    REQUIRE(diagnostic.error ==
        AlgorithmCoordinateReadError::TableAddressOverflow);
    REQUIRE(coordinate.x == 0.0f && coordinate.y == 0.0f &&
        coordinate.z == 0.0f);

    REQUIRE(!AlgorithmCoordinateReader{}.Read(
        kModule, 0, coordinate, diagnostic, noRead));
    REQUIRE(diagnostic.error == AlgorithmCoordinateReadError::InvalidInput);
    REQUIRE(diagnostic.actor == 0);
}

void TestProbeSuccessPolicy() {
    AlgorithmCoordinateDiagnostic successful{};
    successful.x = 1.0f;
    successful.y = 2.0f;
    successful.z = 3.0f;
    REQUIRE(lengjing::game::native::
        IsAlgorithmCoordinateSampleValid(true, true, 1, successful));
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateProbeSuccessful(true, true, 1, successful));
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateSampleValid(false, true, 1, successful));
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateSampleValid(true, false, 1, successful));
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateSampleValid(true, true, 0, successful));
    successful.error = AlgorithmCoordinateReadError::ActorNotFound;
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateSampleValid(true, true, 1, successful));
    successful.error = AlgorithmCoordinateReadError::None;
    successful.x = 0.0f;
    successful.y = 0.0f;
    successful.z = -90.0f;
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateSampleValid(true, true, 1, successful));

    lengjing::game::native::RuntimeCoordinateCodecDiagnostic object{};
    object.stage = lengjing::game::native::
        RuntimeCoordinateCodecStage::RingDecoded;
    object.decodedX = 1.0f;
    object.decodedY = 2.0f;
    object.decodedZ = 93.0f;
    object.verticalAdjustmentFirst = 90.0f;
    object.verticalAdjustmentSecond = 90.0f;
    object.presentedZ = 3.0f;
    REQUIRE(lengjing::game::native::
        IsAlgorithmCoordinateObjectSampleValid(true, true, 1, 0, object));
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateObjectProbeSuccessful(true, true, 1, 0, object));
    REQUIRE(!lengjing::game::native::
        kAlgorithmCoordinateVisualAcceptanceCompleted);
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateObjectSampleValid(true, true, 1, 1, object));
    object.stage = lengjing::game::native::
        RuntimeCoordinateCodecStage::Ready;
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateObjectSampleValid(true, true, 1, 0, object));
    object.stage = lengjing::game::native::
        RuntimeCoordinateCodecStage::RingDecoded;
    object.decodedX = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateObjectSampleValid(true, true, 1, 0, object));
    object.decodedX = 1.0f;
    object.verticalAdjustmentSecond = 59.99f;
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateObjectSampleValid(true, true, 1, 0, object));
    object.verticalAdjustmentSecond = 90.0f;
    object.presentedZ = 93.0f;
    REQUIRE(!lengjing::game::native::
        IsAlgorithmCoordinateObjectSampleValid(true, true, 1, 0, object));
}

}  // namespace

int main() {
    try {
        TestLookupAndStride();
        TestBoundsAndFailure();
        TestSnapshotAndDiagnostics();
        TestInvalidValuesAndOverflow();
        TestProbeSuccessPolicy();
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
}
