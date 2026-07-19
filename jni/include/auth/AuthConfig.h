#pragma once

#include <string_view>

// 验证参数必须由私有构建配置注入，仓库默认不携带签名材料。
#ifndef LENGJING_T3_LOGIN_CODE
#define LENGJING_T3_LOGIN_CODE ""
#endif

#ifndef LENGJING_T3_NOTICE_CODE
#define LENGJING_T3_NOTICE_CODE ""
#endif

#ifndef LENGJING_T3_VERSION_CODE
#define LENGJING_T3_VERSION_CODE ""
#endif

#ifndef LENGJING_T3_HEARTBEAT_CODE
#define LENGJING_T3_HEARTBEAT_CODE ""
#endif

#ifndef LENGJING_T3_APP_KEY
#define LENGJING_T3_APP_KEY ""
#endif

#ifndef LENGJING_T3_RSA_PUBLIC_KEY
#define LENGJING_T3_RSA_PUBLIC_KEY ""
#endif

#ifndef LENGJING_T3_GET_VARIABLE_CODE
#define LENGJING_T3_GET_VARIABLE_CODE ""
#endif

#ifndef LENGJING_T3_LAYOUT_VALUE_ID
#define LENGJING_T3_LAYOUT_VALUE_ID ""
#endif

#ifndef LENGJING_T3_LAYOUT_VALUE_NAME
#define LENGJING_T3_LAYOUT_VALUE_NAME ""
#endif

#ifndef LENGJING_T3_LAYOUT_PACKAGE
#define LENGJING_T3_LAYOUT_PACKAGE "com.tencent.tmgp.dfm"
#endif

#ifndef LENGJING_T3_LAYOUT_MODULE
#define LENGJING_T3_LAYOUT_MODULE "libUE4.so"
#endif

#ifndef LENGJING_T3_LAYOUT_BUILD_ID
#define LENGJING_T3_LAYOUT_BUILD_ID ""
#endif

namespace lengjing::auth {

struct CloudVariableConfig {
    std::string_view callCode;
    std::string_view valueId;
    std::string_view valueName;

    constexpr bool IsConfigured() const noexcept {
        return !callCode.empty() && !valueId.empty() && !valueName.empty();
    }

    constexpr bool HasAnyValue() const noexcept {
        return !callCode.empty() || !valueId.empty() || !valueName.empty();
    }
};

struct CloudIdentityConfig {
    std::string_view packageName;
    std::string_view moduleName;
    std::string_view buildId;

    constexpr bool IsConfigured() const noexcept {
        return !packageName.empty() && !moduleName.empty() && !buildId.empty();
    }
};

struct T3AuthConfig {
    std::string_view loginCode;
    std::string_view noticeCode;
    std::string_view versionCode;
    std::string_view heartbeatCode;
    std::string_view appKey;
    std::string_view rsaPublicKey;
    CloudVariableConfig cloudVariable;
    CloudIdentityConfig cloudIdentity;

    constexpr bool IsLoginConfigured() const noexcept {
        return !loginCode.empty() && !noticeCode.empty() &&
            !versionCode.empty() && !heartbeatCode.empty() &&
            !appKey.empty() && !rsaPublicKey.empty();
    }
};

inline constexpr int kHeartbeatIntervalSeconds = 60;
inline constexpr int kMaximumHeartbeatFailures = 5;

inline constexpr T3AuthConfig kDefaultT3AuthConfig{
    LENGJING_T3_LOGIN_CODE,
    LENGJING_T3_NOTICE_CODE,
    LENGJING_T3_VERSION_CODE,
    LENGJING_T3_HEARTBEAT_CODE,
    LENGJING_T3_APP_KEY,
    LENGJING_T3_RSA_PUBLIC_KEY,
    {LENGJING_T3_GET_VARIABLE_CODE,
     LENGJING_T3_LAYOUT_VALUE_ID,
     LENGJING_T3_LAYOUT_VALUE_NAME},
    {LENGJING_T3_LAYOUT_PACKAGE,
     LENGJING_T3_LAYOUT_MODULE,
     LENGJING_T3_LAYOUT_BUILD_ID},
};

}  // namespace lengjing::auth
