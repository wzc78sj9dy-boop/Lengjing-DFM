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

bool ParseActorRecords(const Json& values,
                       CloudActorRecordLayout& layout,
                       ParseFailure& failure) {
    if (!HasExactArraySize(values, 8)) {
        failure.detail = "d[0][3] must contain exactly 8 values";
        return false;
    }
    if (!ParseOffset(values.at(0), 4, kMaximumModuleOffset, 4, true,
                     layout.taggedContainerOffset, "d[0][3][0]", failure) ||
        !ParseOffset(values.at(1), 4, kMaximumModuleOffset, 4, true,
                     layout.plainArrayOffset, "d[0][3][1]", failure) ||
        !ParseOffset(values.at(2), 4, kMaximumObjectOffset, 4, true,
                     layout.plainRootOffset, "d[0][3][2]", failure) ||
        !ParseOffset(values.at(3), 4, kMaximumObjectOffset, 4, true,
                     layout.plainMeshOffset, "d[0][3][3]", failure)) {
        return false;
    }

    std::uint64_t encryptedCount = 0;
    std::uint64_t stride = 0;
    std::uint64_t maximumCount = 0;
    std::uint64_t fallbackCount = 0;
    if (!ParseUnsigned(values.at(4), 0, 65536, encryptedCount,
                       "d[0][3][4]", failure) ||
        !ParseUnsigned(values.at(5), 0, 256, stride, "d[0][3][5]",
                       failure) ||
        !ParseUnsigned(values.at(6), 0, 65536, maximumCount,
                       "d[0][3][6]", failure) ||
        !ParseUnsigned(values.at(7), 0, 65536, fallbackCount,
                       "d[0][3][7]", failure)) {
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
        failure.detail = "actor record values are incomplete or inconsistent";
        return false;
    }

    layout.encryptedRecordCount = static_cast<std::uint32_t>(encryptedCount);
    layout.plainRecordStride = static_cast<std::uint32_t>(stride);
    layout.maximumPlainCount = static_cast<std::int32_t>(maximumCount);
    layout.fallbackPlainCount = static_cast<std::int32_t>(fallbackCount);
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

bool ParsePool(const Json& values,
               CloudCoordinatePoolLayout& layout,
               std::string_view prefix,
               ParseFailure& failure) {
    if (!HasExactArraySize(values, 8)) {
        failure.detail = std::string(prefix) +
            " must contain exactly 8 values";
        return false;
    }
    const auto field = [&](std::size_t index) {
        return std::string(prefix) + "[" + std::to_string(index) + "]";
    };
    std::int64_t contextOffset = 0;
    std::uint64_t stride = 0;
    std::uint64_t headSkip = 0;
    std::uint64_t refreshFrames = 0;
    if (!ParseOffset(values.at(0), 4, kMaximumModuleOffset, 4, false,
                     layout.rootRva, field(0), failure) ||
        !ParseOffset(values.at(1), 0, kMaximumExecutionFieldOffset, 4, true,
                     layout.bridgeOffset, field(1), failure) ||
        !ParseSigned(values.at(2), -0x10000, 0x10000, contextOffset,
                     field(2), failure)) {
        return false;
    }
    if (contextOffset == 0 || contextOffset % 8 != 0) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = field(2) + " must be nonzero and 8-byte aligned";
        return false;
    }
    if (!ParseOffset(values.at(3), 8, kMaximumExecutionFieldOffset, 8,
                     false, layout.entryOffset, field(3), failure) ||
        !ParseOffset(values.at(4), 8, kMaximumObjectOffset, 8, false,
                     layout.componentKeyOffset, field(4), failure) ||
        !ParseUnsigned(values.at(5), 12, 4096, stride, field(5), failure) ||
        !ParseUnsigned(values.at(6), 0, 4084, headSkip, field(6), failure) ||
        !ParseUnsigned(values.at(7), 1, 3600, refreshFrames, field(7),
                       failure)) {
        return false;
    }
    if (stride % 4 != 0 || headSkip + 12 > stride ||
        layout.rootRva > kMaximumModuleOffset - layout.bridgeOffset) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = std::string(prefix) + " values are inconsistent";
        return false;
    }
    layout.contextOffset = static_cast<std::int32_t>(contextOffset);
    layout.entryStride = static_cast<std::uint32_t>(stride);
    layout.poolHeadSkip = static_cast<std::uint32_t>(headSkip);
    layout.ringRefreshFrames = static_cast<std::uint32_t>(refreshFrames);
    return true;
}

bool ParseLayout(const Json& values,
                 CloudOffsetLayout& layout,
                 ParseFailure& failure) {
    if (!HasExactArraySize(values, 7)) {
        failure.detail = "d[0] must contain exactly 7 values";
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

    return ParseActorRecords(values.at(3), layout.actorRecords, failure) &&
        ParseActorSubject(values.at(4), layout.actorSubject, failure) &&
        ParseOffset(values.at(5), 4, kMaximumModuleOffset, 4, false,
                    layout.trackingMatrixRootOffset, "d[0][5]", failure) &&
        ParseOffset(values.at(6), 1, kMaximumModuleOffset, 1, false,
                    layout.componentPositionFlagOffset, "d[0][6]", failure);
}

bool ParseMode1(const Json& values,
                CloudDecryptMode1Layout& layout,
                ParseFailure& failure) {
    if (!HasExactArraySize(values, 3)) {
        failure.detail = "d[1][0] must contain exactly 3 values";
        return false;
    }
    if (!ParsePool(values.at(0), layout.pool, "d[1][0][0]", failure) ||
        !ParseHexUnsigned(values.at(1), 0,
                          std::numeric_limits<std::uint64_t>::max(),
                          layout.pacgaData, "d[1][0][1]", failure) ||
        !ParseHexUnsigned(values.at(2), 0,
                          std::numeric_limits<std::uint64_t>::max(),
                          layout.pacgaModifier, "d[1][0][2]", failure)) {
        return false;
    }
    if (layout.pacgaData == 0 && layout.pacgaModifier == 0) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "mode 1 PACGA inputs must not both be zero";
        return false;
    }
    return true;
}

bool ParseMode2(const Json& values,
                CloudDecryptMode2Layout& layout,
                ParseFailure& failure) {
    if (!HasExactArraySize(values, 1)) {
        failure.detail = "d[1][1] must contain exactly 1 value";
        return false;
    }
    return ParsePool(values.at(0), layout.pool, "d[1][1][0]", failure);
}

bool ParseDiscovery(const Json& values,
                    CloudExecutionDiscoveryLayout& layout,
                    std::string_view prefix,
                    ParseFailure& failure) {
    if (!HasExactArraySize(values, 4)) {
        failure.detail = std::string(prefix) +
            " must contain exactly 4 values";
        return false;
    }
    const auto field = [prefix](std::size_t index) {
        return std::string(prefix) + "[" + std::to_string(index) + "]";
    };
    return ParseOffset(values.at(0), 4, kMaximumModuleOffset, 4, false,
                       layout.rootOffset, field(0), failure) &&
        ParseOffset(values.at(1), 4, kMaximumExecutionFieldOffset, 4,
                    false, layout.pointerOffset, field(1),
                    failure) &&
        ParseOffset(values.at(2), 8, kMaximumExecutionFieldOffset, 8,
                    false, layout.entryOffset, field(2), failure) &&
        ParseHexUnsigned(values.at(3), 1,
                         std::numeric_limits<std::uint64_t>::max(),
                         layout.returnStubMagic, field(3),
                         failure);
}

bool ParseDiscoveryProfiles(const Json& values,
                            CloudExecutionLayout& layout,
                            ParseFailure& failure) {
    layout.discovery = {};
    layout.profile12Discovery = {};
    layout.hasProfile12Discovery = false;
    if (HasExactArraySize(values, 4)) {
        return ParseDiscovery(
            values, layout.discovery, "d[1][2][0]", failure);
    }
    if (!HasExactArraySize(values, 2)) {
        failure.detail =
            "d[1][2][0] must contain one or two discovery profiles";
        return false;
    }
    if (!ParseDiscovery(
            values.at(0),
            layout.discovery,
            "d[1][2][0][0]",
            failure) ||
        !ParseDiscovery(
            values.at(1),
            layout.profile12Discovery,
            "d[1][2][0][1]",
            failure)) {
        return false;
    }
    layout.hasProfile12Discovery = true;
    return true;
}

bool ParseResult(const Json& values,
                 CloudExecutionResultLayout& layout,
                 ParseFailure& failure) {
    if (!HasExactArraySize(values, 2)) {
        failure.detail = "d[1][2][1] must contain exactly 2 values";
        return false;
    }
    return ParseOffset(values.at(0), 8, kMaximumExecutionFieldOffset, 8,
                       false, layout.slotOffset, "d[1][2][1][0]",
                       failure) &&
        ParseOffset(values.at(1), 4, kMaximumExecutionFieldOffset, 4,
                    false, layout.positionOffset, "d[1][2][1][1]",
                    failure);
}

bool ParseHookOffsets(const Json& values,
                      CloudExecutionHookOffsetLayout& layout,
                      ParseFailure& failure) {
    using Member = std::uintptr_t CloudExecutionHookOffsetLayout::*;
    static constexpr Member members[] = {
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
    constexpr std::size_t count = sizeof(members) / sizeof(members[0]);
    if (!HasExactArraySize(values, count)) {
        failure.detail = "d[1][2][2] must contain exactly 26 values";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::string field =
            "d[1][2][2][" + std::to_string(index) + "]";
        if (!ParseOffset(values.at(index), 4, kMaximumModuleOffset, 4,
                         true, layout.*members[index], field, failure)) {
            return false;
        }
    }
    return true;
}

bool ParseFieldOffsets(const Json& values,
                       CloudExecutionFieldOffsetLayout& layout,
                       ParseFailure& failure) {
    using Member = std::uintptr_t CloudExecutionFieldOffsetLayout::*;
    static constexpr Member members[] = {
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
    constexpr std::size_t count = sizeof(members) / sizeof(members[0]);
    if (!HasExactArraySize(values, count)) {
        failure.detail = "d[1][2][3] must contain exactly 15 values";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::string field =
            "d[1][2][3][" + std::to_string(index) + "]";
        if (!ParseOffset(values.at(index), 4,
                         kMaximumExecutionFieldOffset, 4, true,
                         layout.*members[index], field, failure)) {
            return false;
        }
    }
    return true;
}

bool ParseContext(const Json& values,
                  CloudExecutionContextLayout& layout,
                  ParseFailure& failure) {
    if (!HasExactArraySize(values, 2)) {
        failure.detail = "d[1][2][4] must contain exactly 2 values";
        return false;
    }
    if (!values.at(0).is_string()) {
        failure.detail = "d[1][2][4][0] must be a string";
        return false;
    }
    layout.threadName = values.at(0).get<std::string>();
    if (!IsValidThreadName(layout.threadName)) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "d[1][2][4][0] is outside the accepted range";
        return false;
    }
    std::uint64_t opcode = 0;
    if (!ParseHexUnsigned(values.at(1), 1, UINT32_MAX, opcode,
                          "d[1][2][4][1]", failure)) {
        return false;
    }
    const auto encoded = static_cast<std::uint32_t>(opcode);
    const std::uint32_t destination = encoded & 0x1fU;
    const std::uint32_t data = (encoded >> 5U) & 0x1fU;
    const std::uint32_t modifier = (encoded >> 16U) & 0x1fU;
    if ((encoded & UINT32_C(0xFFE0FC00)) != UINT32_C(0x9AC03000) ||
        destination == 31U || data == 31U || modifier == 31U) {
        failure.status = CloudLayoutStatus::RangeError;
        failure.detail = "d[1][2][4][1] is invalid";
        return false;
    }
    layout.oracleOpcode = encoded;
    return true;
}

bool ParseExecution(const Json& values,
                    CloudExecutionLayout& layout,
                    ParseFailure& failure) {
    if (!HasExactArraySize(values, 5)) {
        failure.detail = "d[1][2] must contain exactly 5 values";
        return false;
    }
    return ParseDiscoveryProfiles(values.at(0), layout, failure) &&
        ParseResult(values.at(1), layout.result, failure) &&
        ParseHookOffsets(values.at(2), layout.hookOffsets, failure) &&
        ParseFieldOffsets(values.at(3), layout.fieldOffsets, failure) &&
        ParseContext(values.at(4), layout.context, failure);
}

bool ParseDecrypt(const Json& values,
                  CloudDecryptLayout& layout,
                  ParseFailure& failure) {
    if (!HasExactArraySize(values, 3)) {
        failure.detail = "d[1] must contain exactly 3 values";
        return false;
    }
    return ParseMode1(values.at(0), layout.mode1, failure) &&
        ParseMode2(values.at(1), layout.mode2, failure) &&
        ParseExecution(values.at(2), layout.execution, failure);
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
            le.profile12Discovery.rootOffset,
            le.profile12Discovery.pointerOffset,
            le.profile12Discovery.entryOffset,
            le.profile12Discovery.returnStubMagic,
            le.hasProfile12Discovery,
            le.result.slotOffset, le.result.positionOffset,
            le.context.threadName, le.context.oracleOpcode) ==
        std::tie(
            re.discovery.rootOffset, re.discovery.pointerOffset,
            re.discovery.entryOffset, re.discovery.returnStubMagic,
            re.profile12Discovery.rootOffset,
            re.profile12Discovery.pointerOffset,
            re.profile12Discovery.entryOffset,
            re.profile12Discovery.returnStubMagic,
            re.hasProfile12Discovery,
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

    if (!HasExactKeys(root, {"v", "b", "r", "d"})) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "root keys do not match schema version 4", previous);
    }

    ParseFailure failure;
    std::uint64_t schemaVersion = 0;
    std::uint64_t revision = 0;
    if (!ParseUnsigned(root.at("v"), 0, UINT32_MAX, schemaVersion, "v",
                       failure)) {
        return Failure(failure.status, std::move(failure.detail), previous);
    }
    if (schemaVersion != kCloudLayoutSchemaVersion) {
        return Failure(CloudLayoutStatus::SchemaMismatch,
                       "unsupported cloud layout schema version", previous);
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
