#include "test_support.h"

#include "auth/CloudLayout.h"

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kBuildId =
    "0123456789abcdef0123456789abcdef01234567";

std::string LayoutJson(std::uint64_t revision,
                       std::string buildId = kBuildId,
                       std::string worldOffset = "0x13002000",
                       std::string replayEntry = "0x0") {
    std::ostringstream stream;
    stream
        << R"({"schema_version":2,"package":"com.example.runtime",)"
        << R"("module":"libUE4.so","build_id":")" << buildId
        << R"(","revision":)" << revision
        << R"(,"layout":{"name_pool":"0x12001000","world":")"
        << worldOffset
        << R"(","coordinate_replay_entry":")" << replayEntry
        << R"(","geometry_instances":["0x14003000","0x15004000"],)"
        << R"("tracking_matrix_root":"0x18005000",)"
        << R"("component_position_flag":"0x19006001",)"
        << R"("actor_records":{"tagged_container":"0x16007000",)"
        << R"("plain_array":"0x17008000","plain_root":"0x188",)"
        << R"("plain_mesh":"0x3e0","encrypted_record_count":1024,)"
        << R"("plain_record_stride":32,"maximum_plain_count":8192,)"
        << R"("fallback_plain_count":2048},)"
        << R"("coordinate_pool":{"root_rva":"0x1a009000",)"
        << R"("bridge_offset":"0x14","context_offset":-16,)"
        << R"("entry_offset":"0xb0","component_key_offset":"0x220",)"
        << R"("pacga_data":"0x13579bdf",)"
        << R"("pacga_modifier":"0x2468ace0","entry_stride":64,)"
        << R"("pool_head_skip":24,"ring_refresh_frames":90}}})";
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
    REQUIRE(first.snapshot->layout.namePoolOffset == 0x12001000ULL);
    REQUIRE(first.snapshot->layout.coordinateReplayEntryOffset == 0);
    REQUIRE(first.snapshot->layout.trackingMatrixRootOffset ==
            0x18005000ULL);
    REQUIRE(first.snapshot->layout.componentPositionFlagOffset ==
            0x19006001ULL);
    REQUIRE(first.snapshot->layout.geometryInstancePointerOffsets[0] ==
            0x14003000ULL);
    REQUIRE(first.snapshot->layout.geometryInstancePointerOffsets[1] ==
            0x15004000ULL);
    REQUIRE(first.snapshot->layout.actorRecords.taggedContainerOffset ==
            0x16007000ULL);
    REQUIRE(first.snapshot->layout.actorRecords.plainArrayOffset ==
            0x17008000ULL);
    REQUIRE(first.snapshot->layout.actorRecords.plainRootOffset == 0x188);
    REQUIRE(first.snapshot->layout.actorRecords.plainMeshOffset == 0x3e0);
    REQUIRE(first.snapshot->layout.actorRecords.encryptedRecordCount == 1024);
    REQUIRE(first.snapshot->layout.actorRecords.plainRecordStride == 32);
    REQUIRE(first.snapshot->layout.actorRecords.maximumPlainCount == 8192);
    REQUIRE(first.snapshot->layout.actorRecords.fallbackPlainCount == 2048);
    REQUIRE(first.snapshot->layout.coordinatePool.rootRva == 0x1a009000ULL);
    REQUIRE(first.snapshot->layout.coordinatePool.bridgeOffset == 0x14);
    REQUIRE(first.snapshot->layout.coordinatePool.contextOffset == -16);
    REQUIRE(first.snapshot->layout.coordinatePool.entryOffset == 0xb0);
    REQUIRE(first.snapshot->layout.coordinatePool.componentKeyOffset == 0x220);
    REQUIRE(first.snapshot->layout.coordinatePool.pacgaData == 0x13579bdfULL);
    REQUIRE(first.snapshot->layout.coordinatePool.pacgaModifier ==
            0x2468ace0ULL);
    REQUIRE(first.snapshot->layout.coordinatePool.entryStride == 64);
    REQUIRE(first.snapshot->layout.coordinatePool.poolHeadSkip == 24);
    REQUIRE(first.snapshot->layout.coordinatePool.ringRefreshFrames == 90);

    const auto stable = store.Snapshot();
    const CloudLayoutUpdateResult unchanged =
        store.ValidateAndPublish(LayoutJson(7));
    REQUIRE(unchanged.status == CloudLayoutStatus::Unchanged);
    REQUIRE(unchanged.snapshot == stable);

    const CloudLayoutUpdateResult conflict = store.ValidateAndPublish(
        LayoutJson(7, kBuildId, "0x13002008"));
    REQUIRE(conflict.status == CloudLayoutStatus::RevisionConflict);
    REQUIRE(store.Snapshot() == stable);

    const std::string coordinateConflict = ReplaceFirst(
        LayoutJson(7), "\"pacga_modifier\":\"0x2468ace0\"",
        "\"pacga_modifier\":\"0x2468ace1\"");
    const CloudLayoutUpdateResult rejectedCoordinateConflict =
        store.ValidateAndPublish(coordinateConflict);
    REQUIRE(rejectedCoordinateConflict.status ==
            CloudLayoutStatus::RevisionConflict);
    REQUIRE(store.Snapshot() == stable);

    const auto requireRevisionConflict = [&](const std::string& needle,
                                             const std::string& replacement) {
        const CloudLayoutUpdateResult changed = store.ValidateAndPublish(
            ReplaceFirst(LayoutJson(7), needle, replacement));
        REQUIRE(changed.status == CloudLayoutStatus::RevisionConflict);
        REQUIRE(changed.snapshot == stable);
        REQUIRE(store.Snapshot() == stable);
    };
    requireRevisionConflict(
        "\"name_pool\":\"0x12001000\"",
        "\"name_pool\":\"0x12001008\"");
    requireRevisionConflict(
        "\"coordinate_replay_entry\":\"0x0\"",
        "\"coordinate_replay_entry\":\"0x1234\"");
    requireRevisionConflict(
        "\"0x14003000\"", "\"0x14003008\"");
    requireRevisionConflict(
        "\"tagged_container\":\"0x16007000\"",
        "\"tagged_container\":\"0x16007004\"");
    requireRevisionConflict(
        "\"plain_array\":\"0x17008000\"",
        "\"plain_array\":\"0x17008008\"");
    requireRevisionConflict(
        "\"plain_root\":\"0x188\"",
        "\"plain_root\":\"0x18c\"");
    requireRevisionConflict(
        "\"plain_mesh\":\"0x3e0\"",
        "\"plain_mesh\":\"0x3e4\"");
    requireRevisionConflict(
        "\"encrypted_record_count\":1024",
        "\"encrypted_record_count\":1025");
    requireRevisionConflict(
        "\"plain_record_stride\":32",
        "\"plain_record_stride\":40");
    requireRevisionConflict(
        "\"maximum_plain_count\":8192",
        "\"maximum_plain_count\":8193");
    requireRevisionConflict(
        "\"fallback_plain_count\":2048",
        "\"fallback_plain_count\":2047");
    requireRevisionConflict(
        "\"tracking_matrix_root\":\"0x18005000\"",
        "\"tracking_matrix_root\":\"0x18005008\"");
    requireRevisionConflict(
        "\"component_position_flag\":\"0x19006001\"",
        "\"component_position_flag\":\"0x19006002\"");
    requireRevisionConflict(
        "\"root_rva\":\"0x1a009000\"",
        "\"root_rva\":\"0x1a009004\"");
    requireRevisionConflict(
        "\"bridge_offset\":\"0x14\"",
        "\"bridge_offset\":\"0x18\"");
    requireRevisionConflict(
        "\"context_offset\":-16",
        "\"context_offset\":-24");
    requireRevisionConflict(
        "\"entry_offset\":\"0xb0\"",
        "\"entry_offset\":\"0xb8\"");
    requireRevisionConflict(
        "\"component_key_offset\":\"0x220\"",
        "\"component_key_offset\":\"0x228\"");
    requireRevisionConflict(
        "\"pacga_data\":\"0x13579bdf\"",
        "\"pacga_data\":\"0x13579be0\"");
    requireRevisionConflict(
        "\"entry_stride\":64", "\"entry_stride\":68");
    requireRevisionConflict(
        "\"pool_head_skip\":24", "\"pool_head_skip\":28");
    requireRevisionConflict(
        "\"ring_refresh_frames\":90",
        "\"ring_refresh_frames\":91");

    const CloudLayoutUpdateResult rollback =
        store.ValidateAndPublish(LayoutJson(6));
    REQUIRE(rollback.status == CloudLayoutStatus::RollbackRejected);
    REQUIRE(store.Snapshot() == stable);

    const CloudLayoutUpdateResult wrongBuild = store.ValidateAndPublish(
        LayoutJson(8, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    REQUIRE(wrongBuild.status == CloudLayoutStatus::IdentityMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string missingField = ReplaceFirst(
        LayoutJson(8), "\"plain_mesh\":\"0x3e0\",", "");
    const CloudLayoutUpdateResult missing =
        store.ValidateAndPublish(missingField);
    REQUIRE(missing.status == CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string wrongType = ReplaceFirst(
        LayoutJson(8), "\"name_pool\":\"0x12001000\"",
        "\"name_pool\":483178688");
    const CloudLayoutUpdateResult typed =
        store.ValidateAndPublish(wrongType);
    REQUIRE(typed.status == CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string partialPlain = ReplaceFirst(
        LayoutJson(8), "\"plain_array\":\"0x17008000\"",
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

    const std::string unknownActorKey = ReplaceFirst(
        LayoutJson(8), "\"fallback_plain_count\":2048",
        "\"fallback_plain_count\":2048,\"unknown\":1");
    REQUIRE(store.ValidateAndPublish(unknownActorKey).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string unknownCoordinateKey = ReplaceFirst(
        LayoutJson(8), "\"ring_refresh_frames\":90",
        "\"ring_refresh_frames\":90,\"unknown\":1");
    REQUIRE(store.ValidateAndPublish(unknownCoordinateKey).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string wrongGeometryCount = ReplaceFirst(
        LayoutJson(8),
        "[\"0x14003000\",\"0x15004000\"]",
        "[\"0x14003000\"]");
    REQUIRE(store.ValidateAndPublish(wrongGeometryCount).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string duplicateKey = ReplaceFirst(
        LayoutJson(8), "\"revision\":8,",
        "\"revision\":8,\"revision\":9,");
    const CloudLayoutUpdateResult duplicate =
        store.ValidateAndPublish(duplicateKey);
    REQUIRE(duplicate.status == CloudLayoutStatus::InvalidJson);
    REQUIRE(store.Snapshot() == stable);

    const std::string nestedDuplicateKey = ReplaceFirst(
        LayoutJson(8), "\"root_rva\":\"0x1a009000\",",
        "\"root_rva\":\"0x1a009000\",\"root_rva\":\"0x1a009004\",");
    const CloudLayoutUpdateResult nestedDuplicate =
        store.ValidateAndPublish(nestedDuplicateKey);
    REQUIRE(nestedDuplicate.status == CloudLayoutStatus::InvalidJson);
    REQUIRE(store.Snapshot() == stable);

    const std::string actorDuplicateKey = ReplaceFirst(
        LayoutJson(8), "\"plain_root\":\"0x188\",",
        "\"plain_root\":\"0x188\",\"plain_root\":\"0x18c\",");
    REQUIRE(store.ValidateAndPublish(actorDuplicateKey).status ==
            CloudLayoutStatus::InvalidJson);
    REQUIRE(store.Snapshot() == stable);

    const std::string oldSchema = ReplaceFirst(
        LayoutJson(8), "\"schema_version\":2",
        "\"schema_version\":1");
    REQUIRE(store.ValidateAndPublish(oldSchema).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string missingCoordinate = ReplaceFirst(
        LayoutJson(8),
        R"(,"coordinate_pool":{"root_rva":"0x1a009000","bridge_offset":"0x14","context_offset":-16,"entry_offset":"0xb0","component_key_offset":"0x220","pacga_data":"0x13579bdf","pacga_modifier":"0x2468ace0","entry_stride":64,"pool_head_skip":24,"ring_refresh_frames":90})",
        "");
    REQUIRE(store.ValidateAndPublish(missingCoordinate).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const std::string badContext = ReplaceFirst(
        LayoutJson(8), "\"context_offset\":-16",
        "\"context_offset\":-4");
    REQUIRE(store.ValidateAndPublish(badContext).status ==
            CloudLayoutStatus::RangeError);
    REQUIRE(store.Snapshot() == stable);

    const std::string badEntryAlignment = ReplaceFirst(
        LayoutJson(8), "\"entry_offset\":\"0xb0\"",
        "\"entry_offset\":\"0xb4\"");
    REQUIRE(store.ValidateAndPublish(badEntryAlignment).status ==
            CloudLayoutStatus::RangeError);
    REQUIRE(store.Snapshot() == stable);

    const std::string overflowingBridge = ReplaceFirst(
        LayoutJson(8), "\"root_rva\":\"0x1a009000\"",
        "\"root_rva\":\"0xfffffffc\"");
    REQUIRE(store.ValidateAndPublish(overflowingBridge).status ==
            CloudLayoutStatus::RangeError);
    REQUIRE(store.Snapshot() == stable);

    const std::string badPacga = ReplaceFirst(
        LayoutJson(8), "\"pacga_data\":\"0x13579bdf\"",
        "\"pacga_data\":\"0x13579BDF\"");
    REQUIRE(store.ValidateAndPublish(badPacga).status ==
            CloudLayoutStatus::SchemaMismatch);
    REQUIRE(store.Snapshot() == stable);

    const CloudLayoutUpdateResult newer =
        store.ValidateAndPublish(LayoutJson(8, kBuildId, "0x13002000",
                                            "0x1234"));
    REQUIRE(newer.status == CloudLayoutStatus::Published);
    REQUIRE(newer.snapshot->revision == 8);
    REQUIRE(newer.snapshot->layout.coordinateReplayEntryOffset == 0x1234);

    CloudLayoutStore concurrentStore(RuntimeIdentity());
    std::atomic_bool start{false};
    std::atomic_bool done{false};
    std::atomic_bool invalidSnapshot{false};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!done.load(std::memory_order_acquire)) {
                const auto snapshot = concurrentStore.Snapshot();
                if (snapshot != nullptr &&
                    (snapshot->schemaVersion != kCloudLayoutSchemaVersion ||
                     snapshot->revision == 0 ||
                      snapshot->identity.buildId != kBuildId ||
                      snapshot->layout.namePoolOffset != 0x12001000ULL ||
                      snapshot->layout.worldOffset != 0x13002000ULL ||
                      snapshot->layout.coordinatePool.rootRva !=
                          0x1a009000ULL ||
                     snapshot->layout.coordinatePool.entryOffset != 0xb0)) {
                    invalidSnapshot.store(true, std::memory_order_release);
                    break;
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    bool publishFailed = false;
    for (std::uint64_t revision = 1; revision <= 64; ++revision) {
        if (concurrentStore.ValidateAndPublish(LayoutJson(revision)).status !=
            CloudLayoutStatus::Published) {
            publishFailed = true;
            break;
        }
    }
    done.store(true, std::memory_order_release);
    for (std::thread& reader : readers) reader.join();
    REQUIRE(!publishFailed);
    REQUIRE(!invalidSnapshot.load(std::memory_order_acquire));
    REQUIRE(concurrentStore.Snapshot()->revision == 64);
}
