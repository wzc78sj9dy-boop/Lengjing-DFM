#include "test_support.h"

#include "auth/CloudLayout.h"

#include <cstdint>
#include <sstream>
#include <string>

namespace {

constexpr const char* kBuildId =
    "0123456789abcdef0123456789abcdef01234567";

std::string LayoutJson(std::uint64_t revision,
                       std::string buildId = kBuildId,
                       std::string worldOffset = "0x1d0e8668",
                       std::string replayEntry = "0x0") {
    std::ostringstream stream;
    stream
        << R"({"schema_version":1,"package":"com.example.runtime",)"
        << R"("module":"libUE4.so","build_id":")" << buildId
        << R"(","revision":)" << revision
        << R"(,"layout":{"name_pool":"0x1cdcb8c0","world":")"
        << worldOffset
        << R"(","coordinate_replay_entry":")" << replayEntry
        << R"(","geometry_instances":["0x1c3c5368","0x1af01c68"],)"
        << R"("tracking_matrix_root":"0x1d0ad4c0",)"
        << R"("component_position_flag":"0x1dcfb4f",)"
        << R"("actor_records":{"tagged_container":"0xeebdb14",)"
        << R"("plain_array":"0x1d0a6908","plain_root":"0x180",)"
        << R"("plain_mesh":"0x3d0","encrypted_record_count":1000,)"
        << R"("plain_record_stride":24,"maximum_plain_count":10000,)"
        << R"("fallback_plain_count":3000}}})";
    return stream.str();
}

std::string ReplaceFirst(std::string value,
                         const std::string& needle,
                         const std::string& replacement) {
    const std::size_t offset = value.find(needle);
    REQUIRE(offset != std::string::npos);
    value.replace(offset, needle.size(), replacement);
    return value;
}

lengjing::auth::CloudRuntimeIdentity RuntimeIdentity() {
    return {"com.example.runtime", "libUE4.so", kBuildId};
}

}  // namespace

void RunCloudLayoutTests() {
    using namespace lengjing::auth;

    CloudLayoutStore store(RuntimeIdentity());
    const CloudLayoutUpdateResult first =
        store.ValidateAndPublish(LayoutJson(7));
    REQUIRE(first.status == CloudLayoutStatus::Published);
    REQUIRE(first.snapshot != nullptr);
    REQUIRE(first.snapshot->revision == 7);
    REQUIRE(first.snapshot->layout.namePoolOffset == 0x1cdcb8c0ULL);
    REQUIRE(first.snapshot->layout.coordinateReplayEntryOffset == 0);
    REQUIRE(first.snapshot->layout.trackingMatrixRootOffset ==
            0x1d0ad4c0ULL);
    REQUIRE(first.snapshot->layout.componentPositionFlagOffset ==
            0x1dcfb4fULL);

    const auto stable = store.Snapshot();
    const CloudLayoutUpdateResult unchanged =
        store.ValidateAndPublish(LayoutJson(7));
    REQUIRE(unchanged.status == CloudLayoutStatus::Unchanged);
    REQUIRE(unchanged.snapshot == stable);

    const CloudLayoutUpdateResult conflict = store.ValidateAndPublish(
        LayoutJson(7, kBuildId, "0x1d0e8670"));
    REQUIRE(conflict.status == CloudLayoutStatus::RevisionConflict);
    REQUIRE(store.Snapshot() == stable);

    const CloudLayoutUpdateResult rollback =
        store.ValidateAndPublish(LayoutJson(6));
    REQUIRE(rollback.status == CloudLayoutStatus::RollbackRejected);
    REQUIRE(store.Snapshot() == stable);

    const CloudLayoutUpdateResult wrongBuild = store.ValidateAndPublish(
        LayoutJson(8, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    REQUIRE(wrongBuild.status == CloudLayoutStatus::IdentityMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string missingField = ReplaceFirst(
        LayoutJson(8), "\"plain_mesh\":\"0x3d0\",", "");
    const CloudLayoutUpdateResult missing =
        store.ValidateAndPublish(missingField);
    REQUIRE(missing.status == CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string wrongType = ReplaceFirst(
        LayoutJson(8), "\"name_pool\":\"0x1cdcb8c0\"",
        "\"name_pool\":483178688");
    const CloudLayoutUpdateResult typed =
        store.ValidateAndPublish(wrongType);
    REQUIRE(typed.status == CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string partialPlain = ReplaceFirst(
        LayoutJson(8), "\"plain_array\":\"0x1d0a6908\"",
        "\"plain_array\":\"0x0\"");
    const CloudLayoutUpdateResult partial =
        store.ValidateAndPublish(partialPlain);
    REQUIRE(partial.status == CloudLayoutStatus::RangeError);
    REQUIRE(store.Snapshot() == stable);

    const std::string unknownKey = ReplaceFirst(
        LayoutJson(8), "\"revision\":8,",
        "\"revision\":8,\"unknown\":1,");
    const CloudLayoutUpdateResult unknown =
        store.ValidateAndPublish(unknownKey);
    REQUIRE(unknown.status == CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string duplicateKey = ReplaceFirst(
        LayoutJson(8), "\"revision\":8,",
        "\"revision\":8,\"revision\":9,");
    const CloudLayoutUpdateResult duplicate =
        store.ValidateAndPublish(duplicateKey);
    REQUIRE(duplicate.status == CloudLayoutStatus::InvalidJson);
    REQUIRE(store.Snapshot() == stable);

    const CloudLayoutUpdateResult newer =
        store.ValidateAndPublish(LayoutJson(8, kBuildId, "0x1d0e8668",
                                            "0x1234"));
    REQUIRE(newer.status == CloudLayoutStatus::Published);
    REQUIRE(newer.snapshot->revision == 8);
    REQUIRE(newer.snapshot->layout.coordinateReplayEntryOffset == 0x1234);
}
