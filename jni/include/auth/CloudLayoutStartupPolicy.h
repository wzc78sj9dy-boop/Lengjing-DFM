#pragma once

namespace lengjing::auth {

inline constexpr int kCloudLayoutStartupFailureExitCode = 3;

enum class CloudLayoutStartupAction {
    FetchCloudLayout,
    UseCloudLayout,
    StopStartup,
};

constexpr CloudLayoutStartupAction ResolveCloudLayoutStartupAction(
    bool configurationComplete,
    bool refreshAttempted,
    bool refreshSucceeded,
    bool snapshotAvailable) noexcept {
    if (!configurationComplete) {
        return CloudLayoutStartupAction::StopStartup;
    }
    if (!refreshAttempted) {
        return CloudLayoutStartupAction::FetchCloudLayout;
    }
    if (!refreshSucceeded || !snapshotAvailable) {
        return CloudLayoutStartupAction::StopStartup;
    }
    return CloudLayoutStartupAction::UseCloudLayout;
}

}  // namespace lengjing::auth
