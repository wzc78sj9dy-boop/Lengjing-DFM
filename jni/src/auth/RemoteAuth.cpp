#include "auth/RemoteAuth.h"

#include "app/RuntimePresentationPolicy.h"
#include "auth/CardInputPolicy.h"
#include "t3/t3sdk.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
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
#endif
#if defined(_WIN32)
#include <io.h>
#else
#include <termios.h>
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

    AuthVersionResult GetLatestVersion() override {
        if (verifier_ == nullptr) {
            return {false, "T3 SDK is not initialized", {}};
        }
        const T3VersionResult result = verifier_->getLatestVersion();
        return {result.success, result.error, result.version};
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

#if !defined(_WIN32)
struct TerminalEchoState {
    int descriptor = STDIN_FILENO;
    termios original{};
    bool captured = false;
};

bool DisableTerminalEcho(void* opaque) noexcept {
    auto& state = *static_cast<TerminalEchoState*>(opaque);
    int result = 0;
    do {
        result = tcgetattr(state.descriptor, &state.original);
    } while (result != 0 && errno == EINTR);
    if (result != 0) return false;

    termios hidden = state.original;
    hidden.c_lflag &= static_cast<tcflag_t>(
        ~(ECHO | ECHONL | ECHOE | ECHOK));
    do {
        result = tcsetattr(state.descriptor, TCSAFLUSH, &hidden);
    } while (result != 0 && errno == EINTR);
    if (result != 0) return false;
    state.captured = true;
    return true;
}

bool RestoreTerminalEcho(void* opaque) noexcept {
    auto& state = *static_cast<TerminalEchoState*>(opaque);
    if (!state.captured) return true;
    int result = 0;
    do {
        result = tcsetattr(state.descriptor, TCSAFLUSH, &state.original);
    } while (result != 0 && errno == EINTR);
    if (result != 0) return false;
    state.captured = false;
    return true;
}
#endif

enum class RemainingEntityStatus {
    None,
    Supported,
    Unsupported,
};

bool DecodeSupportedEntity(std::string_view payload,
                           std::size_t offset,
                           char& decoded,
                           std::size_t& consumed) noexcept {
    if (payload.compare(offset, 6, "&quot;") == 0) {
        decoded = '"';
        consumed = 6;
        return true;
    }
    if (payload.compare(offset, 5, "&amp;") == 0) {
        decoded = '&';
        consumed = 5;
        return true;
    }
    if (payload.compare(offset, 5, "&#34;") == 0) {
        decoded = '"';
        consumed = 5;
        return true;
    }
    if (payload.compare(offset, 6, "&#x22;") == 0) {
        decoded = '"';
        consumed = 6;
        return true;
    }
    return false;
}

bool LooksLikeEntity(std::string_view payload,
                     std::size_t offset) noexcept {
    constexpr std::size_t kMaximumEntityBytes = 16;
    const std::size_t end = payload.find(';', offset + 1U);
    if (end == std::string_view::npos || end == offset + 1U ||
        end - offset + 1U > kMaximumEntityBytes) {
        return false;
    }
    for (std::size_t index = offset + 1U; index < end; ++index) {
        const char character = payload[index];
        const bool allowed =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '#';
        if (!allowed) return false;
    }
    return true;
}

CloudVariablePayloadDecodeStatus DecodeEntityPass(
    std::string& payload,
    bool allowLiteralAmpersands,
    std::size_t maximumOutputBytes) noexcept {
    std::size_t readOffset = 0;
    std::size_t writeOffset = 0;
    while (readOffset < payload.size()) {
        char decoded = payload[readOffset];
        std::size_t consumed = 1;
        if (decoded == '&' &&
            !DecodeSupportedEntity(payload, readOffset,
                                   decoded, consumed)) {
            if (!allowLiteralAmpersands ||
                LooksLikeEntity(payload, readOffset)) {
                payload.clear();
                return CloudVariablePayloadDecodeStatus::UnsupportedEntity;
            }
        }

        if (writeOffset == maximumOutputBytes) {
            payload.clear();
            return CloudVariablePayloadDecodeStatus::OutputTooLarge;
        }
        payload[writeOffset++] = decoded;
        readOffset += consumed;
    }
    payload.resize(writeOffset);
    return CloudVariablePayloadDecodeStatus::Success;
}

RemainingEntityStatus FindRemainingEntity(
    std::string_view payload) noexcept {
    for (std::size_t offset = 0; offset < payload.size(); ++offset) {
        if (payload[offset] != '&') continue;
        char decoded = 0;
        std::size_t consumed = 0;
        if (DecodeSupportedEntity(payload, offset, decoded, consumed)) {
            return RemainingEntityStatus::Supported;
        }
        if (LooksLikeEntity(payload, offset)) {
            return RemainingEntityStatus::Unsupported;
        }
    }
    return RemainingEntityStatus::None;
}

}  // namespace

CloudVariablePayloadDecodeStatus DecodeCloudVariablePayload(
    std::string& payload) noexcept {
    if (payload.size() > kMaximumEncodedCloudVariablePayloadBytes) {
        payload.clear();
        return CloudVariablePayloadDecodeStatus::InputTooLarge;
    }

    CloudVariablePayloadDecodeStatus status =
        DecodeEntityPass(payload, false,
                         kMaximumCloudLayoutPayloadBytes * 6U);
    if (status != CloudVariablePayloadDecodeStatus::Success) return status;

    RemainingEntityStatus remaining = FindRemainingEntity(payload);
    if (remaining == RemainingEntityStatus::Unsupported) {
        payload.clear();
        return CloudVariablePayloadDecodeStatus::UnsupportedEntity;
    }
    if (remaining == RemainingEntityStatus::None) {
        if (payload.size() > kMaximumCloudLayoutPayloadBytes) {
            payload.clear();
            return CloudVariablePayloadDecodeStatus::OutputTooLarge;
        }
        return CloudVariablePayloadDecodeStatus::Success;
    }

    status = DecodeEntityPass(payload, true,
                              kMaximumCloudLayoutPayloadBytes);
    if (status != CloudVariablePayloadDecodeStatus::Success) return status;

    remaining = FindRemainingEntity(payload);
    if (remaining != RemainingEntityStatus::None) {
        payload.clear();
        return CloudVariablePayloadDecodeStatus::UnsupportedEntity;
    }
    return CloudVariablePayloadDecodeStatus::Success;
}

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
        AuthVariableResult result = runtime_->gateway->GetVariableByCard(
            runtime_->cardKey,
            runtime_->cloudVariable.valueId,
            runtime_->cloudVariable.valueName);
        if (!IsValid()) {
            return {CloudLayoutStatus::SessionInvalid,
                    "authentication session ended during cloud layout fetch",
                    store.Snapshot()};
        }
        if (!result.success) {
            return {CloudLayoutStatus::FetchFailed,
                    result.error.empty()
                        ? "getVariableByKami failed"
                        : runtime_->Sanitize(result.error),
                    store.Snapshot()};
        }

        const CloudVariablePayloadDecodeStatus decodeStatus =
            DecodeCloudVariablePayload(result.value);
        if (decodeStatus != CloudVariablePayloadDecodeStatus::Success) {
            const char* detail =
                "cloud variable payload contains an unsupported HTML entity";
            if (decodeStatus ==
                CloudVariablePayloadDecodeStatus::InputTooLarge) {
                detail = "encoded cloud variable payload is too large";
            } else if (decodeStatus ==
                       CloudVariablePayloadDecodeStatus::OutputTooLarge) {
                detail = "decoded cloud variable payload is too large";
            }
            return {CloudLayoutStatus::InvalidJson, detail, store.Snapshot()};
        }
        const CloudLayoutUpdateResult update =
            store.ValidateAndPublish(result.value);
        if (!IsValid()) {
            return {CloudLayoutStatus::SessionInvalid,
                    "authentication session ended during cloud layout validation",
                    store.Snapshot()};
        }
        return update;
    } catch (const std::exception& exception) {
        return {CloudLayoutStatus::FetchFailed,
                runtime_->Sanitize(exception.what()),
                store.Snapshot()};
    } catch (...) {
        return {CloudLayoutStatus::FetchFailed,
                "getVariableByKami threw an unknown exception",
                store.Snapshot()};
    }
}

std::shared_ptr<AuthGateway> CreateT3Gateway(
    const T3AuthConfig& config,
    std::string& error) {
    auto gateway = std::make_shared<T3Gateway>();
    if (!gateway->Initialize(config, error)) return {};
    error.clear();
    return gateway;
}

CloudVersionStatus CheckCloudVersion(
    AuthGateway& gateway,
    std::string_view currentVersion) noexcept {
    if (currentVersion.empty()) return CloudVersionStatus::CheckFailed;
    try {
        const AuthVersionResult result = gateway.GetLatestVersion();
        if (!result.success || result.version.empty()) {
            return CloudVersionStatus::CheckFailed;
        }
        return result.version == currentVersion
            ? CloudVersionStatus::Current
            : CloudVersionStatus::UpdateRequired;
    } catch (...) {
        return CloudVersionStatus::CheckFailed;
    }
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
                      std::string_view currentVersion,
                      std::string_view deviceCode,
                      const T3AuthConfig& config) {
    std::string error;
    std::shared_ptr<AuthGateway> gateway = CreateT3Gateway(config, error);
    if (gateway == nullptr) {
        if (!error.empty()) {
            std::fprintf(stderr, "[验证] %s\n", error.c_str());
        }
        std::fprintf(
            stderr, "%s\n", app::VerificationFailureText());
        return false;
    }

    const CloudVersionStatus versionStatus =
        CheckCloudVersion(*gateway, currentVersion);
    if (versionStatus != CloudVersionStatus::Current) {
        if (versionStatus == CloudVersionStatus::CheckFailed) {
            std::fprintf(stderr, "[验证] version check failed\n");
        }
        std::fprintf(
            stderr, "%s\n",
            versionStatus == CloudVersionStatus::UpdateRequired
                ? app::UpdateRequiredText()
                : app::VerificationFailureText());
        return false;
    }

    const bool inputIsTerminal = InputIsTerminal();
    input::TerminalEchoControl terminalEcho;
#if !defined(_WIN32)
    TerminalEchoState terminalEchoState;
    terminalEcho = {
        &terminalEchoState, &DisableTerminalEcho, &RestoreTerminalEcho};
#endif
    input::CardInputResult card = input::ReadCardKeyFromStream(
        std::cin, std::cout, inputIsTerminal, terminalEcho);
    if (!card) {
        std::fprintf(stderr, "[验证] card input status=%u\n",
                     static_cast<unsigned int>(card.status));
        std::fprintf(
            stderr, "%s\n", app::VerificationFailureText());
        return false;
    }
    std::string resolvedDeviceCode = deviceCode.empty()
        ? ResolveDeviceCode()
        : std::string(deviceCode);
    if (resolvedDeviceCode.empty()) {
        SecureClear(card.value);
        std::fprintf(stderr, "[验证] device code is empty\n");
        std::fprintf(
            stderr, "%s\n", app::VerificationFailureText());
        return false;
    }

    AuthSessionOptions options;
    options.cloudVariable = config.cloudVariable;
    if (!session.Login(std::move(gateway), std::move(card.value),
                       std::move(resolvedDeviceCode), options)) {
        const std::string detail = session.LastError();
        if (!detail.empty()) {
            std::fprintf(stderr, "[验证] %s\n", detail.c_str());
        }
        std::fprintf(
            stderr, "%s\n", app::VerificationFailureText());
        return false;
    }
    return true;
}

}  // namespace lengjing::auth
