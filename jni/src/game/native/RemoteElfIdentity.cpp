#include "game/native/RemoteElfIdentity.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uint8_t kElfClass64 = 2;
constexpr std::uint8_t kElfDataLittleEndian = 1;
constexpr std::uint16_t kElfTypeSharedObject = 3;
constexpr std::uint16_t kElfMachineAarch64 = 183;
constexpr std::uint32_t kProgramHeaderNote = 4;
constexpr std::uint32_t kGnuBuildIdNote = 3;
constexpr std::size_t kMaximumProgramHeaders = 256;
constexpr std::size_t kMaximumNoteBytes = 1024U * 1024U;

#pragma pack(push, 1)
struct Elf64Header {
    std::uint8_t identity[16];
    std::uint16_t type;
    std::uint16_t machine;
    std::uint32_t version;
    std::uint64_t entry;
    std::uint64_t programHeaderOffset;
    std::uint64_t sectionHeaderOffset;
    std::uint32_t flags;
    std::uint16_t headerSize;
    std::uint16_t programHeaderSize;
    std::uint16_t programHeaderCount;
    std::uint16_t sectionHeaderSize;
    std::uint16_t sectionHeaderCount;
    std::uint16_t sectionNameIndex;
};

struct Elf64ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t virtualAddress;
    std::uint64_t physicalAddress;
    std::uint64_t fileSize;
    std::uint64_t memorySize;
    std::uint64_t alignment;
};

struct Elf64NoteHeader {
    std::uint32_t nameSize;
    std::uint32_t descriptorSize;
    std::uint32_t type;
};
#pragma pack(pop)

static_assert(sizeof(Elf64Header) == 64, "bad ELF header layout");
static_assert(sizeof(Elf64ProgramHeader) == 56,
              "bad ELF program header layout");
static_assert(sizeof(Elf64NoteHeader) == 12, "bad ELF note layout");

constexpr std::size_t Align4(std::size_t value) noexcept {
    return (value + 3U) & ~std::size_t{3U};
}

bool AddAddress(std::uintptr_t base,
                std::uint64_t offset,
                std::uintptr_t& result) noexcept {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool IsElfHeaderValid(const Elf64Header& header) noexcept {
    return header.identity[0] == 0x7fU &&
        header.identity[1] == 'E' &&
        header.identity[2] == 'L' &&
        header.identity[3] == 'F' &&
        header.identity[4] == kElfClass64 &&
        header.identity[5] == kElfDataLittleEndian &&
        header.type == kElfTypeSharedObject &&
        header.machine == kElfMachineAarch64 &&
        header.headerSize == sizeof(Elf64Header) &&
        header.programHeaderSize >= sizeof(Elf64ProgramHeader) &&
        header.programHeaderCount != 0 &&
        header.programHeaderCount <= kMaximumProgramHeaders;
}

bool EncodeBuildId(const std::uint8_t* bytes,
                   std::size_t size,
                   std::string& buildId) {
    if (bytes == nullptr || size < 4 || size > 64) return false;
    static constexpr char kHex[] = "0123456789abcdef";
    buildId.clear();
    buildId.reserve(size * 2U);
    for (std::size_t index = 0; index < size; ++index) {
        buildId.push_back(kHex[bytes[index] >> 4U]);
        buildId.push_back(kHex[bytes[index] & 0x0fU]);
    }
    return true;
}

bool ParseBuildIdNotes(const std::uint8_t* bytes,
                       std::size_t size,
                       std::string& buildId) {
    std::size_t cursor = 0;
    while (cursor <= size && size - cursor >= sizeof(Elf64NoteHeader)) {
        Elf64NoteHeader note{};
        std::memcpy(&note, bytes + cursor, sizeof(note));
        cursor += sizeof(note);

        const std::size_t alignedName = Align4(note.nameSize);
        const std::size_t alignedDescriptor = Align4(note.descriptorSize);
        if (alignedName < note.nameSize ||
            alignedDescriptor < note.descriptorSize ||
            alignedName > size - cursor) {
            return false;
        }
        const std::uint8_t* name = bytes + cursor;
        cursor += alignedName;
        if (alignedDescriptor > size - cursor) return false;
        const std::uint8_t* descriptor = bytes + cursor;

        const bool gnuName = note.nameSize == 4 &&
            std::memcmp(name, "GNU\0", 4) == 0;
        if (gnuName && note.type == kGnuBuildIdNote &&
            EncodeBuildId(descriptor, note.descriptorSize, buildId)) {
            return true;
        }
        cursor += alignedDescriptor;
    }
    return false;
}

}  // namespace

bool ReadRemoteElfBuildId(std::uintptr_t imageBase,
                          RemoteElfReadCallback reader,
                          void* readerContext,
                          std::string& buildId) {
    buildId.clear();
    if (imageBase == 0 || reader == nullptr) return false;

    Elf64Header header{};
    if (!reader(readerContext, imageBase, &header, sizeof(header)) ||
        !IsElfHeaderValid(header)) {
        return false;
    }

    std::uintptr_t programHeadersAddress = 0;
    const std::uint64_t programHeadersSize =
        static_cast<std::uint64_t>(header.programHeaderSize) *
        header.programHeaderCount;
    if (programHeadersSize > kMaximumNoteBytes ||
        !AddAddress(
            imageBase, header.programHeaderOffset, programHeadersAddress)) {
        return false;
    }

    std::vector<std::uint8_t> programHeaders(
        static_cast<std::size_t>(programHeadersSize));
    if (!reader(
            readerContext,
            programHeadersAddress,
            programHeaders.data(),
            programHeaders.size())) {
        return false;
    }

    for (std::size_t index = 0;
         index < header.programHeaderCount; ++index) {
        Elf64ProgramHeader programHeader{};
        std::memcpy(
            &programHeader,
            programHeaders.data() + index * header.programHeaderSize,
            sizeof(programHeader));
        if (programHeader.type != kProgramHeaderNote ||
            programHeader.fileSize < sizeof(Elf64NoteHeader) ||
            programHeader.fileSize > kMaximumNoteBytes) {
            continue;
        }

        std::uintptr_t noteAddress = 0;
        if (!AddAddress(
                imageBase, programHeader.virtualAddress, noteAddress)) {
            continue;
        }
        std::vector<std::uint8_t> notes(
            static_cast<std::size_t>(programHeader.fileSize));
        if (!reader(
                readerContext,
                noteAddress,
                notes.data(),
                notes.size())) {
            continue;
        }
        if (ParseBuildIdNotes(notes.data(), notes.size(), buildId)) {
            return true;
        }
    }
    buildId.clear();
    return false;
}

}  // namespace lengjing::game::native
