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
    document.layout.maximumActorCount = 12288;
    document.layout.actorSubject = {0x1a0, 0x410, 0x420};
    document.layout.trackingMatrixRootOffset = 0x27007000;
    document.decrypt.firstVeneerRva = 0xe7f5514;
    return document;
}

}  // namespace

void RunRuntimeLayoutOverrideTests() {
    using namespace lengjing::game::native;

    const auto document = ValidDocument();
    const auto applied = BuildRuntimeLayoutOverride(
        &document, "com.example.runtime", "libUE4.so",
        document.identity.buildId);
    REQUIRE(applied.has_value());
    REQUIRE(applied->namePoolOffset == 0x21001000);
    REQUIRE(applied->worldOffset == 0x22002000);
    REQUIRE(applied->maximumActorCount == 12288);
    REQUIRE(applied->actorSubject.rootOffset == 0x1a0);
    REQUIRE(applied->firstVeneerRva == 0xe7f5514);

    REQUIRE(!BuildRuntimeLayoutOverride(
        nullptr, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
    REQUIRE(!BuildRuntimeLayoutOverride(
        &document, "com.example.other", "libUE4.so",
        document.identity.buildId).has_value());

    auto invalid = document;
    invalid.decrypt.firstVeneerRva = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
    invalid = document;
    invalid.decrypt.firstVeneerRva = 0xe7f5512;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
    invalid = document;
    invalid.layout.worldOffset = invalid.layout.namePoolOffset;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
    invalid = document;
    invalid.layout.actorSubject.rootOffset = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
    invalid = document;
    invalid.layout.maximumActorCount = 0;
    REQUIRE(!BuildRuntimeLayoutOverride(
        &invalid, "com.example.runtime", "libUE4.so",
        document.identity.buildId).has_value());
}
