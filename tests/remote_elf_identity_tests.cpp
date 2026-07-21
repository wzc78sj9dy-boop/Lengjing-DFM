#include "game/native/RemoteElfIdentity.h"
#include "test_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

#pragma pack(push, 1)
struct TestElfHeader {
    std::uint8_t identity[16]{};
    std::uint16_t type = 0;
    std::uint16_t machine = 0;
    std::uint32_t version = 0;
    std::uint64_t entry = 0;
    std::uint64_t programHeaderOffset = 0;
    std::uint64_t sectionHeaderOffset = 0;
    std::uint32_t flags = 0;
    std::uint16_t headerSize = 0;
    std::uint16_t programHeaderSize = 0;
    std::uint16_t programHeaderCount = 0;
    std::uint16_t sectionHeaderSize = 0;
    std::uint16_t sectionHeaderCount = 0;
    std::uint16_t sectionNameIndex = 0;
};

struct TestProgramHeader {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;
    std::uint64_t virtualAddress = 0;
    std::uint64_t physicalAddress = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t memorySize = 0;
    std::uint64_t alignment = 0;
};

struct TestNoteHeader {
    std::uint32_t nameSize = 0;
    std::uint32_t descriptorSize = 0;
    std::uint32_t type = 0;
};
#pragma pack(pop)

struct FakeImage {
    std::uintptr_t base = UINT64_C(0x7000000000);
    std::vector<std::uint8_t> bytes;
};

bool ReadFakeImage(void* context,
                   std::uintptr_t address,
                   void* destination,
                   std::size_t size) {
    auto* image = static_cast<FakeImage*>(context);
    if (image == nullptr || destination == nullptr ||
        address < image->base) {
        return false;
    }
    const std::size_t offset =
        static_cast<std::size_t>(address - image->base);
    if (offset > image->bytes.size() ||
        size > image->bytes.size() - offset) {
        return false;
    }
    std::memcpy(destination, image->bytes.data() + offset, size);
    return true;
}

FakeImage MakeImage() {
    FakeImage image{};
    image.bytes.resize(0x400);

    TestElfHeader header{};
    header.identity[0] = 0x7fU;
    header.identity[1] = 'E';
    header.identity[2] = 'L';
    header.identity[3] = 'F';
    header.identity[4] = 2;
    header.identity[5] = 1;
    header.type = 3;
    header.machine = 183;
    header.version = 1;
    header.programHeaderOffset = sizeof(TestElfHeader);
    header.headerSize = sizeof(TestElfHeader);
    header.programHeaderSize = sizeof(TestProgramHeader);
    header.programHeaderCount = 1;
    std::memcpy(image.bytes.data(), &header, sizeof(header));

    TestProgramHeader programHeader{};
    programHeader.type = 4;
    programHeader.virtualAddress = 0x200;
    programHeader.fileSize = 36;
    programHeader.memorySize = 36;
    std::memcpy(
        image.bytes.data() + header.programHeaderOffset,
        &programHeader,
        sizeof(programHeader));

    TestNoteHeader note{4, 20, 3};
    std::memcpy(image.bytes.data() + 0x200, &note, sizeof(note));
    std::memcpy(image.bytes.data() + 0x20c, "GNU\0", 4);
    const std::array<std::uint8_t, 20> descriptor{
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x01, 0x23,
        0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x55, 0xaa, 0x11, 0xee,
    };
    std::memcpy(image.bytes.data() + 0x210,
                descriptor.data(), descriptor.size());
    return image;
}

}  // namespace

void RunRemoteElfIdentityTests() {
    using lengjing::game::native::ReadRemoteElfBuildId;

    FakeImage image = MakeImage();
    std::string buildId;
    REQUIRE(ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId == "1032547698badcfe0123456789abcdef55aa11ee");

    image.bytes[4] = 1;
    REQUIRE(!ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId.empty());

    image = MakeImage();
    image.bytes[0x20c] = 'X';
    REQUIRE(!ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId.empty());

    image = MakeImage();
    TestElfHeader header{};
    std::memcpy(&header, image.bytes.data(), sizeof(header));
    header.programHeaderSize = 64;
    std::memcpy(image.bytes.data(), &header, sizeof(header));
    REQUIRE(ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId == "1032547698badcfe0123456789abcdef55aa11ee");

    image = MakeImage();
    std::memcpy(&header, image.bytes.data(), sizeof(header));
    header.programHeaderCount = 2;
    std::memcpy(image.bytes.data(), &header, sizeof(header));
    image.bytes[0x20c] = 'X';
    std::memcpy(image.bytes.data() + 0x300,
                image.bytes.data() + 0x200, 36);
    image.bytes[0x30c] = 'G';
    TestProgramHeader secondProgramHeader{};
    secondProgramHeader.type = 4;
    secondProgramHeader.virtualAddress = 0x300;
    secondProgramHeader.fileSize = 36;
    secondProgramHeader.memorySize = 36;
    std::memcpy(
        image.bytes.data() + header.programHeaderOffset +
            sizeof(TestProgramHeader),
        &secondProgramHeader,
        sizeof(secondProgramHeader));
    REQUIRE(ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId == "1032547698badcfe0123456789abcdef55aa11ee");

    image = MakeImage();
    TestNoteHeader note{};
    std::memcpy(&note, image.bytes.data() + 0x200, sizeof(note));
    note.descriptorSize = 2;
    std::memcpy(image.bytes.data() + 0x200, &note, sizeof(note));
    REQUIRE(!ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId.empty());

    image = MakeImage();
    std::memcpy(&header, image.bytes.data(), sizeof(header));
    header.programHeaderOffset = UINT64_MAX;
    std::memcpy(image.bytes.data(), &header, sizeof(header));
    REQUIRE(!ReadRemoteElfBuildId(
        image.base, &ReadFakeImage, &image, buildId));
    REQUIRE(buildId.empty());
}
