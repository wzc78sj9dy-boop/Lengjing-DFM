#include "game/native/ActorRecordSource.h"
#include "test_support.h"

void RunActorRecordSourceTests() {
    using lengjing::game::native::ActorRecordSource;
    using lengjing::game::native::FillOrdinaryActorPointers;
    using lengjing::game::native::IsActorPresentInCurrentLevel;
    using lengjing::game::native::MakeOrdinaryActorRecord;
    using lengjing::game::native::MakeResolvedActorRecord;
    using lengjing::game::native::MergeActorRecordSource;
    using lengjing::game::native::ReadActorRecordSourceWithFallback;

    REQUIRE(IsActorPresentInCurrentLevel(false, false));
    REQUIRE(IsActorPresentInCurrentLevel(false, true));
    REQUIRE(IsActorPresentInCurrentLevel(true, true));
    REQUIRE(!IsActorPresentInCurrentLevel(true, false));

    constexpr std::uintptr_t actor = 0x1000;
    const ActorRecordSource decoded =
        MakeResolvedActorRecord(actor, 0x2000, 0x3000, true);
    const ActorRecordSource ordinary =
        MakeOrdinaryActorRecord(actor, 0x4000, 0x5000);

    ActorRecordSource decodedFirst = decoded;
    REQUIRE(MergeActorRecordSource(decodedFirst, ordinary));
    REQUIRE(decodedFirst.root == 0x2000);
    REQUIRE(decodedFirst.mesh == 0x3000);
    REQUIRE(decodedFirst.encryptedRecord);
    REQUIRE(decodedFirst.ordinarySource);
    REQUIRE(decodedFirst.ordinaryRoot == 0x4000);
    REQUIRE(decodedFirst.ordinaryMesh == 0x5000);

    ActorRecordSource pendingOrdinary = decoded;
    REQUIRE(MergeActorRecordSource(
        pendingOrdinary,
        MakeOrdinaryActorRecord(actor)));
    int pointerReads = 0;
    FillOrdinaryActorPointers(
        pendingOrdinary,
        [&](std::uintptr_t address) {
            ++pointerReads;
            if (address == actor + 0x180) return std::uintptr_t{0x4000};
            if (address == actor + 0x3D0) return std::uintptr_t{0x5000};
            return std::uintptr_t{0};
        });
    REQUIRE(pointerReads == 2);
    REQUIRE(pendingOrdinary.root == 0x2000);
    REQUIRE(pendingOrdinary.mesh == 0x3000);
    REQUIRE(pendingOrdinary.encryptedRecord);
    REQUIRE(pendingOrdinary.ordinaryRoot == 0x4000);
    REQUIRE(pendingOrdinary.ordinaryMesh == 0x5000);

    FillOrdinaryActorPointers(
        pendingOrdinary,
        [&](std::uintptr_t) {
            ++pointerReads;
            return std::uintptr_t{0};
        });
    REQUIRE(pointerReads == 2);

    int ignoredReads = 0;
    ActorRecordSource resolvedOnly = decoded;
    FillOrdinaryActorPointers(
        resolvedOnly,
        [&](std::uintptr_t) {
            ++ignoredReads;
            return std::uintptr_t{0};
        });
    REQUIRE(ignoredReads == 0);

    ActorRecordSource ordinaryFirst = ordinary;
    REQUIRE(MergeActorRecordSource(ordinaryFirst, decoded));
    REQUIRE(ordinaryFirst.root == 0x2000);
    REQUIRE(ordinaryFirst.mesh == 0x3000);
    REQUIRE(ordinaryFirst.encryptedRecord);
    REQUIRE(ordinaryFirst.ordinarySource);

    REQUIRE(!MergeActorRecordSource(
        ordinaryFirst,
        MakeOrdinaryActorRecord(0x6000)));

    int preferredAttempts = 0;
    int ordinaryAttempts = 0;
    REQUIRE(ReadActorRecordSourceWithFallback(
        decodedFirst,
        [&] {
            ++preferredAttempts;
            return false;
        },
        [&] {
            ++ordinaryAttempts;
            return true;
        }));
    REQUIRE(preferredAttempts == 1);
    REQUIRE(ordinaryAttempts == 1);

    preferredAttempts = 0;
    ordinaryAttempts = 0;
    REQUIRE(ReadActorRecordSourceWithFallback(
        decodedFirst,
        [&] {
            ++preferredAttempts;
            return true;
        },
        [&] {
            ++ordinaryAttempts;
            return true;
        }));
    REQUIRE(preferredAttempts == 1);
    REQUIRE(ordinaryAttempts == 0);

    preferredAttempts = 0;
    ordinaryAttempts = 0;
    REQUIRE(ReadActorRecordSourceWithFallback(
        pendingOrdinary,
        [&] {
            ++preferredAttempts;
            return true;
        },
        [&] {
            ++ordinaryAttempts;
            return true;
        }));
    REQUIRE(preferredAttempts == 1);
    REQUIRE(ordinaryAttempts == 0);

    preferredAttempts = 0;
    ordinaryAttempts = 0;
    REQUIRE(ReadActorRecordSourceWithFallback(
        ordinary,
        [&] {
            ++preferredAttempts;
            return true;
        },
        [&] {
            ++ordinaryAttempts;
            return true;
        }));
    REQUIRE(preferredAttempts == 0);
    REQUIRE(ordinaryAttempts == 1);

    const ActorRecordSource decodedOnly =
        MakeResolvedActorRecord(actor, 0x2000, 0x3000, true);
    REQUIRE(!ReadActorRecordSourceWithFallback(
        decodedOnly,
        [] { return false; },
        [] { return true; }));
}
