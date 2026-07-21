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
        0x16000000, 0x17000000, 0x188, 0x3e0,
        1024, 32, 8192, 2048};
    document.layout.trackingMatrixRootOffset = 0x18000000;
    document.layout.componentPositionFlagOffset = 0x19000000;
    document.layout.coordinatePool = {
        0x1a000000,
        0x14,
        -16,
        0xb0,
        0x220,
        0x13579bdf,
        0x2468ace0,
        64,
        24,
        90,
    };
    return document;
}

}  // namespace

void RunRuntimeLayoutOverrideTests() {
    using namespace lengjing::game::native;

    const lengjing::auth::CloudLayoutDocument document = ValidDocument();
    const auto applied = BuildRuntimeLayoutOverride(
        &document, "com.example.runtime", "libUE4.so",
        document.identity.buildId);
    REQUIRE(applied.has_value());
    REQUIRE(applied->namePoolOffset == 0x12000000);
    REQUIRE(applied->worldOffset == 0x13000000);
    REQUIRE(applied->coordinateReplayEntryOffset == 0x1234);
    REQUIRE(applied->geometryInstancePointerOffsets[1] == 0x15000000);
    REQUIRE(applied->actorRecords.taggedContainerOffset == 0x16000000);
    REQUIRE(applied->actorRecords.plainMeshOffset == 0x3e0);
    REQUIRE(applied->actorRecords.plainRecordStride == 32);
    REQUIRE(applied->trackingMatrixRootOffset == 0x18000000);
    REQUIRE(applied->componentPositionFlagOffset == 0x19000000);
    REQUIRE(applied->coordinatePool.rootRva == 0x1a000000);
    REQUIRE(applied->coordinatePool.contextOffset == -16);
    REQUIRE(applied->coordinatePool.componentKeyOffset == 0x220);
    REQUIRE(applied->coordinateTransport.rootRva == 0x1a000000);
    REQUIRE(applied->coordinateTransport.entryOffset == 0xb0);
    REQUIRE(applied->coordinateTransport.pacgaData == 0x13579bdf);
    REQUIRE(applied->coordinateTransport.pacgaModifier == 0x2468ace0);
    REQUIRE(applied->coordinatePool.rootRva ==
            applied->coordinateTransport.rootRva);
    REQUIRE(applied->coordinatePool.bridgeOffset ==
            applied->coordinateTransport.bridgeOffset);
    REQUIRE(applied->coordinatePool.entryOffset ==
            applied->coordinateTransport.entryOffset);

    lengjing::auth::CloudLayoutDocument dynamicPoolDocument = document;
    dynamicPoolDocument.layout.coordinateReplayEntryOffset = 0;
    const auto dynamicPool = BuildRuntimeLayoutOverride(
        &dynamicPoolDocument, "com.example.runtime", "libUE4.so",
        document.identity.buildId);
    REQUIRE(dynamicPool.has_value());
    REQUIRE(dynamicPool->coordinateReplayEntryOffset == 0);

    REQUIRE(!BuildRuntimeLayoutOverride(
        nullptr, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
    REQUIRE(!BuildRuntimeLayoutOverride(
        &document, "com.example.other", "libUE4.so",
        document.identity.buildId).has_value());
    REQUIRE(!BuildRuntimeLayoutOverride(
        &document, "com.example.runtime", "libOther.so",
        document.identity.buildId).has_value());
    REQUIRE(!BuildRuntimeLayoutOverride(
        &document, "com.example.runtime", "libUE4.so",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").has_value());

    lengjing::auth::CloudLayoutDocument invalid = document;
    invalid.layout.worldOffset = invalid.layout.namePoolOffset;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.coordinatePool.contextOffset = -4;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.coordinatePool.pacgaData = 0;
    invalid.layout.coordinatePool.pacgaModifier = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.coordinatePool.rootRva = invalid.layout.worldOffset;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.namePoolOffset = 0x100000000ULL;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.coordinateReplayEntryOffset = 0x100000000ULL;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.geometryInstancePointerOffsets[0] = 0x14000004;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorRecords.encryptedRecordCount = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorRecords.plainMeshOffset = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorRecords.plainRecordStride = 12;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorRecords.fallbackPlainCount = 10001;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.trackingMatrixRootOffset = 0x100000000ULL;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.componentPositionFlagOffset = 1;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
}
