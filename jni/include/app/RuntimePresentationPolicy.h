#pragma once

#include <cstdint>

namespace lengjing::app {

enum class CoordinateDecryptPresentation {
    Failed,
    Succeeded,
};

constexpr CoordinateDecryptPresentation ResolveCoordinateDecryptPresentation(
    bool requested,
    bool entryReady,
    bool contextReady,
    std::uint64_t successfulDecryptions) noexcept {
    return requested && entryReady && contextReady &&
            successfulDecryptions != 0
        ? CoordinateDecryptPresentation::Succeeded
        : CoordinateDecryptPresentation::Failed;
}

constexpr const char* CoordinateDecryptPresentationText(
    CoordinateDecryptPresentation presentation) noexcept {
    return presentation == CoordinateDecryptPresentation::Succeeded
        ? "解密成功"
        : "解密失败";
}

constexpr bool ShouldNotifyCoordinateDecryptSuccess(
    CoordinateDecryptPresentation presentation,
    bool successAlreadyNotified) noexcept {
    return presentation == CoordinateDecryptPresentation::Succeeded &&
        !successAlreadyNotified;
}

constexpr const char* VerificationFailureText() noexcept {
    return "[验证] 验证失败";
}

constexpr const char* UpdateRequiredText() noexcept {
    return "请到网盘更新最新版本";
}

constexpr const char* RuntimeFaultText() noexcept {
    return "运行模块发生错误";
}

constexpr const char* RuntimeDataUnavailableText() noexcept {
    return "运行数据暂不可用";
}

constexpr const char* RuntimeDataRestoredText() noexcept {
    return "运行数据已恢复";
}

}  // namespace lengjing::app
