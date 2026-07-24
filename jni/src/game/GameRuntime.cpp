#include "game/GameRuntime.h"
#include "game/FrameRetentionPolicy.h"
#include "game/aim/AimModePolicy.h"
#include "platform/PerformanceTrace.h"

#include <chrono>
#include <exception>
#include <utility>

#ifndef LENGJING_ENABLE_ALGORITHM_COORDINATE
#define LENGJING_ENABLE_ALGORITHM_COORDINATE 0
#endif

namespace lengjing::game {
namespace {

constexpr auto kDataFrameInterval = std::chrono::nanoseconds(
    1'000'000'000LL / 120LL);

void EnsureRuntimeFailure(
    RuntimeProbe& probe,
    RuntimeError fallback) noexcept {
    if (probe.runtimeError == RuntimeError::None) {
        probe.runtimeError = fallback;
        probe.runtimeSystemError = 0;
    }
}

}  // namespace

GameRuntime::GameRuntime(std::unique_ptr<GameBackend> backend)
    : backend_(std::move(backend)),
      latestFrame_(std::make_shared<GameFrame>()) {}

GameRuntime::~GameRuntime() {
    Stop();
    WaitUntilStopped();
}

bool GameRuntime::Start(const RuntimeOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend_ == nullptr || worker_.joinable() ||
        status_.phase == RuntimePhase::Starting ||
        status_.phase == RuntimePhase::Running ||
        status_.phase == RuntimePhase::Stopping) {
        return false;
    }

    stopRequested_.store(false, std::memory_order_release);
    requestedScreenWidth_.store(
        options.screenWidth, std::memory_order_release);
    requestedScreenHeight_.store(
        options.screenHeight, std::memory_order_release);
    requestedOrientation_.store(
        options.orientation, std::memory_order_release);
    displayGeometryDirty_.store(false, std::memory_order_release);
    status_ = RuntimeStatus{};
    status_.phase = RuntimePhase::Starting;
    latestFrame_ = std::make_shared<GameFrame>();
    worker_ = std::thread(&GameRuntime::WorkerMain, this, options);
    return true;
}

void GameRuntime::Stop() {
    stopRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.phase == RuntimePhase::Starting ||
            status_.phase == RuntimePhase::Running) {
            status_.phase = RuntimePhase::Stopping;
            status_.message.clear();
        }
    }
    stopCondition_.notify_all();
}

void GameRuntime::WaitUntilStopped() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable()) return;
        worker = std::move(worker_);
    }
    worker.join();
}

void GameRuntime::UpdateSettings(const FeatureSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    settings_.aim.trajectoryTracking =
        IsProjectileTrackingRequested(settings.aim.trajectoryTracking);
}

void GameRuntime::UpdateDisplayGeometry(
    int width, int height, int orientation) {
    if (width <= 1 || height <= 1) return;
    requestedScreenWidth_.store(width, std::memory_order_release);
    requestedScreenHeight_.store(height, std::memory_order_release);
    requestedOrientation_.store(
        ((orientation % 4) + 4) % 4, std::memory_order_release);
    displayGeometryDirty_.store(true, std::memory_order_release);
}

void GameRuntime::SetAimEnabled(bool enabled) {
    requestedAimEnabled_.store(enabled, std::memory_order_release);
    aimStateDirty_.store(true, std::memory_order_release);
}

void GameRuntime::ReloadCustomItems() {
    reloadItemsRequested_.store(true, std::memory_order_release);
}

RuntimeStatus GameRuntime::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::shared_ptr<const GameFrame> GameRuntime::LatestFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latestFrame_;
}

void GameRuntime::WorkerMain(RuntimeOptions options) {
    RuntimeProbe probe{};
    std::string error;
    try {
        if (!backend_->Open(options, probe, error)) {
            EnsureRuntimeFailure(probe, RuntimeError::BackendOpenFailed);
            SetStatus(RuntimePhase::Stopping, probe, error);
            CloseBackendUntilSafe();
            SetStatus(RuntimePhase::Faulted, probe, std::move(error));
            return;
        }

        backend_->SetAimEnabled(
            requestedAimEnabled_.load(std::memory_order_acquire));
        aimStateDirty_.store(false, std::memory_order_release);
        SetStatus(RuntimePhase::Running, probe);

        std::uint64_t sequence = 0;
        bool hasReadyFrame = false;
        FrameClock::time_point lastReadyFrameAt{};
        auto nextFrameAt = FrameClock::now();
        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (displayGeometryDirty_.exchange(
                    false, std::memory_order_acq_rel)) {
                backend_->UpdateDisplayGeometry(
                    requestedScreenWidth_.load(std::memory_order_acquire),
                    requestedScreenHeight_.load(std::memory_order_acquire),
                    requestedOrientation_.load(std::memory_order_acquire));
            }
            if (aimStateDirty_.exchange(false, std::memory_order_acq_rel)) {
                backend_->SetAimEnabled(
                    requestedAimEnabled_.load(std::memory_order_acquire));
            }
            if (reloadItemsRequested_.exchange(
                    false, std::memory_order_acq_rel)) {
                backend_->ReloadCustomItems();
            }

            FeatureSettings settings;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                settings = settings_;
            }

            auto frame = std::make_shared<GameFrame>();
            frame->sequence = ++sequence;
            error.clear();
            platform::PerformanceTraceScope dataFrameTrace(
                platform::PerformancePhase::DataFrame);
            platform::RecordPerformanceCount(
                platform::PerformanceCounter::DataFrames);
            if (settings.visual.coordinateDecrypt) {
                platform::RecordPerformanceCount(
                    platform::PerformanceCounter::CoordinateFrames);
            }
            const bool frameRead =
                backend_->ReadFrame(settings, *frame, probe, error);
            dataFrameTrace.Finish();
            if (!frameRead) {
                if (stopRequested_.load(std::memory_order_acquire)) break;
                EnsureRuntimeFailure(
                    probe, RuntimeError::BackendReadFailed);
                SetStatus(RuntimePhase::Stopping, probe, error);
                CloseBackendUntilSafe();
                SetStatus(RuntimePhase::Faulted, probe, std::move(error));
                return;
            }
            if (!error.empty() &&
                probe.coordinateError == CoordinateDecryptError::None) {
                EnsureRuntimeFailure(
                    probe, RuntimeError::FrameDataUnavailable);
            }

            if (frame->ready) {
                platform::RecordPerformanceCount(
                    platform::PerformanceCounter::ReadyFrames);
            }
            platform::RecordPerformanceCount(
                platform::PerformanceCounter::OutputPlayers,
                frame->players.size());

            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto frameTime = FrameClock::now();
                if (frame->ready) {
                    latestFrame_ = std::move(frame);
                    lastReadyFrameAt = frameTime;
                    hasReadyFrame = true;
                } else if (ShouldPublishWaitingFrame(
                               hasReadyFrame,
                               frameTime - lastReadyFrameAt)) {
                    frame->ready = true;
                    latestFrame_ = std::move(frame);
                }
                status_.processId = probe.processId;
                status_.baseReady = probe.baseReady;
                status_.customItemCount = probe.customItemCount;
                status_.coordinateRequested = probe.coordinateRequested;
                status_.coordinateEntryReady = probe.coordinateEntryReady;
                status_.coordinateContextReady = probe.coordinateContextReady;
                status_.coordinateThreadId = probe.coordinateThreadId;
                status_.coordinateGuestPc = probe.coordinateGuestPc;
                status_.coordinateContextGeneration =
                    probe.coordinateContextGeneration;
                status_.coordinateAttempts = probe.coordinateAttempts;
                status_.coordinateSuccesses = probe.coordinateSuccesses;
                status_.coordinateError = probe.coordinateError;
                status_.coordinateSystemError =
                    probe.coordinateSystemError;
                status_.coordinateRead = probe.coordinateRead;
                status_.coordinatePoolPointer =
                    probe.coordinatePoolPointer;
                status_.coordinateEntry = probe.coordinateEntry;
                status_.coordinateRuntimeDetail =
                    probe.coordinateRuntimeDetail;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
                status_.algorithmCoordinateRequested =
                    probe.algorithmCoordinateRequested;
                status_.algorithmCoordinateActive =
                    probe.algorithmCoordinateActive;
                status_.algorithmCoordinateTableReady =
                    probe.algorithmCoordinateTableReady;
                status_.algorithmCoordinateRuntimeReady =
                    probe.algorithmCoordinateRuntimeReady;
                status_.algorithmCoordinateRefreshes =
                    probe.algorithmCoordinateRefreshes;
                status_.algorithmCoordinateResolveAttempts =
                    probe.algorithmCoordinateResolveAttempts;
                status_.algorithmCoordinateResolveSuccesses =
                    probe.algorithmCoordinateResolveSuccesses;
                status_.algorithmCoordinateAttempts =
                    probe.algorithmCoordinateAttempts;
                status_.algorithmCoordinateSuccesses =
                    probe.algorithmCoordinateSuccesses;
                status_.algorithmCoordinateObjectAttempts =
                    probe.algorithmCoordinateObjectAttempts;
                status_.algorithmCoordinateObjectSuccesses =
                    probe.algorithmCoordinateObjectSuccesses;
                status_.algorithmCoordinateTableAttempts =
                    probe.algorithmCoordinateTableAttempts;
                status_.algorithmCoordinateTableSuccesses =
                    probe.algorithmCoordinateTableSuccesses;
                status_.algorithmCoordinateFallbacks =
                    probe.algorithmCoordinateFallbacks;
                status_.algorithmCoordinateSource =
                    probe.algorithmCoordinateSource;
                status_.algorithmCoordinate = probe.algorithmCoordinate;
                status_.algorithmCoordinateRuntime =
                    probe.algorithmCoordinateRuntime;
#endif
                status_.runtimeError = probe.runtimeError;
                status_.runtimeSystemError = probe.runtimeSystemError;
                status_.failureKind = probe.failureKind;
                status_.message = std::move(error);
            }

            platform::PublishPerformanceTrace();

            const auto now = FrameClock::now();
            do {
                nextFrameAt += kDataFrameInterval;
            } while (nextFrameAt <= now);
            std::unique_lock<std::mutex> lock(mutex_);
            stopCondition_.wait_until(lock, nextFrameAt, [this] {
                return stopRequested_.load(std::memory_order_acquire);
            });
        }
    } catch (const std::exception& exception) {
        const std::string message = exception.what();
        probe.runtimeError = RuntimeError::StandardException;
        probe.runtimeSystemError = 0;
        SetStatus(RuntimePhase::Stopping, probe, message);
        CloseBackendUntilSafe();
        SetStatus(RuntimePhase::Faulted, probe, message);
        return;
    } catch (...) {
        const std::string message = "运行模块发生未知错误";
        probe.runtimeError = RuntimeError::UnknownException;
        probe.runtimeSystemError = 0;
        SetStatus(RuntimePhase::Stopping, probe, message);
        CloseBackendUntilSafe();
        SetStatus(RuntimePhase::Faulted, probe, message);
        return;
    }

    backend_->SetAimEnabled(false);
    CloseBackendUntilSafe();
    SetStatus(RuntimePhase::Stopped, RuntimeProbe{});
}

void GameRuntime::CloseBackendUntilSafe() noexcept {
    using namespace std::chrono_literals;
    while (!backend_->Close()) {
        std::this_thread::sleep_for(10ms);
    }
}

void GameRuntime::SetStatus(RuntimePhase phase,
                            const RuntimeProbe& probe,
                            std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.phase = phase;
    status_.processId = probe.processId;
    status_.baseReady = probe.baseReady;
    status_.customItemCount = probe.customItemCount;
    status_.coordinateRequested = probe.coordinateRequested;
    status_.coordinateEntryReady = probe.coordinateEntryReady;
    status_.coordinateContextReady = probe.coordinateContextReady;
    status_.coordinateThreadId = probe.coordinateThreadId;
    status_.coordinateGuestPc = probe.coordinateGuestPc;
    status_.coordinateContextGeneration = probe.coordinateContextGeneration;
    status_.coordinateAttempts = probe.coordinateAttempts;
    status_.coordinateSuccesses = probe.coordinateSuccesses;
    status_.coordinateError = probe.coordinateError;
    status_.coordinateSystemError = probe.coordinateSystemError;
    status_.coordinateRead = probe.coordinateRead;
    status_.coordinatePoolPointer = probe.coordinatePoolPointer;
    status_.coordinateEntry = probe.coordinateEntry;
    status_.coordinateRuntimeDetail = probe.coordinateRuntimeDetail;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    status_.algorithmCoordinateRequested =
        probe.algorithmCoordinateRequested;
    status_.algorithmCoordinateActive = probe.algorithmCoordinateActive;
    status_.algorithmCoordinateTableReady =
        probe.algorithmCoordinateTableReady;
    status_.algorithmCoordinateRuntimeReady =
        probe.algorithmCoordinateRuntimeReady;
    status_.algorithmCoordinateRefreshes =
        probe.algorithmCoordinateRefreshes;
    status_.algorithmCoordinateResolveAttempts =
        probe.algorithmCoordinateResolveAttempts;
    status_.algorithmCoordinateResolveSuccesses =
        probe.algorithmCoordinateResolveSuccesses;
    status_.algorithmCoordinateAttempts =
        probe.algorithmCoordinateAttempts;
    status_.algorithmCoordinateSuccesses =
        probe.algorithmCoordinateSuccesses;
    status_.algorithmCoordinateObjectAttempts =
        probe.algorithmCoordinateObjectAttempts;
    status_.algorithmCoordinateObjectSuccesses =
        probe.algorithmCoordinateObjectSuccesses;
    status_.algorithmCoordinateTableAttempts =
        probe.algorithmCoordinateTableAttempts;
    status_.algorithmCoordinateTableSuccesses =
        probe.algorithmCoordinateTableSuccesses;
    status_.algorithmCoordinateFallbacks =
        probe.algorithmCoordinateFallbacks;
    status_.algorithmCoordinateSource = probe.algorithmCoordinateSource;
    status_.algorithmCoordinate = probe.algorithmCoordinate;
    status_.algorithmCoordinateRuntime =
        probe.algorithmCoordinateRuntime;
#endif
    status_.runtimeError = probe.runtimeError;
    status_.runtimeSystemError = probe.runtimeSystemError;
    status_.failureKind = probe.failureKind;
    status_.message = std::move(message);
}

}  // namespace lengjing::game
