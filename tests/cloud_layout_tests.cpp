#include "test_support.h"

#include "auth/CloudLayout.h"

#include <cstdint>
#include <sstream>
#include <string>

namespace {

constexpr const char* kBuildId =
    "fedcba98765432100123456789abcdef";
constexpr const char* kCoordinateBuildId =
    "8187ddb9edbc9d5201201ffd7b008df3bfe533db";

std::string LayoutJson(std::uint64_t revision,
                       std::string buildId = kBuildId,
                       std::uint64_t maximumActorCount = 12288,
                       bool includeLegacyTail = false) {
    std::ostringstream stream;
    stream << R"({"v":5,"b":")" << buildId
           << R"(","r":)" << revision
           << R"(,"d":[["0x21001000","0x22002000",)"
           << R"(["0x23003000","0x24004000"],)"
           << maximumActorCount << ','
           << R"(["0x1a0","0x410","0x420"],)"
           << R"("0x27007000"])";
    if (includeLegacyTail) {
        stream << R"(,["ignored"])";
    }
    stream << "]}";
    return stream.str();
}

std::string PreviousLayoutJson(std::string buildId = kCoordinateBuildId) {
    std::ostringstream stream;
    stream << R"({"v":4,"b":")" << buildId
           << R"(","r":1,"d":[["0x21001000","0x22002000",)"
           << R"(["0x23003000","0x24004000"],)"
           << R"(["0x25005000","0x26006000","0x284","0x414",)"
           << R"(1536,40,12288,3072],)"
           << R"(["0x1a0","0x410","0x420"],)"
           << R"("0x27007000","0x28008003"],[["discarded"]]]})";
    return stream.str();
}

lengjing::auth::CloudRuntimeTarget RuntimeTarget() {
    return {"com.example.runtime", "libSynthetic.so"};
}

}  // namespace

void RunCloudLayoutTests() {
    using namespace lengjing::auth;

    CloudLayoutStore store(RuntimeTarget());
    const auto first = store.ValidateAndPublish(LayoutJson(1));
    REQUIRE(first.status == CloudLayoutStatus::Published);
    REQUIRE(first.snapshot != nullptr);
    REQUIRE(first.snapshot->schemaVersion == kCloudLayoutSchemaVersion);
    REQUIRE(first.snapshot->layout.maximumActorCount == 12288);
    REQUIRE(first.snapshot->layout.actorSubject.meshOffset == 0x410);

    const auto unchanged = store.ValidateAndPublish(LayoutJson(1));
    REQUIRE(unchanged.status == CloudLayoutStatus::Unchanged);

    const auto updated = store.ValidateAndPublish(LayoutJson(2));
    REQUIRE(updated.status == CloudLayoutStatus::Published);
    REQUIRE(store.ValidateAndPublish(
                LayoutJson(2, kBuildId, 12288, true))
                .status == CloudLayoutStatus::Unchanged);

    REQUIRE(store.ValidateAndPublish(LayoutJson(1)).status ==
            CloudLayoutStatus::RollbackRejected);
    REQUIRE(store.ValidateAndPublish(LayoutJson(3, "bad")).status ==
            CloudLayoutStatus::IdentityMismatch);
    REQUIRE(store.ValidateAndPublish(
        R"({"v":4,"b":"fedcba98765432100123456789abcdef","r":3,"d":[]})")
                .status == CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.ValidateAndPublish(
                LayoutJson(3, kBuildId, 0))
                .status == CloudLayoutStatus::RangeError);
    REQUIRE(store.ValidateAndPublish(
                LayoutJson(3, kBuildId, 65537))
                .status == CloudLayoutStatus::RangeError);

    CloudLayoutStore upgradedStore(
        {"com.tencent.tmgp.dfm", "libUE4.so"});
    const auto upgraded =
        upgradedStore.ValidateAndPublish(PreviousLayoutJson());
    REQUIRE(upgraded.status == CloudLayoutStatus::Published);
    REQUIRE(upgraded.snapshot != nullptr);
    REQUIRE(upgraded.snapshot->schemaVersion == kCloudLayoutSchemaVersion);
    REQUIRE(upgraded.snapshot->identity.buildId == kCoordinateBuildId);
    REQUIRE(upgraded.snapshot->layout.maximumActorCount == 12288);

    CloudLayoutStore wrongTarget(RuntimeTarget());
    REQUIRE(wrongTarget.ValidateAndPublish(PreviousLayoutJson()).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(upgradedStore.ValidateAndPublish(
                PreviousLayoutJson(kBuildId)).status ==
            CloudLayoutStatus::SchemaMismatch);
}
