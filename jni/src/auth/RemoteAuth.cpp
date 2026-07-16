#include "auth/RemoteAuth.h"

#include "auth/AuthConfig.h"
#include "auth/CardInputPolicy.h"
#include "t3/t3sdk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lengjing::auth {
namespace {

#if defined(__clang__)
#define LENGJING_AUTH_GUARD                                                  \
    __attribute__((annotate(                                                \
        "+fla +indbr ^indbr=3 +icall ^icall=3 +indgv ^indgv=3 "            \
        "+cie ^cie=3 +cfe ^cfe=3 +bcf")))
#else
#define LENGJING_AUTH_GUARD
#endif

constexpr std::string_view kCardCachePath =
    "/data/local/tmp/.lengjing_key";
constexpr std::string_view kDeviceCachePath =
    "/data/local/tmp/.lengjing_device";
constexpr std::string_view kDeviceLockPath =
    "/data/local/tmp/.lengjing_device.lock";
constexpr std::string_view kLegacyDeviceCachePath =
    "/data/local/tmp/dev_info";
constexpr std::size_t kMaximumStoredValueLength = 4096;

class ScopedDescriptor final {
public:
    explicit ScopedDescriptor(int value = -1) noexcept : value_(value) {}

    ~ScopedDescriptor() {
        if (value_ >= 0) {
            close(value_);
        }
    }

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    ScopedDescriptor(ScopedDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}

    ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    int Get() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ >= 0;
    }

private:
    int value_ = -1;
};

class ScopedFileLock final {
public:
    ScopedFileLock() {
        descriptor_ = ScopedDescriptor(open(
            std::string(kDeviceLockPath).c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            0600));
        if (descriptor_ && flock(descriptor_.Get(), LOCK_EX) != 0) {
            descriptor_ = ScopedDescriptor{};
        }
    }

    ~ScopedFileLock() {
        if (descriptor_) {
            flock(descriptor_.Get(), LOCK_UN);
        }
    }

    bool Acquired() const noexcept {
        return static_cast<bool>(descriptor_);
    }

private:
    ScopedDescriptor descriptor_;
};

struct Md5Context {
    std::array<std::uint32_t, 4> state{
        0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
    std::uint64_t bitCount = 0;
    std::array<std::uint8_t, 64> buffer{};
    std::size_t bufferSize = 0;
};

constexpr std::array<std::uint32_t, 64> kMd5Shifts{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr std::array<std::uint32_t, 64> kMd5Constants{
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
    0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
    0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
    0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
    0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
    0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
    0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
    0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
    0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
    0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U,
};

std::uint32_t RotateLeft(std::uint32_t value, std::uint32_t bits) {
    return (value << bits) | (value >> (32U - bits));
}

void TransformMd5(Md5Context& context,
                  const std::uint8_t block[64]) {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::size_t offset = index * 4;
        words[index] = static_cast<std::uint32_t>(block[offset]) |
            (static_cast<std::uint32_t>(block[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(block[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(block[offset + 3]) << 24U);
    }

    std::uint32_t a = context.state[0];
    std::uint32_t b = context.state[1];
    std::uint32_t c = context.state[2];
    std::uint32_t d = context.state[3];
    for (std::uint32_t index = 0; index < 64; ++index) {
        std::uint32_t function = 0;
        std::uint32_t wordIndex = 0;
        if (index < 16) {
            function = (b & c) | ((~b) & d);
            wordIndex = index;
        } else if (index < 32) {
            function = (d & b) | ((~d) & c);
            wordIndex = (5U * index + 1U) % 16U;
        } else if (index < 48) {
            function = b ^ c ^ d;
            wordIndex = (3U * index + 5U) % 16U;
        } else {
            function = c ^ (b | (~d));
            wordIndex = (7U * index) % 16U;
        }
        const std::uint32_t previousD = d;
        d = c;
        c = b;
        b += RotateLeft(
            a + function + kMd5Constants[index] + words[wordIndex],
            kMd5Shifts[index]);
        a = previousD;
    }
    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
}

void UpdateMd5(Md5Context& context,
               const std::uint8_t* data,
               std::size_t size) {
    context.bitCount += static_cast<std::uint64_t>(size) * 8U;
    while (size > 0) {
        const std::size_t available =
            context.buffer.size() - context.bufferSize;
        const std::size_t count = std::min(size, available);
        std::memcpy(context.buffer.data() + context.bufferSize, data, count);
        context.bufferSize += count;
        data += count;
        size -= count;
        if (context.bufferSize == context.buffer.size()) {
            TransformMd5(context, context.buffer.data());
            context.bufferSize = 0;
        }
    }
}

std::string Md5Hex(std::string_view value) {
    Md5Context context{};
    UpdateMd5(
        context,
        reinterpret_cast<const std::uint8_t*>(value.data()),
        value.size());
    const std::uint64_t originalBitCount = context.bitCount;
    const std::uint8_t marker = 0x80U;
    UpdateMd5(context, &marker, 1);
    const std::uint8_t zero = 0;
    while (context.bufferSize != 56) {
        UpdateMd5(context, &zero, 1);
    }
    std::array<std::uint8_t, 8> lengthBytes{};
    for (std::size_t index = 0; index < lengthBytes.size(); ++index) {
        lengthBytes[index] = static_cast<std::uint8_t>(
            (originalBitCount >> (8U * index)) & 0xffU);
    }
    UpdateMd5(context, lengthBytes.data(), lengthBytes.size());

    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result(32, '\0');
    for (std::size_t word = 0; word < context.state.size(); ++word) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            const std::uint8_t valueByte = static_cast<std::uint8_t>(
                (context.state[word] >> (8U * byte)) & 0xffU);
            const std::size_t offset = (word * 4 + byte) * 2;
            result[offset] = kHexDigits[valueByte >> 4U];
            result[offset + 1] = kHexDigits[valueByte & 0x0fU];
        }
    }
    return result;
}

void Trim(std::string& value) {
    const auto isWhitespace = [](unsigned char character) {
        return character == ' ' || character == '\t' ||
            character == '\r' || character == '\n';
    };
    while (!value.empty() &&
           isWhitespace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [&](char character) {
            return isWhitespace(static_cast<unsigned char>(character));
        });
    value.erase(value.begin(), first);
}

bool IsValidDeviceCode(std::string_view value) {
    return value.size() == 32 &&
        std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

std::string ReadSingleLine(std::string_view path) {
    std::ifstream input{std::string(path)};
    std::string value;
    if (input) {
        std::getline(input, value);
    }
    if (value.size() > kMaximumStoredValueLength) {
        return {};
    }
    Trim(value);
    return value;
}

bool WriteAtomically(std::string_view path,
                     std::string_view value,
                     mode_t permissions) {
    std::string temporaryTemplate = std::string(path) + ".tmp.XXXXXX";
    std::vector<char> pathBuffer(
        temporaryTemplate.begin(), temporaryTemplate.end());
    pathBuffer.push_back('\0');
    ScopedDescriptor descriptor(mkstemp(pathBuffer.data()));
    if (!descriptor) {
        return false;
    }
    if (fchmod(descriptor.Get(), permissions) != 0) {
        unlink(pathBuffer.data());
        return false;
    }

    const std::string content = std::string(value) + "\n";
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t count = write(
            descriptor.Get(), content.data() + written, content.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            unlink(pathBuffer.data());
            return false;
        }
    }
    if (fsync(descriptor.Get()) != 0 ||
        rename(pathBuffer.data(), std::string(path).c_str()) != 0) {
        unlink(pathBuffer.data());
        return false;
    }
    return chmod(std::string(path).c_str(), permissions) == 0;
}

std::string ReadProperty(const char* name) {
    char value[PROP_VALUE_MAX]{};
    const int length = __system_property_get(name, value);
    if (length <= 0) {
        return {};
    }
    std::string result(value, static_cast<std::size_t>(length));
    Trim(result);
    return result == "unknown" || result == "null" ? std::string{} : result;
}

std::string ReadCommandValue(const char* command) {
    FILE* pipe = popen(command, "r");
    if (pipe == nullptr) {
        return {};
    }
    std::array<char, 256> buffer{};
    std::string result;
    if (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result = buffer.data();
    }
    pclose(pipe);
    Trim(result);
    return result == "unknown" || result == "null" ? std::string{} : result;
}

std::string BuildStableDeviceCode() {
    std::string androidId =
        ReadCommandValue("settings get secure android_id 2>/dev/null");
    if (androidId.size() < 8 || androidId.size() > 32 ||
        !std::all_of(androidId.begin(), androidId.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        })) {
        androidId.clear();
    }
    std::string serial = ReadProperty("ro.serialno");
    if (serial.empty()) {
        serial = ReadProperty("ro.boot.serialno");
    }
    if (androidId.empty() && serial.empty()) {
        return {};
    }

    std::string seed;
    if (!androidId.empty()) {
        seed = "android_id=" + androidId;
    }
    if (!serial.empty()) {
        if (!seed.empty()) {
            seed += '|';
        }
        seed += "serial=" + serial;
    }
    return Md5Hex(seed);
}

std::string ResolveDeviceCode(std::string_view programDirectory) {
    ScopedFileLock lock;
    if (!lock.Acquired()) {
        return {};
    }
    std::string code = ReadSingleLine(kDeviceCachePath);
    if (IsValidDeviceCode(code)) {
        return code;
    }

    const std::string localLegacyPath =
        std::string(programDirectory) + "/dev_info";
    for (const std::string& path :
         {localLegacyPath, std::string(kLegacyDeviceCachePath)}) {
        code = ReadSingleLine(path);
        if (IsValidDeviceCode(code)) {
            return WriteAtomically(kDeviceCachePath, code, 0600)
                ? code
                : std::string{};
        }
    }

    code = BuildStableDeviceCode();
    if (!IsValidDeviceCode(code) ||
        !WriteAtomically(kDeviceCachePath, code, 0600)) {
        return {};
    }
    if (localLegacyPath != kDeviceCachePath) {
        WriteAtomically(localLegacyPath, code, 0600);
    }
    return code;
}

std::string ReadCardKey() {
    if (const char* environmentValue = std::getenv("LENGJING_CARD_KEY")) {
        std::string value(environmentValue);
        Trim(value);
        if (!value.empty()) {
            return value;
        }
    }

    const std::string cached = ReadSingleLine(kCardCachePath);
    return input::ReadCardKeyFromStream(
        std::cin,
        std::cout,
        isatty(STDIN_FILENO) == 1,
        cached);
}

}  // namespace

struct AuthSession::Runtime final {
    std::unique_ptr<T3Verify> verifier;
    std::string cardKey;
    std::string stateCode;
    std::atomic_bool valid{false};
    std::atomic_bool stopRequested{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;

    ~Runtime() {
        Stop();
    }

    bool Start() noexcept {
        valid.store(true, std::memory_order_release);
        try {
            worker = std::thread([this] {
                int failureCount = 0;
                std::unique_lock<std::mutex> lock(mutex);
                while (!condition.wait_for(
                    lock,
                    std::chrono::seconds(config::kHeartbeatSeconds),
                    [this] {
                        return stopRequested.load(std::memory_order_acquire);
                    })) {
                    lock.unlock();
                    const T3Result result =
                        verifier->heartbeat(cardKey, stateCode);
                    lock.lock();
                    if (stopRequested.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (result.success) {
                        failureCount = 0;
                        continue;
                    }
                    ++failureCount;
                    std::fprintf(
                        stderr,
                        "[验证] 心跳失败 (%d/%d): %s\n",
                        failureCount,
                        config::kMaximumHeartbeatFailures,
                        result.error.c_str());
                    if (failureCount >= config::kMaximumHeartbeatFailures) {
                        valid.store(false, std::memory_order_release);
                        break;
                    }
                }
            });
            return true;
        } catch (...) {
            valid.store(false, std::memory_order_release);
            return false;
        }
    }

    void Stop() noexcept {
        stopRequested.store(true, std::memory_order_release);
        condition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        valid.store(false, std::memory_order_release);
    }
};

AuthSession::AuthSession() noexcept = default;

AuthSession::~AuthSession() {
    Reset();
}

AuthSession::AuthSession(AuthSession&& other) noexcept
    : cardKey(std::move(other.cardKey)),
      deviceCode(std::move(other.deviceCode)),
      expiresAt(std::move(other.expiresAt)),
      runtime_(std::move(other.runtime_)) {}

AuthSession& AuthSession::operator=(AuthSession&& other) noexcept {
    if (this != &other) {
        Reset();
        cardKey = std::move(other.cardKey);
        deviceCode = std::move(other.deviceCode);
        expiresAt = std::move(other.expiresAt);
        runtime_ = std::move(other.runtime_);
    }
    return *this;
}

bool AuthSession::IsValid() const noexcept {
    return runtime_ != nullptr &&
        runtime_->valid.load(std::memory_order_acquire);
}

bool AuthSession::ExitRequested() const noexcept {
    return runtime_ != nullptr &&
        !runtime_->valid.load(std::memory_order_acquire);
}

void AuthSession::Reset() noexcept {
    runtime_.reset();
    cardKey.clear();
    deviceCode.clear();
    expiresAt.clear();
}

bool LoginInteractive(std::string_view programDirectory,
                      AuthSession& session) LENGJING_AUTH_GUARD {
    session.Reset();
    if (!config::IsComplete()) {
        std::fprintf(stderr, "[验证] 配置不完整\n");
        return false;
    }

    const std::string cardKey = ReadCardKey();
    if (cardKey.empty()) {
        std::fprintf(stderr, "[验证] 卡密为空\n");
        return false;
    }
    const std::string deviceCode = ResolveDeviceCode(programDirectory);
    if (deviceCode.empty()) {
        std::fprintf(stderr, "[验证] 设备码获取失败\n");
        return false;
    }

    auto runtime = std::make_unique<AuthSession::Runtime>();
    runtime->verifier = std::make_unique<T3Verify>();
    if (!runtime->verifier->initRSA(
            std::string(config::kLoginCode),
            std::string(config::kNoticeCode),
            std::string(config::kVersionCode),
            std::string(config::kHeartbeatCode),
            std::string(config::kAppKey),
            std::string(config::kRsaPublicKey))) {
        std::fprintf(stderr, "[验证] SDK 初始化失败\n");
        return false;
    }

    const T3LoginResult result =
        runtime->verifier->login(cardKey, deviceCode);
    if (!result.success) {
        std::fprintf(
            stderr,
            "[验证] 登录失败: %s\n",
            result.error.c_str());
        return false;
    }
    if (result.statecode.empty()) {
        std::fprintf(stderr, "[验证] 登录响应缺少状态码\n");
        return false;
    }
    if (!WriteAtomically(kCardCachePath, cardKey, 0600)) {
        std::fprintf(stderr, "[验证] 卡密缓存保存失败\n");
        return false;
    }

    runtime->cardKey = cardKey;
    runtime->stateCode = result.statecode;
    session.cardKey = cardKey;
    session.deviceCode = deviceCode;
    session.expiresAt = result.end_time;
    session.runtime_ = std::move(runtime);
    if (!session.runtime_->Start()) {
        std::fprintf(stderr, "[验证] 心跳线程启动失败\n");
        session.Reset();
        return false;
    }

    std::cout << "登录成功";
    if (!session.expiresAt.empty()) {
        std::cout << ", 有效期: " << session.expiresAt;
    }
    std::cout << "\n";
    return true;
}

}  // namespace lengjing::auth
