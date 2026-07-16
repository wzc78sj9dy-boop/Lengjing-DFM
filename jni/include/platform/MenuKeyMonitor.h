#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace lengjing::platform {

enum class MenuKeyRequest : std::uint8_t {
    None,
    Show,
    Hide,
};

class MenuKeyMonitor final {
public:
    MenuKeyMonitor() = default;
    ~MenuKeyMonitor();

    MenuKeyMonitor(const MenuKeyMonitor&) = delete;
    MenuKeyMonitor& operator=(const MenuKeyMonitor&) = delete;

    void Start();
    void Stop();
    MenuKeyRequest ConsumeRequest();

private:
    void WorkerMain();

    std::atomic_bool stopRequested_{false};
    std::atomic<MenuKeyRequest> request_{MenuKeyRequest::None};
    std::thread worker_;
};

}  // namespace lengjing::platform
