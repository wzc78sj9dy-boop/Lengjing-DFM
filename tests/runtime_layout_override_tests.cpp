#include "test_support.h"

#include "game/native/RuntimeLayoutOverride.h"

namespace {

lengjing::auth::CloudLayoutDocument ValidDocument() {
    lengjing::auth::CloudLayoutDocument document{};
    document.schemaVersion = lengjing::auth::kCloudLayoutSchemaVersion;
    document.revision = 3;
    document.identity = {
        "com.example.runtime", "libUE4.so",
        "0123456789abcdef0123456789abcdef01234567"};
    document.layout.namePoolOffset = 0x12000000;
    document.layout.worldOffset = 0x13000000;
    document.layout.coordinateReplayEntryOffset = 0x1234;
    document.layout.geometryInstancePointerOffsets = {
        0x14000000, 0x15000000};
    document.layout.actorRecords = {
        0x16000000, 0x17000000, 0x180, 0x3d0,
        1000, 24, 10000, 3000};
    document.layout.trackingMatrixRootOffset = 0x18000000;
    document.layout.componentPositionFlagOffset = 0x19000000;
    return document;
}

}  // namespace

void RunRuntimeLayoutOverrideTests() {
    using namespace lengjing::game::native;

    const lengjing::auth::CloudLayoutDocument document = ValidDocument();
    const auto applied = BuildRuntimeLayoutOverride(
        &document, "com.example.runtime", "libUE4.so");
    REQUIRE(applied.has_value());
    REQUIRE(applied->namePoolOffset == 0x12000000);
    REQUIRE(applied->worldOffset == 0x13000000);
    REQUIRE(applied->coordinateReplayEntryOffset == 0x1234);
    REQUIRE(applied->geometryInstancePointerOffsets[1] == 0x15000000);
    REQUIRE(applied->actorRecords.taggedContainerOffset == 0x16000000);
    REQUIRE(applied->actorRecords.plainMeshOffset == 0x3d0);
    REQUIRE(applied->actorRecords.plainRecordStride == 24);
    REQUIRE(applied->trackingMatrixRootOffset == 0x18000000);
    REQUIRE(applied->componentPositionFlagOffset == 0x19000000);

    REQUIRE(!BuildRuntimeLayoutOverride(
        nullptr, "com.example.runtime", "libUE4.so").has_value());
    REQUIRE(!BuildRuntimeLayoutOverride(
        &document, "com.example.other", "libUE4.so").has_value());
    REQUIRE(!BuildRuntimeLayoutOverride(
        &document, "com.example.runtime", "libOther.so").has_value());

    lengjing::auth::CloudLayoutDocument invalid = document;
    invalid.layout.worldOffset = invalid.layout.namePoolOffset;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so").has_value());
}
