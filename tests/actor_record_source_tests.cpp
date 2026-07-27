#include "game/native/ActorRecordSource.h"
#include "test_support.h"

#include <unordered_map>
#include <vector>

void RunActorRecordSourceTests() {
    using lengjing::game::native::ActorRecordSource;
    using lengjing::game::native::ActorSubjectLayout;
    using lengjing::game::native::BuildActorRecords;
    using lengjing::game::native::FillActorPointers;
    using lengjing::game::native::IsActorCoordinateSubjectPointer;
    using lengjing::game::native::MakeActorRecord;
    using lengjing::game::native::ResolveActorCoordinateSubject;

    constexpr ActorSubjectLayout layout{0x220, 0x428, 0x438};
    static_assert(layout.IsValid());
    static_assert(IsActorCoordinateSubjectPointer(0x10000));
    static_assert(!IsActorCoordinateSubjectPointer(0x1000));

    const std::vector<ActorRecordSource> records =
        BuildActorRecords({0x10000, 0, 0x20000});
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].actor == 0x10000);
    REQUIRE(records[1].actor == 0x20000);

    std::unordered_map<std::uintptr_t, std::uintptr_t> pointers{
        {0x10000 + layout.rootOffset, 0x30000},
        {0x10000 + layout.meshOffset, 0x40000},
        {0x10000 + layout.alternateRootOffset, 0x50000},
    };
    const auto readPointer = [&pointers](std::uintptr_t address) {
        const auto found = pointers.find(address);
        return found == pointers.end() ? std::uintptr_t{0} : found->second;
    };

    ActorRecordSource source = MakeActorRecord(0x10000);
    FillActorPointers(source, layout, readPointer);
    REQUIRE(source.root == 0x30000);
    REQUIRE(source.mesh == 0x40000);

    source.root = 0x60000;
    source.mesh = 0x70000;
    FillActorPointers(source, layout, readPointer);
    REQUIRE(source.root == 0x60000);
    REQUIRE(source.mesh == 0x70000);

    const auto validPointer = [](std::uintptr_t value) {
        return IsActorCoordinateSubjectPointer(value);
    };
    REQUIRE(ResolveActorCoordinateSubject(
                source, layout, readPointer, validPointer) == 0x30000);

    pointers[0x10000 + layout.rootOffset] = 0;
    REQUIRE(ResolveActorCoordinateSubject(
                source, layout, readPointer, validPointer) == 0x50000);
}
