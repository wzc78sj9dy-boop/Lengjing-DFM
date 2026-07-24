#include "test_support.h"

#include "auth/CoordinatePoolCloudLayout.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kBuildId =
    "8187ddb9edbc9d5201201ffd7b008df3bfe533db";

std::string LayoutJson(
    std::uint64_t revision,
    std::string buildId = kBuildId) {
    std::ostringstream stream;
    stream
        << R"({"schema_version":1,"package":"com.example.runtime",)"
        << R"("module":"libUE4.so","build_id":")" << buildId
        << R"(","revision":)" << revision
        << R"(,"coordinate_pool":{"root_rva":"0x0e738950",)"
        << R"("bridge_offset":"0x0c","context_offset":-8,)"
        << R"("entry_offset":"0x00a0",)"
        << R"("component_key_offset":"0x0210","entry_stride":48,)"
        << R"("pool_head_skip":16,"ring_refresh_frames":60}})";
    return stream.str();
}

std::string ReplaceFirst(
    std::string value,
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

void RequireStatus(
    lengjing::auth::CoordinatePoolCloudLayoutStore& store,
    const std::string& payload,
    lengjing::auth::CloudLayoutStatus status,
    const std::shared_ptr<
        const lengjing::auth::CoordinatePoolCloudLayoutDocument>& stable) {
    const auto result = store.ValidateAndPublish(payload);
    REQUIRE(result.status == status);
    REQUIRE(result.snapshot == stable);
    REQUIRE(store.Snapshot() == stable);
}

}  // namespace

void RunCoordinatePoolCloudLayoutStandaloneTests() {
    using namespace lengjing::auth;

    CoordinatePoolCloudLayoutStore store(RuntimeIdentity());
    const auto first = store.ValidateAndPublish(LayoutJson(7));
    REQUIRE(first.status == CloudLayoutStatus::Published);
    REQUIRE(first.Succeeded());
    REQUIRE(first.snapshot != nullptr);
    REQUIRE(first.snapshot->schemaVersion ==
            kCoordinatePoolCloudLayoutSchemaVersion);
    REQUIRE(first.snapshot->revision == 7);
    REQUIRE(first.snapshot->identity.packageName ==
            "com.example.runtime");
    REQUIRE(first.snapshot->identity.moduleName == "libUE4.so");
    REQUIRE(first.snapshot->identity.buildId == kBuildId);
    const auto& layout = first.snapshot->coordinatePool;
    REQUIRE(layout.rootRva == 0x0e738950ULL);
    REQUIRE(layout.bridgeOffset == 0x0c);
    REQUIRE(layout.contextOffset == -8);
    REQUIRE(layout.entryOffset == 0x00a0);
    REQUIRE(layout.componentKeyOffset == 0x0210);
    REQUIRE(layout.entryStride == 48);
    REQUIRE(layout.poolHeadSkip == 16);
    REQUIRE(layout.ringRefreshFrames == 60);
    REQUIRE(layout.IsValid());

    const auto stable = store.Snapshot();
    const auto unchanged = store.ValidateAndPublish(LayoutJson(7));
    REQUIRE(unchanged.status == CloudLayoutStatus::Unchanged);
    REQUIRE(unchanged.Succeeded());
    REQUIRE(unchanged.snapshot == stable);

    const std::vector<std::pair<std::string, std::string>> conflicts{
        {"\"root_rva\":\"0x0e738950\"",
         "\"root_rva\":\"0x0e738954\""},
        {"\"bridge_offset\":\"0x0c\"",
         "\"bridge_offset\":\"0x10\""},
        {"\"context_offset\":-8", "\"context_offset\":-16"},
        {"\"entry_offset\":\"0x00a0\"",
         "\"entry_offset\":\"0x00a8\""},
        {"\"component_key_offset\":\"0x0210\"",
         "\"component_key_offset\":\"0x0218\""},
        {"\"entry_stride\":48", "\"entry_stride\":52"},
        {"\"pool_head_skip\":16", "\"pool_head_skip\":20"},
        {"\"ring_refresh_frames\":60", "\"ring_refresh_frames\":61"},
    };
    for (const auto& conflict : conflicts) {
        RequireStatus(
            store,
            ReplaceFirst(
                LayoutJson(7), conflict.first, conflict.second),
            CloudLayoutStatus::RevisionConflict,
            stable);
    }

    RequireStatus(
        store,
        LayoutJson(6),
        CloudLayoutStatus::RollbackRejected,
        stable);
    RequireStatus(
        store,
        LayoutJson(
            8, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        CloudLayoutStatus::IdentityMismatch,
        stable);

    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"schema_version\":1",
            "\"schema_version\":2"),
        CloudLayoutStatus::SchemaMismatch,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"revision\":8,",
            "\"revision\":8,\"unknown\":1,"),
        CloudLayoutStatus::SchemaMismatch,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"ring_refresh_frames\":60",
            "\"ring_refresh_frames\":60,\"unknown\":1"),
        CloudLayoutStatus::SchemaMismatch,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"pool_head_skip\":16,",
            ""),
        CloudLayoutStatus::SchemaMismatch,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"root_rva\":\"0x0e738950\"",
            "\"root_rva\":242452816"),
        CloudLayoutStatus::SchemaMismatch,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"root_rva\":\"0x0e738950\"",
            "\"root_rva\":\"0x0E738950\""),
        CloudLayoutStatus::SchemaMismatch,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"revision\":8,",
            "\"revision\":8,\"revision\":9,"),
        CloudLayoutStatus::InvalidJson,
        stable);
    RequireStatus(
        store,
        ReplaceFirst(
            LayoutJson(8),
            "\"entry_stride\":48,",
            "\"entry_stride\":48,\"entry_stride\":52,"),
        CloudLayoutStatus::InvalidJson,
        stable);
    RequireStatus(
        store,
        "{",
        CloudLayoutStatus::InvalidJson,
        stable);
    RequireStatus(
        store,
        "",
        CloudLayoutStatus::InvalidJson,
        stable);
    RequireStatus(
        store,
        std::string(
            kMaximumCoordinatePoolCloudLayoutPayloadBytes + 1, 'x'),
        CloudLayoutStatus::InvalidJson,
        stable);

    const std::vector<std::pair<std::string, std::string>> rangeErrors{
        {"\"revision\":8", "\"revision\":0"},
        {"\"root_rva\":\"0x0e738950\"",
         "\"root_rva\":\"0x0\""},
        {"\"root_rva\":\"0x0e738950\"",
         "\"root_rva\":\"0x0e738952\""},
        {"\"root_rva\":\"0x0e738950\"",
         "\"root_rva\":\"0x100000000\""},
        {"\"bridge_offset\":\"0x0c\"",
         "\"bridge_offset\":\"0x02\""},
        {"\"bridge_offset\":\"0x0c\"",
         "\"bridge_offset\":\"0x10004\""},
        {"\"context_offset\":-8", "\"context_offset\":0"},
        {"\"context_offset\":-8", "\"context_offset\":-4"},
        {"\"context_offset\":-8", "\"context_offset\":65544"},
        {"\"entry_offset\":\"0x00a0\"",
         "\"entry_offset\":\"0x04\""},
        {"\"entry_offset\":\"0x00a0\"",
         "\"entry_offset\":\"0x00a4\""},
        {"\"component_key_offset\":\"0x0210\"",
         "\"component_key_offset\":\"0x04\""},
        {"\"component_key_offset\":\"0x0210\"",
         "\"component_key_offset\":\"0x0214\""},
        {"\"component_key_offset\":\"0x0210\"",
         "\"component_key_offset\":\"0x10000\""},
        {"\"entry_stride\":48", "\"entry_stride\":8"},
        {"\"entry_stride\":48", "\"entry_stride\":49"},
        {"\"entry_stride\":48", "\"entry_stride\":4097"},
        {"\"pool_head_skip\":16", "\"pool_head_skip\":4085"},
        {"\"pool_head_skip\":16", "\"pool_head_skip\":40"},
        {"\"ring_refresh_frames\":60", "\"ring_refresh_frames\":0"},
        {"\"ring_refresh_frames\":60", "\"ring_refresh_frames\":3601"},
    };
    for (const auto& invalid : rangeErrors) {
        RequireStatus(
            store,
            ReplaceFirst(
                LayoutJson(8), invalid.first, invalid.second),
            CloudLayoutStatus::RangeError,
            stable);
    }
    RequireStatus(
        store,
        ReplaceFirst(
            ReplaceFirst(
                LayoutJson(8),
                "\"root_rva\":\"0x0e738950\"",
                "\"root_rva\":\"0xfffffffc\""),
            "\"bridge_offset\":\"0x0c\"",
            "\"bridge_offset\":\"0x08\""),
        CloudLayoutStatus::RangeError,
        stable);

    CoordinatePoolCloudLayoutStore invalidIdentityStore(
        {"runtime", "libUE4.so", kBuildId});
    const auto invalidIdentity =
        invalidIdentityStore.ValidateAndPublish(LayoutJson(1));
    REQUIRE(invalidIdentity.status ==
            CloudLayoutStatus::IdentityMismatch);
    REQUIRE(invalidIdentity.snapshot == nullptr);

    const auto newer = store.ValidateAndPublish(LayoutJson(8));
    REQUIRE(newer.status == CloudLayoutStatus::Published);
    REQUIRE(newer.snapshot != stable);
    REQUIRE(newer.snapshot->revision == 8);
    REQUIRE(store.Snapshot() == newer.snapshot);
}

#if defined(LENGJING_COORDINATE_POOL_CLOUD_LAYOUT_TEST_MAIN)
int main() {
    try {
        RunCoordinatePoolCloudLayoutStandaloneTests();
        std::cout << "coordinate pool cloud layout tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
#endif
