#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lengjing::game::native {

struct ActorRecordSource {
    std::uintptr_t actor = 0;
    std::uintptr_t root = 0;
    std::uintptr_t mesh = 0;
    std::uintptr_t ordinaryRoot = 0;
    std::uintptr_t ordinaryMesh = 0;
    bool resolverRecord = false;
    bool encryptedRecord = false;
    bool ordinarySource = false;
};

inline constexpr std::uintptr_t kOrdinaryActorRootOffset = 0x180;
inline constexpr std::uintptr_t kOrdinaryActorMeshOffset = 0x3D0;

inline ActorRecordSource MakeResolvedActorRecord(
    std::uintptr_t actor,
    std::uintptr_t root,
    std::uintptr_t mesh,
    bool encrypted) noexcept {
    ActorRecordSource source{};
    source.actor = actor;
    source.root = root;
    source.mesh = mesh;
    source.resolverRecord = true;
    source.encryptedRecord = encrypted;
    return source;
}

inline ActorRecordSource MakeOrdinaryActorRecord(
    std::uintptr_t actor,
    std::uintptr_t root = 0,
    std::uintptr_t mesh = 0) noexcept {
    ActorRecordSource source{};
    source.actor = actor;
    source.ordinaryRoot = root;
    source.ordinaryMesh = mesh;
    source.ordinarySource = true;
    return source;
}

inline bool MergeActorRecordSource(
    ActorRecordSource& destination,
    const ActorRecordSource& incoming) noexcept {
    if (incoming.actor == 0) return false;
    if (destination.actor == 0) {
        destination = incoming;
        return true;
    }
    if (destination.actor != incoming.actor) return false;

    const bool hasOrdinarySource =
        destination.ordinarySource || incoming.ordinarySource;
    const std::uintptr_t ordinaryRoot = destination.ordinaryRoot != 0
        ? destination.ordinaryRoot
        : incoming.ordinaryRoot;
    const std::uintptr_t ordinaryMesh = destination.ordinaryMesh != 0
        ? destination.ordinaryMesh
        : incoming.ordinaryMesh;
    const bool replacePreferred = incoming.resolverRecord &&
        (!destination.resolverRecord ||
         (incoming.encryptedRecord && !destination.encryptedRecord));
    if (replacePreferred) {
        destination.root = incoming.root;
        destination.mesh = incoming.mesh;
        destination.resolverRecord = true;
        destination.encryptedRecord = incoming.encryptedRecord;
    }
    destination.ordinaryRoot = ordinaryRoot;
    destination.ordinaryMesh = ordinaryMesh;
    destination.ordinarySource = hasOrdinarySource;
    return true;
}

inline std::vector<ActorRecordSource> MergeCurrentLevelActorRecordSources(
    const std::vector<std::uintptr_t>& currentActorAddresses,
    const std::vector<ActorRecordSource>& decodedRecords) {
    std::vector<ActorRecordSource> result;
    result.reserve(currentActorAddresses.size());
    std::unordered_map<std::uintptr_t, std::size_t> indices;
    indices.reserve(currentActorAddresses.size());

    for (const std::uintptr_t actor : currentActorAddresses) {
        if (actor == 0) continue;
        const auto inserted = indices.emplace(actor, result.size());
        if (!inserted.second) continue;
        result.push_back(MakeOrdinaryActorRecord(actor));
    }

    for (const ActorRecordSource& decoded : decodedRecords) {
        const auto found = indices.find(decoded.actor);
        if (found == indices.end()) continue;
        MergeActorRecordSource(result[found->second], decoded);
    }
    return result;
}

template <typename ReadPointer>
void FillOrdinaryActorPointers(
    ActorRecordSource& source,
    ReadPointer&& readPointer) {
    if (!source.ordinarySource || source.actor == 0) return;
    if (source.ordinaryRoot == 0) {
        source.ordinaryRoot =
            readPointer(source.actor + kOrdinaryActorRootOffset);
    }
    if (source.ordinaryMesh == 0) {
        source.ordinaryMesh =
            readPointer(source.actor + kOrdinaryActorMeshOffset);
    }
}

template <typename PreferredRead, typename OrdinaryRead>
bool ReadActorRecordSourceWithFallback(
    const ActorRecordSource& source,
    PreferredRead&& readPreferred,
    OrdinaryRead&& readOrdinary) {
    if (source.resolverRecord &&
        std::forward<PreferredRead>(readPreferred)()) {
        return true;
    }
    return source.ordinarySource &&
        std::forward<OrdinaryRead>(readOrdinary)();
}

}  // namespace lengjing::game::native
