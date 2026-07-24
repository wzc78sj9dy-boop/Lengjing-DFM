#include "auth/CoordinatePoolCloudLayout.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <atomic>
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

constexpr std::uint64_t kMaximumModuleOffset = 0xffffffffULL;
constexpr std::uint64_t kMaximumObjectOffset = 0xffffULL;
constexpr std::uint64_t kMaximumCoordinateFieldOffset = 0x10000ULL;

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
    const std::uint64_t parsed = value.get<std::uint64_t>();
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
        const std::uint64_t unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            failure.status = CloudLayoutStatus::RangeError;
            failure.detail =
                std::string(field) + " is outside the accepted range";
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
                      std::uint64_t maximum,
                      std::uint64_t& output,
                      std::string_view field,
                      ParseFailure& failure) {
    if (!value.is_string()) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail = std::string(field) + " must be a hexadecimal string";
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.size() < 3 || text.size() > 18 ||
        text[0] != '0' || text[1] != 'x') {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail =
            std::string(field) + " has an invalid hexadecimal format";
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
            failure.detail =
                std::string(field) + " must use lowercase hexadecimal";
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
    if (parsed > maximum) {
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
    if (!ParseHexUnsigned(value, maximum, parsed, field, failure)) {
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

bool ParseCoordinatePool(
    const Json& object,
    game::native::CoordinatePoolRuntimeLayout& layout,
    ParseFailure& failure) {
    if (!HasExactKeys(
            object,
            {"root_rva", "bridge_offset", "context_offset",
             "entry_offset", "component_key_offset", "entry_stride",
             "pool_head_skip", "ring_refresh_frames"})) {
        failure.status = CloudLayoutStatus::SchemaMismatch;
        failure.detail =
            "coordinate_pool keys do not match schema version 1";
        return false;
    }

    std::int64_t contextOffset = 0;
    std::uint64_t entryStride = 0;
    std::uint64_t poolHeadSkip = 0;
    std::uint64_t ringRefreshFrames = 0;
    if (!ParseOffset(
            object.at("root_rva"),
            4,
            kMaximumModuleOffset,
            4,
            false,
            layout.rootRva,
            "coordinate_pool.root_rva",
            failure) ||
        !ParseOffset(
            object.at("bridge_offset"),
            0,
            kMaximumCoordinateFieldOffset,
            4,
            true,
            layout.bridgeOffset,
            "coordinate_pool.bridge_offset",
            failure) ||
        !ParseSigned(
            object.at("context_offset"),
            -0x10000,
            0x10000,
            contextOffset,
            "coordinate_pool.context_offset",
            failure)) {
        return false;
    }
    if (contextOffset == 0 || contextOffset % 8 != 0) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail =
            "coordinate_pool.context_offset must be nonzero and 8-byte aligned";
        return false;
    }
    if (!ParseOffset(
            object.at("entry_offset"),
            8,
            kMaximumCoordinateFieldOffset,
            8,
            false,
            layout.entryOffset,
            "coordinate_pool.entry_offset",
            failure) ||
        !ParseOffset(
            object.at("component_key_offset"),
            8,
            kMaximumObjectOffset,
            8,
            false,
            layout.componentKeyOffset,
            "coordinate_pool.component_key_offset",
            failure) ||
        !ParseUnsigned(
            object.at("entry_stride"),
            12,
            4096,
            entryStride,
            "coordinate_pool.entry_stride",
            failure) ||
        !ParseUnsigned(
            object.at("pool_head_skip"),
            0,
            4084,
            poolHeadSkip,
            "coordinate_pool.pool_head_skip",
            failure) ||
        !ParseUnsigned(
            object.at("ring_refresh_frames"),
            1,
            3600,
            ringRefreshFrames,
            "coordinate_pool.ring_refresh_frames",
            failure)) {
        return false;
    }

    layout.contextOffset = static_cast<std::int32_t>(contextOffset);
    layout.entryStride = static_cast<std::uint32_t>(entryStride);
    layout.poolHeadSkip = static_cast<std::uint32_t>(poolHeadSkip);
    layout.ringRefreshFrames =
        static_cast<std::uint32_t>(ringRefreshFrames);
    if (!layout.IsValid()) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "coordinate_pool fields are inconsistent";
        return false;
    }
    return true;
}

bool Equivalent(const CoordinatePoolCloudLayoutDocument& left,
                const CoordinatePoolCloudLayoutDocument& right) noexcept {
    const auto& ll = left.coordinatePool;
    const auto& rl = right.coordinatePool;
    return left.schemaVersion == right.schemaVersion &&
        left.revision == right.revision &&
        SameIdentity(left.identity, right.identity) &&
        ll.rootRva == rl.rootRva &&
        ll.bridgeOffset == rl.bridgeOffset &&
        ll.contextOffset == rl.contextOffset &&
        ll.entryOffset == rl.entryOffset &&
        ll.componentKeyOffset == rl.componentKeyOffset &&
        ll.entryStride == rl.entryStride &&
        ll.poolHeadSkip == rl.poolHeadSkip &&
        ll.ringRefreshFrames == rl.ringRefreshFrames;
}

CoordinatePoolCloudLayoutUpdateResult Failure(
    CloudLayoutStatus status,
    std::string detail,
    std::shared_ptr<const CoordinatePoolCloudLayoutDocument> snapshot) {
    return {status, std::move(detail), std::move(snapshot)};
}

}  // namespace

CoordinatePoolCloudLayoutStore::CoordinatePoolCloudLayoutStore(
    CloudRuntimeIdentity expectedIdentity)
    : expectedIdentity_(std::move(expectedIdentity)) {}

const CloudRuntimeIdentity&
CoordinatePoolCloudLayoutStore::ExpectedIdentity() const noexcept {
    return expectedIdentity_;
}

std::shared_ptr<const CoordinatePoolCloudLayoutDocument>
CoordinatePoolCloudLayoutStore::Snapshot() const noexcept {
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

CoordinatePoolCloudLayoutUpdateResult
CoordinatePoolCloudLayoutStore::ValidateAndPublish(
    std::string_view payload) {
    const auto previous = Snapshot();
    if (!expectedIdentity_.IsValid()) {
        return Failure(
            CloudLayoutStatus::IdentityMismatch,
            "expected runtime identity is invalid",
            previous);
    }
    if (payload.empty() ||
        payload.size() > kMaximumCoordinatePoolCloudLayoutPayloadBytes) {
        return Failure(
            CloudLayoutStatus::InvalidJson,
            "coordinate pool payload is empty or too large",
            previous);
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
        root = Json::parse(
            payload.begin(), payload.end(), callback, true, false);
        if (!objectKeys.empty()) {
            throw std::runtime_error("unbalanced JSON object");
        }
    } catch (const std::exception& exception) {
        return Failure(
            CloudLayoutStatus::InvalidJson,
            exception.what(),
            previous);
    }

    if (!HasExactKeys(
            root,
            {"schema_version", "package", "module", "build_id",
             "revision", "coordinate_pool"})) {
        return Failure(
            CloudLayoutStatus::SchemaMismatch,
            "root keys do not match schema version 1",
            previous);
    }

    ParseFailure failure;
    std::uint64_t schemaVersion = 0;
    std::uint64_t revision = 0;
    if (!ParseUnsigned(
            root.at("schema_version"),
            0,
            std::numeric_limits<std::uint32_t>::max(),
            schemaVersion,
            "schema_version",
            failure)) {
        return Failure(
            failure.status, std::move(failure.detail), previous);
    }
    if (schemaVersion != kCoordinatePoolCloudLayoutSchemaVersion) {
        return Failure(
            CloudLayoutStatus::SchemaMismatch,
            "unsupported coordinate pool schema version",
            previous);
    }
    if (!ParseUnsigned(
            root.at("revision"),
            1,
            std::numeric_limits<std::uint64_t>::max(),
            revision,
            "revision",
            failure)) {
        return Failure(
            failure.status, std::move(failure.detail), previous);
    }

    auto candidate =
        std::make_shared<CoordinatePoolCloudLayoutDocument>();
    candidate->schemaVersion =
        static_cast<std::uint32_t>(schemaVersion);
    candidate->revision = revision;
    if (!ParseIdentity(root, candidate->identity, failure) ||
        !ParseCoordinatePool(
            root.at("coordinate_pool"),
            candidate->coordinatePool,
            failure)) {
        return Failure(
            failure.status, std::move(failure.detail), previous);
    }
    if (!SameIdentity(candidate->identity, expectedIdentity_)) {
        return Failure(
            CloudLayoutStatus::IdentityMismatch,
            "layout identity does not match the running module",
            previous);
    }

    std::lock_guard<std::mutex> lock(publishMutex_);
    const auto current = Snapshot();
    if (current != nullptr) {
        if (candidate->revision < current->revision) {
            return Failure(
                CloudLayoutStatus::RollbackRejected,
                "layout revision rollback rejected",
                current);
        }
        if (candidate->revision == current->revision) {
            if (Equivalent(*candidate, *current)) {
                return {CloudLayoutStatus::Unchanged, {}, current};
            }
            return Failure(
                CloudLayoutStatus::RevisionConflict,
                "same revision contains different layout data",
                current);
        }
    }

    std::shared_ptr<const CoordinatePoolCloudLayoutDocument> published =
        std::move(candidate);
    std::atomic_store_explicit(
        &current_, published, std::memory_order_release);
    return {
        CloudLayoutStatus::Published,
        {},
        std::move(published),
    };
}

}  // namespace lengjing::auth
