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
constexpr std::uint64_t kPreviousCloudLayoutSchemaVersion = 4;
constexpr std::string_view kCoordinatePackage =
    "com.tencent.tmgp.dfm";
constexpr std::string_view kCoordinateModule = "libUE4.so";
constexpr std::string_view kCoordinateBuildId =
    "8187ddb9edbc9d5201201ffd7b008df3bfe533db";
constexpr std::string_view kCoordinateFirstVeneerRva = "0xe7f5514";

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

bool HasExactArraySize(const Json& array, std::size_t size) {
    return array.is_array() && array.size() == size;
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

bool ParseMaximumActorCount(const Json& value,
                            std::int32_t& output,
                            ParseFailure& failure) {
    std::uint64_t count = 0;
    if (!ParseUnsigned(
            value, 1, 65536, count, "d[0][3]", failure)) {
        return false;
    }
    output = static_cast<std::int32_t>(count);
    return true;
}

bool ParseActorSubject(const Json& values,
                       CloudActorSubjectLayout& layout,
                       ParseFailure& failure) {
    if (!HasExactArraySize(values, 3)) {
        failure.detail = "d[0][4] must contain exactly 3 values";
        return false;
    }
    if (!ParseOffset(values.at(0), 4, kMaximumObjectOffset, 4, false,
                     layout.rootOffset, "d[0][4][0]", failure) ||
        !ParseOffset(values.at(1), 4, kMaximumObjectOffset, 4, false,
                     layout.meshOffset, "d[0][4][1]", failure) ||
        !ParseOffset(values.at(2), 4, kMaximumObjectOffset, 4, false,
                     layout.alternateRootOffset, "d[0][4][2]", failure)) {
        return false;
    }
    if (layout.rootOffset == layout.meshOffset ||
        layout.rootOffset == layout.alternateRootOffset ||
        layout.meshOffset == layout.alternateRootOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "actor subject offsets must be distinct";
        return false;
    }
    return true;
}

bool ParseLayout(const Json& values,
                 CloudOffsetLayout& layout,
                 ParseFailure& failure) {
    if (!HasExactArraySize(values, 6)) {
        failure.detail = "d[0] must contain exactly 6 values";
        return false;
    }
    if (!ParseOffset(values.at(0), 4, kMaximumModuleOffset, 4, false,
                     layout.namePoolOffset, "d[0][0]", failure) ||
        !ParseOffset(values.at(1), 4, kMaximumModuleOffset, 4, false,
                     layout.worldOffset, "d[0][1]", failure)) {
        return false;
    }
    if (layout.namePoolOffset == layout.worldOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "name pool and world offsets must be distinct";
        return false;
    }

    const Json& geometry = values.at(2);
    if (!HasExactArraySize(
            geometry, layout.geometryInstancePointerOffsets.size())) {
        failure.detail = "d[0][2] must contain exactly 2 values";
        return false;
    }
    for (std::size_t index = 0;
         index < layout.geometryInstancePointerOffsets.size(); ++index) {
        if (!ParseOffset(geometry.at(index), 8, kMaximumModuleOffset, 8,
                         false,
                         layout.geometryInstancePointerOffsets[index],
                         "d[0][2]", failure)) {
            return false;
        }
    }
    if (layout.geometryInstancePointerOffsets[0] ==
        layout.geometryInstancePointerOffsets[1]) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "geometry offsets must be distinct";
        return false;
    }

    return ParseMaximumActorCount(
               values.at(3), layout.maximumActorCount, failure) &&
        ParseActorSubject(values.at(4), layout.actorSubject, failure) &&
        ParseOffset(values.at(5), 4, kMaximumModuleOffset, 4, false,
                    layout.trackingMatrixRootOffset, "d[0][5]", failure);
}

bool ParseDecrypt(const Json& values,
                  CloudDecryptLayout& layout,
                  ParseFailure& failure) {
    if (!HasExactArraySize(values, 1)) {
        failure.detail = "d[1] must contain exactly 1 value";
        return false;
    }
    return ParseOffset(
        values.at(0), 4, kMaximumModuleOffset, 4, false,
        layout.firstVeneerRva, "d[1][0]", failure);
}

bool ParseData(const Json& values,
               CloudLayoutDocument& document,
               ParseFailure& failure) {
    if (!HasExactArraySize(values, 2)) {
        failure.detail = "d must contain exactly 2 values";
        return false;
    }
    return ParseLayout(values.at(0), document.layout, failure) &&
        ParseDecrypt(values.at(1), document.decrypt, failure);
}

bool UpgradeRemoteLayout(Json& root,
                         const CloudRuntimeTarget& target,
                         const std::string& buildId,
                         std::uint64_t& schemaVersion) {
    if (schemaVersion != kPreviousCloudLayoutSchemaVersion ||
        target.packageName != kCoordinatePackage ||
        target.moduleName != kCoordinateModule ||
        buildId != kCoordinateBuildId ||
        !HasExactArraySize(root.at("d"), 2) ||
        !HasExactArraySize(root.at("d").at(0), 7) ||
        !HasExactArraySize(root.at("d").at(0).at(3), 8) ||
        !root.at("d").at(0).at(3).at(6).is_number_unsigned() ||
        !root.at("d").at(1).is_array()) {
        return false;
    }
    const Json maximumActorCount = root.at("d").at(0).at(3).at(6);
    root["d"][0] = Json::array({
        root.at("d").at(0).at(0),
        root.at("d").at(0).at(1),
        root.at("d").at(0).at(2),
        maximumActorCount,
        root.at("d").at(0).at(4),
        root.at("d").at(0).at(5),
    });
    root["v"] = kCloudLayoutSchemaVersion;
    root["d"][1] = Json::array({kCoordinateFirstVeneerRva});
    schemaVersion = kCloudLayoutSchemaVersion;
    return true;
}

bool SameIdentity(const CloudRuntimeIdentity& left,
                  const CloudRuntimeIdentity& right) noexcept {
    return std::tie(left.packageName, left.moduleName, left.buildId) ==
        std::tie(right.packageName, right.moduleName, right.buildId);
}

bool Equivalent(const CloudLayoutDocument& left,
                const CloudLayoutDocument& right) noexcept {
    return left.schemaVersion == right.schemaVersion &&
        left.revision == right.revision &&
        SameIdentity(left.identity, right.identity) &&
        std::tie(
            left.layout.namePoolOffset, left.layout.worldOffset,
            left.layout.geometryInstancePointerOffsets,
            left.layout.maximumActorCount,
            left.layout.actorSubject.rootOffset,
            left.layout.actorSubject.meshOffset,
            left.layout.actorSubject.alternateRootOffset,
            left.layout.trackingMatrixRootOffset) ==
        std::tie(
            right.layout.namePoolOffset, right.layout.worldOffset,
            right.layout.geometryInstancePointerOffsets,
            right.layout.maximumActorCount,
            right.layout.actorSubject.rootOffset,
            right.layout.actorSubject.meshOffset,
            right.layout.actorSubject.alternateRootOffset,
            right.layout.trackingMatrixRootOffset) &&
        left.decrypt.firstVeneerRva ==
            right.decrypt.firstVeneerRva;
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

    if (!HasExactKeys(root, {"v", "b", "r", "d"})) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "root keys do not match schema version 5", previous);
    }

    ParseFailure failure;
    std::uint64_t schemaVersion = 0;
    std::uint64_t revision = 0;
    if (!ParseUnsigned(root.at("v"), 0, UINT32_MAX, schemaVersion, "v",
                       failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }
    if (!root.at("b").is_string()) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "b must be a string", previous);
    }
    const std::string buildId = root.at("b").get<std::string>();
    if (!IsValidBuildId(buildId)) {
        return Failure(CloudLayoutStatus::IdentityMismatch,
                       "b is malformed", previous);
    }
    if (schemaVersion != kCloudLayoutSchemaVersion &&
        !UpgradeRemoteLayout(root, target_, buildId, schemaVersion)) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "unsupported cloud layout schema version", previous);
    }
    if (!ParseUnsigned(root.at("r"), 1,
                       std::numeric_limits<std::uint64_t>::max(), revision,
                       "r", failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }

    auto candidate = std::make_shared<CloudLayoutDocument>();
    candidate->schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    candidate->revision = revision;
    candidate->identity = {target_.packageName, target_.moduleName, buildId};
    if (!ParseData(root.at("d"), *candidate, failure)) {
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
