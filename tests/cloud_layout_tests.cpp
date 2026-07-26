#include "test_support.h"

#include "auth/CloudLayout.h"

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kSyntheticBuildId =
    "fedcba98765432100123456789abcdef";

std::string LayoutJson(std::uint64_t revision,
                       std::string buildId = kSyntheticBuildId,
                       std::string worldOffset = "0x22002000") {
    std::ostringstream stream;
    stream
        << R"({"schema_version":3,"build_id":")" << buildId
        << R"(","revision":)" << revision
        << R"(,"layout":{"name_pool":"0x21001000","world":")"
        << worldOffset
        << R"(","geometry_instances":["0x23003000","0x24004000"],)"
        << R"("actor_records":{"tagged_container":"0x25005000",)"
        << R"("plain_array":"0x26006000","plain_root":"0x284",)"
        << R"("plain_mesh":"0x414","encrypted_record_count":1536,)"
        << R"("plain_record_stride":40,"maximum_plain_count":12288,)"
        << R"("fallback_plain_count":3072},)"
        << R"("actor_subject":{"root":"0x1a0","mesh":"0x410",)"
        << R"("alternate_root":"0x420"},)"
        << R"("tracking_matrix_root":"0x27007000",)"
        << R"("component_position_flag":"0x28008003"},)"
        << R"("decrypt":{"mode1":{"pool":{"root_rva":"0x29009000",)"
        << R"("bridge_offset":"0x24","context_offset":-24,)"
        << R"("entry_offset":"0xc0","component_key_offset":"0x260",)"
        << R"("entry_stride":80,"pool_head_skip":32,)"
        << R"("ring_refresh_frames":111},"pacga_data":"0x123456789",)"
        << R"("pacga_modifier":"0x987654321"},)"
        << R"("mode2":{"pool":{"root_rva":"0x2a00a000",)"
        << R"("bridge_offset":"0x28","context_offset":-32,)"
        << R"("entry_offset":"0xd0","component_key_offset":"0x270",)"
        << R"("entry_stride":96,"pool_head_skip":36,)"
        << R"("ring_refresh_frames":121}},)"
        << R"("execution":{"discovery":{"root_offset":"0x2b00b000",)"
        << R"("pointer_offset":"0x14","entry_offset":"0xd8",)"
        << R"("return_stub_magic":"0x13572468abcdef01"},)"
        << R"("result":{"slot_offset":"0x280",)"
        << R"("position_offset":"0x184"},)"
        << R"("hook_offsets":{"subject_load":"0x400",)"
        << R"("callback_entry":"0x404","callback_return":"0x408",)"
        << R"("callback_index":"0x40c","callback_copy_prepare":"0x410",)"
        << R"("callback_copy_after":"0x414","table_pointer":"0x418",)"
        << R"("table_value":"0x41c","lock":"0x420",)"
        << R"("lock_return":"0x424","first_call":"0x428",)"
        << R"("first_return":"0x42c","external_call":"0x430",)"
        << R"("external_return":"0x434","primary_gate_write":"0x438",)"
        << R"("alternate_gate_write":"0x43c","gate_probe":"0x440",)"
        << R"("record_count":"0x444","target_key":"0x448",)"
        << R"("ring_setup":"0x44c","ring_probe":"0x450",)"
        << R"("ring_hit":"0x454","dispatch":"0x458",)"
        << R"("dispatch_return":"0x45c","result_prepare":"0x460",)"
        << R"("result":"0x464"},)"
        << R"("field_offsets":{"context_expected":"0x300",)"
        << R"("stack_prior_gate":"0x304",)"
        << R"("stack_primary_gate_source":"0x308",)"
        << R"("stack_gate_flag":"0x30c",)"
        << R"("stack_gate_snapshot_a":"0x310",)"
        << R"("stack_gate_snapshot_b":"0x314",)"
        << R"("stack_ring_mid":"0x318",)"
        << R"("object_position":"0x31c",)"
        << R"("stack_capture_a":"0x320",)"
        << R"("stack_capture_b":"0x324",)"
        << R"("stack_capture_c":"0x328",)"
        << R"("stack_capture_d":"0x32c",)"
        << R"("capture_field":"0x330",)"
        << R"("stack_pool_selector":"0x334",)"
        << R"("context_pool_table":"0x338"},)"
        << R"("context":{"thread_name":"WorkerAlpha",)"
        << R"("oracle_opcode":"0x9ac33041"}}}})";
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

lengjing::auth::CloudRuntimeTarget RuntimeTarget() {
    return {"com.example.runtime", "libSynthetic.so"};
}

}  // namespace

void RunCloudLayoutTests() {
    using namespace lengjing::auth;

    const CloudLayoutDocument empty{};
    REQUIRE(empty.schemaVersion == 0);
    REQUIRE(empty.revision == 0);
    REQUIRE(empty.identity.packageName.empty());
    REQUIRE(empty.layout.namePoolOffset == 0);
    REQUIRE(empty.layout.actorSubject.rootOffset == 0);
    REQUIRE(empty.decrypt.mode1.pool.rootRva == 0);
    REQUIRE(empty.decrypt.mode1.pacgaData == 0);
    REQUIRE(empty.decrypt.mode2.pool.rootRva == 0);
    REQUIRE(empty.decrypt.execution.discovery.rootOffset == 0);
    REQUIRE(empty.decrypt.execution.result.slotOffset == 0);
    REQUIRE(empty.decrypt.execution.hookOffsets.subjectLoad == 0);
    REQUIRE(empty.decrypt.execution.fieldOffsets.contextExpected == 0);
    REQUIRE(empty.decrypt.execution.context.threadName.empty());
    REQUIRE(empty.decrypt.execution.context.oracleOpcode == 0);

    CloudLayoutStore store(RuntimeTarget());
    REQUIRE(store.ExpectedTarget().packageName == "com.example.runtime");
    REQUIRE(store.ExpectedTarget().moduleName == "libSynthetic.so");

    const CloudLayoutUpdateResult first =
        store.ValidateAndPublish(LayoutJson(17));
    REQUIRE(first.status == CloudLayoutStatus::Published);
    REQUIRE(first.snapshot != nullptr);
    REQUIRE(first.snapshot->schemaVersion == 3);
    REQUIRE(first.snapshot->revision == 17);
    REQUIRE(first.snapshot->identity.packageName == "com.example.runtime");
    REQUIRE(first.snapshot->identity.moduleName == "libSynthetic.so");
    REQUIRE(first.snapshot->identity.buildId == kSyntheticBuildId);
    REQUIRE(first.snapshot->layout.namePoolOffset == 0x21001000ULL);
    REQUIRE(first.snapshot->layout.worldOffset == 0x22002000ULL);
    REQUIRE(first.snapshot->layout.geometryInstancePointerOffsets[1] ==
            0x24004000ULL);
    REQUIRE(first.snapshot->layout.actorRecords.taggedContainerOffset ==
            0x25005000ULL);
    REQUIRE(first.snapshot->layout.actorRecords.plainArrayOffset ==
            0x26006000ULL);
    REQUIRE(first.snapshot->layout.actorRecords.plainRootOffset == 0x284);
    REQUIRE(first.snapshot->layout.actorRecords.plainMeshOffset == 0x414);
    REQUIRE(first.snapshot->layout.actorRecords.encryptedRecordCount == 1536);
    REQUIRE(first.snapshot->layout.actorRecords.plainRecordStride == 40);
    REQUIRE(first.snapshot->layout.actorRecords.maximumPlainCount == 12288);
    REQUIRE(first.snapshot->layout.actorRecords.fallbackPlainCount == 3072);
    REQUIRE(first.snapshot->layout.actorSubject.rootOffset == 0x1a0);
    REQUIRE(first.snapshot->layout.actorSubject.meshOffset == 0x410);
    REQUIRE(first.snapshot->layout.actorSubject.alternateRootOffset == 0x420);
    REQUIRE(first.snapshot->layout.trackingMatrixRootOffset ==
            0x27007000ULL);
    REQUIRE(first.snapshot->layout.componentPositionFlagOffset ==
            0x28008003ULL);

    const auto& mode1 = first.snapshot->decrypt.mode1;
    REQUIRE(mode1.pool.rootRva == 0x29009000ULL);
    REQUIRE(mode1.pool.bridgeOffset == 0x24);
    REQUIRE(mode1.pool.contextOffset == -24);
    REQUIRE(mode1.pool.entryOffset == 0xc0);
    REQUIRE(mode1.pool.componentKeyOffset == 0x260);
    REQUIRE(mode1.pool.entryStride == 80);
    REQUIRE(mode1.pool.poolHeadSkip == 32);
    REQUIRE(mode1.pool.ringRefreshFrames == 111);
    REQUIRE(mode1.pacgaData == 0x123456789ULL);
    REQUIRE(mode1.pacgaModifier == 0x987654321ULL);
    const auto& mode2 = first.snapshot->decrypt.mode2;
    REQUIRE(mode2.pool.rootRva == 0x2a00a000ULL);
    REQUIRE(mode2.pool.contextOffset == -32);
    REQUIRE(mode2.pool.entryOffset == 0xd0);
    REQUIRE(mode2.pool.ringRefreshFrames == 121);

    const auto& execution = first.snapshot->decrypt.execution;
    REQUIRE(execution.discovery.rootOffset == 0x2b00b000ULL);
    REQUIRE(execution.discovery.pointerOffset == 0x14);
    REQUIRE(execution.discovery.entryOffset == 0xd8);
    REQUIRE(execution.discovery.returnStubMagic ==
            0x13572468abcdef01ULL);
    REQUIRE(execution.result.slotOffset == 0x280);
    REQUIRE(execution.result.positionOffset == 0x184);
    REQUIRE(execution.hookOffsets.subjectLoad == 0x400);
    REQUIRE(execution.hookOffsets.callbackCopyAfter == 0x414);
    REQUIRE(execution.hookOffsets.alternateGateWrite == 0x43c);
    REQUIRE(execution.hookOffsets.result == 0x464);
    REQUIRE(execution.fieldOffsets.contextExpected == 0x300);
    REQUIRE(execution.fieldOffsets.objectPosition == 0x31c);
    REQUIRE(execution.fieldOffsets.contextPoolTable == 0x338);
    REQUIRE(execution.context.threadName == "WorkerAlpha");
    REQUIRE(execution.context.oracleOpcode == 0x9ac33041U);

    const auto stable = store.Snapshot();
    const CloudLayoutUpdateResult unchanged =
        store.ValidateAndPublish(LayoutJson(17));
    REQUIRE(unchanged.status == CloudLayoutStatus::Unchanged);
    REQUIRE(unchanged.snapshot == stable);

    const auto requireConflict = [&](const std::string& needle,
                                     const std::string& replacement) {
        const auto result = store.ValidateAndPublish(
            ReplaceFirst(LayoutJson(17), needle, replacement));
        REQUIRE(result.status == CloudLayoutStatus::RevisionConflict);
        REQUIRE(result.snapshot == stable);
        REQUIRE(store.Snapshot() == stable);
    };
    requireConflict("\"world\":\"0x22002000\"",
                    "\"world\":\"0x22002008\"");
    requireConflict("\"pacga_modifier\":\"0x987654321\"",
                    "\"pacga_modifier\":\"0x987654322\"");
    requireConflict("\"result\":\"0x464\"",
                    "\"result\":\"0x468\"");
    requireConflict("\"context_pool_table\":\"0x338\"",
                    "\"context_pool_table\":\"0x33c\"");
    requireConflict(std::string("\"build_id\":\"") + kSyntheticBuildId,
                    "\"build_id\":\"0011223344556677");

    REQUIRE(store.ValidateAndPublish(LayoutJson(16)).status ==
            CloudLayoutStatus::RollbackRejected);
    REQUIRE(store.Snapshot() == stable);

    const auto requireSchemaMismatch = [&](const std::string& needle,
                                           const std::string& replacement) {
        REQUIRE(store.ValidateAndPublish(
                    ReplaceFirst(LayoutJson(18), needle, replacement))
                    .status == CloudLayoutStatus::SchemaMismatch);
        REQUIRE(store.Snapshot() == stable);
    };
    requireSchemaMismatch("\"revision\":18,",
                          "\"revision\":18,\"package\":\"x.y\",");
    requireSchemaMismatch("\"actor_subject\":{",
                          "\"actor_subject\":{\"unknown\":1,");
    requireSchemaMismatch("\"ring_refresh_frames\":111}",
                          "\"ring_refresh_frames\":111,\"unknown\":1}");
    requireSchemaMismatch("\"result\":\"0x464\"}",
                          "\"result\":\"0x464\",\"unknown\":1}");
    requireSchemaMismatch("\"context_pool_table\":\"0x338\"}",
                          "\"context_pool_table\":\"0x338\",\"unknown\":1}");
    requireSchemaMismatch("\"thread_name\":\"WorkerAlpha\"",
                          "\"thread_name\":7");
    requireSchemaMismatch("\"name_pool\":\"0x21001000\"",
                          "\"name_pool\":553652224");
    requireSchemaMismatch("\"schema_version\":3",
                          "\"schema_version\":2");

    const std::string duplicateRoot = ReplaceFirst(
        LayoutJson(18), "\"revision\":18,",
        "\"revision\":18,\"revision\":19,");
    REQUIRE(store.ValidateAndPublish(duplicateRoot).status ==
            CloudLayoutStatus::InvalidJson);
    const std::string duplicateNested = ReplaceFirst(
        LayoutJson(18), "\"subject_load\":\"0x400\",",
        "\"subject_load\":\"0x400\",\"subject_load\":\"0x404\",");
    REQUIRE(store.ValidateAndPublish(duplicateNested).status ==
            CloudLayoutStatus::InvalidJson);
    REQUIRE(store.Snapshot() == stable);

    const auto requireRangeError = [&](const std::string& needle,
                                       const std::string& replacement) {
        REQUIRE(store.ValidateAndPublish(
                    ReplaceFirst(LayoutJson(18), needle, replacement))
                    .status == CloudLayoutStatus::RangeError);
        REQUIRE(store.Snapshot() == stable);
    };
    requireRangeError("\"callback_entry\":\"0x404\"",
                      "\"callback_entry\":\"0x402\"");
    requireRangeError("\"context_offset\":-24",
                      "\"context_offset\":-12");
    requireRangeError("\"alternate_root\":\"0x420\"",
                      "\"alternate_root\":\"0x1a0\"");
    requireRangeError("\"oracle_opcode\":\"0x9ac33041\"",
                      "\"oracle_opcode\":\"0xd503201f\"");
    requireRangeError("\"thread_name\":\"WorkerAlpha\"",
                      "\"thread_name\":\"WorkerAlphaLongName\"");
    const std::string bothPacgaZero = ReplaceFirst(
        ReplaceFirst(LayoutJson(18),
                     "\"pacga_data\":\"0x123456789\"",
                     "\"pacga_data\":\"0x0\""),
        "\"pacga_modifier\":\"0x987654321\"",
        "\"pacga_modifier\":\"0x0\"");
    REQUIRE(store.ValidateAndPublish(bothPacgaZero).status ==
            CloudLayoutStatus::RangeError);
    const std::string malformedBuild = LayoutJson(18, "ABCDEF12");
    REQUIRE(store.ValidateAndPublish(malformedBuild).status ==
            CloudLayoutStatus::IdentityMismatch);
    REQUIRE(store.Snapshot() == stable);

    const CloudLayoutUpdateResult newer = store.ValidateAndPublish(
        LayoutJson(18, "00112233445566778899aabbccddeeff"));
    REQUIRE(newer.status == CloudLayoutStatus::Published);
    REQUIRE(newer.snapshot->revision == 18);
    REQUIRE(newer.snapshot->identity.buildId ==
            "00112233445566778899aabbccddeeff");

    CloudLayoutStore invalidTarget({"invalid", "bad/path.so"});
    REQUIRE(invalidTarget.ValidateAndPublish(LayoutJson(1)).status ==
            CloudLayoutStatus::IdentityMismatch);

    CloudLayoutStore concurrentStore(RuntimeTarget());
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
                     snapshot->identity.packageName != "com.example.runtime" ||
                     snapshot->identity.buildId != kSyntheticBuildId ||
                     snapshot->layout.worldOffset != 0x22002000ULL ||
                     snapshot->decrypt.mode1.pool.rootRva !=
                         0x29009000ULL ||
                     snapshot->decrypt.execution.hookOffsets.result !=
                         0x464)) {
                    invalidSnapshot.store(true, std::memory_order_release);
                    break;
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    bool publishFailed = false;
    for (std::uint64_t revision = 1; revision <= 48; ++revision) {
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
    REQUIRE(concurrentStore.Snapshot()->revision == 48);
}
