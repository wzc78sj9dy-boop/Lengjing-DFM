#include "auth/CloudLayout.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lengjing::auth {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumPayloadBytes = 64U * 1024U;
constexpr std::uint64_t kMaximumModuleOffset = 0xffffffffULL;
constexpr std::uint64_t kMaximumObjectOffset = 0xffffULL;

struct ParseFailure {
    CloudLayoutStatus status = CloudLayoutStatus::SchemaMismatch;
    std::string detail;
};

bool HasExactKeys(const Json& object,
                  std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.size() != keys.size()) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) {
        return object.contains(key);
    });
}

bool IsAsciiAlphaNumeric(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0;
}

bool IsValidPackageName(std::string_view value) noexcept {
    if (value.size() < 3 || value.size() > 255 ||
        value.front() == '.' || value.back() == '.' ||
        value.find('.') == std::string_view::npos ||
        value.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return IsAsciiAlphaNumeric(character) ||
            character == '.' || character == '_';
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

bool SameIdentity(const CloudRuntimeIdentity& left,
                  const CloudRuntimeIdentity& right) noexcept {
    return left.packageName == right.packageName &&
        left.moduleName == right.moduleName &&
        left.buildId == right.buildId;
}

bool ParseIdentity(const Json& root,
                   CloudRuntimeIdentity& identity,
                   ParseFailure& failure) {
    for (const char* key : {"package", "module", "build_id"}) {
        if (!root.at(key).is_string()) {
            failure.detail = std::string(key) + " must be a string";
            return false;
        }
    }
    identity.packageName = root.at("package").get<std::string>();
    identity.moduleName = root.at("module").get<std::string>();
    identity.buildId = root.at("build_id").get<std::string>();
    if (!identity.IsValid()) {
        failure.detail = "package, module, or build_id is malformed";
        return false;
    }
    return true;
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

bool ParseOffset(const Json& value,
                 std::uint64_t minimum,
                 std::uint64_t maximum,
                 std::uint64_t alignment,
                 bool allowZero,
                 std::uintptr_t& output,
                 std::string_view field,
                 ParseFailure& failure) {
    if (!value.is_string()) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) + " must be a hexadecimal string";
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.size() < 3 || text.size() > 18 || text[0] != '0' ||
        text[1] != 'x') {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) + " has an invalid hexadecimal format";
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
            failure.detail = std::string(field) + " must use lowercase hexadecimal";
            return false;
        }
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 16U) {
            failure.status = CloudLayoutStatus::RangeError;
            failure.detail = std::string(field) + " overflows uint64";
            return false;
        }
        parsed = parsed * 16U + digit;
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
    if (parsed < minimum || parsed > maximum ||
        parsed > std::numeric_limits<std::uintptr_t>::max() ||
        (alignment != 0 && parsed % alignment != 0)) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(field) + " is outside the accepted range";
        return false;
    }
    output = static_cast<std::uintptr_t>(parsed);
    return true;
}

bool ParseActorLayout(const Json& object,
                      CloudActorRecordLayout& layout,
                      ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"tagged_container", "plain_array", "plain_root", "plain_mesh",
             "encrypted_record_count", "plain_record_stride",
             "maximum_plain_count", "fallback_plain_count"})) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = "actor_records keys do not match schema version 1";
        return false;
    }

    if (!ParseOffset(object.at("tagged_container"), 4, kMaximumModuleOffset,
                     4, true, layout.taggedContainerOffset,
                     "actor_records.tagged_container", failure) ||
        !ParseOffset(object.at("plain_array"), 4, kMaximumModuleOffset,
                     4, true, layout.plainArrayOffset,
                     "actor_records.plain_array", failure) ||
        !ParseOffset(object.at("plain_root"), 4, kMaximumObjectOffset,
                     4, true, layout.plainRootOffset,
                     "actor_records.plain_root", failure) ||
        !ParseOffset(object.at("plain_mesh"), 4, kMaximumObjectOffset,
                     4, true, layout.plainMeshOffset,
                     "actor_records.plain_mesh", failure)) {
        return false;
    }

    std::uint64_t encryptedRecordCount = 0;
    std::uint64_t plainRecordStride = 0;
    std::uint64_t maximumPlainCount = 0;
    std::uint64_t fallbackPlainCount = 0;
    if (!ParseUnsigned(object.at("encrypted_record_count"), 0, 65536,
                       encryptedRecordCount,
                       "actor_records.encrypted_record_count", failure) ||
        !ParseUnsigned(object.at("plain_record_stride"), 0, 256,
                       plainRecordStride,
                       "actor_records.plain_record_stride", failure) ||
        !ParseUnsigned(object.at("maximum_plain_count"), 0, 65536,
                       maximumPlainCount,
                       "actor_records.maximum_plain_count", failure) ||
        !ParseUnsigned(object.at("fallback_plain_count"), 0, 65536,
                       fallbackPlainCount,
                       "actor_records.fallback_plain_count", failure)) {
        return false;
    }

    const bool taggedEnabled = layout.taggedContainerOffset != 0;
    if (taggedEnabled != (encryptedRecordCount != 0)) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "tagged actor fields must be enabled or disabled together";
        return false;
    }

    const bool plainEnabled = layout.plainArrayOffset != 0;
    const bool plainFieldsPresent = layout.plainRootOffset != 0 &&
        layout.plainMeshOffset != 0 && plainRecordStride != 0 &&
        maximumPlainCount != 0 && fallbackPlainCount != 0;
    const bool plainFieldsEmpty = layout.plainRootOffset == 0 &&
        layout.plainMeshOffset == 0 && plainRecordStride == 0 &&
        maximumPlainCount == 0 && fallbackPlainCount == 0;
    if ((!plainEnabled && !plainFieldsEmpty) ||
        (plainEnabled && !plainFieldsPresent) ||
        (plainEnabled && (plainRecordStride < 8 ||
                          plainRecordStride % 8 != 0 ||
                          fallbackPlainCount > maximumPlainCount))) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "plain actor fields are incomplete or inconsistent";
        return false;
    }

    layout.encryptedRecordCount =
        static_cast<std::uint32_t>(encryptedRecordCount);
    layout.plainRecordStride = static_cast<std::uint32_t>(plainRecordStride);
    layout.maximumPlainCount = static_cast<std::int32_t>(maximumPlainCount);
    layout.fallbackPlainCount = static_cast<std::int32_t>(fallbackPlainCount);
    return true;
}

bool ParseLayout(const Json& object,
                 CloudOffsetLayout& layout,
                 ParseFailure& failure) {
    const bool legacyKeys = HasExactKeys(
        object,
        {"name_pool", "world", "geometry_instances", "actor_records",
         "tracking_matrix_root", "component_position_flag"});
    const bool replayKeys = HasExactKeys(
        object,
        {"name_pool", "world", "coordinate_replay_entry",
         "geometry_instances", "actor_records", "tracking_matrix_root",
         "component_position_flag"});
    if (!legacyKeys && !replayKeys) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = "layout keys do not match schema version 1";
        return false;
    }
    if (!ParseOffset(object.at("name_pool"), 4, kMaximumModuleOffset, 4,
                     false, layout.namePoolOffset, "layout.name_pool",
                     failure) ||
        !ParseOffset(object.at("world"), 4, kMaximumModuleOffset, 4,
                     false, layout.worldOffset, "layout.world", failure)) {
        return false;
    }
    if (layout.namePoolOffset == layout.worldOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "name_pool and world must be distinct";
        return false;
    }
    if (replayKeys &&
        !ParseOffset(object.at("coordinate_replay_entry"), 4,
                     kMaximumModuleOffset, 4, true,
                     layout.coordinateReplayEntryOffset,
                     "layout.coordinate_replay_entry", failure)) {
        return false;
    }

    const Json& geometry = object.at("geometry_instances");
    if (!geometry.is_array() || geometry.size() !=
            layout.geometryInstancePointerOffsets.size()) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = "geometry_instances must contain exactly two offsets";
        return false;
    }
    for (std::size_t index = 0;
         index < layout.geometryInstancePointerOffsets.size(); ++index) {
        if (!ParseOffset(
                geometry.at(index), 8, kMaximumModuleOffset, 8, true,
                layout.geometryInstancePointerOffsets[index],
                "layout.geometry_instances", failure)) {
            return false;
        }
    }
    return ParseActorLayout(object.at("actor_records"),
                            layout.actorRecords, failure) &&
        ParseOffset(object.at("tracking_matrix_root"), 4,
                    kMaximumModuleOffset, 4, true,
                    layout.trackingMatrixRootOffset,
                    "layout.tracking_matrix_root", failure) &&
        ParseOffset(object.at("component_position_flag"), 4,
                    kMaximumModuleOffset, 1, true,
                    layout.componentPositionFlagOffset,
                    "layout.component_position_flag", failure);
}

bool Equivalent(const CloudLayoutDocument& left,
                const CloudLayoutDocument& right) noexcept {
    const auto& la = left.layout.actorRecords;
    const auto& ra = right.layout.actorRecords;
    return left.schemaVersion == right.schemaVersion &&
        left.revision == right.revision &&
        SameIdentity(left.identity, right.identity) &&
        left.layout.namePoolOffset == right.layout.namePoolOffset &&
        left.layout.worldOffset == right.layout.worldOffset &&
        left.layout.coordinateReplayEntryOffset ==
            right.layout.coordinateReplayEntryOffset &&
        left.layout.geometryInstancePointerOffsets ==
            right.layout.geometryInstancePointerOffsets &&
        left.layout.trackingMatrixRootOffset ==
            right.layout.trackingMatrixRootOffset &&
        left.layout.componentPositionFlagOffset ==
            right.layout.componentPositionFlagOffset &&
        la.taggedContainerOffset == ra.taggedContainerOffset &&
        la.plainArrayOffset == ra.plainArrayOffset &&
        la.plainRootOffset == ra.plainRootOffset &&
        la.plainMeshOffset == ra.plainMeshOffset &&
        la.encryptedRecordCount == ra.encryptedRecordCount &&
        la.plainRecordStride == ra.plainRecordStride &&
        la.maximumPlainCount == ra.maximumPlainCount &&
        la.fallbackPlainCount == ra.fallbackPlainCount;
}

CloudLayoutUpdateResult Failure(
    CloudLayoutStatus status,
    std::string detail,
    std::shared_ptr<const CloudLayoutDocument> snapshot) {
    return {status, std::move(detail), std::move(snapshot)};
}

}  // namespace

bool CloudRuntimeIdentity::IsValid() const noexcept {
    return IsValidPackageName(packageName) &&
        IsValidModuleName(moduleName) && IsValidBuildId(buildId);
}

CloudLayoutStore::CloudLayoutStore(CloudRuntimeIdentity expectedIdentity)
    : expectedIdentity_(std::move(expectedIdentity)) {}

const CloudRuntimeIdentity& CloudLayoutStore::ExpectedIdentity() const noexcept {
    return expectedIdentity_;
}

std::shared_ptr<const CloudLayoutDocument>
CloudLayoutStore::Snapshot() const noexcept {
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

CloudLayoutUpdateResult CloudLayoutStore::ValidateAndPublish(
    std::string_view payload) {
    const auto previous = Snapshot();
    if (!expectedIdentity_.IsValid()) {
        return Failure(CloudLayoutStatus::IdentityMismatch,
                       "expected runtime identity is invalid", previous);
    }
    if (payload.empty() || payload.size() > kMaximumPayloadBytes) {
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
        root = Json::parse(payload.begin(), payload.end(), callback, true, false);
        if (!objectKeys.empty()) {
            throw std::runtime_error("unbalanced JSON object");
        }
    } catch (const std::exception& exception) {
        return Failure(CloudLayoutStatus::InvalidJson, exception.what(), previous);
    }

    if (!HasExactKeys(root,
                      {"schema_version", "package", "module", "build_id",
                       "revision", "layout"})) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "root keys do not match schema version 1", previous);
    }

    ParseFailure failure;
    std::uint64_t schemaVersion = 0;
    std::uint64_t revision = 0;
    if (!ParseUnsigned(root.at("schema_version"), kCloudLayoutSchemaVersion,
                       kCloudLayoutSchemaVersion, schemaVersion, "schema_version",
                       failure) ||
        !ParseUnsigned(root.at("revision"), 1,
                       std::numeric_limits<std::uint64_t>::max(), revision,
                       "revision", failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }

    auto candidate = std::make_shared<CloudLayoutDocument>();
    candidate->schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    candidate->revision = revision;
    if (!ParseIdentity(root, candidate->identity, failure) ||
        !ParseLayout(root.at("layout"), candidate->layout, failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }
    if (!SameIdentity(candidate->identity, expectedIdentity_)) {
        return Failure(CloudLayoutStatus::IdentityMismatch,
                       "layout identity does not match the running module",
                       previous);
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

    std::shared_ptr<const CloudLayoutDocument> published = std::move(candidate);
    std::atomic_store_explicit(
        &current_, published, std::memory_order_release);
    return {CloudLayoutStatus::Published, {}, std::move(published)};
}

}  // namespace lengjing::auth
