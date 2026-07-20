#pragma once

#include "auth/AuthConfig.h"
#include "auth/CloudLayout.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace lengjing::auth {

inline constexpr std::size_t kMaximumEncodedCloudVariablePayloadBytes =
    kMaximumCloudLayoutPayloadBytes * 10U;

enum class CloudVariablePayloadDecodeStatus {
    Success,
    InputTooLarge,
    OutputTooLarge,
    UnsupportedEntity,
};

CloudVariablePayloadDecodeStatus DecodeCloudVariablePayload(
    std::string& payload) noexcept;

enum class AuthState {
    Idle,
    Authenticating,
    Valid,
    Invalid,
    Stopped,
};

struct AuthLoginResult {
    bool success = false;
    std::string error;
    std::string stateCode;
    std::string expiresAt;
};

struct AuthCallResult {
    bool success = false;
    std::string error;
};

struct AuthVariableResult {
    bool success = false;
    std::string error;
    std::string value;
};

class AuthGateway {
public:
    virtual ~AuthGateway() = default;

    virtual AuthLoginResult Login(std::string_view cardKey,
                                  std::string_view deviceCode) = 0;
    virtual AuthCallResult Heartbeat(std::string_view cardKey,
                                     std::string_view stateCode) = 0;
    virtual AuthVariableResult GetVariableByCard(
        std::string_view cardKey,
        std::string_view valueId,
        std::string_view valueName) = 0;
    virtual void CancelPendingRequests() noexcept = 0;
};

struct AuthSessionOptions {
    std::chrono::milliseconds heartbeatInterval{
        std::chrono::seconds(kHeartbeatIntervalSeconds)};
    std::chrono::milliseconds stopTimeout{500};
    int maximumHeartbeatFailures = kMaximumHeartbeatFailures;
    CloudVariableConfig cloudVariable{};
};

class AuthSession final {
public:
    AuthSession();
    ~AuthSession();

    AuthSession(const AuthSession&) = delete;
    AuthSession& operator=(const AuthSession&) = delete;
    AuthSession(AuthSession&&) = delete;
    AuthSession& operator=(AuthSession&&) = delete;

    bool Login(std::shared_ptr<AuthGateway> gateway,
               std::string cardKey,
               std::string deviceCode,
               AuthSessionOptions options = {});
    void Stop() noexcept;

    AuthState State() const noexcept;
    bool IsValid() const noexcept;
    bool ExitRequested() const noexcept;
    std::string LastError() const;
    std::string ExpiresAt() const;

    CloudLayoutUpdateResult RefreshCloudLayout(CloudLayoutStore& store);

private:
    struct Runtime;
    std::shared_ptr<Runtime> runtime_;
};

std::shared_ptr<AuthGateway> CreateT3Gateway(
    const T3AuthConfig& config,
    std::string& error);

std::string ResolveDeviceCode();

CloudRuntimeIdentity ResolveCloudRuntimeIdentity(
    const T3AuthConfig& config = kDefaultT3AuthConfig);

bool LoginInteractive(
    AuthSession& session,
    std::string_view deviceCode = {},
    const T3AuthConfig& config = kDefaultT3AuthConfig);

}  // namespace lengjing::auth
