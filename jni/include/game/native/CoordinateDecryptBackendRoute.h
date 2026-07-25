#pragma once

#include "game/native/CoordinateExecutionRuntime.h"
#include "ui/UiModel.h"

#include <cstdint>

namespace lengjing::game::native {

struct CoordinateDecryptBackendRoute {
    bool coordinateReplay = false;
    bool coordinateDecrypt2 = false;
    CoordinateExecutionMode executionMode =
        static_cast<CoordinateExecutionMode>(0);

    constexpr std::uint8_t ExecutionModeValue() const noexcept {
        return static_cast<std::uint8_t>(executionMode);
    }
};

constexpr CoordinateDecryptBackendRoute ResolveCoordinateDecryptBackendRoute(
    ui::CoordinateDecryptSelection selection,
    bool fallbackExecutionToDecrypt2) noexcept {
    switch (selection) {
        case ui::CoordinateDecryptSelection::Decrypt1:
            return CoordinateDecryptBackendRoute{true, false, {}};
        case ui::CoordinateDecryptSelection::Decrypt2:
            return CoordinateDecryptBackendRoute{false, true, {}};
        case ui::CoordinateDecryptSelection::Decrypt3:
            return fallbackExecutionToDecrypt2
                ? CoordinateDecryptBackendRoute{false, true, {}}
                : CoordinateDecryptBackendRoute{
                      false, false, CoordinateExecutionMode::Emulate};
        case ui::CoordinateDecryptSelection::Decrypt4:
            return fallbackExecutionToDecrypt2
                ? CoordinateDecryptBackendRoute{false, true, {}}
                : CoordinateDecryptBackendRoute{
                      false, false, CoordinateExecutionMode::Interpret};
        case ui::CoordinateDecryptSelection::Decrypt5:
            return fallbackExecutionToDecrypt2
                ? CoordinateDecryptBackendRoute{false, true, {}}
                : CoordinateDecryptBackendRoute{
                      false, false, CoordinateExecutionMode::Predecode};
        case ui::CoordinateDecryptSelection::Decrypt6:
            return fallbackExecutionToDecrypt2
                ? CoordinateDecryptBackendRoute{false, true, {}}
                : CoordinateDecryptBackendRoute{
                      false, false, CoordinateExecutionMode::Jit};
        case ui::CoordinateDecryptSelection::None:
            return {};
    }
    return {};
}

}  // namespace lengjing::game::native
