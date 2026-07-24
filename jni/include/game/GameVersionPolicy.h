#pragma once

#include "auth/CloudLayout.h"
#include "auth/CoordinatePoolCloudLayout.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

namespace lengjing::game {

inline constexpr std::array<std::string_view, 3> kGameVersionPackages{
    "com.tencent.tmgp.dfm",
    "com.proxima.dfm",
    "com.garena.game.df",
};

constexpr std::string_view ResolveGameVersionPackage(
    int gameVersionIndex) noexcept {
    return gameVersionIndex >= 0 &&
            gameVersionIndex < static_cast<int>(kGameVersionPackages.size())
        ? kGameVersionPackages[static_cast<std::size_t>(gameVersionIndex)]
        : std::string_view{};
}

inline bool CloudLayoutMatchesGameVersion(
    const auth::CloudLayoutDocument* cloudLayout,
    int gameVersionIndex) noexcept {
    const std::string_view package =
        ResolveGameVersionPackage(gameVersionIndex);
    return cloudLayout != nullptr && !package.empty() &&
        cloudLayout->identity.packageName == package;
}

inline std::shared_ptr<const auth::CloudLayoutDocument>
SelectCloudLayoutForGameVersion(
    const std::shared_ptr<const auth::CloudLayoutDocument>& cloudLayout,
    int gameVersionIndex) noexcept {
    return CloudLayoutMatchesGameVersion(
               cloudLayout.get(), gameVersionIndex)
        ? cloudLayout
        : nullptr;
}

inline bool CoordinatePoolCloudLayoutMatchesGameVersion(
    const auth::CoordinatePoolCloudLayoutDocument* cloudLayout,
    int gameVersionIndex) noexcept {
    const std::string_view package =
        ResolveGameVersionPackage(gameVersionIndex);
    return cloudLayout != nullptr && !package.empty() &&
        cloudLayout->identity.packageName == package;
}

inline std::shared_ptr<const auth::CoordinatePoolCloudLayoutDocument>
SelectCoordinatePoolCloudLayoutForGameVersion(
    const std::shared_ptr<
        const auth::CoordinatePoolCloudLayoutDocument>& cloudLayout,
    int gameVersionIndex) noexcept {
    return CoordinatePoolCloudLayoutMatchesGameVersion(
               cloudLayout.get(), gameVersionIndex)
        ? cloudLayout
        : nullptr;
}

}  // namespace lengjing::game
