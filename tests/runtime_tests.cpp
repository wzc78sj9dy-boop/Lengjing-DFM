#include "test_support.h"

#include "app/RuntimeExitPolicy.h"
#include "game/FrameRetentionPolicy.h"
#include "game/GameRuntime.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct BackendState {
    std::atomic_int opens{0};
    std::atomic_int closes{0};
    std::atomic_int reads{0};
    std::atomic_int reloads{0};
    std::atomic_int screenWidth{0};
    std::atomic_int screenHeight{0};
    std::atomic_int orientation{0};
    std::atomic_int closeFailuresRemaining{0};
    std::atomic_uintptr_t algorithmDecryptRva{0};
    std::atomic_bool aimEnabled{false};
    std::atomic_bool selfAimSetting{false};
    std::atomic_bool projectileTrackingSetting{false};
    std::atomic_bool frameReady{true};
    std::atomic<std::uint16_t> coordinateError{0};
    std::atomic_int coordinateSystemError{0};
};

class FakeBackend final : public lengjing::game::GameBackend {
public:
    explicit FakeBackend(std::shared_ptr<BackendState> state)
        : state_(std::move(state)) {}

    bool Open(const lengjing::game::RuntimeOptions& options,
              lengjing::game::RuntimeProbe& probe,
              std::string&) override {
        ++state_->opens;
        state_->algorithmDecryptRva.store(
            options.algorithmPosition.decryptRva);
        probe.processId = 42;
        probe.baseReady = true;
        return true;
    }

    bool Close() noexcept override {
        ++state_->closes;
        int remaining = state_->closeFailuresRemaining.load();
        while (remaining > 0) {
            if (state_->closeFailuresRemaining.compare_exchange_weak(
                    remaining, remaining - 1)) {
                return false;
            }
        }
        return true;
    }

    bool ReadFrame(const lengjing::game::FeatureSettings& settings,
                   lengjing::game::GameFrame& frame,
                   lengjing::game::RuntimeProbe& probe,
                   std::string& error) override {
        frame.ready = state_->frameReady.load();
        frame.playerCount =
            frame.ready && settings.visual.enabled ? 3 : 0;
        state_->selfAimSetting.store(settings.aim.enabled);
        state_->projectileTrackingSetting.store(
            settings.aim.trajectoryTracking);
        error = frame.ready ? std::string{} : "waiting";
        probe.coordinateError =
            static_cast<lengjing::game::CoordinateDecryptError>(
                state_->coordinateError.load());
        probe.coordinateSystemError =
            state_->coordinateSystemError.load();
        ++state_->reads;
        return true;
    }

    void SetAimEnabled(bool enabled) override {
        state_->aimEnabled.store(enabled);
    }

    void UpdateDisplayGeometry(
        int width, int height, int orientation) override {
        state_->screenWidth.store(width);
        state_->screenHeight.store(height);
        state_->orientation.store(orientation);
    }

    void ReloadCustomItems() override {
        ++state_->reloads;
    }

private:
    std::shared_ptr<BackendState> state_;
};

class OpenFailureBackend final : public lengjing::game::GameBackend {
public:
    explicit OpenFailureBackend(lengjing::game::RuntimeFailureKind kind)
        : kind_(kind) {}

    bool Open(const lengjing::game::RuntimeOptions&,
              lengjing::game::RuntimeProbe& probe,
              std::string& error) override {
        probe.failureKind = kind_;
        error = kind_ == lengjing::game::RuntimeFailureKind::CloudLayoutRejected
            ? "cloud layout rejected"
            : "ordinary open failure";
        return false;
    }

    bool Close() noexcept override {
        return true;
    }

    bool ReadFrame(const lengjing::game::FeatureSettings&,
                   lengjing::game::GameFrame&,
                   lengjing::game::RuntimeProbe&,
                   std::string&) override {
        return false;
    }

    void SetAimEnabled(bool) override {}
    void UpdateDisplayGeometry(int, int, int) override {}
    void ReloadCustomItems() override {}

private:
    lengjing::game::RuntimeFailureKind kind_;
};

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 1000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

}  // namespace

void RunRuntimeTests() {
    REQUIRE(lengjing::game::ShouldPublishWaitingFrame(false, 0ms));
    REQUIRE(!lengjing::game::ShouldPublishWaitingFrame(true, 249ms));
    REQUIRE(lengjing::game::ShouldPublishWaitingFrame(true, 250ms));

    auto state = std::make_shared<BackendState>();
    lengjing::game::GameRuntime runtime(std::make_unique<FakeBackend>(state));

    lengjing::game::FeatureSettings settings;
    settings.visual.enabled = true;
    settings.aim.enabled = true;
    settings.aim.trajectoryTracking = true;
    runtime.UpdateSettings(settings);

    lengjing::game::RuntimeOptions options;
    options.algorithmPosition.decryptRva = 0x1234;
    REQUIRE(runtime.Start(options));
    REQUIRE(!runtime.Start(options));
    REQUIRE(WaitFor([&] {
        return runtime.Status().phase ==
            lengjing::game::RuntimePhase::Running;
    }));
    REQUIRE(WaitFor([&] {
        const auto frame = runtime.LatestFrame();
        return frame->sequence >= 2 && frame->playerCount == 3;
    }));
    REQUIRE(runtime.Status().processId == 42);
    REQUIRE(runtime.Status().baseReady);
    REQUIRE(state->algorithmDecryptRva.load() == 0x1234);
    REQUIRE(state->selfAimSetting.load());
    REQUIRE(!state->projectileTrackingSetting.load());

    state->coordinateError.store(
        lengjing::game::CoordinateDecryptErrorCode(
            lengjing::game::CoordinateDecryptError::
                ContextDeviceProtocolMismatch));
    state->coordinateSystemError.store(-EPROTO);
    REQUIRE(WaitFor([&] {
        const lengjing::game::RuntimeStatus status = runtime.Status();
        return status.coordinateError ==
                lengjing::game::CoordinateDecryptError::
                    ContextDeviceProtocolMismatch &&
            status.coordinateSystemError == -EPROTO;
    }));

    state->coordinateError.store(0);
    state->coordinateSystemError.store(0);
    REQUIRE(WaitFor([&] {
        const lengjing::game::RuntimeStatus status = runtime.Status();
        return status.coordinateError ==
                lengjing::game::CoordinateDecryptError::None &&
            status.coordinateSystemError == 0;
    }));

    state->frameReady.store(false);
    REQUIRE(WaitFor([&] {
        return runtime.Status().message == "waiting";
    }));
    const auto retainedFrame = runtime.LatestFrame();
    const std::uint64_t retainedSequence = retainedFrame->sequence;
    state->frameReady.store(true);
    REQUIRE(WaitFor([&] {
        const auto frame = runtime.LatestFrame();
        return frame->sequence > retainedSequence &&
            frame->playerCount == 3;
    }));

    runtime.SetAimEnabled(true);
    runtime.UpdateDisplayGeometry(1080, 2376, 0);
    runtime.ReloadCustomItems();
    REQUIRE(WaitFor([&] { return state->aimEnabled.load(); }));
    REQUIRE(WaitFor([&] {
        return state->screenWidth.load() == 1080 &&
            state->screenHeight.load() == 2376 &&
            state->orientation.load() == 0;
    }));
    REQUIRE(WaitFor([&] { return state->reloads.load() == 1; }));

    state->closeFailuresRemaining.store(2);
    runtime.Stop();
    runtime.WaitUntilStopped();
    REQUIRE(runtime.Status().phase ==
        lengjing::game::RuntimePhase::Stopped);
    REQUIRE(state->opens.load() == 1);
    REQUIRE(state->closes.load() == 3);
    REQUIRE(!state->aimEnabled.load());
    REQUIRE(state->reads.load() >= 2);

    {
        lengjing::game::GameRuntime rejected(
            std::make_unique<OpenFailureBackend>(
                lengjing::game::RuntimeFailureKind::CloudLayoutRejected));
        REQUIRE(rejected.Start({}));
        REQUIRE(WaitFor([&] {
            return rejected.Status().phase ==
                lengjing::game::RuntimePhase::Faulted;
        }));
        const lengjing::game::RuntimeStatus status = rejected.Status();
        REQUIRE(status.failureKind ==
                lengjing::game::RuntimeFailureKind::CloudLayoutRejected);
        REQUIRE(lengjing::app::ResolveRuntimeExitCode(
                    true, status.phase, status.failureKind) ==
                lengjing::auth::kCloudLayoutStartupFailureExitCode);
        REQUIRE(lengjing::app::ResolveRuntimeExitCode(
                    false, status.phase, status.failureKind) == 0);
        rejected.WaitUntilStopped();
    }

    {
        lengjing::game::GameRuntime ordinary(
            std::make_unique<OpenFailureBackend>(
                lengjing::game::RuntimeFailureKind::None));
        REQUIRE(ordinary.Start({}));
        REQUIRE(WaitFor([&] {
            return ordinary.Status().phase ==
                lengjing::game::RuntimePhase::Faulted;
        }));
        const lengjing::game::RuntimeStatus status = ordinary.Status();
        REQUIRE(status.failureKind ==
                lengjing::game::RuntimeFailureKind::None);
        REQUIRE(lengjing::app::ResolveRuntimeExitCode(
                    true, status.phase, status.failureKind) == 0);
        ordinary.WaitUntilStopped();
    }
}
