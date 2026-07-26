#include "auth/CloudLayout.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lengjing::auth {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kMaximumModuleOffset = 0xffffffffULL;
constexpr std::uint64_t kMaximumObjectOffset = 0xffffULL;
constexpr std::uint64_t kMaximumExecutionFieldOffset = 0xffffULL;

struct ParseFailure {
    CloudLayoutStatus status = CloudLayoutStatus::SchemaMismatch;
    std::string detail;
};

bool HasExactKeys(const Json& object,
                  std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) {
        return object.contains(key);
    });
}

bool IsAsciiAlphaNumeric(char character) noexcept {
    return std::isalnum(static_cast<unsigned char>(character)) != 0;
}

bool IsValidPackageName(std::string_view value) noexcept {
    if (value.size() < 3 || value.size() > 255 || value.front() == '.' ||
        value.back() == '.' || value.find('.') == std::string_view::npos ||
        value.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return IsAsciiAlphaNumeric(character) || character == '.' ||
            character == '_';
    });
}

bool IsValidModuleName(std::string_view value) noexcept {
    if (value.size() < 4 || value.size() > 128 ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos ||
        value.substr(value.size() - 3) != ".so") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return IsAsciiAlphaNumeric(character) || character == '.' ||
            character == '_' || character == '-' || character == '+';
    });
}

bool IsValidBuildId(std::string_view value) noexcept {
    if (value.size() < 8 || value.size() > 128 ||
        (value.size() & 1U) != 0U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

bool IsValidThreadName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 15) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte <= 0x7eU;
    });
}

bool ParseUnsigned(const Json& value,
                   std::uint64_t minimum,
                   std::uint64_t maximum,
                   std::uint64_t& output,
                   std::string_view field,
                   ParseFailure& failure) {
    if (!value.is_number_unsigned()) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) + " must be an unsigned integer";
        return false;
    }
    const auto parsed = value.get<std::uint64_t>();
    if (parsed < minimum || parsed > maximum) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(field) + " is outside the accepted range";
        return false;
    }
    output = parsed;
    return true;
}

bool ParseSigned(const Json& value,
                 std::int64_t minimum,
                 std::int64_t maximum,
                 std::int64_t& output,
                 std::string_view field,
                 ParseFailure& failure) {
    if (!value.is_number_integer()) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) + " must be an integer";
        return false;
    }
    std::int64_t parsed = 0;
    if (value.is_number_unsigned()) {
        const auto unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
            failure.status = CloudLayoutStatus::RangeError;
            failure.detail = std::string(field) +
                " is outside the accepted range";
            return false;
        }
        parsed = static_cast<std::int64_t>(unsignedValue);
    } else {
        parsed = value.get<std::int64_t>();
    }
    if (parsed < minimum || parsed > maximum) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(field) + " is outside the accepted range";
        return false;
    }
    output = parsed;
    return true;
}

bool ParseHexUnsigned(const Json& value,
                      std::uint64_t minimum,
                      std::uint64_t maximum,
                      std::uint64_t& output,
                      std::string_view field,
                      ParseFailure& failure) {
    if (!value.is_string()) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) +
            " must be a hexadecimal string";
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.size() < 3 || text.size() > 18 || text[0] != '0' ||
        text[1] != 'x') {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) +
            " has an invalid hexadecimal format";
        return false;
    }

    std::uint64_t parsed = 0;
    for (std::size_t index = 2; index < text.size(); ++index) {
        const char character = text[index];
        std::uint64_t digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint64_t>(character - 'a' + 10);
        } else {
            failure.status = CloudLayoutStatus::SchemaMismatch;
            failure.detail = std::string(field) +
                " must use lowercase hexadecimal";
            return false;
        }
        if (parsed >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 16U) {
            failure.status = CloudLayoutStatus::RangeError;
            failure.detail = std::string(field) + " overflows uint64";
            return false;
        }
        parsed = parsed * 16U + digit;
    }
    if (parsed < minimum || parsed > maximum) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(field) + " is outside the accepted range";
        return false;
    }
    output = parsed;
    return true;
}

bool ParseOffset(const Json& value,
                 std::uint64_t minimum,
                 std::uint64_t maximum,
                 std::uint64_t alignment,
                 bool allowZero,
                 std::uintptr_t& output,
                 std::string_view field,
                 ParseFailure& failure) {
    std::uint64_t parsed = 0;
    if (!ParseHexUnsigned(value, 0, maximum, parsed, field, failure)) {
        return false;
    }
    if (parsed == 0) {
        if (!allowZero) {
            failure.status = CloudLayoutStatus::RangeError;
            failure.detail = std::string(field) + " must not be zero";
            return false;
        }
        output = 0;
        return true;
    }
    if (parsed < minimum ||
        parsed > std::numeric_limits<std::uintptr_t>::max() ||
        (alignment != 0 && parsed % alignment != 0)) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(field) + " is outside the accepted range";
        return false;
    }
    output = static_cast<std::uintptr_t>(parsed);
    return true;
}

bool ParseActorRecords(const Json& object,
                       CloudActorRecordLayout& layout,
                       ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"tagged_container", "plain_array", "plain_root", "plain_mesh",
             "encrypted_record_count", "plain_record_stride",
             "maximum_plain_count", "fallback_plain_count"})) {
        failure.detail = "actor_records keys do not match schema version 3";
        return false;
    }
    if (!ParseOffset(object.at("tagged_container"), 4, kMaximumModuleOffset,
                     4, true, layout.taggedContainerOffset,
                     "layout.actor_records.tagged_container", failure) ||
        !ParseOffset(object.at("plain_array"), 4, kMaximumModuleOffset, 4,
                     true, layout.plainArrayOffset,
                     "layout.actor_records.plain_array", failure) ||
        !ParseOffset(object.at("plain_root"), 4, kMaximumObjectOffset, 4,
                     true, layout.plainRootOffset,
                     "layout.actor_records.plain_root", failure) ||
        !ParseOffset(object.at("plain_mesh"), 4, kMaximumObjectOffset, 4,
                     true, layout.plainMeshOffset,
                     "layout.actor_records.plain_mesh", failure)) {
        return false;
    }

    std::uint64_t encryptedCount = 0;
    std::uint64_t stride = 0;
    std::uint64_t maximumCount = 0;
    std::uint64_t fallbackCount = 0;
    if (!ParseUnsigned(object.at("encrypted_record_count"), 0, 65536,
                       encryptedCount,
                       "layout.actor_records.encrypted_record_count",
                       failure) ||
        !ParseUnsigned(object.at("plain_record_stride"), 0, 256, stride,
                       "layout.actor_records.plain_record_stride", failure) ||
        !ParseUnsigned(object.at("maximum_plain_count"), 0, 65536,
                       maximumCount,
                       "layout.actor_records.maximum_plain_count", failure) ||
        !ParseUnsigned(object.at("fallback_plain_count"), 0, 65536,
                       fallbackCount,
                       "layout.actor_records.fallback_plain_count", failure)) {
        return false;
    }

    const bool taggedEnabled = layout.taggedContainerOffset != 0;
    const bool plainEnabled = layout.plainArrayOffset != 0;
    const bool plainComplete = layout.plainRootOffset != 0 &&
        layout.plainMeshOffset != 0 && stride >= 8 && stride % 8 == 0 &&
        maximumCount != 0 && fallbackCount != 0 &&
        fallbackCount <= maximumCount;
    const bool plainEmpty = layout.plainRootOffset == 0 &&
        layout.plainMeshOffset == 0 && stride == 0 && maximumCount == 0 &&
        fallbackCount == 0;
    if (taggedEnabled != (encryptedCount != 0) ||
        (!plainEnabled && !plainEmpty) || (plainEnabled && !plainComplete) ||
        (!taggedEnabled && !plainEnabled)) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "actor_records fields are incomplete or inconsistent";
        return false;
    }

    layout.encryptedRecordCount = static_cast<std::uint32_t>(encryptedCount);
    layout.plainRecordStride = static_cast<std::uint32_t>(stride);
    layout.maximumPlainCount = static_cast<std::int32_t>(maximumCount);
    layout.fallbackPlainCount = static_cast<std::int32_t>(fallbackCount);
    return true;
}

bool ParseActorSubject(const Json& object,
                       CloudActorSubjectLayout& layout,
                       ParseFailure& failure) {
    if (!HasExactKeys(object, {"root", "mesh", "alternate_root"})) {
        failure.detail = "actor_subject keys do not match schema version 3";
        return false;
    }
    if (!ParseOffset(object.at("root"), 4, kMaximumObjectOffset, 4, false,
                     layout.rootOffset, "layout.actor_subject.root", failure) ||
        !ParseOffset(object.at("mesh"), 4, kMaximumObjectOffset, 4, false,
                     layout.meshOffset, "layout.actor_subject.mesh", failure) ||
        !ParseOffset(object.at("alternate_root"), 4,
                     kMaximumObjectOffset, 4, false,
                     layout.alternateRootOffset,
                     "layout.actor_subject.alternate_root", failure)) {
        return false;
    }
    if (layout.rootOffset == layout.meshOffset ||
        layout.rootOffset == layout.alternateRootOffset ||
        layout.meshOffset == layout.alternateRootOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "actor_subject offsets must be distinct";
        return false;
    }
    return true;
}

bool ParsePool(const Json& object,
               CloudCoordinatePoolLayout& layout,
               std::string_view prefix,
               ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"root_rva", "bridge_offset", "context_offset", "entry_offset",
             "component_key_offset", "entry_stride", "pool_head_skip",
             "ring_refresh_frames"})) {
        failure.detail = std::string(prefix) +
            " keys do not match schema version 3";
        return false;
    }
    const auto field = [&](std::string_view name) {
        return std::string(prefix) + "." + std::string(name);
    };
    std::int64_t contextOffset = 0;
    std::uint64_t stride = 0;
    std::uint64_t headSkip = 0;
    std::uint64_t refreshFrames = 0;
    if (!ParseOffset(object.at("root_rva"), 4, kMaximumModuleOffset, 4,
                     false, layout.rootRva, field("root_rva"), failure) ||
        !ParseOffset(object.at("bridge_offset"), 0,
                     kMaximumExecutionFieldOffset, 4, true,
                     layout.bridgeOffset, field("bridge_offset"), failure) ||
        !ParseSigned(object.at("context_offset"), -0x10000, 0x10000,
                     contextOffset, field("context_offset"), failure)) {
        return false;
    }
    if (contextOffset == 0 || contextOffset % 8 != 0) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = field("context_offset") +
            " must be nonzero and 8-byte aligned";
        return false;
    }
    if (!ParseOffset(object.at("entry_offset"), 8,
                     kMaximumExecutionFieldOffset, 8, false,
                     layout.entryOffset, field("entry_offset"), failure) ||
        !ParseOffset(object.at("component_key_offset"), 8,
                     kMaximumObjectOffset, 8, false,
                     layout.componentKeyOffset,
                     field("component_key_offset"), failure) ||
        !ParseUnsigned(object.at("entry_stride"), 12, 4096, stride,
                       field("entry_stride"), failure) ||
        !ParseUnsigned(object.at("pool_head_skip"), 0, 4084, headSkip,
                       field("pool_head_skip"), failure) ||
        !ParseUnsigned(object.at("ring_refresh_frames"), 1, 3600,
                       refreshFrames, field("ring_refresh_frames"), failure)) {
        return false;
    }
    if (stride % 4 != 0 || headSkip + 12 > stride ||
        layout.rootRva > kMaximumModuleOffset - layout.bridgeOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(prefix) + " fields are inconsistent";
        return false;
    }
    layout.contextOffset = static_cast<std::int32_t>(contextOffset);
    layout.entryStride = static_cast<std::uint32_t>(stride);
    layout.poolHeadSkip = static_cast<std::uint32_t>(headSkip);
    layout.ringRefreshFrames = static_cast<std::uint32_t>(refreshFrames);
    return true;
}

bool ParseLayout(const Json& object,
                 CloudOffsetLayout& layout,
                 ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"name_pool", "world", "geometry_instances", "actor_records",
             "actor_subject", "tracking_matrix_root",
             "component_position_flag"})) {
        failure.detail = "layout keys do not match schema version 3";
        return false;
    }
    if (!ParseOffset(object.at("name_pool"), 4, kMaximumModuleOffset, 4,
                     false, layout.namePoolOffset, "layout.name_pool",
                     failure) ||
        !ParseOffset(object.at("world"), 4, kMaximumModuleOffset, 4, false,
                     layout.worldOffset, "layout.world", failure)) {
        return false;
    }
    if (layout.namePoolOffset == layout.worldOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "name_pool and world must be distinct";
        return false;
    }

    const Json& geometry = object.at("geometry_instances");
    if (!geometry.is_array() ||
        geometry.size() != layout.geometryInstancePointerOffsets.size()) {
        failure.detail =
            "geometry_instances must contain exactly two offsets";
        return false;
    }
    for (std::size_t index = 0;
         index < layout.geometryInstancePointerOffsets.size(); ++index) {
        if (!ParseOffset(geometry.at(index), 8, kMaximumModuleOffset, 8,
                         false,
                         layout.geometryInstancePointerOffsets[index],
                         "layout.geometry_instances", failure)) {
            return false;
        }
    }
    if (layout.geometryInstancePointerOffsets[0] ==
        layout.geometryInstancePointerOffsets[1]) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "geometry_instances offsets must be distinct";
        return false;
    }

    return ParseActorRecords(object.at("actor_records"),
                             layout.actorRecords, failure) &&
        ParseActorSubject(object.at("actor_subject"),
                          layout.actorSubject, failure) &&
        ParseOffset(object.at("tracking_matrix_root"), 4,
                    kMaximumModuleOffset, 4, false,
                    layout.trackingMatrixRootOffset,
                    "layout.tracking_matrix_root", failure) &&
        ParseOffset(object.at("component_position_flag"), 1,
                    kMaximumModuleOffset, 1, false,
                    layout.componentPositionFlagOffset,
                    "layout.component_position_flag", failure);
}

bool ParseMode1(const Json& object,
                CloudDecryptMode1Layout& layout,
                ParseFailure& failure) {
    if (!HasExactKeys(object, {"pool", "pacga_data", "pacga_modifier"})) {
        failure.detail = "decrypt.mode1 keys do not match schema version 3";
        return false;
    }
    if (!ParsePool(object.at("pool"), layout.pool, "decrypt.mode1.pool",
                   failure) ||
        !ParseHexUnsigned(object.at("pacga_data"), 0,
                          std::numeric_limits<std::uint64_t>::max(),
                          layout.pacgaData, "decrypt.mode1.pacga_data",
                          failure) ||
        !ParseHexUnsigned(object.at("pacga_modifier"), 0,
                          std::numeric_limits<std::uint64_t>::max(),
                          layout.pacgaModifier,
                          "decrypt.mode1.pacga_modifier", failure)) {
        return false;
    }
    if (layout.pacgaData == 0 && layout.pacgaModifier == 0) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "decrypt.mode1 PACGA inputs must not both be zero";
        return false;
    }
    return true;
}

bool ParseMode2(const Json& object,
                CloudDecryptMode2Layout& layout,
                ParseFailure& failure) {
    if (!HasExactKeys(object, {"pool"})) {
        failure.detail = "decrypt.mode2 keys do not match schema version 3";
        return false;
    }
    return ParsePool(object.at("pool"), layout.pool, "decrypt.mode2.pool",
                     failure);
}

bool ParseDiscovery(const Json& object,
                    CloudExecutionDiscoveryLayout& layout,
                    ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"root_offset", "pointer_offset", "entry_offset",
             "return_stub_magic"})) {
        failure.detail =
            "decrypt.execution.discovery keys do not match schema version 3";
        return false;
    }
    return ParseOffset(object.at("root_offset"), 4, kMaximumModuleOffset, 4,
                       false, layout.rootOffset,
                       "decrypt.execution.discovery.root_offset", failure) &&
        ParseOffset(object.at("pointer_offset"), 4,
                    kMaximumExecutionFieldOffset, 4, false,
                    layout.pointerOffset,
                    "decrypt.execution.discovery.pointer_offset", failure) &&
        ParseOffset(object.at("entry_offset"), 8,
                    kMaximumExecutionFieldOffset, 8, false,
                    layout.entryOffset,
                    "decrypt.execution.discovery.entry_offset", failure) &&
        ParseHexUnsigned(object.at("return_stub_magic"), 1,
                         std::numeric_limits<std::uint64_t>::max(),
                         layout.returnStubMagic,
                         "decrypt.execution.discovery.return_stub_magic",
                         failure);
}

bool ParseResult(const Json& object,
                 CloudExecutionResultLayout& layout,
                 ParseFailure& failure) {
    if (!HasExactKeys(object, {"slot_offset", "position_offset"})) {
        failure.detail =
            "decrypt.execution.result keys do not match schema version 3";
        return false;
    }
    return ParseOffset(object.at("slot_offset"), 8,
                       kMaximumExecutionFieldOffset, 8, false,
                       layout.slotOffset,
                       "decrypt.execution.result.slot_offset", failure) &&
        ParseOffset(object.at("position_offset"), 4,
                    kMaximumExecutionFieldOffset, 4, false,
                    layout.positionOffset,
                    "decrypt.execution.result.position_offset", failure);
}

bool ParseHookOffsets(const Json& object,
                      CloudExecutionHookOffsetLayout& layout,
                      ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"subject_load", "callback_entry", "callback_return",
             "callback_index", "callback_copy_prepare",
             "callback_copy_after", "table_pointer", "table_value", "lock",
             "lock_return", "first_call", "first_return", "external_call",
             "external_return", "primary_gate_write",
             "alternate_gate_write", "gate_probe", "record_count",
             "target_key", "ring_setup", "ring_probe", "ring_hit",
             "dispatch", "dispatch_return", "result_prepare", "result"})) {
        failure.detail =
            "decrypt.execution.hook_offsets keys do not match schema version 3";
        return false;
    }

    struct Field {
        const char* key;
        std::uintptr_t CloudExecutionHookOffsetLayout::*member;
    };
    static constexpr Field fields[] = {
        {"subject_load", &CloudExecutionHookOffsetLayout::subjectLoad},
        {"callback_entry", &CloudExecutionHookOffsetLayout::callbackEntry},
        {"callback_return", &CloudExecutionHookOffsetLayout::callbackReturn},
        {"callback_index", &CloudExecutionHookOffsetLayout::callbackIndex},
        {"callback_copy_prepare",
         &CloudExecutionHookOffsetLayout::callbackCopyPrepare},
        {"callback_copy_after",
         &CloudExecutionHookOffsetLayout::callbackCopyAfter},
        {"table_pointer", &CloudExecutionHookOffsetLayout::tablePointer},
        {"table_value", &CloudExecutionHookOffsetLayout::tableValue},
        {"lock", &CloudExecutionHookOffsetLayout::lock},
        {"lock_return", &CloudExecutionHookOffsetLayout::lockReturn},
        {"first_call", &CloudExecutionHookOffsetLayout::firstCall},
        {"first_return", &CloudExecutionHookOffsetLayout::firstReturn},
        {"external_call", &CloudExecutionHookOffsetLayout::externalCall},
        {"external_return", &CloudExecutionHookOffsetLayout::externalReturn},
        {"primary_gate_write",
         &CloudExecutionHookOffsetLayout::primaryGateWrite},
        {"alternate_gate_write",
         &CloudExecutionHookOffsetLayout::alternateGateWrite},
        {"gate_probe", &CloudExecutionHookOffsetLayout::gateProbe},
        {"record_count", &CloudExecutionHookOffsetLayout::recordCount},
        {"target_key", &CloudExecutionHookOffsetLayout::targetKey},
        {"ring_setup", &CloudExecutionHookOffsetLayout::ringSetup},
        {"ring_probe", &CloudExecutionHookOffsetLayout::ringProbe},
        {"ring_hit", &CloudExecutionHookOffsetLayout::ringHit},
        {"dispatch", &CloudExecutionHookOffsetLayout::dispatch},
        {"dispatch_return", &CloudExecutionHookOffsetLayout::dispatchReturn},
        {"result_prepare", &CloudExecutionHookOffsetLayout::resultPrepare},
        {"result", &CloudExecutionHookOffsetLayout::result},
    };
    for (const Field& field : fields) {
        if (!ParseOffset(
                object.at(field.key), 4, kMaximumModuleOffset, 4, false,
                layout.*(field.member),
                std::string("decrypt.execution.hook_offsets.") + field.key,
                failure)) {
            return false;
        }
    }
    return true;
}

bool ParseFieldOffsets(const Json& object,
                       CloudExecutionFieldOffsetLayout& layout,
                       ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"context_expected", "stack_prior_gate",
             "stack_primary_gate_source", "stack_gate_flag",
             "stack_gate_snapshot_a", "stack_gate_snapshot_b",
             "stack_ring_mid", "object_position", "stack_capture_a",
             "stack_capture_b", "stack_capture_c", "stack_capture_d",
             "capture_field", "stack_pool_selector", "context_pool_table"})) {
        failure.detail =
            "decrypt.execution.field_offsets keys do not match schema version 3";
        return false;
    }

    struct Field {
        const char* key;
        std::uintptr_t CloudExecutionFieldOffsetLayout::*member;
    };
    static constexpr Field fields[] = {
        {"context_expected",
         &CloudExecutionFieldOffsetLayout::contextExpected},
        {"stack_prior_gate",
         &CloudExecutionFieldOffsetLayout::stackPriorGate},
        {"stack_primary_gate_source",
         &CloudExecutionFieldOffsetLayout::stackPrimaryGateSource},
        {"stack_gate_flag",
         &CloudExecutionFieldOffsetLayout::stackGateFlag},
        {"stack_gate_snapshot_a",
         &CloudExecutionFieldOffsetLayout::stackGateSnapshotA},
        {"stack_gate_snapshot_b",
         &CloudExecutionFieldOffsetLayout::stackGateSnapshotB},
        {"stack_ring_mid", &CloudExecutionFieldOffsetLayout::stackRingMid},
        {"object_position", &CloudExecutionFieldOffsetLayout::objectPosition},
        {"stack_capture_a", &CloudExecutionFieldOffsetLayout::stackCaptureA},
        {"stack_capture_b", &CloudExecutionFieldOffsetLayout::stackCaptureB},
        {"stack_capture_c", &CloudExecutionFieldOffsetLayout::stackCaptureC},
        {"stack_capture_d", &CloudExecutionFieldOffsetLayout::stackCaptureD},
        {"capture_field", &CloudExecutionFieldOffsetLayout::captureField},
        {"stack_pool_selector",
         &CloudExecutionFieldOffsetLayout::stackPoolSelector},
        {"context_pool_table",
         &CloudExecutionFieldOffsetLayout::contextPoolTable},
    };
    for (const Field& field : fields) {
        if (!ParseOffset(
                object.at(field.key), 4, kMaximumExecutionFieldOffset, 4,
                false, layout.*(field.member),
                std::string("decrypt.execution.field_offsets.") + field.key,
                failure)) {
            return false;
        }
    }
    return true;
}

bool ParseContext(const Json& object,
                  CloudExecutionContextLayout& layout,
                  ParseFailure& failure) {
    if (!HasExactKeys(object, {"thread_name", "oracle_opcode"})) {
        failure.detail =
            "decrypt.execution.context keys do not match schema version 3";
        return false;
    }
    if (!object.at("thread_name").is_string()) {
        failure.detail = "decrypt.execution.context.thread_name must be a string";
        return false;
    }
    layout.threadName = object.at("thread_name").get<std::string>();
    if (!IsValidThreadName(layout.threadName)) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail =
            "decrypt.execution.context.thread_name is outside the accepted range";
        return false;
    }
    std::uint64_t opcode = 0;
    if (!ParseHexUnsigned(object.at("oracle_opcode"), 1, UINT32_MAX, opcode,
                          "decrypt.execution.context.oracle_opcode",
                          failure)) {
        return false;
    }
    const auto encoded = static_cast<std::uint32_t>(opcode);
    const std::uint32_t destination = encoded & 0x1fU;
    const std::uint32_t data = (encoded >> 5U) & 0x1fU;
    const std::uint32_t modifier = (encoded >> 16U) & 0x1fU;
    if ((encoded & UINT32_C(0xFFE0FC00)) !=
            UINT32_C(0x9AC03000) ||
        destination == 31U || data == 31U || modifier == 31U) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail =
            "decrypt.execution.context.oracle_opcode is invalid";
        return false;
    }
    layout.oracleOpcode = encoded;
    return true;
}

bool ParseExecution(const Json& object,
                    CloudExecutionLayout& layout,
                    ParseFailure& failure) {
    if (!HasExactKeys(object,
                      {"discovery", "result", "hook_offsets",
                       "field_offsets", "context"})) {
        failure.detail =
            "decrypt.execution keys do not match schema version 3";
        return false;
    }
    return ParseDiscovery(object.at("discovery"), layout.discovery,
                          failure) &&
        ParseResult(object.at("result"), layout.result, failure) &&
        ParseHookOffsets(object.at("hook_offsets"), layout.hookOffsets,
                         failure) &&
        ParseFieldOffsets(object.at("field_offsets"), layout.fieldOffsets,
                          failure) &&
        ParseContext(object.at("context"), layout.context, failure);
}

bool ParseDecrypt(const Json& object,
                  CloudDecryptLayout& layout,
                  ParseFailure& failure) {
    if (!HasExactKeys(object, {"mode1", "mode2", "execution"})) {
        failure.detail = "decrypt keys do not match schema version 3";
        return false;
    }
    return ParseMode1(object.at("mode1"), layout.mode1, failure) &&
        ParseMode2(object.at("mode2"), layout.mode2, failure) &&
        ParseExecution(object.at("execution"), layout.execution, failure);
}

bool SameIdentity(const CloudRuntimeIdentity& left,
                  const CloudRuntimeIdentity& right) noexcept {
    return std::tie(left.packageName, left.moduleName, left.buildId) ==
        std::tie(right.packageName, right.moduleName, right.buildId);
}

bool SameActorRecords(const CloudActorRecordLayout& left,
                      const CloudActorRecordLayout& right) noexcept {
    return std::tie(
               left.taggedContainerOffset, left.plainArrayOffset,
               left.plainRootOffset, left.plainMeshOffset,
               left.encryptedRecordCount, left.plainRecordStride,
               left.maximumPlainCount, left.fallbackPlainCount) ==
        std::tie(
               right.taggedContainerOffset, right.plainArrayOffset,
               right.plainRootOffset, right.plainMeshOffset,
               right.encryptedRecordCount, right.plainRecordStride,
               right.maximumPlainCount, right.fallbackPlainCount);
}

bool SamePool(const CloudCoordinatePoolLayout& left,
              const CloudCoordinatePoolLayout& right) noexcept {
    return std::tie(
               left.rootRva, left.bridgeOffset, left.contextOffset,
               left.entryOffset, left.componentKeyOffset, left.entryStride,
               left.poolHeadSkip, left.ringRefreshFrames) ==
        std::tie(
               right.rootRva, right.bridgeOffset, right.contextOffset,
               right.entryOffset, right.componentKeyOffset, right.entryStride,
               right.poolHeadSkip, right.ringRefreshFrames);
}

bool SameHooks(const CloudExecutionHookOffsetLayout& left,
               const CloudExecutionHookOffsetLayout& right) noexcept {
    return std::tie(
               left.subjectLoad, left.callbackEntry, left.callbackReturn,
               left.callbackIndex, left.callbackCopyPrepare,
               left.callbackCopyAfter, left.tablePointer, left.tableValue,
               left.lock, left.lockReturn, left.firstCall, left.firstReturn,
               left.externalCall, left.externalReturn, left.primaryGateWrite,
               left.alternateGateWrite, left.gateProbe, left.recordCount,
               left.targetKey, left.ringSetup, left.ringProbe, left.ringHit,
               left.dispatch, left.dispatchReturn, left.resultPrepare,
               left.result) ==
        std::tie(
               right.subjectLoad, right.callbackEntry, right.callbackReturn,
               right.callbackIndex, right.callbackCopyPrepare,
               right.callbackCopyAfter, right.tablePointer, right.tableValue,
               right.lock, right.lockReturn, right.firstCall,
               right.firstReturn, right.externalCall, right.externalReturn,
               right.primaryGateWrite, right.alternateGateWrite,
               right.gateProbe, right.recordCount, right.targetKey,
               right.ringSetup, right.ringProbe, right.ringHit,
               right.dispatch, right.dispatchReturn, right.resultPrepare,
               right.result);
}

bool SameFields(const CloudExecutionFieldOffsetLayout& left,
                const CloudExecutionFieldOffsetLayout& right) noexcept {
    return std::tie(
               left.contextExpected, left.stackPriorGate,
               left.stackPrimaryGateSource, left.stackGateFlag,
               left.stackGateSnapshotA, left.stackGateSnapshotB,
               left.stackRingMid, left.objectPosition, left.stackCaptureA,
               left.stackCaptureB, left.stackCaptureC, left.stackCaptureD,
               left.captureField, left.stackPoolSelector,
               left.contextPoolTable) ==
        std::tie(
               right.contextExpected, right.stackPriorGate,
               right.stackPrimaryGateSource, right.stackGateFlag,
               right.stackGateSnapshotA, right.stackGateSnapshotB,
               right.stackRingMid, right.objectPosition,
               right.stackCaptureA, right.stackCaptureB, right.stackCaptureC,
               right.stackCaptureD, right.captureField,
               right.stackPoolSelector, right.contextPoolTable);
}

bool Equivalent(const CloudLayoutDocument& left,
                const CloudLayoutDocument& right) noexcept {
    const auto& le = left.decrypt.execution;
    const auto& re = right.decrypt.execution;
    return left.schemaVersion == right.schemaVersion &&
        left.revision == right.revision &&
        SameIdentity(left.identity, right.identity) &&
        std::tie(
            left.layout.namePoolOffset, left.layout.worldOffset,
            left.layout.geometryInstancePointerOffsets,
            left.layout.actorSubject.rootOffset,
            left.layout.actorSubject.meshOffset,
            left.layout.actorSubject.alternateRootOffset,
            left.layout.trackingMatrixRootOffset,
            left.layout.componentPositionFlagOffset) ==
        std::tie(
            right.layout.namePoolOffset, right.layout.worldOffset,
            right.layout.geometryInstancePointerOffsets,
            right.layout.actorSubject.rootOffset,
            right.layout.actorSubject.meshOffset,
            right.layout.actorSubject.alternateRootOffset,
            right.layout.trackingMatrixRootOffset,
            right.layout.componentPositionFlagOffset) &&
        SameActorRecords(left.layout.actorRecords,
                         right.layout.actorRecords) &&
        SamePool(left.decrypt.mode1.pool, right.decrypt.mode1.pool) &&
        left.decrypt.mode1.pacgaData == right.decrypt.mode1.pacgaData &&
        left.decrypt.mode1.pacgaModifier ==
            right.decrypt.mode1.pacgaModifier &&
        SamePool(left.decrypt.mode2.pool, right.decrypt.mode2.pool) &&
        std::tie(
            le.discovery.rootOffset, le.discovery.pointerOffset,
            le.discovery.entryOffset, le.discovery.returnStubMagic,
            le.result.slotOffset, le.result.positionOffset,
            le.context.threadName, le.context.oracleOpcode) ==
        std::tie(
            re.discovery.rootOffset, re.discovery.pointerOffset,
            re.discovery.entryOffset, re.discovery.returnStubMagic,
            re.result.slotOffset, re.result.positionOffset,
            re.context.threadName, re.context.oracleOpcode) &&
        SameHooks(le.hookOffsets, re.hookOffsets) &&
        SameFields(le.fieldOffsets, re.fieldOffsets);
}

CloudLayoutUpdateResult Failure(
    CloudLayoutStatus status,
    std::string detail,
    std::shared_ptr<const CloudLayoutDocument> snapshot) {
    return {status, std::move(detail), std::move(snapshot)};
}

}  // namespace

bool CloudRuntimeTarget::IsValid() const noexcept {
    return IsValidPackageName(packageName) && IsValidModuleName(moduleName);
}

bool CloudRuntimeIdentity::IsValid() const noexcept {
    return IsValidPackageName(packageName) &&
        IsValidModuleName(moduleName) && IsValidBuildId(buildId);
}

CloudLayoutStore::CloudLayoutStore(CloudRuntimeTarget target)
    : target_(std::move(target)) {}

const CloudRuntimeTarget& CloudLayoutStore::ExpectedTarget() const noexcept {
    return target_;
}

std::shared_ptr<const CloudLayoutDocument>
CloudLayoutStore::Snapshot() const noexcept {
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

CloudLayoutUpdateResult CloudLayoutStore::ValidateAndPublish(
    std::string_view payload) {
    const auto previous = Snapshot();
    if (!target_.IsValid()) {
        return Failure(CloudLayoutStatus::IdentityMismatch,
                       "runtime target is invalid", previous);
    }
    if (payload.empty() || payload.size() > kMaximumCloudLayoutPayloadBytes) {
        return Failure(CloudLayoutStatus::InvalidJson,
                       "layout payload is empty or too large", previous);
    }

    Json root;
    try {
        std::vector<std::unordered_set<std::string>> objectKeys;
        Json::parser_callback_t callback =
            [&objectKeys](int, Json::parse_event_t event, Json& parsed) {
                if (event == Json::parse_event_t::object_start) {
                    objectKeys.emplace_back();
                } else if (event == Json::parse_event_t::key) {
                    const std::string key = parsed.get<std::string>();
                    if (objectKeys.empty() ||
                        !objectKeys.back().insert(key).second) {
                        throw std::runtime_error("duplicate JSON key");
                    }
                } else if (event == Json::parse_event_t::object_end) {
                    if (objectKeys.empty()) {
                        throw std::runtime_error("unbalanced JSON object");
                    }
                    objectKeys.pop_back();
                }
                return true;
            };
        root = Json::parse(payload.begin(), payload.end(), callback, true,
                           false);
        if (!objectKeys.empty()) {
            throw std::runtime_error("unbalanced JSON object");
        }
    } catch (const std::exception& exception) {
        return Failure(CloudLayoutStatus::InvalidJson, exception.what(),
                       previous);
    }

    if (!HasExactKeys(root,
                      {"schema_version", "build_id", "revision", "layout",
                       "decrypt"})) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "root keys do not match schema version 3", previous);
    }

    ParseFailure failure;
    std::uint64_t schemaVersion = 0;
    std::uint64_t revision = 0;
    if (!ParseUnsigned(root.at("schema_version"), 0, UINT32_MAX,
                       schemaVersion, "schema_version", failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }
    if (schemaVersion != kCloudLayoutSchemaVersion) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "unsupported cloud layout schema version", previous);
    }
    if (!root.at("build_id").is_string()) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "build_id must be a string", previous);
    }
    const std::string buildId = root.at("build_id").get<std::string>();
    if (!IsValidBuildId(buildId)) {
        return Failure(CloudLayoutStatus::IdentityMismatch,
                       "build_id is malformed", previous);
    }
    if (!ParseUnsigned(root.at("revision"), 1,
                       std::numeric_limits<std::uint64_t>::max(), revision,
                       "revision", failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }

    auto candidate = std::make_shared<CloudLayoutDocument>();
    candidate->schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    candidate->revision = revision;
    candidate->identity = {target_.packageName, target_.moduleName, buildId};
    if (!ParseLayout(root.at("layout"), candidate->layout, failure) ||
        !ParseDecrypt(root.at("decrypt"), candidate->decrypt, failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }

    std::lock_guard<std::mutex> lock(publishMutex_);
    const auto current = Snapshot();
    if (current != nullptr) {
        if (candidate->revision < current->revision) {
            return Failure(CloudLayoutStatus::RollbackRejected,
                           "layout revision rollback rejected", current);
        }
        if (candidate->revision == current->revision) {
            if (Equivalent(*candidate, *current)) {
                return {CloudLayoutStatus::Unchanged, {}, current};
            }
            return Failure(CloudLayoutStatus::RevisionConflict,
                           "same revision contains different layout data",
                           current);
        }
    }

    std::shared_ptr<const CloudLayoutDocument> published =
        std::move(candidate);
    std::atomic_store_explicit(&current_, published,
                               std::memory_order_release);
    return {CloudLayoutStatus::Published, {}, std::move(published)};
}

}  // namespace lengjing::auth
