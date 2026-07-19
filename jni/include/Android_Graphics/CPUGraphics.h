//
// CPU 软件绘制后端。参考来源及逆向证据记录在项目外的分析报告中。
//
// 架构(与 Zenith 反编译一致):
//   1. 双缓冲 uint32(BGRA/RGBA8888)像素缓冲:前台 m_Buffer 负责光栅化,
//      后台 SubmitState.buffer 负责提交,二者在渲染锁下交换。
//   2. RenderPool:固定 4 个 worker 线程,每帧把脏矩形按行切成 4 个分带
//      并行光栅化,主线程用 wait_for_ms 做看门狗等待。
//   3. 提交线程:独占 ANativeWindow,锁定 Surface 把渲染好的帧拷入并 post,
//      与光栅化解耦,避免被 vsync/Surface 锁拖慢主流程。
//

#ifndef LENGJING_CPU_GRAPHICS_H
#define LENGJING_CPU_GRAPHICS_H

#include <android/native_window.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "AndroidImgui.h"
#include "CPURasterRules.h"

// 固定 4 线程光栅化线程池:一次提交 4 个分带任务并行执行
class RenderPool {
public:
    static constexpr int kWorkers = 4;

    RenderPool();
    ~RenderPool();

    // 提交 4 个分带光栅化任务并唤醒全部 worker
    void run(std::function<void()> t0, std::function<void()> t1,
             std::function<void()> t2, std::function<void()> t3);

    // 最多等待 ms 毫秒,直到 4 个任务全部完成;超时返回 false
    bool wait_for_ms(int ms);

private:
    std::thread             m_Threads[kWorkers];
    std::mutex              m_Mutex;
    std::condition_variable m_CondWork;   // 唤醒 worker 干活
    std::condition_variable m_CondDone;   // 通知主线程已全部完成
    std::function<void()>   m_Tasks[kWorkers];
    int                     m_Generation = 0;          // 任务批次号
    int                     m_Seen[kWorkers] = {0};    // 各 worker 已处理到的批次
    std::atomic<int>        m_Done{0};                 // 本批已完成任务数
    bool                    m_Alive = true;

    void worker(int index);
};

// 提交线程与光栅线程之间交换的后台缓冲快照
struct SubmitState {
    std::mutex              m_Mutex;
    std::condition_variable m_Cond;
    std::vector<uint32_t>   buffer;                       // 后台像素(已渲染待提交)
    int  m_BufW = 0, m_BufH = 0;                          // 后台缓冲尺寸
    lengjing::render::cpu::PixelRect m_ContentRect;        // buffer 当前内容范围
    lengjing::render::cpu::PixelRect m_DirtyRect;          // 本次 Surface 提交范围
    bool m_HasWork = false;                               // 有新帧待提交
    std::atomic<bool>    m_Running{false};                // 提交线程存活标志
    std::atomic<bool>    m_Exited{true};                   // 线程函数已完全退出
    std::atomic<bool>    m_InLock{false};                 // 正在 ANativeWindow_lock
    std::atomic<bool>    m_InPost{false};                 // 正在 ANativeWindow_unlockAndPost
    std::atomic<int64_t> m_LastPostMs{0};                 // 上次 post 时间(ms)
    std::atomic<int64_t> m_RetryAfterMs{0};               // 连续失败后的线程重启时间
    ANativeWindow*       m_Window = nullptr;
    std::shared_ptr<lengjing::render::PresentationRateTracker>
        m_PresentationRate;
    std::shared_ptr<std::atomic_bool> m_SurfaceRecoveryRequested;
};

class CPUGraphics final : public AndroidImgui {
private:
    struct FrameScratch;

    // 加载纹理:整张 RGBA8888 像素
    struct CpuTextureData : BaseTexData {
        std::vector<uint32_t> pixels;
    };

    int m_BufWidth = 0;
    int m_BufHeight = 0;
    // 元数据随像素缓冲轮转；提交范围只由相邻画面内容求并集。
    lengjing::render::cpu::PixelRect m_BufferContentRect;
    lengjing::render::cpu::PixelRect m_LastSubmittedContentRect;

    std::vector<uint32_t> m_Buffer;                  // 前台像素缓冲

    std::shared_ptr<SubmitState> m_Submit;           // 提交线程共享状态
    std::thread m_SubmitThread;
    std::mutex  m_ThreadMutex;                        // 保护提交线程启停
    std::mutex  m_WindowMutex;                        // 保护 m_Window 访问
    std::mutex  m_RenderMutex;                        // 保护前台缓冲/脏矩形
    std::atomic<bool> m_Abort{false};                 // 看门狗中止标志
    bool m_SurfaceReady = false;                      // geometry 已成功配置
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
    void Render(ImDrawData* drawData) override;
    void PrepareShutdown() override;
    void Cleanup() override;
    BaseTexData* LoadTexture(BaseTexData* tex_data, void* pixel_data) override;
    void RemoveTexture(BaseTexData* tex_data) override;
};

#endif
