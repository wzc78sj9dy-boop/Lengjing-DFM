#pragma once

#include <string_view>

#if __has_include("auth/AuthConfigPrivate.h")
#include "auth/AuthConfigPrivate.h"
#endif

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

#ifndef LENGJING_T3_COORDINATE_SUITE_VALUE_ID
#define LENGJING_T3_COORDINATE_SUITE_VALUE_ID ""
#endif

#ifndef LENGJING_T3_COORDINATE_SUITE_VALUE_NAME
#define LENGJING_T3_COORDINATE_SUITE_VALUE_NAME ""
#endif

#ifndef LENGJING_T3_COORDINATE_SUITE_PACKAGE
#define LENGJING_T3_COORDINATE_SUITE_PACKAGE "com.tencent.tmgp.dfm"
#endif

#ifndef LENGJING_T3_COORDINATE_SUITE_MODULE
#define LENGJING_T3_COORDINATE_SUITE_MODULE "libUE4.so"
#endif

#ifndef LENGJING_COORDINATE_REMOTE_PLAN_URL
#define LENGJING_COORDINATE_REMOTE_PLAN_URL ""
#endif

#ifndef LENGJING_COORDINATE_REMOTE_PLAN_DEVICE_ID
#define LENGJING_COORDINATE_REMOTE_PLAN_DEVICE_ID ""
#endif

#ifndef LENGJING_COORDINATE_REMOTE_PLAN_SEED_PATH
#define LENGJING_COORDINATE_REMOTE_PLAN_SEED_PATH ""
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

struct T3AuthConfig {
    std::string_view loginCode;
    std::string_view noticeCode;
    std::string_view versionCode;
    std::string_view heartbeatCode;
    std::string_view appKey;
    std::string_view rsaPublicKey;
    CloudVariableConfig coordinateSuiteVariable;
    std::string_view targetPackage;
    std::string_view targetModule;

    constexpr bool IsLoginConfigured() const noexcept {
        return !loginCode.empty() && !noticeCode.empty() &&
            !versionCode.empty() && !heartbeatCode.empty() &&
            !appKey.empty() && !rsaPublicKey.empty();
    }

    constexpr bool IsCoordinateSuiteConfigured() const noexcept {
        return coordinateSuiteVariable.IsConfigured() &&
            !targetPackage.empty() && !targetModule.empty();
    }
};

struct CoordinateRemotePlanConfig {
    std::string_view url;
    std::string_view deviceId;
    std::string_view seedPath;

    constexpr bool IsConfigured() const noexcept {
        return !url.empty();
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
     LENGJING_T3_COORDINATE_SUITE_VALUE_ID,
     LENGJING_T3_COORDINATE_SUITE_VALUE_NAME},
    LENGJING_T3_COORDINATE_SUITE_PACKAGE,
    LENGJING_T3_COORDINATE_SUITE_MODULE,
};

inline constexpr CoordinateRemotePlanConfig
    kDefaultCoordinateRemotePlanConfig{
        LENGJING_COORDINATE_REMOTE_PLAN_URL,
        LENGJING_COORDINATE_REMOTE_PLAN_DEVICE_ID,
        LENGJING_COORDINATE_REMOTE_PLAN_SEED_PATH,
    };

}  // namespace lengjing::auth
