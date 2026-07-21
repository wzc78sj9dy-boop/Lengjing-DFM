#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>

namespace lengjing::game::native {

class MemoryTransport;

struct AlgorithmPacgaKey {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

std::uint64_t ComputeAlgorithmPositionPacga(
    std::uint64_t data,
    std::uint64_t modifier,
    const AlgorithmPacgaKey& key) noexcept;

inline constexpr std::uint64_t kAlgorithmPositionPointerPayloadMask =
    UINT64_C(0x0000FFFFFFFFFFFF);

constexpr std::uint64_t NormalizeAlgorithmPositionRemoteAddress(
    std::uint64_t address) noexcept {
    return address & kAlgorithmPositionPointerPayloadMask;
}

constexpr std::uint64_t FormatAlgorithmPacgaResult(
    std::uint64_t computedPac) noexcept {
    return computedPac & UINT64_C(0xFFFFFFFF00000000);
}

struct AlgorithmPositionRuntimeConfig {
    std::uintptr_t decryptRva = 0;
    std::uintptr_t absoluteGuestPc = 0;

    constexpr bool HasRelativeEntry() const noexcept {
        return decryptRva >= 4 &&
            decryptRva <= std::numeric_limits<std::uint32_t>::max() &&
            (decryptRva & 3U) == 0;
    }

    constexpr bool HasAbsoluteEntry() const noexcept {
        return absoluteGuestPc >= 4 &&
            absoluteGuestPc <= kAlgorithmPositionPointerPayloadMask &&
            (absoluteGuestPc & 3U) == 0;
    }

    constexpr bool IsConfigured() const noexcept {
        return HasRelativeEntry() != HasAbsoluteEntry();
    }
};

inline AlgorithmPositionRuntimeConfig ParseAlgorithmPositionDecryptRva(
    std::string_view value) noexcept {
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
    }
    if (value.empty()) return {};

    std::uintptr_t decryptRva = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), decryptRva, 16);
    const AlgorithmPositionRuntimeConfig config{decryptRva, 0};
    return parsed.ec == std::errc{} &&
            parsed.ptr == value.data() + value.size() &&
            config.IsConfigured()
        ? config
        : AlgorithmPositionRuntimeConfig{};
}

inline AlgorithmPositionRuntimeConfig ParseAlgorithmPositionGuestPc(
    std::string_view value) noexcept {
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
    }
    if (value.empty()) return {};

    std::uintptr_t guestPc = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), guestPc, 16);
    const AlgorithmPositionRuntimeConfig config{0, guestPc};
    return parsed.ec == std::errc{} &&
            parsed.ptr == value.data() + value.size() &&
            config.IsConfigured()
        ? config
        : AlgorithmPositionRuntimeConfig{};
}

constexpr bool ResolveAlgorithmPositionGuestPc(
    const AlgorithmPositionRuntimeConfig& config,
    std::uintptr_t moduleBase,
    std::uintptr_t& guestPc) noexcept {
    guestPc = 0;
    if (!config.IsConfigured()) return false;
    if (config.HasAbsoluteEntry()) {
        guestPc = config.absoluteGuestPc;
        return true;
    }
    if (moduleBase == 0 ||
        config.decryptRva >
            std::numeric_limits<std::uintptr_t>::max() - moduleBase) {
        return false;
    }
    const std::uintptr_t resolved = moduleBase + config.decryptRva;
    if (resolved > kAlgorithmPositionPointerPayloadMask ||
        (resolved & 3U) != 0) {
        return false;
    }
    guestPc = resolved;
    return true;
}

constexpr bool ShouldAttemptAlgorithmPosition(
    bool localActor,
    bool coordinateDecryptRequested,
    bool executionContextReady,
    const AlgorithmPositionRuntimeConfig& config) noexcept {
    return !localActor && coordinateDecryptRequested &&
        executionContextReady && config.IsConfigured();
}

constexpr bool ShouldRequireAlgorithmPosition(
    bool localActor,
    bool coordinateDecryptRequested) noexcept {
    return !localActor && coordinateDecryptRequested;
}

struct AlgorithmPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

class AlgorithmPositionRuntime final {
public:
    AlgorithmPositionRuntime();
    ~AlgorithmPositionRuntime();

    AlgorithmPositionRuntime(const AlgorithmPositionRuntime&) = delete;
    AlgorithmPositionRuntime& operator=(const AlgorithmPositionRuntime&) =
        delete;

    bool Execute(MemoryTransport& memory,
                 std::uintptr_t moduleBase,
                 std::uintptr_t decryptRva,
                 std::uintptr_t entityAddress,
                 std::uint64_t tpidrEl0,
                 const AlgorithmPacgaKey& pacgaKey,
                 AlgorithmPosition& position,
                 bool refreshCachedPages = true) noexcept;
    bool ExecuteAtGuestPc(MemoryTransport& memory,
                          std::uintptr_t guestPc,
                          std::uintptr_t entityAddress,
                          std::uint64_t tpidrEl0,
                          const AlgorithmPacgaKey& pacgaKey,
                          AlgorithmPosition& position,
                          bool refreshCachedPages = true) noexcept;
    void Invalidate() noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
