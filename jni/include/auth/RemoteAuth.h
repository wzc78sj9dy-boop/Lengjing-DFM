#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace lengjing::auth {

class AuthSession final {
public:
    AuthSession() noexcept;
    ~AuthSession();

    AuthSession(const AuthSession&) = delete;
    AuthSession& operator=(const AuthSession&) = delete;
    AuthSession(AuthSession&& other) noexcept;
    AuthSession& operator=(AuthSession&& other) noexcept;

    bool IsValid() const noexcept;
    bool ExitRequested() const noexcept;

    std::string cardKey;
    std::string deviceCode;
    std::string expiresAt;

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;

    void Reset() noexcept;

    friend bool LoginInteractive(std::string_view programDirectory,
                                 AuthSession& session);
};

bool LoginInteractive(std::string_view programDirectory,
                      AuthSession& session);

}  // namespace lengjing::auth
