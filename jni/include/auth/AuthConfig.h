#pragma once

#include <cstddef>
#include <string_view>

#ifndef LENGJING_AUTH_LOGIN_CODE
#define LENGJING_AUTH_LOGIN_CODE "94B85662DCA20F58"
#endif

#ifndef LENGJING_AUTH_NOTICE_CODE
#define LENGJING_AUTH_NOTICE_CODE "6E11BEAE1A780265"
#endif

#ifndef LENGJING_AUTH_VERSION_CODE
#define LENGJING_AUTH_VERSION_CODE "00B4CF3B7EA10DBC"
#endif

#ifndef LENGJING_AUTH_HEARTBEAT_CODE
#define LENGJING_AUTH_HEARTBEAT_CODE "07EC1E2FBF7AA5EA"
#endif

#ifndef LENGJING_AUTH_APP_KEY
#define LENGJING_AUTH_APP_KEY "ec0914feb5a0f337f9b10f81151131e9"
#endif

#ifndef LENGJING_AUTH_RSA_PUBLIC_KEY
#define LENGJING_AUTH_RSA_PUBLIC_KEY                                      \
    "-----BEGIN PUBLIC KEY-----\n"                                        \
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDk1nOnsM375bj1fmmQ0m9nc3I7\n" \
    "I2bAkyxNN0BQWQJlCUDMiy3aOHstlMXghCLHkPH2xxaDDL4Wr4F9/5t5Za13M10/\n" \
    "IMOHZOd5bqP7BSDbTdNC0a2EB/YvfhzgP7G3nCHSrJqsDCcXHNaCMUDaKX+Glaok\n" \
    "Ugi/gqIZ6dr5mRPH6QIDAQAB\n"                                          \
    "-----END PUBLIC KEY-----"
#endif

#ifndef LENGJING_AUTH_HEARTBEAT_SECONDS
#define LENGJING_AUTH_HEARTBEAT_SECONDS 60
#endif

#ifndef LENGJING_AUTH_MAX_HEARTBEAT_FAILURES
#define LENGJING_AUTH_MAX_HEARTBEAT_FAILURES 5
#endif

namespace lengjing::auth::config {

inline constexpr std::string_view kLoginCode = LENGJING_AUTH_LOGIN_CODE;
inline constexpr std::string_view kNoticeCode = LENGJING_AUTH_NOTICE_CODE;
inline constexpr std::string_view kVersionCode = LENGJING_AUTH_VERSION_CODE;
inline constexpr std::string_view kHeartbeatCode =
    LENGJING_AUTH_HEARTBEAT_CODE;
inline constexpr std::string_view kAppKey = LENGJING_AUTH_APP_KEY;
inline constexpr std::string_view kRsaPublicKey =
    LENGJING_AUTH_RSA_PUBLIC_KEY;
inline constexpr int kHeartbeatSeconds = LENGJING_AUTH_HEARTBEAT_SECONDS;
inline constexpr int kMaximumHeartbeatFailures =
    LENGJING_AUTH_MAX_HEARTBEAT_FAILURES;

inline constexpr bool IsComplete() {
    return !kLoginCode.empty() &&
        !kHeartbeatCode.empty() &&
        !kAppKey.empty() &&
        !kRsaPublicKey.empty() &&
        kHeartbeatSeconds > 0 &&
        kMaximumHeartbeatFailures > 0;
}

}  // namespace lengjing::auth::config
