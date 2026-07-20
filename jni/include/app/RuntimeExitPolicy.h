#pragma once

#include "auth/CloudLayoutStartupPolicy.h"
#include "game/GameRuntime.h"

namespace lengjing::app {

constexpr int ResolveRuntimeExitCode(
    bool cloudLayoutActive,
    game::RuntimePhase phase,
    game::RuntimeFailureKind failureKind) noexcept {
    return cloudLayoutActive && phase == game::RuntimePhase::Faulted &&
            failureKind == game::RuntimeFailureKind::CloudLayoutRejected
        ? auth::kCloudLayoutStartupFailureExitCode
        : 0;
}

}  // namespace lengjing::app
