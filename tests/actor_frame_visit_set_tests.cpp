#include "game/native/ActorFrameVisitSet.h"
#include "test_support.h"

void RunActorFrameVisitSetTests() {
    lengjing::game::native::ActorFrameVisitSet visits;
    visits.Reserve(4);

    REQUIRE(!visits.TryVisit(0));
    REQUIRE(visits.TryVisit(0x1000));
    REQUIRE(!visits.TryVisit(0x1000));
    REQUIRE(visits.TryVisit(0x2000));
    REQUIRE(visits.Size() == 2);
}
