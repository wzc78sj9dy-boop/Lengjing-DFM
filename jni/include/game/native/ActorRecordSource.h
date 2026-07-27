#pragma once

#include <cstdint>
#include <vector>

namespace lengjing::game::native {

inline constexpr std::uintptr_t kActorCoordinateSubjectMinimum =
    UINT64_C(0x10000);
inline constexpr std::uintptr_t kActorCoordinateSubjectMaximum =
    UINT64_C(0x10000000000);

constexpr bool IsActorCoordinateSubjectPointer(
    std::uintptr_t pointer) noexcept {
    return pointer >= kActorCoordinateSubjectMinimum &&
        pointer < kActorCoordinateSubjectMaximum;
}

struct ActorRecordSource {
    std::uintptr_t actor = 0;
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
};

struct ActorSubjectLayout {
    std::uintptr_t rootOffset = 0;
    std::uintptr_t meshOffset = 0;
    std::uintptr_t alternateRootOffset = 0;

    constexpr bool IsValid() const noexcept {
        return rootOffset >= 8 && rootOffset <= 0xffff &&
            (rootOffset & 7U) == 0 && meshOffset >= 8 &&
            meshOffset <= 0xffff && (meshOffset & 7U) == 0 &&
            alternateRootOffset >= 8 &&
            alternateRootOffset <= 0xffff &&
            (alternateRootOffset & 7U) == 0 &&
            rootOffset != meshOffset &&
            rootOffset != alternateRootOffset &&
            meshOffset != alternateRootOffset;
    }

    friend constexpr bool operator==(
        const ActorSubjectLayout& left,
        const ActorSubjectLayout& right) noexcept {
        return left.rootOffset == right.rootOffset &&
            left.meshOffset == right.meshOffset &&
            left.alternateRootOffset == right.alternateRootOffset;
    }
};

inline ActorRecordSource MakeActorRecord(
    std::uintptr_t actor,
    std::uintptr_t root = 0,
    std::uintptr_t mesh = 0) noexcept {
    return ActorRecordSource{actor, root, mesh};
}

inline std::vector<ActorRecordSource> BuildActorRecords(
    const std::vector<std::uintptr_t>& actorAddresses) {
    std::vector<ActorRecordSource> result;
    result.reserve(actorAddresses.size());
    for (const std::uintptr_t actor : actorAddresses) {
        if (actor == 0) continue;
        result.push_back(MakeActorRecord(actor));
    }
    return result;
}

template <typename ReadPointer>
void FillActorPointers(
    ActorRecordSource& source,
    const ActorSubjectLayout& layout,
    ReadPointer&& readPointer) {
    if (source.actor == 0 || !layout.IsValid()) return;
    if (source.root == 0) {
        source.root =
            readPointer(source.actor + layout.rootOffset);
    }
    if (source.mesh == 0) {
        source.mesh =
            readPointer(source.actor + layout.meshOffset);
    }
}

template <typename ReadPointer, typename ValidatePointer>
std::uintptr_t ResolveActorCoordinateSubject(
    const ActorRecordSource& source,
    const ActorSubjectLayout& layout,
    ReadPointer&& readPointer,
    ValidatePointer&& validatePointer) {
    if (!layout.IsValid() || source.actor == 0) return 0;
    std::uintptr_t subject =
        readPointer(source.actor + layout.rootOffset);
    if (!validatePointer(subject)) {
        subject = readPointer(
            source.actor + layout.alternateRootOffset);
    }
    return validatePointer(subject) ? subject : 0;
}

}  // namespace lengjing::game::native
