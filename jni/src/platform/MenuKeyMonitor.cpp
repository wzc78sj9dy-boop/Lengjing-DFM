#include "platform/MenuKeyMonitor.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace lengjing::platform {
namespace {

std::vector<int> OpenInputDevices() {
    std::vector<int> descriptors;
    DIR* directory = opendir("/dev/input");
    if (directory == nullptr) {
        return descriptors;
    }

    while (dirent* entry = readdir(directory)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        const std::string path = std::string("/dev/input/") + entry->d_name;
        const int descriptor = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor >= 0) {
            descriptors.push_back(descriptor);
        }
    }
    closedir(directory);
    return descriptors;
}

void CloseInputDevices(std::vector<int>& descriptors) {
    for (const int descriptor : descriptors) {
        if (descriptor >= 0) {
            close(descriptor);
        }
    }
    descriptors.clear();
}

}  // namespace

MenuKeyMonitor::~MenuKeyMonitor() {
    Stop();
}

void MenuKeyMonitor::Start() {
    if (worker_.joinable()) {
        return;
    }
    stopRequested_.store(false, std::memory_order_release);
    worker_ = std::thread(&MenuKeyMonitor::WorkerMain, this);
}

void MenuKeyMonitor::Stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

MenuKeyRequest MenuKeyMonitor::ConsumeRequest() {
    return request_.exchange(MenuKeyRequest::None, std::memory_order_acq_rel);
}

void MenuKeyMonitor::WorkerMain() {
    using namespace std::chrono_literals;
    std::vector<int> descriptors;
    auto nextScan = std::chrono::steady_clock::time_point{};

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (descriptors.empty() || now >= nextScan) {
            CloseInputDevices(descriptors);
            descriptors = OpenInputDevices();
            nextScan = now + 5s;
        }

        input_event event{};
        for (const int descriptor : descriptors) {
            while (read(descriptor, &event, sizeof(event)) == sizeof(event)) {
                if (event.type != EV_KEY || event.value != 1) {
                    continue;
                }
                if (event.code == KEY_VOLUMEUP) {
                    request_.store(MenuKeyRequest::Show, std::memory_order_release);
                } else if (event.code == KEY_VOLUMEDOWN) {
                    request_.store(MenuKeyRequest::Hide, std::memory_order_release);
                }
            }
        }
        std::this_thread::sleep_for(40ms);
    }

    CloseInputDevices(descriptors);
}

}  // namespace lengjing::platform
