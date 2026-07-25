#pragma once

#include "game/native/coordinate_pool_internal/FindDec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lengjing::game::native {

enum class CoordinatePoolRemotePlanError : std::uint8_t {
    None,
    EmptyPayload,
    PayloadTooLarge,
    InvalidJson,
    RemoteFailure,
    MissingField,
    InvalidField,
    InvalidAddress,
    InvalidParameter,
    InvalidExpression,
    InvalidPatch,
};

struct CoordinatePoolRemotePlanResult {
    CoordinatePoolRemotePlanError error =
        CoordinatePoolRemotePlanError::None;
    std::string detail;
    coordinate_pool_internal::coord_dec::RuntimePlan plan;

    bool Ok() const noexcept {
        return error == CoordinatePoolRemotePlanError::None;
    }
};

inline constexpr std::size_t kMaximumCoordinatePoolRemotePlanPayloadBytes =
    512U * 1024U;

CoordinatePoolRemotePlanResult ParseCoordinatePoolRemotePlan(
    std::string_view payload,
    std::uint64_t mappingBase,
    std::size_t mappingSize,
    std::uint64_t expectedEntry) noexcept;

}  // namespace lengjing::game::native
