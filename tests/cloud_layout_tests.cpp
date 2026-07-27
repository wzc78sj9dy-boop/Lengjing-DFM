#include "test_support.h"

#include "auth/CloudLayout.h"
#include "vendor/json.hpp"

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
        << R"({"v":4,"b":")" << buildId
        << R"(","r":)" << revision
        << R"(,"d":[["0x21001000",")"
        << worldOffset
        << R"(",["0x23003000","0x24004000"],)"
        << R"(["0x25005000","0x26006000","0x284","0x414",)"
        << R"(1536,40,12288,3072],)"
        << R"(["0x1a0","0x410","0x420"],)"
        << R"("0x27007000","0x28008003"],)"
        << R"([[["0x29009000","0x24",-24,"0xc0","0x260",)"
        << R"(80,32,111],"0x123456789","0x987654321"],)"
        << R"([["0x2a00a000","0x28",-32,"0xd0","0x270",)"
        << R"(96,36,121]],)"
        << R"([["0x2b00b000","0x14","0xd8",)"
        << R"("0x13572468abcdef01"],["0x280","0x184"],)"
        << R"(["0x400","0x404","0x408","0x40c","0x410","0x414",)"
        << R"("0x418","0x41c","0x420","0x424","0x428","0x42c",)"
        << R"("0x430","0x434","0x438","0x43c","0x440","0x444",)"
        << R"("0x448","0x44c","0x450","0x454","0x458","0x45c",)"
        << R"("0x460","0x464"],)"
        << R"(["0x300","0x304","0x308","0x30c","0x310","0x314",)"
        << R"("0x318","0x31c","0x320","0x324","0x328","0x32c",)"
        << R"("0x330","0x334","0x338"],)"
        << R"(["WorkerAlpha","0x9ac33041"]]]]})";
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

std::string DualProfileLayoutJson(std::uint64_t revision) {
    using Json = nlohmann::json;
    Json root = Json::parse(LayoutJson(revision));
    Json primary = root.at("d").at(1).at(2).at(0);
    Json profile12 = primary;
    profile12.at(0) = "0x2c00c000";
    profile12.at(1) = "0x18";
    profile12.at(2) = "0xe0";
    profile12.at(3) = "0x24681357abcdef01";
    root.at("d").at(1).at(2).at(0) =
        Json::array({primary, profile12});
    return root.dump();
}

std::string CoreOnlyExecutionLayoutJson(std::uint64_t revision) {
    using Json = nlohmann::json;
    Json root = Json::parse(LayoutJson(revision));
    Json hooks = Json::array();
    Json fields = Json::array();
    for (std::size_t index = 0; index < 26; ++index) {
        hooks.push_back("0x0");
    }
    for (std::size_t index = 0; index < 15; ++index) {
        fields.push_back("0x0");
    }
    root.at("d").at(1).at(2).at(2) = std::move(hooks);
    root.at("d").at(1).at(2).at(3) = std::move(fields);
    return root.dump();
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
    REQUIRE(empty.decrypt.execution.profile12Discovery.rootOffset == 0);
    REQUIRE(!empty.decrypt.execution.hasProfile12Discovery);
    REQUIRE(empty.decrypt.execution.result.slotOffset == 0);
    REQUIRE(empty.decrypt.execution.hookOffsets.subjectLoad == 0);
    REQUIRE(empty.decrypt.execution.fieldOffsets.contextExpected == 0);
    REQUIRE(empty.decrypt.execution.context.threadName.empty());
    REQUIRE(empty.decrypt.execution.context.oracleOpcode == 0);

    std::string boundaryPayload = LayoutJson(1);
    REQUIRE(boundaryPayload.size() < kMaximumCloudLayoutPayloadBytes);
    boundaryPayload.append(
        kMaximumCloudLayoutPayloadBytes - boundaryPayload.size(), ' ');
    CloudLayoutStore boundaryStore(RuntimeTarget());
    REQUIRE(boundaryStore.ValidateAndPublish(boundaryPayload).status ==
            CloudLayoutStatus::Published);
    boundaryPayload.push_back(' ');
    REQUIRE(boundaryStore.ValidateAndPublish(boundaryPayload).status ==
            CloudLayoutStatus::InvalidJson);

    CloudLayoutStore store(RuntimeTarget());
    REQUIRE(store.ExpectedTarget().packageName == "com.example.runtime");
    REQUIRE(store.ExpectedTarget().moduleName == "libSynthetic.so");

    const CloudLayoutUpdateResult first =
        store.ValidateAndPublish(LayoutJson(17));
    REQUIRE(first.status == CloudLayoutStatus::Published);
    REQUIRE(first.snapshot != nullptr);
    REQUIRE(first.snapshot->schemaVersion == 4);
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
    REQUIRE(mode2.pool.bridgeOffset == 0x28);
    REQUIRE(mode2.pool.contextOffset == -32);
    REQUIRE(mode2.pool.entryOffset == 0xd0);
    REQUIRE(mode2.pool.componentKeyOffset == 0x270);
    REQUIRE(mode2.pool.entryStride == 96);
    REQUIRE(mode2.pool.poolHeadSkip == 36);
    REQUIRE(mode2.pool.ringRefreshFrames == 121);

    const auto& execution = first.snapshot->decrypt.execution;
    REQUIRE(execution.discovery.rootOffset == 0x2b00b000ULL);
    REQUIRE(execution.discovery.pointerOffset == 0x14);
    REQUIRE(execution.discovery.entryOffset == 0xd8);
    REQUIRE(execution.discovery.returnStubMagic ==
            0x13572468abcdef01ULL);
    REQUIRE(!execution.hasProfile12Discovery);
    REQUIRE(execution.result.slotOffset == 0x280);
    REQUIRE(execution.result.positionOffset == 0x184);
    using HookMember =
        std::uintptr_t CloudExecutionHookOffsetLayout::*;
    static constexpr HookMember hookMembers[] = {
        &CloudExecutionHookOffsetLayout::subjectLoad,
        &CloudExecutionHookOffsetLayout::callbackEntry,
        &CloudExecutionHookOffsetLayout::callbackReturn,
        &CloudExecutionHookOffsetLayout::callbackIndex,
        &CloudExecutionHookOffsetLayout::callbackCopyPrepare,
        &CloudExecutionHookOffsetLayout::callbackCopyAfter,
        &CloudExecutionHookOffsetLayout::tablePointer,
        &CloudExecutionHookOffsetLayout::tableValue,
        &CloudExecutionHookOffsetLayout::lock,
        &CloudExecutionHookOffsetLayout::lockReturn,
        &CloudExecutionHookOffsetLayout::firstCall,
        &CloudExecutionHookOffsetLayout::firstReturn,
        &CloudExecutionHookOffsetLayout::externalCall,
        &CloudExecutionHookOffsetLayout::externalReturn,
        &CloudExecutionHookOffsetLayout::primaryGateWrite,
        &CloudExecutionHookOffsetLayout::alternateGateWrite,
        &CloudExecutionHookOffsetLayout::gateProbe,
        &CloudExecutionHookOffsetLayout::recordCount,
        &CloudExecutionHookOffsetLayout::targetKey,
        &CloudExecutionHookOffsetLayout::ringSetup,
        &CloudExecutionHookOffsetLayout::ringProbe,
        &CloudExecutionHookOffsetLayout::ringHit,
        &CloudExecutionHookOffsetLayout::dispatch,
        &CloudExecutionHookOffsetLayout::dispatchReturn,
        &CloudExecutionHookOffsetLayout::resultPrepare,
        &CloudExecutionHookOffsetLayout::result,
    };
    for (std::size_t index = 0;
         index < sizeof(hookMembers) / sizeof(hookMembers[0]); ++index) {
        REQUIRE(execution.hookOffsets.*hookMembers[index] ==
                0x400U + index * 4U);
    }
    using FieldMember =
        std::uintptr_t CloudExecutionFieldOffsetLayout::*;
    static constexpr FieldMember fieldMembers[] = {
        &CloudExecutionFieldOffsetLayout::contextExpected,
        &CloudExecutionFieldOffsetLayout::stackPriorGate,
        &CloudExecutionFieldOffsetLayout::stackPrimaryGateSource,
        &CloudExecutionFieldOffsetLayout::stackGateFlag,
        &CloudExecutionFieldOffsetLayout::stackGateSnapshotA,
        &CloudExecutionFieldOffsetLayout::stackGateSnapshotB,
        &CloudExecutionFieldOffsetLayout::stackRingMid,
        &CloudExecutionFieldOffsetLayout::objectPosition,
        &CloudExecutionFieldOffsetLayout::stackCaptureA,
        &CloudExecutionFieldOffsetLayout::stackCaptureB,
        &CloudExecutionFieldOffsetLayout::stackCaptureC,
        &CloudExecutionFieldOffsetLayout::stackCaptureD,
        &CloudExecutionFieldOffsetLayout::captureField,
        &CloudExecutionFieldOffsetLayout::stackPoolSelector,
        &CloudExecutionFieldOffsetLayout::contextPoolTable,
    };
    for (std::size_t index = 0;
         index < sizeof(fieldMembers) / sizeof(fieldMembers[0]); ++index) {
        REQUIRE(execution.fieldOffsets.*fieldMembers[index] ==
                0x300U + index * 4U);
    }
    REQUIRE(execution.context.threadName == "WorkerAlpha");
    REQUIRE(execution.context.oracleOpcode == 0x9ac33041U);

    CloudLayoutStore dualProfileStore(RuntimeTarget());
    const CloudLayoutUpdateResult dualProfile =
        dualProfileStore.ValidateAndPublish(DualProfileLayoutJson(18));
    REQUIRE(dualProfile.status == CloudLayoutStatus::Published);
    REQUIRE(dualProfile.snapshot != nullptr);
    REQUIRE(dualProfile.snapshot->decrypt.execution.hasProfile12Discovery);
    REQUIRE(dualProfile.snapshot->decrypt.execution.discovery.rootOffset ==
            0x2b00b000ULL);
    REQUIRE(dualProfile.snapshot->decrypt.execution.profile12Discovery
                .rootOffset == 0x2c00c000ULL);
    REQUIRE(dualProfile.snapshot->decrypt.execution.profile12Discovery
                .pointerOffset == 0x18);

    CloudLayoutStore coreOnlyStore(RuntimeTarget());
    const CloudLayoutUpdateResult coreOnly =
        coreOnlyStore.ValidateAndPublish(CoreOnlyExecutionLayoutJson(19));
    REQUIRE(coreOnly.status == CloudLayoutStatus::Published);
    REQUIRE(coreOnly.snapshot != nullptr);
    REQUIRE(coreOnly.snapshot->decrypt.execution.hookOffsets.subjectLoad == 0);
    REQUIRE(coreOnly.snapshot->decrypt.execution.fieldOffsets.contextExpected ==
            0);

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
    requireConflict(
        "\"0x22002000\",[\"0x23003000\"",
        "\"0x22002008\",[\"0x23003000\"");
    requireConflict("\"0x987654321\"]",
                    "\"0x987654322\"]");
    requireConflict("\"0x460\",\"0x464\"]",
                    "\"0x460\",\"0x468\"]");
    requireConflict("\"0x334\",\"0x338\"]",
                    "\"0x334\",\"0x33c\"]");
    requireConflict(std::string("\"b\":\"") + kSyntheticBuildId,
                    "\"b\":\"0011223344556677");

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
    requireSchemaMismatch("\"r\":18,",
                          "\"r\":18,\"x\":1,");
    requireSchemaMismatch(
        "[\"0x1a0\",\"0x410\",\"0x420\"]",
        "[\"0x1a0\",\"0x410\",\"0x420\",\"0x424\"]");
    requireSchemaMismatch("80,32,111]",
                          "80,32,111,112]");
    requireSchemaMismatch("\"0x460\",\"0x464\"]",
                          "\"0x460\",\"0x464\",\"0x468\"]");
    requireSchemaMismatch("\"0x334\",\"0x338\"]",
                          "\"0x334\",\"0x338\",\"0x33c\"]");
    requireSchemaMismatch("\"WorkerAlpha\"",
                          "7");
    requireSchemaMismatch("[\"0x21001000\"",
                          "[553652224");
    requireSchemaMismatch("\"v\":4",
                          "\"v\":3");

    const std::string duplicateRoot = ReplaceFirst(
        LayoutJson(18), "\"r\":18,",
        "\"r\":18,\"r\":19,");
    REQUIRE(store.ValidateAndPublish(duplicateRoot).status ==
            CloudLayoutStatus::InvalidJson);
    const std::string duplicateNested = ReplaceFirst(
        LayoutJson(18), "\"d\":[",
        "\"d\":[],\"d\":[");
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
    requireRangeError("\"0x400\",\"0x404\",\"0x408\"",
                      "\"0x400\",\"0x402\",\"0x408\"");
    requireRangeError("\"0x24\",-24,\"0xc0\"",
                      "\"0x24\",-12,\"0xc0\"");
    requireRangeError("[\"0x1a0\",\"0x410\",\"0x420\"]",
                      "[\"0x1a0\",\"0x410\",\"0x1a0\"]");
    requireRangeError("\"0x9ac33041\"",
                      "\"0xd503201f\"");
    requireRangeError("\"WorkerAlpha\"",
                      "\"WorkerAlphaLongName\"");
    const std::string bothPacgaZero = ReplaceFirst(
        ReplaceFirst(LayoutJson(18),
                     "\"0x123456789\"",
                     "\"0x0\""),
        "\"0x987654321\"",
        "\"0x0\"");
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
