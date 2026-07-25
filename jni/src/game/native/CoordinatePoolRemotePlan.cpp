#include "game/native/CoordinatePoolRemotePlan.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

namespace pool = coordinate_pool_internal;
namespace dec = coordinate_pool_internal::coord_dec;
using Json = nlohmann::json;

constexpr std::size_t kMaximumExpressionBase64Bytes = 128U * 1024U;
constexpr std::size_t kMaximumExpressionBinaryBytes = 96U * 1024U;
constexpr std::size_t kMaximumExpressionNodes = 4096;
constexpr std::size_t kMaximumExpressionDepth = 128;
constexpr std::size_t kMaximumParameterCount = 256;
constexpr std::size_t kMaximumParameterOffsets = 16;
constexpr std::size_t kMaximumPatchCount = 512;
constexpr std::size_t kMaximumNameBytes = 128;

CoordinatePoolRemotePlanResult Failure(
    CoordinatePoolRemotePlanError error,
    std::string detail) {
    CoordinatePoolRemotePlanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

bool IsPlanNameValid(std::string_view value) {
    if (value.empty() || value.size() > kMaximumNameBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20U && character <= 0x7EU;
    });
}

bool ReadUnsigned(const Json& value, std::uint64_t& result) {
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const std::int64_t signedValue = value.get<std::int64_t>();
        if (signedValue < 0) return false;
        result = static_cast<std::uint64_t>(signedValue);
        return true;
    }
    return false;
}

bool ReadSigned(const Json& value, std::int64_t& result) {
    if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
        return true;
    }
    if (value.is_number_unsigned()) {
        const std::uint64_t unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        result = static_cast<std::int64_t>(unsignedValue);
        return true;
    }
    return false;
}

const Json* Field(const Json& object, const char* name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

bool ReadUnsignedField(const Json& object,
                       const char* name,
                       std::uint64_t& result) {
    const Json* value = Field(object, name);
    return value != nullptr && ReadUnsigned(*value, result);
}

bool ReadSignedField(const Json& object,
                     const char* name,
                     std::int64_t& result) {
    const Json* value = Field(object, name);
    return value != nullptr && ReadSigned(*value, result);
}

bool ReadStringField(const Json& object,
                     const char* name,
                     std::string& result,
                     std::size_t maximumSize) {
    const Json* value = Field(object, name);
    if (value == nullptr || !value->is_string()) return false;
    result = value->get<std::string>();
    return result.size() <= maximumSize;
}

int Base64Value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

bool DecodeBase64(std::string_view encoded,
                  std::vector<std::uint8_t>& decoded) {
    decoded.clear();
    if (encoded.empty() || encoded.size() > kMaximumExpressionBase64Bytes ||
        (encoded.size() & 3U) != 0) {
        return false;
    }
    const std::size_t maximumDecoded = encoded.size() / 4U * 3U;
    if (maximumDecoded > kMaximumExpressionBinaryBytes) return false;
    decoded.reserve(maximumDecoded);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4U) {
        const bool last = offset + 4U == encoded.size();
        const unsigned char c0 = static_cast<unsigned char>(encoded[offset]);
        const unsigned char c1 = static_cast<unsigned char>(encoded[offset + 1]);
        const unsigned char c2 = static_cast<unsigned char>(encoded[offset + 2]);
        const unsigned char c3 = static_cast<unsigned char>(encoded[offset + 3]);
        const int v0 = Base64Value(c0);
        const int v1 = Base64Value(c1);
        const int v2 = c2 == '=' ? 0 : Base64Value(c2);
        const int v3 = c3 == '=' ? 0 : Base64Value(c3);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0 ||
            (!last && (c2 == '=' || c3 == '=')) ||
            (c2 == '=' && c3 != '=') ||
            (c2 == '=' && (v1 & 0x0F) != 0) ||
            (c3 == '=' && c2 != '=' && (v2 & 0x03) != 0)) {
            decoded.clear();
            return false;
        }
        decoded.push_back(static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
        if (c2 != '=') {
            decoded.push_back(static_cast<std::uint8_t>(
                (v1 << 4) | (v2 >> 2)));
        }
        if (c3 != '=') {
            decoded.push_back(static_cast<std::uint8_t>((v2 << 6) | v3));
        }
    }
    return !decoded.empty() && decoded.size() <= kMaximumExpressionBinaryBytes;
}

class ExpressionReader final {
public:
    ExpressionReader(
        const std::vector<std::uint8_t>& bytes,
        const std::unordered_map<std::string, const dec::param*>& memory,
        std::string_view ringIndexParameter)
        : bytes_(bytes),
          memory_(memory),
          ringIndexParameter_(ringIndexParameter) {}

    std::shared_ptr<pool::Expr> Read() {
        std::shared_ptr<pool::Expr> expression = ReadNode(0);
        if (!expression || offset_ != bytes_.size()) return nullptr;
        return expression;
    }

    const std::string& Detail() const noexcept { return detail_; }

private:
    bool ReadU32(std::uint32_t& value) {
        if (offset_ > bytes_.size() || bytes_.size() - offset_ < 4U) {
            detail_ = "expression is truncated";
            return false;
        }
        value = static_cast<std::uint32_t>(bytes_[offset_]) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8U) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16U) |
            (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24U);
        offset_ += 4U;
        return true;
    }

    bool ReadI32(std::int32_t& value) {
        std::uint32_t encoded = 0;
        if (!ReadU32(encoded)) return false;
        std::memcpy(&value, &encoded, sizeof(value));
        return true;
    }

    bool ReadU64(std::uint64_t& value) {
        if (offset_ > bytes_.size() || bytes_.size() - offset_ < 8U) {
            detail_ = "expression is truncated";
            return false;
        }
        value = 0;
        for (unsigned int index = 0; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) <<
                (index * 8U);
        }
        offset_ += 8U;
        return true;
    }

    bool ReadName(std::string& name) {
        std::uint64_t size = 0;
        if (!ReadU64(size) || size > kMaximumNameBytes ||
            size > bytes_.size() - offset_) {
            detail_ = "expression name is invalid";
            return false;
        }
        name.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            static_cast<std::size_t>(size));
        offset_ += static_cast<std::size_t>(size);
        if (!IsPlanNameValid(name)) {
            detail_ = "expression name is invalid";
            return false;
        }
        return true;
    }

    std::shared_ptr<pool::Expr> ReadNode(std::size_t depth) {
        if (depth >= kMaximumExpressionDepth ||
            ++nodeCount_ > kMaximumExpressionNodes) {
            detail_ = "expression complexity limit exceeded";
            return nullptr;
        }
        std::int32_t type = 0;
        if (!ReadI32(type)) return nullptr;
        switch (type) {
            case pool::EXPR_CONST: {
                std::uint64_t value = 0;
                if (!ReadU64(value)) return nullptr;
                return std::make_shared<pool::ConstExpr>(value, 0);
            }
            case pool::EXPR_VAR: {
                std::string name;
                if (!ReadName(name)) return nullptr;
                return std::make_shared<pool::VarExpr>(std::move(name), 0);
            }
            case pool::EXPR_BINARY: {
                std::int32_t operation = 0;
                if (!ReadI32(operation) || operation < pool::OP_ADD ||
                    operation > pool::OP_UMULH) {
                    detail_ = "binary expression operation is invalid";
                    return nullptr;
                }
                std::shared_ptr<pool::Expr> left = ReadNode(depth + 1U);
                std::shared_ptr<pool::Expr> right = ReadNode(depth + 1U);
                if (!left || !right) return nullptr;
                return std::make_shared<pool::BinaryExpr>(
                    static_cast<pool::BinOp>(operation),
                    std::move(left),
                    std::move(right),
                    0);
            }
            case pool::EXPR_SHIFT: {
                std::int32_t operation = 0;
                std::int32_t amount = 0;
                if (!ReadI32(operation) || !ReadI32(amount) ||
                    operation < pool::SHIFT_OP_LSL ||
                    operation > pool::SHIFT_OP_ASR ||
                    amount < 0 || amount >= 64) {
                    detail_ = "shift expression is invalid";
                    return nullptr;
                }
                std::shared_ptr<pool::Expr> child = ReadNode(depth + 1U);
                if (!child) return nullptr;
                return std::make_shared<pool::ShiftExpr>(
                    static_cast<pool::ShiftOp>(operation),
                    std::move(child),
                    amount,
                    0);
            }
            case pool::EXPR_UNARY: {
                std::int32_t operation = 0;
                if (!ReadI32(operation) || operation < pool::OP_MVN ||
                    operation > pool::OP_NEG) {
                    detail_ = "unary expression operation is invalid";
                    return nullptr;
                }
                std::shared_ptr<pool::Expr> child = ReadNode(depth + 1U);
                if (!child) return nullptr;
                return std::make_shared<pool::UnaryExpr>(
                    static_cast<pool::UnaryOp>(operation),
                    std::move(child),
                    0);
            }
            case pool::EXPR_MEMORY: {
                std::string name;
                if (!ReadName(name)) return nullptr;
                std::uint32_t size = sizeof(std::uint64_t);
                std::int32_t displacement = 0;
                std::vector<std::int32_t> offsets;
                const auto parameter = memory_.find(name);
                if (parameter != memory_.end()) {
                    size = parameter->second->size;
                    displacement = parameter->second->disp;
                    offsets = parameter->second->offset;
                } else if (name != ringIndexParameter_) {
                    detail_ = "expression memory parameter is unknown";
                    return nullptr;
                }
                auto expression = std::make_shared<pool::MemVarExpr>(
                    name, size, displacement, 0, "SP");
                expression->off_ = std::move(offsets);
                return expression;
            }
            default:
                detail_ = "expression node type is invalid";
                return nullptr;
        }
    }

    const std::vector<std::uint8_t>& bytes_;
    const std::unordered_map<std::string, const dec::param*>& memory_;
    std::string ringIndexParameter_;
    std::size_t offset_ = 0;
    std::size_t nodeCount_ = 0;
    std::string detail_;
};

bool IsCaptureRegisterValid(arm64_reg value) {
    if (value >= ARM64_REG_W0 && value <= ARM64_REG_W28) {
        value = static_cast<arm64_reg>(
            value - ARM64_REG_W0 + ARM64_REG_X0);
    } else if (value == ARM64_REG_W29) {
        value = ARM64_REG_X29;
    } else if (value == ARM64_REG_W30) {
        value = ARM64_REG_X30;
    }
    return (value >= ARM64_REG_X0 && value <= ARM64_REG_X28) ||
        value == ARM64_REG_X29 || value == ARM64_REG_X30 ||
        value == ARM64_REG_SP || value == ARM64_REG_XZR;
}

}  // namespace

CoordinatePoolRemotePlanResult ParseCoordinatePoolRemotePlan(
    std::string_view payload,
    std::uint64_t mappingBase,
    std::size_t mappingSize,
    std::uint64_t expectedEntry) noexcept {
    if (payload.empty()) {
        return Failure(
            CoordinatePoolRemotePlanError::EmptyPayload,
            "remote plan payload is empty");
    }
    if (payload.size() > kMaximumCoordinatePoolRemotePlanPayloadBytes) {
        return Failure(
            CoordinatePoolRemotePlanError::PayloadTooLarge,
            "remote plan payload exceeds size limit");
    }
    if (mappingSize < 8U ||
        mappingBase > std::numeric_limits<std::uint64_t>::max() - mappingSize) {
        return Failure(
            CoordinatePoolRemotePlanError::InvalidAddress,
            "mapping range is invalid");
    }
    const std::uint64_t mappingEnd = mappingBase + mappingSize;
    const auto containsInstruction = [&](std::uint64_t address) {
        return (address & 3U) == 0 && address >= mappingBase &&
            address < mappingEnd && mappingEnd - address >= 4U;
    };

    try {
        const Json root = Json::parse(
            payload.begin(), payload.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidJson,
                "remote plan JSON is invalid");
        }
        std::int64_t remoteCode = -1;
        if (!ReadSignedField(root, "code", remoteCode)) {
            return Failure(
                CoordinatePoolRemotePlanError::MissingField,
                "remote plan code is missing");
        }
        if (remoteCode != 0) {
            std::string detail = "remote parser returned an error";
            const Json* message = Field(root, "msg");
            if (message != nullptr && message->is_string()) {
                const std::string candidate = message->get<std::string>();
                if (!candidate.empty() && candidate.size() <= 256U) {
                    detail = candidate;
                }
            }
            return Failure(
                CoordinatePoolRemotePlanError::RemoteFailure,
                std::move(detail));
        }
        const Json* data = Field(root, "data");
        if (data == nullptr || !data->is_object()) {
            return Failure(
                CoordinatePoolRemotePlanError::MissingField,
                "remote plan data is missing");
        }

        dec::RuntimePlan plan;
        std::int64_t indexOffset = 0;
        std::int64_t poolPointerOffset = 0;
        std::uint64_t ringOffset = 0;
        std::uint64_t entryStart = 0;
        std::uint64_t duplicateEntryStart = 0;
        std::uint64_t v87End = 0;
        std::uint64_t v87Register = 0;
        std::uint64_t searchEnd = 0;
        std::uint64_t searchRegister = 0;
        std::uint64_t parameterEnd = 0;
        if (!ReadSignedField(*data, "A", indexOffset) ||
            !ReadSignedField(*data, "B", poolPointerOffset) ||
            !ReadUnsignedField(*data, "C", ringOffset) ||
            !ReadUnsignedField(*data, "D", entryStart) ||
            !ReadUnsignedField(*data, "E", v87End) ||
            !ReadUnsignedField(*data, "F", v87Register) ||
            !ReadUnsignedField(*data, "G", duplicateEntryStart) ||
            !ReadUnsignedField(*data, "H", searchEnd) ||
            !ReadUnsignedField(*data, "I", searchRegister) ||
            !ReadUnsignedField(*data, "N", parameterEnd)) {
            return Failure(
                CoordinatePoolRemotePlanError::MissingField,
                "remote plan scalar field is missing or invalid");
        }
        if (poolPointerOffset < std::numeric_limits<std::int32_t>::min() ||
            poolPointerOffset > std::numeric_limits<std::int32_t>::max() ||
            poolPointerOffset == 0 ||
            ringOffset > std::numeric_limits<std::uint32_t>::max()) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidField,
                "remote plan offset is out of range");
        }
        if (entryStart != duplicateEntryStart || entryStart != expectedEntry ||
            !containsInstruction(entryStart) ||
            !containsInstruction(v87End) ||
            !containsInstruction(searchEnd) ||
            !containsInstruction(parameterEnd) ||
            v87End <= entryStart || searchEnd <= entryStart ||
            parameterEnd <= entryStart) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidAddress,
                "remote plan execution address is invalid");
        }
        if (v87Register > std::numeric_limits<unsigned int>::max() ||
            searchRegister > std::numeric_limits<unsigned int>::max() ||
            !IsCaptureRegisterValid(static_cast<arm64_reg>(v87Register)) ||
            !IsCaptureRegisterValid(static_cast<arm64_reg>(searchRegister))) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidField,
                "remote plan capture register is invalid");
        }
        plan.indexOffset = indexOffset;
        plan.poolPointerOffset = static_cast<std::int32_t>(poolPointerOffset);
        plan.ringOffset = static_cast<std::uint32_t>(ringOffset);
        plan.entryStart = entryStart;
        plan.v87End = v87End;
        plan.v87Register = static_cast<arm64_reg>(v87Register);
        plan.searchEnd = searchEnd;
        plan.searchRegister = static_cast<arm64_reg>(searchRegister);
        plan.parameterEnd = parameterEnd;

        const Json* memoryParameters = Field(*data, "J");
        if (memoryParameters == nullptr || !memoryParameters->is_array() ||
            memoryParameters->size() > kMaximumParameterCount) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidParameter,
                "remote memory parameter list is invalid");
        }
        std::set<std::string> parameterNames;
        for (const Json& item : *memoryParameters) {
            dec::param parameter{};
            std::uint64_t size = 0;
            std::int64_t displacement = 0;
            if (!ReadStringField(item, "A", parameter.name, kMaximumNameBytes) ||
                !IsPlanNameValid(parameter.name) ||
                !ReadUnsignedField(item, "B", size) ||
                !ReadSignedField(item, "D", displacement) ||
                size == 0 || size > sizeof(parameter.value) ||
                displacement < std::numeric_limits<std::int32_t>::min() ||
                displacement > std::numeric_limits<std::int32_t>::max() ||
                !parameterNames.insert(parameter.name).second) {
                return Failure(
                    CoordinatePoolRemotePlanError::InvalidParameter,
                    "remote memory parameter is invalid");
            }
            const Json* offsets = Field(item, "C");
            if (offsets == nullptr || !offsets->is_array() ||
                offsets->size() > kMaximumParameterOffsets) {
                return Failure(
                    CoordinatePoolRemotePlanError::InvalidParameter,
                    "remote memory parameter offsets are invalid");
            }
            for (const Json& offset : *offsets) {
                std::int64_t value = 0;
                if (!ReadSigned(offset, value) ||
                    value < std::numeric_limits<std::int32_t>::min() ||
                    value > std::numeric_limits<std::int32_t>::max()) {
                    return Failure(
                        CoordinatePoolRemotePlanError::InvalidParameter,
                        "remote memory parameter offset is invalid");
                }
                parameter.offset.push_back(static_cast<std::int32_t>(value));
            }
            parameter.size = static_cast<std::uint32_t>(size);
            parameter.disp = static_cast<std::int32_t>(displacement);
            parameter.value = 0;
            plan.memoryParameters.push_back(std::move(parameter));
        }

        const Json* variableParameters = Field(*data, "K");
        if (variableParameters == nullptr || !variableParameters->is_array() ||
            variableParameters->size() > kMaximumParameterCount) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidParameter,
                "remote variable parameter list is invalid");
        }
        for (const Json& item : *variableParameters) {
            pool::VarParam parameter{};
            std::uint64_t address = 0;
            std::uint64_t registerValue = 0;
            std::uint64_t value = 0;
            if (!ReadStringField(item, "A", parameter.name, kMaximumNameBytes) ||
                !IsPlanNameValid(parameter.name) ||
                !parameterNames.insert(parameter.name).second ||
                !ReadUnsignedField(item, "B", address) ||
                !ReadUnsignedField(item, "C", registerValue) ||
                !ReadUnsignedField(item, "D", value) ||
                !containsInstruction(address) || address > parameterEnd ||
                registerValue > std::numeric_limits<unsigned int>::max() ||
                !IsCaptureRegisterValid(
                    static_cast<arm64_reg>(registerValue))) {
                return Failure(
                    CoordinatePoolRemotePlanError::InvalidParameter,
                    "remote variable parameter is invalid");
            }
            parameter.addr = address;
            parameter.reg = static_cast<arm64_reg>(registerValue);
            parameter.value = value;
            plan.variableParameters.push_back(std::move(parameter));
        }

        std::string expressionBase64;
        if (!ReadStringField(
                *data,
                "L",
                expressionBase64,
                kMaximumExpressionBase64Bytes) ||
            !ReadStringField(
                *data,
                "M",
                plan.ringIndexParameter,
                kMaximumNameBytes) ||
            !IsPlanNameValid(plan.ringIndexParameter) ||
            parameterNames.find(plan.ringIndexParameter) !=
                parameterNames.end()) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidExpression,
                "remote index expression metadata is invalid");
        }
        std::vector<std::uint8_t> expressionBytes;
        if (!DecodeBase64(expressionBase64, expressionBytes)) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidExpression,
                "remote index expression encoding is invalid");
        }
        std::unordered_map<std::string, const dec::param*> memoryLookup;
        memoryLookup.reserve(plan.memoryParameters.size());
        for (const dec::param& parameter : plan.memoryParameters) {
            memoryLookup.emplace(parameter.name, &parameter);
        }
        ExpressionReader expressionReader(
            expressionBytes, memoryLookup, plan.ringIndexParameter);
        plan.indexExpression = expressionReader.Read();
        if (!plan.indexExpression) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidExpression,
                expressionReader.Detail().empty()
                    ? "remote index expression is invalid"
                    : expressionReader.Detail());
        }
        std::set<std::string> dependencies;
        plan.indexExpression->dependencies(dependencies);
        if (dependencies.erase(plan.ringIndexParameter) != 1U) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidExpression,
                "remote index expression has no ring index parameter");
        }
        for (const std::string& parameterName : parameterNames) {
            dependencies.erase(parameterName);
        }
        if (!dependencies.empty()) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidExpression,
                "remote index expression has unknown dependencies");
        }

        const Json* patches = Field(*data, "P");
        if (patches == nullptr || !patches->is_array() || patches->empty() ||
            patches->size() > kMaximumPatchCount) {
            return Failure(
                CoordinatePoolRemotePlanError::InvalidPatch,
                "remote patch list is invalid");
        }
        std::set<std::uint64_t> patchAddresses;
        for (const Json& item : *patches) {
            if (!item.is_array() || item.size() != 2U) {
                return Failure(
                    CoordinatePoolRemotePlanError::InvalidPatch,
                    "remote patch record is invalid");
            }
            std::uint64_t address = 0;
            std::uint64_t instruction = 0;
            if (!ReadUnsigned(item[0], address) ||
                !ReadUnsigned(item[1], instruction) ||
                !containsInstruction(address) ||
                instruction > std::numeric_limits<std::uint32_t>::max() ||
                !patchAddresses.insert(address).second) {
                return Failure(
                    CoordinatePoolRemotePlanError::InvalidPatch,
                    "remote patch record is out of range");
            }
            plan.patches.push_back({
                address,
                static_cast<std::uint32_t>(instruction),
            });
        }

        CoordinatePoolRemotePlanResult result;
        result.plan = std::move(plan);
        return result;
    } catch (const std::exception& exception) {
        return Failure(
            CoordinatePoolRemotePlanError::InvalidJson,
            exception.what());
    } catch (...) {
        return Failure(
            CoordinatePoolRemotePlanError::InvalidJson,
            "remote plan parsing failed");
    }
}

}  // namespace lengjing::game::native
