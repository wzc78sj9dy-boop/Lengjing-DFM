#include "game/native/AlgorithmCoordinateReader.h"
#include "test_support.h"

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
using lengjing::game::native::AlgorithmCoordinateReader;

constexpr std::uintptr_t kModule = 0x10000000;
constexpr std::uintptr_t kTable = 0x20000000;
constexpr std::uintptr_t kRecords = 0x30000000;
constexpr std::uintptr_t kActor = 0x40000000;

class Memory final {
public:
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        std::vector<std::uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        values_[address] = std::move(bytes);
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
        values_[address] = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) {
        const auto found = values_.find(address);
        if (found == values_.end() || found->second.size() != size) return false;
        std::memcpy(destination, found->second.data(), size);
        return true;
    }

private:
    std::unordered_map<std::uintptr_t, std::vector<std::uint8_t>> values_;
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
    auto noRead = [](std::uintptr_t, void*, std::size_t) { return false; };
    const std::uintptr_t maximum =
        std::numeric_limits<std::uintptr_t>::max();
    REQUIRE(!AlgorithmCoordinateReader{}.Read(
        maximum - AlgorithmCoordinateReader::kRecordTableRva + 1,
        kActor, coordinate, noRead));
    REQUIRE(coordinate.x == 0.0f && coordinate.y == 0.0f &&
        coordinate.z == 0.0f);
}

}  // namespace

int main() {
    try {
        TestLookupAndStride();
        TestBoundsAndFailure();
        TestInvalidValuesAndOverflow();
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
}
