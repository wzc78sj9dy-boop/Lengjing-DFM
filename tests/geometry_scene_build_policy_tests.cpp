#include "game/native/GeometrySceneBuildPolicy.h"
#include "game/native/GeometryShapeFilterPolicy.h"
#include "test_support.h"

#include <chrono>
#include <memory>
#include <vector>

void RunGeometrySceneBuildPolicyTests() {
    using lengjing::game::native::GeometrySceneKind;
    using lengjing::game::native::GeometryBodyType;
    using lengjing::game::native::ResolveGeometrySceneBuildPolicy;
    using lengjing::game::native::ShouldIncludeGeometryShape;
    using lengjing::game::native::ShouldRequestGeometryRefresh;

    REQUIRE(!ShouldRequestGeometryRefresh(0, 0x1000));
    REQUIRE(!ShouldRequestGeometryRefresh(0, 0));
    REQUIRE(!ShouldRequestGeometryRefresh(0x1000, 0x1000));
    REQUIRE(ShouldRequestGeometryRefresh(0x1000, 0x2000));
    REQUIRE(ShouldRequestGeometryRefresh(0x1000, 0));

    const auto staticPolicy =
        ResolveGeometrySceneBuildPolicy(GeometrySceneKind::Static);
    REQUIRE(!staticPolicy.lowBuildQuality);
    REQUIRE(!staticPolicy.dynamicScene);
    REQUIRE(staticPolicy.robust);
    REQUIRE(staticPolicy.compact);

    const auto dynamicPolicy =
        ResolveGeometrySceneBuildPolicy(GeometrySceneKind::Dynamic);
    REQUIRE(dynamicPolicy.lowBuildQuality);
    REQUIRE(dynamicPolicy.dynamicScene);
    REQUIRE(!dynamicPolicy.robust);
    REQUIRE(dynamicPolicy.compact);

    struct Mesh {
        int value = 0;
    };
    using lengjing::game::native::CanReuseGeometryScene;
    using lengjing::game::native::ReuseEquivalentGeometryMesh;
    const auto first = std::make_shared<const Mesh>(Mesh{1});
    const auto second = std::make_shared<const Mesh>(Mesh{2});
    const std::vector<std::uintptr_t> scenes{0x1000, 0x2000};
    const std::vector<std::shared_ptr<const Mesh>> previous{first, second};

    REQUIRE(CanReuseGeometryScene(
        0x3000, scenes, previous, 0x3000, scenes, previous));

    const auto equalValueButDifferentObject =
        std::make_shared<const Mesh>(Mesh{1});
    REQUIRE(!CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        scenes,
        std::vector<std::shared_ptr<const Mesh>>{
            equalValueButDifferentObject, second}));
    REQUIRE(!CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        scenes,
        std::vector<std::shared_ptr<const Mesh>>{second, first}));
    REQUIRE(!CanReuseGeometryScene(
        0x3000, scenes, previous, 0x4000, scenes, previous));
    REQUIRE(!CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        std::vector<std::uintptr_t>{0x1000, 0x2100},
        previous));
    REQUIRE(!CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        std::vector<std::uintptr_t>{0x2000, 0x1000},
        previous));
    REQUIRE(!CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        scenes,
        std::vector<std::shared_ptr<const Mesh>>{first}));

    auto equivalentFirst = std::make_shared<const Mesh>(Mesh{1});
    REQUIRE(ReuseEquivalentGeometryMesh(
        first, equivalentFirst,
        [](const Mesh& left, const Mesh& right) {
            return left.value == right.value;
        }));
    REQUIRE(equivalentFirst.get() == first.get());
    REQUIRE(CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        scenes,
        std::vector<std::shared_ptr<const Mesh>>{
            equivalentFirst, second}));

    auto changedFirst = std::make_shared<const Mesh>(Mesh{3});
    const auto* changedAddress = changedFirst.get();
    REQUIRE(!ReuseEquivalentGeometryMesh(
        first, changedFirst,
        [](const Mesh& left, const Mesh& right) {
            return left.value == right.value;
        }));
    REQUIRE(changedFirst.get() == changedAddress);
    REQUIRE(!CanReuseGeometryScene(
        0x3000,
        scenes,
        previous,
        0x3000,
        scenes,
        std::vector<std::shared_ptr<const Mesh>>{changedFirst, second}));

    std::shared_ptr<const Mesh> missing;
    REQUIRE(!ReuseEquivalentGeometryMesh(
        first, missing,
        [](const Mesh& left, const Mesh& right) {
            return left.value == right.value;
        }));

    using lengjing::game::native::ShouldPublishGeometryUpdate;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point publishedAt{};
    REQUIRE(ShouldPublishGeometryUpdate(
        false, publishedAt, publishedAt, false));
    REQUIRE(!ShouldPublishGeometryUpdate(
        true, publishedAt, publishedAt + std::chrono::milliseconds(32),
        false));
    REQUIRE(ShouldPublishGeometryUpdate(
        true, publishedAt, publishedAt + std::chrono::milliseconds(33),
        false));
    REQUIRE(ShouldPublishGeometryUpdate(
        true, publishedAt, publishedAt + std::chrono::milliseconds(1),
        true));
    REQUIRE(ShouldPublishGeometryUpdate(
        true, publishedAt + std::chrono::milliseconds(1), publishedAt,
        false));

    REQUIRE(ShouldIncludeGeometryShape(
        GeometryBodyType::Static, 5, 0, 1, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Static, 5, 3, 0, 0.0f));
    REQUIRE(ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 5, 0, 0, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 5, 3, 1, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Static, 3, 1, 0, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Static, 4, 1, 0, 0.0f));
    REQUIRE(ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 0, 1, 0, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 0, 4, 0, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 0, 2, 0, 0.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 0, 1, 1, 0.0f));
    REQUIRE(ShouldIncludeGeometryShape(
        GeometryBodyType::Static, 6, 0, 0, 200.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Static, 6, 0, 0, 100.0f));
    REQUIRE(!ShouldIncludeGeometryShape(
        GeometryBodyType::Dynamic, 6, 0, 0, 200.0f));
}
