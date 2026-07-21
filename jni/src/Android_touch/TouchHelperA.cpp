#include "TouchHelperA.h"

#include "Android_touch/TouchTransform.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr int kFingerCount = 10;
constexpr int kReadPollMilliseconds = 100;
constexpr int kRetryDelayMilliseconds = 250;
constexpr int kAimWriteSlot = 8;
constexpr int kTrackingIdBase = 2000;
constexpr std::size_t kUiEventCapacity = 256;
constexpr const char* kInputDirectory = "/dev/input";

using TouchCallback = void (*)(TouchFinger*);

struct DeviceRange {
    int minimumX = 0;
    int maximumX = 1;
    int minimumY = 0;
    int maximumY = 1;
    int minimumSlot = 0;
    int maximumSlot = kFingerCount - 1;
    int minimumTrackingId = 0;
    int maximumTrackingId = 65535;
    bool hasButtonTouch = false;
    bool hasButtonToolFinger = false;
    bool hasWidthMajor = false;
    bool hasTouchMajor = false;
    bool hasTouchMinor = false;
};

struct ScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct DisplayGeometry {
    int width = 1;
    int height = 1;
    int orientation = 0;
};

struct UiTouchEvent {
    int rawX = -1;
    int rawY = -1;
    bool hasPosition = false;
    bool down = false;
};

std::array<TouchFinger, kFingerCount> g_fingers{};
std::array<bool, kFingerCount> g_writtenActive{};
std::recursive_mutex g_fingerMutex;
std::mutex g_outputMutex;
std::mutex g_rangeMutex;
std::mutex g_displayMutex;
std::mutex g_lifecycleMutex;
std::mutex g_uiEventMutex;

std::atomic<TouchCallback> g_callback{nullptr};
std::atomic_bool g_running{false};
std::atomic_int g_mode{0};
std::atomic_uint g_trackingSequence{0};
std::atomic_bool g_ignoreWriteSlot{false};

DeviceRange g_inputRange{};
DeviceRange g_outputRange{};
DisplayGeometry g_displayGeometry{};
std::deque<UiTouchEvent> g_uiEvents;
std::thread g_worker;
int g_writeTouchFile = -1;
thread_local bool g_insideInputWorker = false;

bool IsValidSlot(int slot) {
    return slot >= 0 && slot < kFingerCount;
}

bool IsUiTouchSlot(int slot) {
    return IsValidSlot(slot) &&
        !(g_ignoreWriteSlot.load(std::memory_order_acquire) &&
          slot == kAimWriteSlot);
}

DisplayGeometry LoadDisplayGeometry() {
    std::lock_guard<std::mutex> lock(g_displayMutex);
    return g_displayGeometry;
}

template <std::size_t N>
bool TestBit(const std::array<unsigned long, N>& bits, int bit) {
    constexpr int bitsPerWord = static_cast<int>(sizeof(unsigned long) * 8);
    const std::size_t index = static_cast<std::size_t>(bit / bitsPerWord);
    return index < bits.size() &&
        ((bits[index] >> (bit % bitsPerWord)) & 1UL) != 0;
}

DeviceRange ConfiguredRange() {
    const DisplayGeometry display = LoadDisplayGeometry();
    const int width = std::max(1, display.width);
    const int height = std::max(1, display.height);
    const int orientation = display.orientation & 3;
    const int naturalWidth = orientation == 1 || orientation == 3 ? height : width;
    const int naturalHeight = orientation == 1 || orientation == 3 ? width : height;
    DeviceRange range{};
    range.maximumX = std::max(1, naturalWidth - 1);
    range.maximumY = std::max(1, naturalHeight - 1);
    return range;
}

void StoreInputRange(const DeviceRange& range) {
    std::lock_guard<std::mutex> lock(g_rangeMutex);
    g_inputRange = range;
}

void StoreOutputRange(const DeviceRange& range) {
    std::lock_guard<std::mutex> lock(g_rangeMutex);
    g_outputRange = range;
}

DeviceRange LoadInputRange() {
    std::lock_guard<std::mutex> lock(g_rangeMutex);
    return g_inputRange;
}

DeviceRange LoadOutputRange() {
    std::lock_guard<std::mutex> lock(g_rangeMutex);
    return g_outputRange;
}

bool ReadDeviceRange(int file, DeviceRange& range) {
    std::array<unsigned long, (ABS_MAX / (sizeof(unsigned long) * 8)) + 2> absBits{};
    if (ioctl(file, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits.data()) < 0) {
        return false;
    }
    if (!TestBit(absBits, ABS_MT_SLOT) ||
        !TestBit(absBits, ABS_MT_TRACKING_ID) ||
        !TestBit(absBits, ABS_MT_POSITION_X) ||
        !TestBit(absBits, ABS_MT_POSITION_Y)) {
        return false;
    }

    input_absinfo xInfo{};
    input_absinfo yInfo{};
    input_absinfo slotInfo{};
    input_absinfo trackingInfo{};
    if (ioctl(file, EVIOCGABS(ABS_MT_POSITION_X), &xInfo) < 0 ||
        ioctl(file, EVIOCGABS(ABS_MT_POSITION_Y), &yInfo) < 0 ||
        ioctl(file, EVIOCGABS(ABS_MT_SLOT), &slotInfo) < 0 ||
        ioctl(file, EVIOCGABS(ABS_MT_TRACKING_ID), &trackingInfo) < 0 ||
        xInfo.maximum <= xInfo.minimum || yInfo.maximum <= yInfo.minimum) {
        return false;
    }

    range.minimumX = xInfo.minimum;
    range.maximumX = xInfo.maximum;
    range.minimumY = yInfo.minimum;
    range.maximumY = yInfo.maximum;
    range.minimumSlot = slotInfo.minimum;
    range.maximumSlot = slotInfo.maximum;
    range.minimumTrackingId = trackingInfo.minimum;
    range.maximumTrackingId = trackingInfo.maximum;
    range.hasWidthMajor = TestBit(absBits, ABS_MT_WIDTH_MAJOR);
    range.hasTouchMajor = TestBit(absBits, ABS_MT_TOUCH_MAJOR);
    range.hasTouchMinor = TestBit(absBits, ABS_MT_TOUCH_MINOR);

    std::array<unsigned long, (KEY_MAX / (sizeof(unsigned long) * 8)) + 2>
        keyBits{};
    if (ioctl(file, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits.data()) >= 0) {
        range.hasButtonTouch = TestBit(keyBits, BTN_TOUCH);
        range.hasButtonToolFinger = TestBit(keyBits, BTN_TOOL_FINGER);
    }
    return true;
}

int OpenTouchInput(DeviceRange& range, std::string* selectedPath = nullptr) {
    DIR* directory = opendir(kInputDirectory);
    if (directory == nullptr) {
        return -1;
    }

    int selectedFile = -1;
    dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        const std::string path = std::string(kInputDirectory) + "/" + entry->d_name;
        const int file = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (file < 0) {
            continue;
        }

        DeviceRange candidate{};
        if (ReadDeviceRange(file, candidate)) {
            selectedFile = file;
            range = candidate;
            if (selectedPath != nullptr) {
                *selectedPath = path;
            }
            break;
        }
        close(file);
    }

    closedir(directory);
    return selectedFile;
}

int SeedInputState(int file) {
    int currentSlot = 0;
    input_absinfo slotInfo{};
    if (ioctl(file, EVIOCGABS(ABS_MT_SLOT), &slotInfo) >= 0 &&
        IsValidSlot(slotInfo.value)) {
        currentSlot = slotInfo.value;
    }

    std::array<int, kFingerCount + 1> xValues{};
    std::array<int, kFingerCount + 1> yValues{};
    xValues[0] = ABS_MT_POSITION_X;
    yValues[0] = ABS_MT_POSITION_Y;
    const bool hasX =
        ioctl(file, EVIOCGMTSLOTS(sizeof(xValues)), xValues.data()) >= 0;
    const bool hasY =
        ioctl(file, EVIOCGMTSLOTS(sizeof(yValues)), yValues.data()) >= 0;
    if (!hasX && !hasY) {
        return currentSlot;
    }

    std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
    for (int slot = 0; slot < kFingerCount; ++slot) {
        TouchFinger& finger = g_fingers[static_cast<std::size_t>(slot)];
        if (hasX) {
            finger.x = xValues[static_cast<std::size_t>(slot + 1)];
        }
        if (hasY) {
            finger.y = yValues[static_cast<std::size_t>(slot + 1)];
        }
    }
    return currentSlot;
}

float NormalizeAxis(int value, int minimum, int maximum) {
    if (maximum <= minimum) {
        return 0.0f;
    }
    return std::clamp(
        static_cast<float>(value - minimum) /
            static_cast<float>(maximum - minimum),
        0.0f, 1.0f);
}

ScreenPoint DeviceToScreen(int rawX, int rawY) {
    const DeviceRange range = LoadInputRange();
    const DisplayGeometry geometry = LoadDisplayGeometry();
    const float naturalX = NormalizeAxis(rawX, range.minimumX, range.maximumX);
    const float naturalY = NormalizeAxis(rawY, range.minimumY, range.maximumY);
    const float width = static_cast<float>(std::max(1, geometry.width));
    const float height = static_cast<float>(std::max(1, geometry.height));

    const lengjing::touch::NormalizedPoint display =
        lengjing::touch::NaturalToDisplay(
            {naturalX, naturalY},
            geometry.orientation);
    return ScreenPoint{display.x * width, display.y * height};
}

ScreenPoint ScreenToDevice(int screenX, int screenY) {
    const DeviceRange range = LoadOutputRange();
    const DisplayGeometry geometry = LoadDisplayGeometry();
    const float width = static_cast<float>(std::max(1, geometry.width));
    const float height = static_cast<float>(std::max(1, geometry.height));
    const float displayX = std::clamp(static_cast<float>(screenX) / width, 0.0f, 1.0f);
    const float displayY = std::clamp(static_cast<float>(screenY) / height, 0.0f, 1.0f);

    const lengjing::touch::NormalizedPoint natural =
        lengjing::touch::DisplayToNatural(
            {displayX, displayY},
            geometry.orientation);

    return ScreenPoint{
        static_cast<float>(range.minimumX) +
            natural.x * static_cast<float>(range.maximumX - range.minimumX),
        static_cast<float>(range.minimumY) +
            natural.y * static_cast<float>(range.maximumY - range.minimumY),
    };
}

bool WriteEventBytes(int file, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(file, bytes + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{file, POLLOUT, 0};
            if (poll(&descriptor, 1, 20) > 0) {
                continue;
            }
        }
        return false;
    }
    return true;
}

template <std::size_t Capacity>
class EventBatch {
public:
    void Add(std::uint16_t type, std::uint16_t code, std::int32_t value) {
        if (count_ >= events_.size()) {
            return;
        }
        input_event& event = events_[count_++];
        event = input_event{};
        gettimeofday(&event.time, nullptr);
        event.type = type;
        event.code = code;
        event.value = value;
    }

    bool Send(int file) const {
        return count_ > 0 && WriteEventBytes(
            file, events_.data(), count_ * sizeof(input_event));
    }

private:
    std::array<input_event, Capacity> events_{};
    std::size_t count_ = 0;
};

bool OpenWriteTouch(const std::string& path, const DeviceRange& fallbackRange) {
    std::lock_guard<std::mutex> lock(g_outputMutex);
    if (path.empty()) {
        return false;
    }

    const int file = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) {
        return false;
    }

    DeviceRange range = fallbackRange;
    if (!ReadDeviceRange(file, range) ||
        kAimWriteSlot < range.minimumSlot ||
        kAimWriteSlot > range.maximumSlot) {
        close(file);
        return false;
    }

    g_writeTouchFile = file;
    g_writtenActive.fill(false);
    g_trackingSequence.store(0, std::memory_order_release);
    g_ignoreWriteSlot.store(true, std::memory_order_release);
    StoreOutputRange(range);
    return true;
}

int NextTrackingId() {
    const DeviceRange range = LoadOutputRange();
    const int minimum = std::min(range.minimumTrackingId, range.maximumTrackingId);
    const int maximum = std::max(range.minimumTrackingId, range.maximumTrackingId);
    const int start = std::clamp(kTrackingIdBase, minimum, maximum);
    const std::uint64_t span =
        static_cast<std::uint64_t>(
            static_cast<std::int64_t>(maximum) -
            static_cast<std::int64_t>(start)) + 1U;
    const std::uint64_t sequence =
        g_trackingSequence.fetch_add(1, std::memory_order_relaxed);
    return start + static_cast<int>(sequence % span);
}

bool IsWriteSlotAvailableLocked(int slot) {
    std::array<int, kFingerCount + 1> trackingIds{};
    trackingIds.fill(-1);
    trackingIds[0] = ABS_MT_TRACKING_ID;
    if (ioctl(
            g_writeTouchFile,
            EVIOCGMTSLOTS(sizeof(trackingIds)),
            trackingIds.data()) >= 0) {
        return trackingIds[static_cast<std::size_t>(slot + 1)] < 0;
    }

    std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
    return g_fingers[static_cast<std::size_t>(slot)].tracking_id < 0;
}

bool HasUnmanagedActiveTouchLocked() {
    std::array<int, kFingerCount + 1> trackingIds{};
    trackingIds.fill(-1);
    trackingIds[0] = ABS_MT_TRACKING_ID;
    if (ioctl(
            g_writeTouchFile,
            EVIOCGMTSLOTS(sizeof(trackingIds)),
            trackingIds.data()) >= 0) {
        for (int slot = 0; slot < kFingerCount; ++slot) {
            const std::size_t index = static_cast<std::size_t>(slot);
            if (trackingIds[index + 1] >= 0 &&
                !g_writtenActive[index]) {
                return true;
            }
        }
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
    for (int slot = 0; slot < kFingerCount; ++slot) {
        const std::size_t index = static_cast<std::size_t>(slot);
        if (g_fingers[index].tracking_id >= 0 &&
            !g_writtenActive[index]) {
            return true;
        }
    }
    return false;
}

void ReleaseWrittenTouchesLocked() {
    if (g_writeTouchFile < 0) {
        g_writtenActive.fill(false);
        return;
    }

    EventBatch<kFingerCount * 2 + 4> batch;
    bool hadActiveTouch = false;
    for (int slot = 0; slot < kFingerCount; ++slot) {
        if (!g_writtenActive[static_cast<std::size_t>(slot)]) {
            continue;
        }
        hadActiveTouch = true;
        batch.Add(EV_ABS, ABS_MT_SLOT, slot);
        batch.Add(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    if (hadActiveTouch) {
        const DeviceRange outputRange = LoadOutputRange();
        const bool releaseGlobalButtons =
            !HasUnmanagedActiveTouchLocked();
        if (releaseGlobalButtons && outputRange.hasButtonTouch) {
            batch.Add(EV_KEY, BTN_TOUCH, 0);
        }
        if (releaseGlobalButtons && outputRange.hasButtonToolFinger) {
            batch.Add(EV_KEY, BTN_TOOL_FINGER, 0);
        }
        batch.Add(EV_SYN, SYN_REPORT, 0);
        batch.Add(EV_ABS, ABS_MT_SLOT, 0);
        batch.Send(g_writeTouchFile);
    }
    g_writtenActive.fill(false);
}

void CloseWriteTouch() {
    std::lock_guard<std::mutex> lock(g_outputMutex);
    if (g_writeTouchFile < 0) {
        g_writtenActive.fill(false);
        g_ignoreWriteSlot.store(false, std::memory_order_release);
        return;
    }
    ReleaseWrittenTouchesLocked();
    close(g_writeTouchFile);
    g_writeTouchFile = -1;
    g_ignoreWriteSlot.store(false, std::memory_order_release);
}

void NotifyFinger(int slot) {
    const TouchCallback callback = g_callback.load(std::memory_order_acquire);
    if (callback == nullptr || !IsValidSlot(slot)) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
    callback(&g_fingers[static_cast<std::size_t>(slot)]);
}

void ResetFingerStatus(int slot) {
    if (!IsValidSlot(slot)) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
    g_fingers[static_cast<std::size_t>(slot)].status = FINGER_NO;
}

void QueueUiInput(const TouchFinger& finger) {
    UiTouchEvent event{};
    if (finger.x >= 0 && finger.y >= 0) {
        event.rawX = finger.x;
        event.rawY = finger.y;
        event.hasPosition = true;
    }
    event.down = finger.tracking_id >= 0 && finger.status != FINGER_UP;

    std::lock_guard<std::mutex> lock(g_uiEventMutex);
    if (g_uiEvents.size() >= kUiEventCapacity) {
        g_uiEvents.pop_front();
    }
    g_uiEvents.push_back(event);
}

void QueueUiRelease() {
    std::lock_guard<std::mutex> lock(g_uiEventMutex);
    g_uiEvents.clear();
    g_uiEvents.push_back(UiTouchEvent{});
}

void MarkAxisUpdate(TouchFinger& finger, bool xAxis) {
    if (xAxis) {
        finger.status = finger.status == FINGER_Y_UPDATE
            ? FINGER_XY_UPDATE
            : FINGER_X_UPDATE;
    } else {
        finger.status = finger.status == FINGER_X_UPDATE
            ? FINGER_XY_UPDATE
            : FINGER_Y_UPDATE;
    }
}

void ProcessInputEvents(const input_event* events,
                        std::size_t count,
                        int& currentSlot,
                        int& uiSlot,
                        std::array<bool, kFingerCount>& dirty) {
    for (std::size_t index = 0; index < count; ++index) {
        const input_event& event = events[index];
        if (event.type == EV_ABS) {
            if (event.code == ABS_MT_SLOT) {
                currentSlot = IsValidSlot(event.value) ? event.value : 0;
                continue;
            }

            std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
            TouchFinger& finger = g_fingers[static_cast<std::size_t>(currentSlot)];
            finger.time = event.time;
            if (event.code == ABS_MT_POSITION_X) {
                finger.x = event.value;
                MarkAxisUpdate(finger, true);
                dirty[static_cast<std::size_t>(currentSlot)] = true;
            } else if (event.code == ABS_MT_POSITION_Y) {
                finger.y = event.value;
                MarkAxisUpdate(finger, false);
                dirty[static_cast<std::size_t>(currentSlot)] = true;
            } else if (event.code == ABS_MT_TRACKING_ID) {
                finger.tracking_id = event.value;
                finger.status = event.value < 0 ? FINGER_UP : FINGER_XY_UPDATE;
                dirty[static_cast<std::size_t>(currentSlot)] = true;
            }
            continue;
        }

        if (event.type != EV_SYN || event.code != SYN_REPORT) {
            continue;
        }

        TouchFinger primary{};
        bool hasPrimary = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
            if (!IsUiTouchSlot(uiSlot)) {
                uiSlot = -1;
            }
            if (IsValidSlot(uiSlot)) {
                primary = g_fingers[static_cast<std::size_t>(uiSlot)];
                hasPrimary = true;
                if (primary.tracking_id < 0) {
                    uiSlot = -1;
                }
            } else {
                for (int slot = 0; slot < kFingerCount; ++slot) {
                    if (!IsUiTouchSlot(slot)) {
                        continue;
                    }
                    const TouchFinger& candidate =
                        g_fingers[static_cast<std::size_t>(slot)];
                    if (candidate.tracking_id < 0) {
                        continue;
                    }
                    uiSlot = slot;
                    primary = candidate;
                    hasPrimary = true;
                    break;
                }
            }
        }
        if (hasPrimary) {
            QueueUiInput(primary);
        }

        for (int slot = 0; slot < kFingerCount; ++slot) {
            if (!dirty[static_cast<std::size_t>(slot)]) {
                continue;
            }
            NotifyFinger(slot);
            ResetFingerStatus(slot);
            dirty[static_cast<std::size_t>(slot)] = false;
        }
    }
}

void InputWorker(int inputFile) {
    g_insideInputWorker = true;
    int currentSlot = inputFile >= 0 ? SeedInputState(inputFile) : 0;
    int uiSlot = -1;
    std::array<bool, kFingerCount> dirty{};

    while (g_running.load(std::memory_order_acquire)) {
        if (inputFile < 0) {
            DeviceRange range = ConfiguredRange();
            inputFile = OpenTouchInput(range);
            if (inputFile >= 0) {
                StoreInputRange(range);
                currentSlot = SeedInputState(inputFile);
            } else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kRetryDelayMilliseconds));
                continue;
            }
        }

        pollfd descriptor{inputFile, POLLIN, 0};
        const int result = poll(&descriptor, 1, kReadPollMilliseconds);
        if (!g_running.load(std::memory_order_acquire)) {
            break;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close(inputFile);
            inputFile = -1;
            continue;
        }
        if ((descriptor.revents & POLLIN) == 0) {
            continue;
        }

        std::array<input_event, 64> events{};
        const ssize_t bytes = read(inputFile, events.data(), sizeof(events));
        if (bytes > 0) {
            ProcessInputEvents(
                events.data(), static_cast<std::size_t>(bytes) / sizeof(input_event),
                currentSlot, uiSlot, dirty);
        } else if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(inputFile);
            inputFile = -1;
        }
    }

    if (inputFile >= 0) {
        close(inputFile);
    }
    g_insideInputWorker = false;
}

void ResetFingers() {
    std::lock_guard<std::recursive_mutex> lock(g_fingerMutex);
    g_fingers.fill(TouchFinger{});
}

void StopTouchScreenLocked() {
    g_running.store(false, std::memory_order_release);
    g_mode.store(0, std::memory_order_release);
    if (g_worker.joinable()) {
        g_worker.join();
    }
    CloseWriteTouch();
    ResetFingers();
    QueueUiRelease();
}

struct TouchCleanup {
    ~TouchCleanup() {
        StopTouchScreen();
    }
};

TouchCleanup g_cleanup;

}  // namespace

bool Touch_Down(int slot, int x, int y) {
    if (!IsValidSlot(slot) || g_mode.load(std::memory_order_acquire) != 1) {
        return false;
    }

    const ScreenPoint mapped = ScreenToDevice(x, y);
    const DeviceRange outputRange = LoadOutputRange();
    if (slot < outputRange.minimumSlot || slot > outputRange.maximumSlot) {
        return false;
    }
    bool notify = false;
    bool outputFailed = false;
    {
        std::lock_guard<std::mutex> outputLock(g_outputMutex);
        if (g_writeTouchFile < 0) {
            return false;
        }

        const std::size_t index = static_cast<std::size_t>(slot);
        if (g_writtenActive[index] || !IsWriteSlotAvailableLocked(slot)) {
            return false;
        }
        const int trackingId = NextTrackingId();
        TouchFinger previous{};
        {
            std::lock_guard<std::recursive_mutex> fingerLock(g_fingerMutex);
            TouchFinger& finger = g_fingers[index];
            previous = finger;
            finger.tracking_id = trackingId;
            finger.x = static_cast<int>(mapped.x);
            finger.y = static_cast<int>(mapped.y);
            finger.status = FINGER_XY_UPDATE;
            gettimeofday(&finger.time, nullptr);
        }

        EventBatch<11> batch;
        batch.Add(EV_ABS, ABS_MT_SLOT, slot);
        batch.Add(EV_ABS, ABS_MT_TRACKING_ID, trackingId);
        if (outputRange.hasButtonTouch) {
            batch.Add(EV_KEY, BTN_TOUCH, 1);
        }
        if (outputRange.hasButtonToolFinger) {
            batch.Add(EV_KEY, BTN_TOOL_FINGER, 1);
        }
        if (outputRange.hasWidthMajor) {
            batch.Add(EV_ABS, ABS_MT_WIDTH_MAJOR, 9);
        }
        if (outputRange.hasTouchMajor) {
            batch.Add(EV_ABS, ABS_MT_TOUCH_MAJOR, 15);
        }
        if (outputRange.hasTouchMinor) {
            batch.Add(EV_ABS, ABS_MT_TOUCH_MINOR, 15);
        }
        batch.Add(EV_ABS, ABS_MT_POSITION_X, static_cast<int>(mapped.x));
        batch.Add(EV_ABS, ABS_MT_POSITION_Y, static_cast<int>(mapped.y));
        batch.Add(EV_SYN, SYN_REPORT, 0);
        batch.Add(EV_ABS, ABS_MT_SLOT, 0);
        if (batch.Send(g_writeTouchFile)) {
            g_writtenActive[index] = true;
            notify = true;
        } else {
            std::lock_guard<std::recursive_mutex> fingerLock(g_fingerMutex);
            g_fingers[index] = previous;
            outputFailed = true;
        }
    }

    if (outputFailed) {
        g_mode.store(0, std::memory_order_release);
        CloseWriteTouch();
        return false;
    }
    if (notify) {
        NotifyFinger(slot);
        ResetFingerStatus(slot);
    }
    return notify;
}

bool Touch_Move(int slot, int x, int y) {
    if (!IsValidSlot(slot) || g_mode.load(std::memory_order_acquire) != 1) {
        return false;
    }

    const ScreenPoint mapped = ScreenToDevice(x, y);
    const DeviceRange outputRange = LoadOutputRange();
    if (slot < outputRange.minimumSlot || slot > outputRange.maximumSlot) {
        return false;
    }
    bool notify = false;
    bool outputFailed = false;
    {
        std::lock_guard<std::mutex> outputLock(g_outputMutex);
        const std::size_t index = static_cast<std::size_t>(slot);
        if (g_writeTouchFile < 0 || !g_writtenActive[index]) {
            return false;
        }

        TouchFinger previous{};
        {
            std::lock_guard<std::recursive_mutex> fingerLock(g_fingerMutex);
            TouchFinger& finger = g_fingers[index];
            previous = finger;
            finger.x = static_cast<int>(mapped.x);
            finger.y = static_cast<int>(mapped.y);
            finger.status = FINGER_XY_UPDATE;
            gettimeofday(&finger.time, nullptr);
        }

        EventBatch<6> batch;
        batch.Add(EV_ABS, ABS_MT_SLOT, slot);
        if (outputRange.hasButtonTouch) {
            batch.Add(EV_KEY, BTN_TOUCH, 1);
        }
        batch.Add(EV_ABS, ABS_MT_POSITION_X, static_cast<int>(mapped.x));
        batch.Add(EV_ABS, ABS_MT_POSITION_Y, static_cast<int>(mapped.y));
        batch.Add(EV_SYN, SYN_REPORT, 0);
        batch.Add(EV_ABS, ABS_MT_SLOT, 0);
        if (batch.Send(g_writeTouchFile)) {
            notify = true;
        } else {
            std::lock_guard<std::recursive_mutex> fingerLock(g_fingerMutex);
            g_fingers[index] = previous;
            outputFailed = true;
        }
    }

    if (outputFailed) {
        g_mode.store(0, std::memory_order_release);
        CloseWriteTouch();
        return false;
    }
    if (notify) {
        NotifyFinger(slot);
        ResetFingerStatus(slot);
    }
    return notify;
}

bool Touch_Up(int slot) {
    if (!IsValidSlot(slot) || g_mode.load(std::memory_order_acquire) != 1) {
        return false;
    }

    bool notify = false;
    bool outputFailed = false;
    {
        std::lock_guard<std::mutex> outputLock(g_outputMutex);
        const std::size_t index = static_cast<std::size_t>(slot);
        if (g_writeTouchFile < 0 || !g_writtenActive[index]) {
            return false;
        }

        const bool lastWrittenTouch =
            std::count(g_writtenActive.begin(), g_writtenActive.end(), true) == 1;
        const bool releaseGlobalButtons =
            lastWrittenTouch && !HasUnmanagedActiveTouchLocked();
        const DeviceRange outputRange = LoadOutputRange();
        EventBatch<6> batch;
        batch.Add(EV_ABS, ABS_MT_SLOT, slot);
        batch.Add(EV_ABS, ABS_MT_TRACKING_ID, -1);
        if (releaseGlobalButtons && outputRange.hasButtonTouch) {
            batch.Add(EV_KEY, BTN_TOUCH, 0);
        }
        if (releaseGlobalButtons && outputRange.hasButtonToolFinger) {
            batch.Add(EV_KEY, BTN_TOOL_FINGER, 0);
        }
        batch.Add(EV_SYN, SYN_REPORT, 0);
        batch.Add(EV_ABS, ABS_MT_SLOT, 0);
        notify = batch.Send(g_writeTouchFile);

        if (!notify) {
            outputFailed = true;
        } else {
            g_writtenActive[index] = false;

            std::lock_guard<std::recursive_mutex> fingerLock(g_fingerMutex);
            TouchFinger& finger = g_fingers[index];
            finger.tracking_id = -1;
            finger.status = FINGER_UP;
            gettimeofday(&finger.time, nullptr);
        }
    }

    if (outputFailed) {
        g_mode.store(0, std::memory_order_release);
        CloseWriteTouch();
        return false;
    }
    if (notify) {
        NotifyFinger(slot);
        ResetFingerStatus(slot);
    }
    return notify;
}

TouchFinger* getTouchFinger(int slot) {
    return IsValidSlot(slot)
        ? &g_fingers[static_cast<std::size_t>(slot)]
        : nullptr;
}

void setTouchCallback(void (*callback)(TouchFinger*)) {
    g_callback.store(callback, std::memory_order_release);
}

bool TouchScreenHandle(int mode) {
    if (g_insideInputWorker) {
        g_running.store(false, std::memory_order_release);
        return false;
    }
    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    StopTouchScreenLocked();

    int selectedMode = mode == 1 ? 1 : 0;

    DeviceRange range = ConfiguredRange();
    std::string selectedPath;
    int inputFile = OpenTouchInput(
        range, selectedMode == 1 ? &selectedPath : nullptr);
    StoreInputRange(range);
    StoreOutputRange(range);

    if (selectedMode == 1 && !OpenWriteTouch(selectedPath, range)) {
        selectedMode = 0;
    }

    ResetFingers();
    g_mode.store(selectedMode, std::memory_order_release);
    g_running.store(true, std::memory_order_release);

    try {
        g_worker = std::thread(InputWorker, inputFile);
    } catch (...) {
        g_running.store(false, std::memory_order_release);
        g_mode.store(0, std::memory_order_release);
        if (inputFile >= 0) {
            close(inputFile);
        }
        CloseWriteTouch();
        return false;
    }
    return mode != 1 || selectedMode == 1;
}

void StopTouchScreen() {
    if (g_insideInputWorker) {
        g_running.store(false, std::memory_order_release);
        g_mode.store(0, std::memory_order_release);
        return;
    }

    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    StopTouchScreenLocked();
}

void PumpTouchInput() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    std::deque<UiTouchEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_uiEventMutex);
        events.swap(g_uiEvents);
    }
    if (events.empty()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    for (const UiTouchEvent& event : events) {
        ScreenPoint position{};
        if (event.hasPosition) {
            position = DeviceToScreen(event.rawX, event.rawY);
            io.AddMousePosEvent(position.x, position.y);
        }
        io.AddMouseButtonEvent(0, event.down);
    }
}

void ConfigureTouchDisplay(int width, int height, int orientation) {
    const DisplayGeometry next{
        std::max(1, width),
        std::max(1, height),
        lengjing::touch::NormalizeOrientation(orientation),
    };
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_displayMutex);
        changed =
            g_displayGeometry.width != next.width ||
            g_displayGeometry.height != next.height ||
            g_displayGeometry.orientation != next.orientation;
        g_displayGeometry = next;
    }
    if (changed) {
        QueueUiRelease();
    }
}

void setOrientation(int orientation) {
    const int normalized =
        lengjing::touch::NormalizeOrientation(orientation);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_displayMutex);
        changed = g_displayGeometry.orientation != normalized;
        g_displayGeometry.orientation = normalized;
    }
    if (changed) {
        QueueUiRelease();
    }
}
