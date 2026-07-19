#ifndef LENGJING_CPU_GRAPHICS_H
#define LENGJING_CPU_GRAPHICS_H

#include <android/native_window.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AndroidImgui.h"
#include "CPURasterRules.h"

class RenderPool {
public:
    static constexpr int kWorkers = 4;

    RenderPool();
    ~RenderPool();

    void run(std::array<std::function<void()>, kWorkers> tasks);
    bool wait_for_ms(int ms);

private:
    std::thread m_Threads[kWorkers];
    std::mutex m_Mutex;
    std::condition_variable m_CondWork;
    std::condition_variable m_CondDone;
    std::function<void()> m_Tasks[kWorkers];
    int m_Generation = 0;
    int m_Seen[kWorkers] = {0};
    std::atomic<int> m_Done{0};
    bool m_Alive = true;

    void worker(int index);
};

struct SubmitState {
    std::mutex m_Mutex;
    std::condition_variable m_Cond;
    std::vector<uint32_t> buffer;
    int m_BufW = 0;
    int m_BufH = 0;
    lengjing::render::cpu::PixelRect m_ContentRect;
    lengjing::render::cpu::PixelRect m_DirtyRect;
    lengjing::render::cpu::PixelRect m_UpdateRect;
    bool m_HasWork = false;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Exited{true};
    std::atomic<bool> m_InLock{false};
    std::atomic<bool> m_InPost{false};
    std::atomic<bool> m_ForceFullDamage{true};
    std::atomic<int64_t> m_LastPostMs{0};
    std::atomic<int64_t> m_RetryAfterMs{0};
    ANativeWindow* m_Window = nullptr;
    std::shared_ptr<lengjing::render::PresentationRateTracker> m_PresentationRate;
    std::shared_ptr<std::atomic_bool> m_SurfaceRecoveryRequested;
};

class CPUGraphics final : public AndroidImgui {
private:
    struct FrameScratch;

    struct CpuTextureData : BaseTexData {
        std::vector<uint32_t> pixels;
    };

    int m_BufWidth = 0;
    int m_BufHeight = 0;
    lengjing::render::cpu::PixelRect m_BufferContentRect;
    lengjing::render::cpu::PixelRect m_LastSubmittedContentRect;
    std::vector<uint32_t> m_Buffer;
    std::vector<uint64_t> m_BufferGroupHashes;
    std::vector<lengjing::render::cpu::PixelRect> m_BufferGroupBounds;

    std::shared_ptr<SubmitState> m_Submit;
    std::thread m_SubmitThread;
    std::mutex m_ThreadMutex;
    std::mutex m_WindowMutex;
    std::mutex m_RenderMutex;
    std::atomic<bool> m_Abort{false};
    bool m_SurfaceReady = false;
    bool m_ForceCanonicalFullSubmit = true;
    int m_GeometryFailures = 0;
    int64_t m_NextGeometryRetryMs = 0;
    std::shared_ptr<std::atomic_bool> m_SurfaceRecoveryRequested =
        std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<RenderPool> m_Pool;
    std::unique_ptr<FrameScratch> m_FrameScratch;

    bool StartSubmitThread();
    bool StopSubmitThread(bool abandonIfBlocked = false);
    void SubmitLoop(std::shared_ptr<SubmitState> state);

public:
    CPUGraphics();
    ~CPUGraphics() override;

    bool ConsumeSurfaceRecoveryRequest() override;
    bool Create() override;
    bool Setup() override;
    void PrepareFrame(bool resize) override;
    void Render(ImDrawData* draw_data) override;
    void PrepareShutdown() override;
    void Cleanup() override;
    BaseTexData* LoadTexture(BaseTexData* texture, void* pixel_data) override;
    void RemoveTexture(BaseTexData* texture) override;
};

#endif
