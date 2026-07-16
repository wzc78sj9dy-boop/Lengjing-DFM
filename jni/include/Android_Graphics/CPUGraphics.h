#ifndef LENGJING_CPU_GRAPHICS_H
#define LENGJING_CPU_GRAPHICS_H

#include <android/native_window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AndroidImgui.h"

class CpuRasterPool {
public:
    static constexpr int kWorkerCount = 4;

    CpuRasterPool();
    ~CpuRasterPool();

    void Run(std::function<void()> first,
             std::function<void()> second,
             std::function<void()> third,
             std::function<void()> fourth);
    bool WaitFor(std::chrono::milliseconds timeout);

private:
    void WorkerMain(int index);

    std::thread threads_[kWorkerCount];
    std::mutex mutex_;
    std::condition_variable workCondition_;
    std::condition_variable doneCondition_;
    std::function<void()> tasks_[kWorkerCount];
    int generation_ = 0;
    int seenGeneration_[kWorkerCount]{};
    std::atomic<int> doneCount_{0};
    bool alive_ = true;
};

struct CpuSubmitState {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::uint32_t> buffer;
    int bufferWidth = 0;
    int bufferHeight = 0;
    int dirtyMinX = 0;
    int dirtyMinY = 0;
    int dirtyMaxX = 0;
    int dirtyMaxY = 0;
    bool hasWork = false;
    std::atomic<bool> running{false};
    std::atomic<bool> exited{true};
    std::atomic<bool> insideWindowLock{false};
    std::atomic<bool> insideWindowPost{false};
    std::atomic<std::int64_t> lastProgressMs{0};
    std::shared_ptr<lengjing::render::PresentationRateTracker>
        presentationRate;
    ANativeWindow* window = nullptr;
};

class CPUGraphics final : public AndroidImgui {
public:
    CPUGraphics();
    ~CPUGraphics() override;

private:
    struct CpuTextureData : BaseTexData {
        std::vector<std::uint32_t> pixels;
    };

    bool Create() override;
    void Setup() override;
    void PrepareFrame(bool resize) override;
    void Render(ImDrawData* drawData) override;
    void PrepareShutdown() override;
    void Cleanup() override;
    BaseTexData* LoadTexture(BaseTexData* texture, void* pixelData) override;
    void RemoveTexture(BaseTexData* texture) override;

    void RefreshFontTexture();
    void StartSubmitThread();
    void StopSubmitThread();
    void RenderBand(ImDrawData* drawData,
                    std::uint32_t* buffer,
                    int stride,
                    int bandMinY,
                    int dirtyMinX,
                    int dirtyMinY,
                    int bandMaxY,
                    int dirtyMaxX,
                    int dirtyMaxY);

    int bufferWidth_ = 0;
    int bufferHeight_ = 0;
    int previousMinX_ = 0;
    int previousMinY_ = 0;
    int previousMaxX_ = 0;
    int previousMaxY_ = 0;
    std::vector<std::uint32_t> frontBuffer_;

    std::shared_ptr<CpuSubmitState> submitState_;
    std::thread submitThread_;
    std::mutex threadMutex_;
    std::mutex windowMutex_;
    std::mutex renderMutex_;
    std::atomic<bool> abortRaster_{false};
    std::unique_ptr<CpuRasterPool> rasterPool_;

    const std::uint8_t* fontPixels_ = nullptr;
    int fontWidth_ = 0;
    int fontHeight_ = 0;
};

#endif
