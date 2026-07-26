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
    document.layout.namePoolOffset = 0x21001000;
    document.layout.worldOffset = 0x22002000;
    document.layout.geometryInstancePointerOffsets = {
        0x23003000, 0x24004000};
    document.layout.actorRecords = {
        0x25005000, 0x26006000, 0x284, 0x414,
        1536, 40, 12288, 3072};
    document.layout.actorSubject = {0x1a0, 0x410, 0x420};
    document.layout.trackingMatrixRootOffset = 0x27007000;
    document.layout.componentPositionFlagOffset = 0x28008003;
    document.decrypt.mode1.pool = {
        0x29009000, 0x24, -24, 0xc0, 0x260, 80, 32, 111};
    document.decrypt.mode1.pacgaData = 0x123456789;
    document.decrypt.mode1.pacgaModifier = 0x987654321;
    document.decrypt.mode2.pool = {
        0x2a00a000, 0x28, -32, 0xd0, 0x270, 96, 36, 121};

    auto& execution = document.decrypt.execution;
    execution.discovery = {
        0x2b00b000, 0x14, 0xd8, 0x13572468abcdef01};
    execution.result = {0x280, 0x184};
    execution.hookOffsets = {
        0x400, 0x404, 0x408, 0x40c, 0x410, 0x414, 0x418,
        0x41c, 0x420, 0x424, 0x428, 0x42c, 0x430, 0x434,
        0x438, 0x43c, 0x440, 0x444, 0x448, 0x44c, 0x450,
        0x454, 0x458, 0x45c, 0x460, 0x464,
    };
    execution.fieldOffsets = {
        0x300, 0x304, 0x308, 0x30c, 0x310,
        0x314, 0x318, 0x31c, 0x320, 0x324,
        0x328, 0x32c, 0x330, 0x334, 0x338,
    };
    execution.context = {"WorkerAlpha", 0x9ac33041};
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
    REQUIRE(applied->namePoolOffset == 0x21001000);
    REQUIRE(applied->worldOffset == 0x22002000);
    REQUIRE(applied->geometryInstancePointerOffsets[1] == 0x24004000);
    REQUIRE(applied->actorRecords.taggedContainerOffset == 0x25005000);
    REQUIRE(applied->actorRecords.plainMeshOffset == 0x414);
    REQUIRE(applied->actorRecords.plainRecordStride == 40);
    REQUIRE(applied->actorSubject.rootOffset == 0x1a0);
    REQUIRE(applied->trackingMatrixRootOffset == 0x27007000);
    REQUIRE(applied->componentPositionFlagOffset == 0x28008003);
    REQUIRE(applied->coordinatePool.rootRva == 0x29009000);
    REQUIRE(applied->coordinatePool.contextOffset == -24);
    REQUIRE(applied->coordinatePool.componentKeyOffset == 0x260);
    REQUIRE(applied->coordinateDecrypt2Pool.rootRva == 0x2a00a000);
    REQUIRE(applied->coordinateTransport.rootRva == 0x29009000);
    REQUIRE(applied->coordinateTransport.entryOffset == 0xc0);
    REQUIRE(applied->coordinateTransport.pacgaData == 0x123456789);
    REQUIRE(applied->coordinateTransport.pacgaModifier == 0x987654321);
    REQUIRE(applied->coordinateExecution.discovery.rootOffset == 0x2b00b000);
    REQUIRE(applied->coordinateExecution.result.positionOffset == 0x184);
    REQUIRE(applied->coordinateExecution.hooks.callbackResultCode == 0x464);
    REQUIRE(applied->coordinateExecution.fields.poolTable == 0x338);
    REQUIRE(applied->coordinateExecutionContext.threadName == "WorkerAlpha");
    REQUIRE(applied->coordinateExecutionContext.oracleOpcode == 0x9ac33041);

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
    invalid.decrypt.mode1.pool.contextOffset = -4;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.decrypt.mode1.pacgaData = 0;
    invalid.decrypt.mode1.pacgaModifier = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.decrypt.mode2.pool.rootRva = invalid.layout.worldOffset;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.namePoolOffset = 0x100000000ULL;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.geometryInstancePointerOffsets[0] = 0x23003004;
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
    invalid.layout.actorRecords.fallbackPlainCount = 15000;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorSubject.rootOffset = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorSubject.meshOffset =
        invalid.layout.actorSubject.rootOffset;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.actorRecords = {};
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.trackingMatrixRootOffset = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.layout.componentPositionFlagOffset = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());

    invalid = document;
    invalid.decrypt.execution.context.oracleOpcode = 0xd503201f;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
}
