#pragma once

#include "game\GameBackend.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace lengjing::game {

enum class RuntimePhase : std::uint8_t {
    Stopped,
    Starting,
    Running,
    Stopping,
    Faulted,
};

struct RuntimeStatus {
    RuntimePhase phase = RuntimePhase::Stopped;
    int processId = 0;
    bool baseReady = false;
    std::size_t customItemCount = 0;
    std::string message;
};

class GameRuntime final {
public:
    explicit GameRuntime(std::unique_ptr<GameBackend> backend);
    ~GameRuntime();

    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;

    bool Start(const RuntimeOptions& options);
    void Stop();
    void WaitUntilStopped();

    void UpdateSettings(const FeatureSettings& settings);
    void UpdateDisplayGeometry(int width, int height, int orientation);
    void SetAimEnabled(bool enabled);
    void ReloadCustomItems();

    RuntimeStatus Status() const;
    std::shared_ptr<const GameFrame> LatestFrame() const;

private:
    void WorkerMain(RuntimeOptions options);
    void SetStatus(RuntimePhase phase,
                   const RuntimeProbe& probe,
                   std::string message = {});

    std::unique_ptr<GameBackend> backend_;
    mutable std::mutex mutex_;
    std::condition_variable stopCondition_;
    std::thread worker_;
    FeatureSettings settings_{};
    RuntimeStatus status_{};
    std::shared_ptr<const GameFrame> latestFrame_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool requestedAimEnabled_{false};
    std::atomic_bool aimStateDirty_{false};
    std::atomic_int requestedScreenWidth_{0};
    std::atomic_int requestedScreenHeight_{0};
    std::atomic_int requestedOrientation_{0};
    std::atomic_bool displayGeometryDirty_{false};
    std::atomic_bool reloadItemsRequested_{false};
};

}  // namespace lengjing::game
