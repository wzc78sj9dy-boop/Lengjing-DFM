#include "test_support.h"

#include "game/FrameRetentionPolicy.h"
#include "game/GameRuntime.h"

#include <atomic>
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
    std::atomic_bool aimEnabled{false};
    std::atomic_bool frameReady{true};
};

class FakeBackend final : public lengjing::game::GameBackend {
public:
    explicit FakeBackend(std::shared_ptr<BackendState> state)
        : state_(std::move(state)) {}

    bool Open(const lengjing::game::RuntimeOptions&,
              lengjing::game::RuntimeProbe& probe,
              std::string&) override {
        ++state_->opens;
        probe.processId = 42;
        probe.baseReady = true;
        return true;
    }

    void Close() noexcept override {
        ++state_->closes;
    }

    bool ReadFrame(const lengjing::game::FeatureSettings& settings,
                   lengjing::game::GameFrame& frame,
                   lengjing::game::RuntimeProbe&,
                   std::string& error) override {
        frame.ready = state_->frameReady.load();
        frame.playerCount =
            frame.ready && settings.visual.enabled ? 3 : 0;
        error = frame.ready ? std::string{} : "数据链等待：测试帧未就绪";
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
    runtime.UpdateSettings(settings);

    lengjing::game::RuntimeOptions options;
    REQUIRE(runtime.Start(options));
    REQUIRE(!runtime.Start(options));
    REQUIRE(WaitFor([&] {
        return runtime.Status().phase == lengjing::game::RuntimePhase::Running;
    }));
    REQUIRE(WaitFor([&] {
        const auto frame = runtime.LatestFrame();
        return frame->sequence >= 2 && frame->playerCount == 3;
    }));
    REQUIRE(runtime.Status().processId == 42);
    REQUIRE(runtime.Status().baseReady);

    state->frameReady.store(false);
    REQUIRE(WaitFor([&] {
        return runtime.Status().message ==
            "数据链等待：测试帧未就绪";
    }));
    const auto retainedFrame = runtime.LatestFrame();
    const std::uint64_t retainedSequence = retainedFrame->sequence;
    state->frameReady.store(true);
    REQUIRE(WaitFor([&] {
        const auto frame = runtime.LatestFrame();
        return frame->sequence > retainedSequence && frame->playerCount == 3;
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

    runtime.Stop();
    runtime.WaitUntilStopped();
    REQUIRE(runtime.Status().phase == lengjing::game::RuntimePhase::Stopped);
    REQUIRE(state->opens.load() == 1);
    REQUIRE(state->closes.load() == 1);
    REQUIRE(!state->aimEnabled.load());
    REQUIRE(state->reads.load() >= 2);
}
