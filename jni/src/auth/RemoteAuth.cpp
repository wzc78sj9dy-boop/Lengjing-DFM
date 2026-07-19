#include "auth/RemoteAuth.h"

#include "auth/CardInputPolicy.h"
#include "t3/t3sdk.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lengjing::auth {
namespace {

struct OwnedCloudVariableConfig {
    std::string callCode;
    std::string valueId;
    std::string valueName;

    bool IsConfigured() const noexcept {
        return !callCode.empty() && !valueId.empty() && !valueName.empty();
    }
};

OwnedCloudVariableConfig Own(const CloudVariableConfig& config) {
    return {std::string(config.callCode), std::string(config.valueId),
            std::string(config.valueName)};
}

class T3Gateway final : public AuthGateway {
public:
    bool Initialize(const T3AuthConfig& config, std::string& error) {
        if (!config.IsLoginConfigured()) {
            error = "T3 login configuration is incomplete";
            return false;
        }
        verifier_ = std::make_unique<T3Verify>();
        if (!verifier_->initRSA(
                std::string(config.loginCode),
                std::string(config.noticeCode),
                std::string(config.versionCode),
                std::string(config.heartbeatCode),
                std::string(config.appKey),
                std::string(config.rsaPublicKey))) {
            verifier_.reset();
            error = "T3 SDK initialization failed";
            return false;
        }
        if (config.cloudVariable.IsConfigured()) {
            verifier_->setCode(
                "get_variable", std::string(config.cloudVariable.callCode));
        }
        return true;
    }

    AuthLoginResult Login(std::string_view cardKey,
                          std::string_view deviceCode) override {
        if (verifier_ == nullptr) {
            return {false, "T3 SDK is not initialized", {}, {}};
        }
        verifier_->resetPendingCancellation();
        const T3LoginResult result = verifier_->login(
            std::string(cardKey), std::string(deviceCode));
        return {result.success, result.error, result.statecode,
                result.end_time};
    }

    AuthCallResult Heartbeat(std::string_view cardKey,
                             std::string_view stateCode) override {
        if (verifier_ == nullptr) {
            return {false, "T3 SDK is not initialized"};
        }
        const T3Result result = verifier_->heartbeat(
            std::string(cardKey), std::string(stateCode));
        return {result.success, result.error};
    }

    AuthVariableResult GetVariableByCard(
        std::string_view cardKey,
        std::string_view valueId,
        std::string_view valueName) override {
        if (verifier_ == nullptr) {
            return {false, "T3 SDK is not initialized", {}};
        }
        const T3VariableResult result = verifier_->getVariableByKami(
            std::string(cardKey), std::string(valueId),
            std::string(valueName));
        return {result.success, result.error, result.value};
    }

    void CancelPendingRequests() noexcept override {
        if (verifier_ != nullptr) verifier_->cancelPendingRequests();
    }

private:
    std::unique_ptr<T3Verify> verifier_;
};

void SecureClear(std::string& value) noexcept {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

std::string SanitizeDiagnostic(
    std::string value,
    std::initializer_list<std::string_view> sensitiveValues) {
    constexpr std::size_t kMaximumDiagnosticBytes = 512;
    for (const std::string_view sensitive : sensitiveValues) {
        if (sensitive.size() < 4) continue;
        std::size_t position = 0;
        while ((position = value.find(sensitive, position)) !=
               std::string::npos) {
            constexpr std::string_view replacement = "[redacted]";
            value.replace(position, sensitive.size(), replacement);
            position += replacement.size();
        }
    }
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > kMaximumDiagnosticBytes) {
        value.resize(kMaximumDiagnosticBytes);
    }
    return value;
}

#if defined(__ANDROID__)
std::string Trimmed(std::string value) {
    const auto whitespace = [](unsigned char character) {
        return character == ' ' || character == '\t' ||
            character == '\r' || character == '\n';
    };
    while (!value.empty() &&
           whitespace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const auto first = std::find_if_not(
        value.begin(), value.end(), [&](char character) {
            return whitespace(static_cast<unsigned char>(character));
        });
    value.erase(value.begin(), first);
    return value;
}

std::string StableDeviceHash(std::string_view seed) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    std::uint64_t first = 1469598103934665603ULL;
    std::uint64_t second = 1099511628211ULL;
    for (const char character : seed) {
        const auto byte = static_cast<unsigned char>(character);
        first = (first ^ byte) * kFnvPrime;
        second = (second ^ static_cast<unsigned char>(byte + 0x5dU)) *
            kFnvPrime;
    }
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << first << std::setw(16) << second;
    return stream.str();
}

std::string ReadCommandValue(const char* command) {
    FILE* pipe = popen(command, "r");
    if (pipe == nullptr) return {};
    char buffer[256]{};
    std::string value;
    if (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        value = buffer;
    }
    pclose(pipe);
    value = Trimmed(std::move(value));
    return value == "null" || value == "unknown" ? std::string{} : value;
}

std::string ReadSystemProperty(const char* name) {
    char buffer[PROP_VALUE_MAX]{};
    const int length = __system_property_get(name, buffer);
    if (length <= 0) return {};
    std::string value(buffer, static_cast<std::size_t>(length));
    value = Trimmed(std::move(value));
    return value == "null" || value == "unknown" ? std::string{} : value;
}
#endif

bool InputIsTerminal() noexcept {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) == 1;
#endif
}

}  // namespace

struct AuthSession::Runtime final {
    std::atomic<AuthState> state{AuthState::Idle};
    std::atomic_bool stopRequested{false};
    std::shared_ptr<AuthGateway> gateway;
    std::string cardKey;
    std::string deviceCode;
    std::string stateCode;
    OwnedCloudVariableConfig cloudVariable;
    AuthSessionOptions options;

    mutable std::mutex metadataMutex;
    std::string lastError;
    std::string expiresAt;

    std::mutex requestMutex;
    std::mutex gatewayMutex;
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::thread heartbeatThread;
    bool heartbeatStarted = false;
    bool heartbeatFinished = false;

    std::string Sanitize(std::string error) const {
        return SanitizeDiagnostic(
            std::move(error), {cardKey, deviceCode, stateCode});
    }

    void SetInvalid(std::string error) {
        {
            std::lock_guard<std::mutex> lock(metadataMutex);
            lastError = Sanitize(std::move(error));
        }
        state.store(AuthState::Invalid, std::memory_order_release);
        stopRequested.store(true, std::memory_order_release);
        waitCondition.notify_all();
    }

    void ClearSensitiveState() noexcept {
        std::lock_guard<std::mutex> requestLock(requestMutex);
        std::lock_guard<std::mutex> gatewayLock(gatewayMutex);
        gateway.reset();
        SecureClear(cardKey);
        deviceCode.clear();
        stateCode.clear();
        cloudVariable = {};
    }

    void HeartbeatLoop() noexcept {
        int failureCount = 0;
        std::unique_lock<std::mutex> waitLock(waitMutex);
        while (!waitCondition.wait_for(
            waitLock, options.heartbeatInterval, [this] {
                return stopRequested.load(std::memory_order_acquire);
            })) {
            waitLock.unlock();
            AuthCallResult result;
            try {
                std::shared_ptr<AuthGateway> gatewaySnapshot;
                {
                    std::lock_guard<std::mutex> gatewayLock(gatewayMutex);
                    gatewaySnapshot = gateway;
                }
                if (gatewaySnapshot == nullptr) {
                    result = {false, "T3 gateway is unavailable"};
                } else {
                    std::lock_guard<std::mutex> requestLock(requestMutex);
                    result = gatewaySnapshot->Heartbeat(cardKey, stateCode);
                }
            } catch (const std::exception& exception) {
                result = {false, exception.what()};
            } catch (...) {
                result = {false, "T3 heartbeat threw an unknown exception"};
            }
            waitLock.lock();
            if (stopRequested.load(std::memory_order_acquire)) break;
            if (result.success) {
                failureCount = 0;
                continue;
            }
            ++failureCount;
            if (failureCount >= options.maximumHeartbeatFailures) {
                SetInvalid(result.error.empty()
                               ? "T3 heartbeat invalidated the session"
                               : std::move(result.error));
                break;
            }
        }
        waitLock.unlock();
        ClearSensitiveState();
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            heartbeatFinished = true;
        }
        waitCondition.notify_all();
    }

    void StartHeartbeat(const std::shared_ptr<Runtime>& self) {
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            heartbeatStarted = false;
            heartbeatFinished = false;
        }
        heartbeatThread = std::thread([self] { self->HeartbeatLoop(); });
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            heartbeatStarted = true;
        }
    }

    bool Stop() noexcept {
        if (state.load(std::memory_order_acquire) != AuthState::Invalid) {
            state.store(AuthState::Stopped, std::memory_order_release);
        }
        stopRequested.store(true, std::memory_order_release);
        waitCondition.notify_all();

        std::shared_ptr<AuthGateway> gatewaySnapshot;
        {
            std::lock_guard<std::mutex> gatewayLock(gatewayMutex);
            gatewaySnapshot = gateway;
        }
        if (gatewaySnapshot != nullptr) {
            try {
                gatewaySnapshot->CancelPendingRequests();
            } catch (...) {
            }
        }

        if (heartbeatThread.joinable()) {
            bool finished = false;
            {
                std::unique_lock<std::mutex> waitLock(waitMutex);
                finished = waitCondition.wait_for(
                    waitLock, options.stopTimeout,
                    [this] { return heartbeatFinished; });
            }
            if (finished) {
                heartbeatThread.join();
                return true;
            }
            heartbeatThread.detach();
            return false;
        }

        bool mayClear = false;
        {
            std::lock_guard<std::mutex> waitLock(waitMutex);
            mayClear = !heartbeatStarted || heartbeatFinished;
        }
        if (mayClear) ClearSensitiveState();
        return mayClear;
    }
};

AuthSession::AuthSession() : runtime_(std::make_shared<Runtime>()) {}

AuthSession::~AuthSession() {
    Stop();
}

bool AuthSession::Login(std::shared_ptr<AuthGateway> gateway,
                        std::string cardKey,
                        std::string deviceCode,
                        AuthSessionOptions options) {
    Stop();
    runtime_ = std::make_shared<Runtime>();
    runtime_->state.store(AuthState::Authenticating,
                          std::memory_order_release);
    runtime_->stopRequested.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(runtime_->metadataMutex);
        runtime_->lastError.clear();
        runtime_->expiresAt.clear();
    }

    constexpr auto kMaximumHeartbeatInterval = std::chrono::hours(24);
    constexpr auto kMaximumStopTimeout = std::chrono::seconds(10);
    if (gateway == nullptr || cardKey.empty() || cardKey.size() > 256 ||
        deviceCode.empty() || deviceCode.size() > 256 ||
        options.heartbeatInterval.count() <= 0 ||
        options.heartbeatInterval > kMaximumHeartbeatInterval ||
        options.stopTimeout.count() <= 0 ||
        options.stopTimeout > kMaximumStopTimeout ||
        options.maximumHeartbeatFailures < 1 ||
        options.maximumHeartbeatFailures > 100) {
        runtime_->SetInvalid("invalid authentication session options");
        SecureClear(cardKey);
        return false;
    }

    runtime_->gateway = std::move(gateway);
    runtime_->cardKey = std::move(cardKey);
    runtime_->deviceCode = std::move(deviceCode);
    runtime_->cloudVariable = Own(options.cloudVariable);
    runtime_->options = options;
    runtime_->options.cloudVariable = {};

    AuthLoginResult result;
    try {
        std::lock_guard<std::mutex> requestLock(runtime_->requestMutex);
        result = runtime_->gateway->Login(
            runtime_->cardKey, runtime_->deviceCode);
    } catch (const std::exception& exception) {
        runtime_->SetInvalid(exception.what());
        runtime_->Stop();
        return false;
    } catch (...) {
        runtime_->SetInvalid("T3 login threw an unknown exception");
        runtime_->Stop();
        return false;
    }
    if (!result.success || result.stateCode.empty()) {
        runtime_->SetInvalid(
            !result.error.empty() ? std::move(result.error)
                                  : "T3 login response has no state code");
        runtime_->Stop();
        return false;
    }

    runtime_->stateCode = std::move(result.stateCode);
    {
        std::lock_guard<std::mutex> lock(runtime_->metadataMutex);
        runtime_->expiresAt = std::move(result.expiresAt);
    }
    runtime_->state.store(AuthState::Valid, std::memory_order_release);
    try {
        runtime_->StartHeartbeat(runtime_);
    } catch (const std::exception& exception) {
        runtime_->SetInvalid(exception.what());
        runtime_->Stop();
        return false;
    } catch (...) {
        runtime_->SetInvalid("failed to start heartbeat thread");
        runtime_->Stop();
        return false;
    }
    return true;
}

void AuthSession::Stop() noexcept {
    if (runtime_ != nullptr) runtime_->Stop();
}

AuthState AuthSession::State() const noexcept {
    return runtime_->state.load(std::memory_order_acquire);
}

bool AuthSession::IsValid() const noexcept {
    return State() == AuthState::Valid;
}

bool AuthSession::ExitRequested() const noexcept {
    return State() == AuthState::Invalid;
}

std::string AuthSession::LastError() const {
    std::lock_guard<std::mutex> lock(runtime_->metadataMutex);
    return runtime_->lastError;
}

std::string AuthSession::ExpiresAt() const {
    std::lock_guard<std::mutex> lock(runtime_->metadataMutex);
    return runtime_->expiresAt;
}

CloudLayoutUpdateResult AuthSession::RefreshCloudLayout(
    CloudLayoutStore& store) {
    AuthVariableResult result;
    try {
        std::lock_guard<std::mutex> requestLock(runtime_->requestMutex);
        if (!IsValid()) {
            return {CloudLayoutStatus::SessionInvalid,
                    "authentication session is not valid", store.Snapshot()};
        }
        if (!runtime_->cloudVariable.IsConfigured()) {
            return {CloudLayoutStatus::NotConfigured,
                    "get_variable call code, value id, or value name is missing",
                    store.Snapshot()};
        }
        result = runtime_->gateway->GetVariableByCard(
            runtime_->cardKey,
            runtime_->cloudVariable.valueId,
            runtime_->cloudVariable.valueName);
    } catch (const std::exception& exception) {
        return {CloudLayoutStatus::FetchFailed,
                runtime_->Sanitize(exception.what()),
                store.Snapshot()};
    } catch (...) {
        return {CloudLayoutStatus::FetchFailed,
                "getVariableByKami threw an unknown exception",
                store.Snapshot()};
    }
    if (!result.success) {
        return {CloudLayoutStatus::FetchFailed,
                result.error.empty()
                    ? "getVariableByKami failed"
                    : runtime_->Sanitize(std::move(result.error)),
                store.Snapshot()};
    }
    return store.ValidateAndPublish(result.value);
}

std::shared_ptr<AuthGateway> CreateT3Gateway(
    const T3AuthConfig& config,
    std::string& error) {
    auto gateway = std::make_shared<T3Gateway>();
    if (!gateway->Initialize(config, error)) return {};
    error.clear();
    return gateway;
}

std::string ResolveDeviceCode() {
#if defined(__ANDROID__)
    std::string androidId =
        ReadCommandValue("settings get secure android_id 2>/dev/null");
    const bool validAndroidId = androidId.size() >= 8 &&
        androidId.size() <= 32 &&
        std::all_of(androidId.begin(), androidId.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        });
    if (!validAndroidId) androidId.clear();
    std::transform(androidId.begin(), androidId.end(), androidId.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });

    std::string serial = ReadSystemProperty("ro.serialno");
    if (serial.empty()) serial = ReadSystemProperty("ro.boot.serialno");
    if (!androidId.empty() || !serial.empty()) {
        return StableDeviceHash(androidId + "|" + serial);
    }
#endif
    return getMachineCode();
}

CloudRuntimeIdentity ResolveCloudRuntimeIdentity(
    const T3AuthConfig& config) {
    return {
        std::string(config.cloudIdentity.packageName),
        std::string(config.cloudIdentity.moduleName),
        std::string(config.cloudIdentity.buildId),
    };
}

bool LoginInteractive(AuthSession& session,
                      std::string_view deviceCode,
                      const T3AuthConfig& config) {
    std::string error;
    std::shared_ptr<AuthGateway> gateway = CreateT3Gateway(config, error);
    if (gateway == nullptr) {
        std::fprintf(stderr, "[验证] 初始化失败: %s\n", error.c_str());
        return false;
    }

    input::CardInputResult card = input::ReadCardKeyFromStream(
        std::cin, std::cout, InputIsTerminal());
    if (!card) {
        std::fprintf(stderr, "[验证] 卡密为空或无效\n");
        return false;
    }
    std::string resolvedDeviceCode = deviceCode.empty()
        ? ResolveDeviceCode()
        : std::string(deviceCode);
    if (resolvedDeviceCode.empty()) {
        SecureClear(card.value);
        std::fprintf(stderr, "[验证] 设备码获取失败\n");
        return false;
    }

    AuthSessionOptions options;
    options.cloudVariable = config.cloudVariable;
    if (!session.Login(std::move(gateway), std::move(card.value),
                       std::move(resolvedDeviceCode), options)) {
        std::fprintf(stderr, "[验证] 登录失败: %s\n",
                     session.LastError().c_str());
        return false;
    }

    const std::string expiresAt = session.ExpiresAt();
    std::cout << "登录成功";
    if (!expiresAt.empty()) std::cout << ", 有效期: " << expiresAt;
    std::cout << '\n';
    return true;
}

}  // namespace lengjing::auth
