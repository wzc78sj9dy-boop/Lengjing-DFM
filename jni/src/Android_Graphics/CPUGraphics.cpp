#include "CPUGraphics.h"

#include <arm_neon.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

#include "imgui.h"

namespace {

using Clock = std::chrono::steady_clock;
using lengjing::render::cpu::ClampRect;
using lengjing::render::cpu::EdgeCovers;
using lengjing::render::cpu::EdgeValue;
using lengjing::render::cpu::EndPixelAtOrBefore;
using lengjing::render::cpu::EndPixelBefore;
using lengjing::render::cpu::FirstPixelAtOrAfter;
using lengjing::render::cpu::IntersectRect;
using lengjing::render::cpu::IsTopLeftEdge;
using lengjing::render::cpu::PixelCenter;
using lengjing::render::cpu::PixelRect;
using lengjing::render::cpu::SaturatingInt;
using lengjing::render::cpu::SubpixelPoint;
using lengjing::render::cpu::ToSubpixel;
using lengjing::render::cpu::UnionRect;

constexpr int kWindowLockWatchdogMs = 500;
constexpr int kSubmitStopWaitMs = 100;
constexpr int kSubmitFailureLimit = 8;
constexpr int kSubmitRetryBaseMs = 4;
constexpr int kSubmitRetryMaxMs = 100;
constexpr int kSubmitRestartDelayMs = 500;
constexpr int kGeometryRetryBaseMs = 16;
constexpr int kGeometryRetryMaxMs = 500;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

bool TryPixelCount(int width, int height, size_t* pixel_count) {
    if (pixel_count == nullptr || width <= 0 || height <= 0)
        return false;

    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    if (w > std::numeric_limits<size_t>::max() / h)
        return false;
    const size_t count = w * h;
    if (count > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
        return false;
    if (count > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    *pixel_count = count;
    return true;
}

int FailureBackoffMs(int failures, int base_ms, int max_ms) {
    const int shift = std::clamp(failures - 1, 0, 6);
    const int64_t delay = static_cast<int64_t>(base_ms) << shift;
    return static_cast<int>(std::min<int64_t>(delay, max_ms));
}

void ScheduleGeometryRetry(int* failures, int64_t* retry_after_ms) {
    if (failures == nullptr || retry_after_ms == nullptr)
        return;
    *failures = std::min(*failures + 1, 32);
    *retry_after_ms = NowMs() + FailureBackoffMs(
        *failures, kGeometryRetryBaseMs, kGeometryRetryMaxMs);
}

bool BackOffSubmitFailure(const std::shared_ptr<SubmitState>& state,
                          int* consecutive_failures) {
    if (state == nullptr || consecutive_failures == nullptr)
        return false;

    *consecutive_failures = std::min(
        *consecutive_failures + 1, kSubmitFailureLimit);
    if (*consecutive_failures >= kSubmitFailureLimit) {
        if (state->m_SurfaceRecoveryRequested != nullptr) {
            state->m_SurfaceRecoveryRequested->store(
                true, std::memory_order_release);
        }
        state->m_RetryAfterMs.store(
            NowMs() + kSubmitRestartDelayMs, std::memory_order_release);
        state->m_Running.store(false, std::memory_order_release);
        state->m_Cond.notify_all();
        return false;
    }

    const int delay_ms = FailureBackoffMs(
        *consecutive_failures, kSubmitRetryBaseMs, kSubmitRetryMaxMs);
    std::unique_lock<std::mutex> lock(state->m_Mutex);
    state->m_Cond.wait_for(lock, std::chrono::milliseconds(delay_ms), [&] {
        return !state->m_Running.load(std::memory_order_acquire);
    });
    return state->m_Running.load(std::memory_order_acquire);
}

inline uint8_t Scale255Exact(uint16_t value) {
    const uint16_t adjusted = static_cast<uint16_t>(value + (value >> 8));
    return static_cast<uint8_t>((adjusted + 0x80u) >> 8);
}

inline uint8_t Div255Floor(uint32_t value) {
    return static_cast<uint8_t>((value * 32897u) >> 23);
}

inline uint32_t BlendPixel(uint32_t dst, uint32_t src) {
    const uint32_t src_alpha = src >> 24;
    if (src_alpha == 0)
        return dst;
    if (src_alpha == 255)
        return src;

    const uint32_t inv_alpha = src_alpha ^ 255u;
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const uint32_t d = (dst >> shift) & 255u;
        const uint32_t s = (src >> shift) & 255u;
        result |= static_cast<uint32_t>(Scale255Exact(
                      static_cast<uint16_t>(d * inv_alpha + s * src_alpha)))
               << shift;
    }
    const uint32_t dst_alpha = dst >> 24;
    const uint32_t out_alpha = src_alpha + Div255Floor(dst_alpha * inv_alpha);
    return result | (out_alpha << 24);
}

void BlendSpan(uint32_t* dst, int count, uint32_t src) {
    if (count <= 0 || (src >> 24) == 0)
        return;

    const uint32_t src_alpha = src >> 24;
    int x = 0;
    if (src_alpha == 255) {
        const uint32x4_t fill = vdupq_n_u32(src);
        for (; x + 4 <= count; x += 4)
            vst1q_u32(dst + x, fill);
        for (; x < count; ++x)
            dst[x] = src;
        return;
    }

    const uint8x8_t inv8 = vdup_n_u8(static_cast<uint8_t>(src_alpha ^ 255u));
    const uint8x8_t alpha8 = vdup_n_u8(static_cast<uint8_t>(src_alpha));
    const uint8x16_t src_bytes = vreinterpretq_u8_u32(vdupq_n_u32(src));
    const uint8x8_t src_lo = vget_low_u8(src_bytes);
    const uint8x8_t src_hi = vget_high_u8(src_bytes);
    const uint32x4_t rgb_mask = vdupq_n_u32(0x00FFFFFFu);
    const uint32x4_t src_alpha4 = vdupq_n_u32(src_alpha);
    const uint32_t alpha_coeff = (src_alpha ^ 255u) * 32897u;

    for (; x + 4 <= count; x += 4) {
        const uint32x4_t dst_words = vld1q_u32(dst + x);
        const uint8x16_t dst_bytes = vreinterpretq_u8_u32(dst_words);

        uint16x8_t lo = vmull_u8(vget_low_u8(dst_bytes), inv8);
        uint16x8_t hi = vmull_u8(vget_high_u8(dst_bytes), inv8);
        lo = vmlal_u8(lo, src_lo, alpha8);
        hi = vmlal_u8(hi, src_hi, alpha8);
        lo = vsraq_n_u16(lo, lo, 8);
        hi = vsraq_n_u16(hi, hi, 8);
        const uint8x16_t blended_bytes = vcombine_u8(
            vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8));

        const uint32x4_t dst_alpha4 = vshrq_n_u32(dst_words, 24);
        const uint32x4_t out_alpha4 = vaddq_u32(
            src_alpha4,
            vshrq_n_u32(vmulq_n_u32(dst_alpha4, alpha_coeff), 23));
        const uint32x4_t out = vorrq_u32(
            vandq_u32(vreinterpretq_u32_u8(blended_bytes), rgb_mask),
            vshlq_n_u32(out_alpha4, 24));
        vst1q_u32(dst + x, out);
    }
    for (; x < count; ++x)
        dst[x] = BlendPixel(dst[x], src);
}

uint32_t ModulateColor(uint32_t texture, uint32_t tint) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const uint16_t product = static_cast<uint16_t>(
            ((texture >> shift) & 255u) * ((tint >> shift) & 255u));
        result |= static_cast<uint32_t>(Scale255Exact(product)) << shift;
    }
    return result;
}

struct RasterVertex {
    float x;
    float y;
    float u;
    float v;
    uint32_t color;
};

using RasterClip = PixelRect;

enum class TextureKind {
    Solid,
    FontAlpha,
    Rgba,
};

struct TextureView {
    TextureKind kind = TextureKind::Solid;
    const uint8_t* alpha = nullptr;
    const uint32_t* rgba = nullptr;
    int width = 0;
    int height = 0;
};

struct PreparedPrimitive {
    RasterVertex vertices[4]{};
    TextureView texture;
    RasterClip clip;
    uint8_t vertex_count = 0;
};

struct PreparedFrame {
    std::vector<PreparedPrimitive> primitives;
    std::array<std::vector<uint32_t>, RenderPool::kWorkers> bands;
    PixelRect content_rect;
};

constexpr uintptr_t kManagedTextureTag = 1u;

struct ManagedTexture {
    ImTextureFormat format = ImTextureFormat_Alpha8;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> alpha;
    std::vector<uint32_t> rgba;
};

static_assert(alignof(ManagedTexture) > kManagedTextureTag);

template <typename Id>
uintptr_t TextureIdBits(Id id) {
    if constexpr (std::is_pointer_v<Id>) {
        return reinterpret_cast<uintptr_t>(id);
    } else {
        return static_cast<uintptr_t>(id);
    }
}

template <typename Id>
Id TextureIdFromBits(uintptr_t bits) {
    if constexpr (std::is_pointer_v<Id>) {
        return reinterpret_cast<Id>(bits);
    } else {
        return static_cast<Id>(bits);
    }
}

template <typename Id>
bool IsManagedTextureId(Id id) {
    return (TextureIdBits(id) & kManagedTextureTag) != 0;
}

ImTextureID MakeManagedTextureId(ManagedTexture* texture) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(texture);
    return TextureIdFromBits<ImTextureID>(address | kManagedTextureTag);
}

template <typename Id>
ManagedTexture* ManagedTextureFromId(Id id) {
    const uintptr_t address =
        TextureIdBits(id) & ~kManagedTextureTag;
    return reinterpret_cast<ManagedTexture*>(address);
}

bool ResizeManagedTexture(ManagedTexture* output, const ImTextureData* input) {
    if (output == nullptr || input == nullptr || input->Pixels == nullptr
        || input->Width <= 0 || input->Height <= 0)
        return false;
    if (input->Format != ImTextureFormat_Alpha8
        && input->Format != ImTextureFormat_RGBA32)
        return false;

    size_t pixel_count = 0;
    if (!TryPixelCount(input->Width, input->Height, &pixel_count))
        return false;

    std::vector<uint8_t> alpha;
    std::vector<uint32_t> rgba;
    try {
        if (input->Format == ImTextureFormat_Alpha8)
            alpha.resize(pixel_count);
        else
            rgba.resize(pixel_count);
    } catch (...) {
        return false;
    }

    output->format = input->Format;
    output->width = input->Width;
    output->height = input->Height;
    output->alpha = std::move(alpha);
    output->rgba = std::move(rgba);
    return true;
}

void CopyManagedTextureRect(ManagedTexture* output, const ImTextureData* input,
                            int x, int y, int width, int height) {
    if (output == nullptr || input == nullptr || input->Pixels == nullptr
        || output->width != input->Width || output->height != input->Height
        || output->format != input->Format)
        return;

    const int x0 = std::clamp(x, 0, input->Width);
    const int y0 = std::clamp(y, 0, input->Height);
    const int x1 = std::clamp(x + width, 0, input->Width);
    const int y1 = std::clamp(y + height, 0, input->Height);
    if (x1 <= x0 || y1 <= y0)
        return;

    const size_t copy_pixels = static_cast<size_t>(x1 - x0);
    if (input->Format == ImTextureFormat_Alpha8) {
        for (int row = y0; row < y1; ++row) {
            const size_t offset = static_cast<size_t>(row) * input->Width + x0;
            std::memcpy(output->alpha.data() + offset,
                        input->Pixels + offset, copy_pixels);
        }
    } else {
        for (int row = y0; row < y1; ++row) {
            const size_t offset = static_cast<size_t>(row) * input->Width + x0;
            std::memcpy(output->rgba.data() + offset,
                        input->Pixels + offset * sizeof(uint32_t),
                        copy_pixels * sizeof(uint32_t));
        }
    }
}

bool UpdateManagedTexture(ImTextureData* texture) {
    if (texture == nullptr)
        return false;

    if (texture->Status == ImTextureStatus_WantCreate) {
        auto managed = std::make_unique<ManagedTexture>();
        if (!ResizeManagedTexture(managed.get(), texture)) {
            return false;
        }
        CopyManagedTextureRect(managed.get(), texture, 0, 0,
                               texture->Width, texture->Height);
        texture->BackendUserData = managed.get();
        texture->SetTexID(MakeManagedTextureId(managed.get()));
        texture->SetStatus(ImTextureStatus_OK);
        managed.release();
        return true;
    }

    if (texture->Status == ImTextureStatus_WantUpdates) {
        auto* managed = static_cast<ManagedTexture*>(texture->BackendUserData);
        if (managed == nullptr || !IsManagedTextureId(texture->TexID)
            || ManagedTextureFromId(texture->TexID) != managed)
            return false;
        if (managed->width != texture->Width || managed->height != texture->Height
            || managed->format != texture->Format) {
            if (!ResizeManagedTexture(managed, texture))
                return false;
            CopyManagedTextureRect(managed, texture, 0, 0,
                                   texture->Width, texture->Height);
        } else if (!texture->Updates.empty()) {
            for (const ImTextureRect& rect : texture->Updates) {
                CopyManagedTextureRect(managed, texture, rect.x, rect.y,
                                       rect.w, rect.h);
            }
        } else {
            const ImTextureRect& rect = texture->UpdateRect;
            CopyManagedTextureRect(managed, texture, rect.x, rect.y,
                                   rect.w, rect.h);
        }
        texture->SetStatus(ImTextureStatus_OK);
        return true;
    }

    if (texture->Status == ImTextureStatus_WantDestroy
        && texture->UnusedFrames > 0) {
        auto* managed = static_cast<ManagedTexture*>(texture->BackendUserData);
        if (managed != nullptr && IsManagedTextureId(texture->TexID)
            && ManagedTextureFromId(texture->TexID) == managed) {
            delete managed;
        }
        texture->BackendUserData = nullptr;
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
    }
    return true;
}

bool UpdateManagedTextures(ImDrawData* draw_data) {
    if (draw_data == nullptr || draw_data->Textures == nullptr)
        return true;
    for (ImTextureData* texture : *draw_data->Textures) {
        if (texture->Status != ImTextureStatus_OK
            && !UpdateManagedTexture(texture))
            return false;
    }
    return true;
}

void DestroyManagedTextures() {
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture == nullptr || texture->BackendUserData == nullptr
            || !IsManagedTextureId(texture->TexID))
            continue;
        auto* managed = static_cast<ManagedTexture*>(texture->BackendUserData);
        if (ManagedTextureFromId(texture->TexID) == managed)
            delete managed;
        texture->BackendUserData = nullptr;
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
    }
}

struct BilinearCoordinates {
    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
    int y1 = 0;
    float xWeight = 0.0f;
    float yWeight = 0.0f;
};

BilinearCoordinates ResolveBilinearCoordinates(
    float u, float v, int width, int height) {
    const float x = std::clamp(
        u * static_cast<float>(width) - 0.5f,
        0.0f,
        static_cast<float>(width - 1));
    const float y = std::clamp(
        v * static_cast<float>(height) - 0.5f,
        0.0f,
        static_cast<float>(height - 1));
    BilinearCoordinates coordinates;
    coordinates.x0 = static_cast<int>(x);
    coordinates.y0 = static_cast<int>(y);
    coordinates.x1 = std::min(coordinates.x0 + 1, width - 1);
    coordinates.y1 = std::min(coordinates.y0 + 1, height - 1);
    coordinates.xWeight = x - static_cast<float>(coordinates.x0);
    coordinates.yWeight = y - static_cast<float>(coordinates.y0);
    return coordinates;
}

float BilinearValue(float topLeft,
                    float topRight,
                    float bottomLeft,
                    float bottomRight,
                    const BilinearCoordinates& coordinates) {
    const float top = topLeft +
        (topRight - topLeft) * coordinates.xWeight;
    const float bottom = bottomLeft +
        (bottomRight - bottomLeft) * coordinates.xWeight;
    return top + (bottom - top) * coordinates.yWeight;
}

uint32_t BilinearRgba(const TextureView& texture,
                      const BilinearCoordinates& coordinates) {
    const uint32_t topLeft =
        texture.rgba[coordinates.y0 * texture.width + coordinates.x0];
    const uint32_t topRight =
        texture.rgba[coordinates.y0 * texture.width + coordinates.x1];
    const uint32_t bottomLeft =
        texture.rgba[coordinates.y1 * texture.width + coordinates.x0];
    const uint32_t bottomRight =
        texture.rgba[coordinates.y1 * texture.width + coordinates.x1];
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const float value = BilinearValue(
            static_cast<float>((topLeft >> shift) & 255u),
            static_cast<float>((topRight >> shift) & 255u),
            static_cast<float>((bottomLeft >> shift) & 255u),
            static_cast<float>((bottomRight >> shift) & 255u),
            coordinates);
        result |= static_cast<uint32_t>(value + 0.5f) << shift;
    }
    return result;
}

uint32_t InterpolateColor(const RasterVertex& a, const RasterVertex& b,
                          const RasterVertex& c, float wa, float wb, float wc) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const float value = static_cast<float>((a.color >> shift) & 255u) * wa
                          + static_cast<float>((b.color >> shift) & 255u) * wb
                          + static_cast<float>((c.color >> shift) & 255u) * wc;
        result |= static_cast<uint32_t>(value) << shift;
    }
    return result;
}

uint32_t ShadePixel(const TextureView& texture, float u, float v, uint32_t color) {
    if (texture.kind == TextureKind::Solid)
        return color;

    if (texture.width <= 0 || texture.height <= 0)
        return 0;
    const BilinearCoordinates coordinates = ResolveBilinearCoordinates(
        u, v, texture.width, texture.height);
    if (texture.kind == TextureKind::Rgba)
        return texture.rgba != nullptr
            ? ModulateColor(BilinearRgba(texture, coordinates), color)
            : 0;

    if (texture.alpha == nullptr)
        return 0;
    const uint32_t coverage = static_cast<uint32_t>(BilinearValue(
        static_cast<float>(
            texture.alpha[coordinates.y0 * texture.width + coordinates.x0]),
        static_cast<float>(
            texture.alpha[coordinates.y0 * texture.width + coordinates.x1]),
        static_cast<float>(
            texture.alpha[coordinates.y1 * texture.width + coordinates.x0]),
        static_cast<float>(
            texture.alpha[coordinates.y1 * texture.width + coordinates.x1]),
        coordinates) + 0.5f);
    if (coverage == 0)
        return 0;
    const uint32_t alpha = Div255Floor(coverage * (color >> 24));
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

bool IsWhiteUv(const RasterVertex& vertex, float white_u, float white_v,
               float tolerance) {
    return std::fabs(vertex.u - white_u) < tolerance
        && std::fabs(vertex.v - white_v) < tolerance;
}

bool TryRasterQuad(const RasterVertex& a, const RasterVertex& b,
                   const RasterVertex& c, const RasterVertex& d,
                   const TextureView& texture, const RasterClip& clip,
                   uint32_t* buffer, int stride, float white_u, float white_v,
                   const std::atomic<bool>& abort) {
    if (std::fabs(a.x - d.x) > 0.01f || std::fabs(b.x - c.x) > 0.01f
        || std::fabs(a.y - b.y) > 0.01f || std::fabs(c.y - d.y) > 0.01f)
        return false;
    if (a.color != b.color || a.color != c.color || a.color != d.color)
        return false;

    const float left = std::min({a.x, b.x, c.x, d.x});
    const float right = std::max({a.x, b.x, c.x, d.x});
    const float top = std::min({a.y, b.y, c.y, d.y});
    const float bottom = std::max({a.y, b.y, c.y, d.y});
    const int x0 = std::max(clip.x0, FirstPixelAtOrAfter(left));
    const int y0 = std::max(clip.y0, FirstPixelAtOrAfter(top));
    const int x1 = std::min(clip.x1, EndPixelBefore(right));
    const int y1 = std::min(clip.y1, EndPixelBefore(bottom));
    if (x1 <= x0 || y1 <= y0)
        return true;

    const bool solid = texture.kind != TextureKind::Rgba
        && IsWhiteUv(a, white_u, white_v, 0.001f)
        && IsWhiteUv(b, white_u, white_v, 0.001f)
        && IsWhiteUv(c, white_u, white_v, 0.001f)
        && IsWhiteUv(d, white_u, white_v, 0.001f);
    if (solid) {
        for (int y = y0; y < y1; ++y) {
            if ((y & 15) == 0 && abort.load(std::memory_order_relaxed))
                return true;
            BlendSpan(buffer + y * stride + x0, x1 - x0, a.color);
        }
        return true;
    }

    // RGBA images remain two triangles so their shared-edge blend matches the
    // software rasterizer. Only Alpha8 font quads use this sampling fast path.
    if (texture.kind != TextureKind::FontAlpha)
        return false;
    if (texture.width <= 0 || texture.height <= 0)
        return false;
    const bool axis_uv = std::fabs(a.u - d.u) <= 0.0005f
        && std::fabs(b.u - c.u) <= 0.0005f
        && std::fabs(a.v - b.v) <= 0.0005f
        && std::fabs(c.v - d.v) <= 0.0005f;
    if (!axis_uv)
        return false;

    const float dx = b.x - a.x;
    const float dy = d.y - a.y;
    if (std::fabs(dx) <= 0.001f || std::fabs(dy) <= 0.001f)
        return true;
    const float du_dx = (b.u - a.u) / dx;
    const float dv_dx = (b.v - a.v) / dx;
    const float du_dy = (d.u - a.u) / dy;
    const float dv_dy = (d.v - a.v) / dy;

    for (int y = y0; y < y1; ++y) {
        if ((y & 15) == 0 && abort.load(std::memory_order_relaxed))
            return true;
        const float py = static_cast<float>(y) + 0.5f;
        float u = a.u + (py - a.y) * du_dy
                + (static_cast<float>(x0) + 0.5f - a.x) * du_dx;
        float v = a.v + (py - a.y) * dv_dy
                + (static_cast<float>(x0) + 0.5f - a.x) * dv_dx;
        uint32_t* dst = buffer + y * stride + x0;
        for (int x = x0; x < x1; ++x, ++dst) {
            const uint32_t src = ShadePixel(texture, u, v, a.color);
            if ((src >> 24) != 0)
                *dst = BlendPixel(*dst, src);
            u += du_dx;
            v += dv_dx;
        }
    }
    return true;
}

void RasterTriangle(const RasterVertex& a, const RasterVertex& b,
                    const RasterVertex& c, const TextureView& texture,
                    const RasterClip& clip, uint32_t* buffer, int stride,
                    float white_u, float white_v,
                    const std::atomic<bool>& abort) {
    RasterVertex vertices[3]{a, b, c};
    SubpixelPoint points[3];
    if (!ToSubpixel(a.x, a.y, &points[0])
        || !ToSubpixel(b.x, b.y, &points[1])
        || !ToSubpixel(c.x, c.y, &points[2]))
        return;

    int64_t area = EdgeValue(points[0], points[1], points[2]);
    if (area == 0)
        return;
    if (area < 0) {
        std::swap(points[1], points[2]);
        std::swap(vertices[1], vertices[2]);
        area = -area;
    }

    const int64_t min_fixed_x = std::min({points[0].x, points[1].x, points[2].x});
    const int64_t max_fixed_x = std::max({points[0].x, points[1].x, points[2].x});
    const int64_t min_fixed_y = std::min({points[0].y, points[1].y, points[2].y});
    const int64_t max_fixed_y = std::max({points[0].y, points[1].y, points[2].y});
    const float fixed_scale = static_cast<float>(
        lengjing::render::cpu::kSubpixelScale);
    const int x0 = std::max(clip.x0, FirstPixelAtOrAfter(
        static_cast<float>(min_fixed_x) / fixed_scale));
    const int y0 = std::max(clip.y0, FirstPixelAtOrAfter(
        static_cast<float>(min_fixed_y) / fixed_scale));
    const int x1 = std::min(clip.x1, EndPixelAtOrBefore(
        static_cast<float>(max_fixed_x) / fixed_scale));
    const int y1 = std::min(clip.y1, EndPixelAtOrBefore(
        static_cast<float>(max_fixed_y) / fixed_scale));
    if (x1 <= x0 || y1 <= y0)
        return;

    const bool edge0_top_left = IsTopLeftEdge(points[1], points[2]);
    const bool edge1_top_left = IsTopLeftEdge(points[2], points[0]);
    const bool edge2_top_left = IsTopLeftEdge(points[0], points[1]);
    const int64_t edge0_step_x =
        -(points[2].y - points[1].y)
        * lengjing::render::cpu::kSubpixelScale;
    const int64_t edge1_step_x =
        -(points[0].y - points[2].y)
        * lengjing::render::cpu::kSubpixelScale;
    const int64_t edge2_step_x =
        -(points[1].y - points[0].y)
        * lengjing::render::cpu::kSubpixelScale;
    const int64_t edge0_step_y =
        (points[2].x - points[1].x)
        * lengjing::render::cpu::kSubpixelScale;
    const int64_t edge1_step_y =
        (points[0].x - points[2].x)
        * lengjing::render::cpu::kSubpixelScale;
    const int64_t edge2_step_y =
        (points[1].x - points[0].x)
        * lengjing::render::cpu::kSubpixelScale;
    const SubpixelPoint first_center = PixelCenter(x0, y0);
    int64_t edge0_row = EdgeValue(points[1], points[2], first_center);
    int64_t edge1_row = EdgeValue(points[2], points[0], first_center);
    int64_t edge2_row = EdgeValue(points[0], points[1], first_center);
    const float inverse_area = 1.0f / static_cast<float>(area);
    const bool same_color = vertices[0].color == vertices[1].color
        && vertices[0].color == vertices[2].color;
    const bool solid = same_color && texture.kind != TextureKind::Rgba
        && IsWhiteUv(vertices[0], white_u, white_v, 0.001f)
        && IsWhiteUv(vertices[1], white_u, white_v, 0.001f)
        && IsWhiteUv(vertices[2], white_u, white_v, 0.001f);

    for (int y = y0; y < y1; ++y) {
        if ((y & 15) == 0 && abort.load(std::memory_order_relaxed))
            return;
        int64_t edge0 = edge0_row;
        int64_t edge1 = edge1_row;
        int64_t edge2 = edge2_row;

        if (solid) {
            int run_start = x0;
            int64_t left_edge0 = edge0;
            int64_t left_edge1 = edge1;
            int64_t left_edge2 = edge2;
            while (run_start < x1) {
                const bool covered = EdgeCovers(left_edge0, edge0_top_left)
                    && EdgeCovers(left_edge1, edge1_top_left)
                    && EdgeCovers(left_edge2, edge2_top_left);
                if (covered)
                    break;
                ++run_start;
                left_edge0 += edge0_step_x;
                left_edge1 += edge1_step_x;
                left_edge2 += edge2_step_x;
            }
            if (run_start < x1) {
                int run_end = x1;
                const int64_t right_offset = x1 - x0 - 1;
                int64_t right_edge0 = edge0 + edge0_step_x * right_offset;
                int64_t right_edge1 = edge1 + edge1_step_x * right_offset;
                int64_t right_edge2 = edge2 + edge2_step_x * right_offset;
                while (run_end > run_start) {
                    const bool covered = EdgeCovers(right_edge0, edge0_top_left)
                        && EdgeCovers(right_edge1, edge1_top_left)
                        && EdgeCovers(right_edge2, edge2_top_left);
                    if (covered)
                        break;
                    --run_end;
                    right_edge0 -= edge0_step_x;
                    right_edge1 -= edge1_step_x;
                    right_edge2 -= edge2_step_x;
                }
                BlendSpan(buffer + y * stride + run_start,
                          run_end - run_start, vertices[0].color);
            }
        } else {
            uint32_t* dst = buffer + y * stride + x0;
            for (int x = x0; x < x1; ++x, ++dst) {
                const bool covered = EdgeCovers(edge0, edge0_top_left)
                    && EdgeCovers(edge1, edge1_top_left)
                    && EdgeCovers(edge2, edge2_top_left);
                if (covered) {
                    const float wa = static_cast<float>(edge0) * inverse_area;
                    const float wb = static_cast<float>(edge1) * inverse_area;
                    const float wc = static_cast<float>(edge2) * inverse_area;
                    const uint32_t color = same_color
                        ? vertices[0].color
                        : InterpolateColor(vertices[0], vertices[1], vertices[2],
                                           wa, wb, wc);
                    const float u = vertices[0].u * wa
                        + vertices[1].u * wb + vertices[2].u * wc;
                    const float v = vertices[0].v * wa
                        + vertices[1].v * wb + vertices[2].v * wc;
                    const uint32_t src = ShadePixel(texture, u, v, color);
                    if ((src >> 24) != 0)
                        *dst = BlendPixel(*dst, src);
                }
                edge0 += edge0_step_x;
                edge1 += edge1_step_x;
                edge2 += edge2_step_x;
            }
        }
        edge0_row += edge0_step_y;
        edge1_row += edge1_step_y;
        edge2_row += edge2_step_y;
    }
}

bool IsFiniteVertex(const RasterVertex& vertex) {
    return std::isfinite(vertex.x) && std::isfinite(vertex.y)
        && std::isfinite(vertex.u) && std::isfinite(vertex.v);
}

PixelRect PrimitiveBounds(const RasterVertex* vertices, int count) {
    if (vertices == nullptr || count < 3 || count > 4)
        return {};

    SubpixelPoint point;
    if (!IsFiniteVertex(vertices[0])
        || !ToSubpixel(vertices[0].x, vertices[0].y, &point))
        return {};
    int64_t min_x = point.x;
    int64_t min_y = point.y;
    int64_t max_x = point.x;
    int64_t max_y = point.y;
    for (int i = 1; i < count; ++i) {
        if (!IsFiniteVertex(vertices[i])
            || !ToSubpixel(vertices[i].x, vertices[i].y, &point))
            return {};
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }

    const float scale = static_cast<float>(
        lengjing::render::cpu::kSubpixelScale);
    return {
        FirstPixelAtOrAfter(static_cast<float>(min_x) / scale),
        FirstPixelAtOrAfter(static_cast<float>(min_y) / scale),
        EndPixelAtOrBefore(static_cast<float>(max_x) / scale),
        EndPixelAtOrBefore(static_cast<float>(max_y) / scale),
    };
}

void AppendPreparedPrimitive(PreparedFrame* frame,
                             const RasterVertex* vertices,
                             int vertex_count,
                             const TextureView& texture,
                             const PixelRect& command_clip,
                             const PixelRect& surface_bounds) {
    if (frame == nullptr || vertices == nullptr)
        return;
    const PixelRect clip = IntersectRect(
        IntersectRect(PrimitiveBounds(vertices, vertex_count), command_clip),
        surface_bounds);
    if (!clip.valid())
        return;

    PreparedPrimitive primitive;
    std::copy_n(vertices, vertex_count, primitive.vertices);
    primitive.vertex_count = static_cast<uint8_t>(vertex_count);
    primitive.texture = texture;
    primitive.clip = clip;
    frame->primitives.push_back(std::move(primitive));
    frame->content_rect = UnionRect(frame->content_rect, clip);
}

void BuildPrimitiveBands(PreparedFrame* frame, int height) {
    if (frame == nullptr || height <= 0)
        return;

    std::array<size_t, RenderPool::kWorkers> counts{};
    for (const PreparedPrimitive& primitive : frame->primitives) {
        for (int band = 0; band < RenderPool::kWorkers; ++band) {
            const int y0 = height * band / RenderPool::kWorkers;
            const int y1 = height * (band + 1) / RenderPool::kWorkers;
            if (primitive.clip.y1 > y0 && primitive.clip.y0 < y1)
                ++counts[band];
        }
    }
    for (int band = 0; band < RenderPool::kWorkers; ++band)
        frame->bands[band].reserve(counts[band]);

    for (size_t index = 0; index < frame->primitives.size(); ++index) {
        const PreparedPrimitive& primitive = frame->primitives[index];
        for (int band = 0; band < RenderPool::kWorkers; ++band) {
            const int y0 = height * band / RenderPool::kWorkers;
            const int y1 = height * (band + 1) / RenderPool::kWorkers;
            if (primitive.clip.y1 > y0 && primitive.clip.y0 < y1)
                frame->bands[band].push_back(static_cast<uint32_t>(index));
        }
    }
}

void RasterPreparedBand(const PreparedFrame& frame, int band,
                        uint32_t* buffer, int stride, int height,
                        float white_u, float white_v,
                        const std::atomic<bool>& abort) {
    if (buffer == nullptr || stride <= 0 || height <= 0
        || band < 0 || band >= RenderPool::kWorkers)
        return;
    const PixelRect band_clip{
        0,
        height * band / RenderPool::kWorkers,
        stride,
        height * (band + 1) / RenderPool::kWorkers,
    };
    for (uint32_t index : frame.bands[band]) {
        if (abort.load(std::memory_order_relaxed))
            return;
        if (index >= frame.primitives.size())
            continue;
        const PreparedPrimitive& primitive = frame.primitives[index];
        const PixelRect clip = IntersectRect(primitive.clip, band_clip);
        if (!clip.valid())
            continue;

        const RasterVertex* v = primitive.vertices;
        if (primitive.vertex_count == 4) {
            if (TryRasterQuad(v[0], v[1], v[2], v[3], primitive.texture,
                              clip, buffer, stride, white_u, white_v, abort))
                continue;
            RasterTriangle(v[0], v[1], v[2], primitive.texture, clip,
                           buffer, stride, white_u, white_v, abort);
            if (abort.load(std::memory_order_relaxed))
                return;
            RasterTriangle(v[0], v[2], v[3], primitive.texture, clip,
                           buffer, stride, white_u, white_v, abort);
        } else if (primitive.vertex_count == 3) {
            RasterTriangle(v[0], v[1], v[2], primitive.texture, clip,
                           buffer, stride, white_u, white_v, abort);
        }
    }
}

std::shared_ptr<SubmitState> MakeSubmitState() {
    return std::shared_ptr<SubmitState>(new SubmitState(), [](SubmitState* state) {
        if (state->m_Window != nullptr)
            ANativeWindow_release(state->m_Window);
        delete state;
    });
}

void RunSubmitLoop(const std::shared_ptr<SubmitState>& state) {
    std::vector<uint32_t> local_buffer;
    PixelRect local_content_rect;
    int consecutive_failures = 0;

    while (state->m_Running.load(std::memory_order_acquire)) {
        int width = 0;
        int height = 0;
        PixelRect dirty_rect;
        {
            std::unique_lock<std::mutex> lock(state->m_Mutex);
            state->m_Cond.wait(lock, [&] {
                return !state->m_Running.load(std::memory_order_acquire)
                    || state->m_HasWork;
            });
            if (!state->m_Running.load(std::memory_order_acquire))
                break;
            local_buffer.swap(state->buffer);
            std::swap(local_content_rect, state->m_ContentRect);
            width = state->m_BufW;
            height = state->m_BufH;
            dirty_rect = state->m_DirtyRect;
            state->m_HasWork = false;
        }

        size_t pixel_count = 0;
        if (!TryPixelCount(width, height, &pixel_count)
            || local_buffer.size() != pixel_count
            || !dirty_rect.valid() || state->m_Window == nullptr) {
            if (!BackOffSubmitFailure(state, &consecutive_failures))
                break;
            continue;
        }

        state->m_LastPostMs.store(NowMs(), std::memory_order_release);
        ANativeWindow* window = state->m_Window;
        ANativeWindow_acquire(window);

        const int window_width = ANativeWindow_getWidth(window);
        const int window_height = ANativeWindow_getHeight(window);
        if (window_width != width || window_height != height) {
            ANativeWindow_release(window);
            if (!BackOffSubmitFailure(state, &consecutive_failures))
                break;
            continue;
        }

        dirty_rect = ClampRect(dirty_rect, width, height);
        ARect dirty{
            dirty_rect.x0,
            dirty_rect.y0,
            dirty_rect.x1,
            dirty_rect.y1,
        };
        ANativeWindow_Buffer native_buffer{};
        state->m_InLock.store(true, std::memory_order_release);
        const int lock_result = ANativeWindow_lock(window, &native_buffer, &dirty);
        state->m_InLock.store(false, std::memory_order_release);
        if (lock_result != 0) {
            ANativeWindow_release(window);
            if (!BackOffSubmitFailure(state, &consecutive_failures))
                break;
            continue;
        }

        dirty.left = std::clamp(dirty.left, 0, width);
        dirty.top = std::clamp(dirty.top, 0, height);
        dirty.right = std::clamp(dirty.right, 0, width);
        dirty.bottom = std::clamp(dirty.bottom, 0, height);
        const int copy_width = dirty.right - dirty.left;
        const int copy_height = dirty.bottom - dirty.top;
        size_t native_pixel_count = 0;
        const bool buffer_valid = native_buffer.bits != nullptr
            && native_buffer.width == width
            && native_buffer.height == height
            && native_buffer.stride >= width
            && native_buffer.format == WINDOW_FORMAT_RGBA_8888
            && TryPixelCount(native_buffer.stride, height, &native_pixel_count)
            && copy_width > 0 && copy_height > 0;
        if (buffer_valid
            && state->m_Running.load(std::memory_order_acquire)) {
            auto* output = static_cast<uint32_t*>(native_buffer.bits);
            const uint32_t* input = local_buffer.data();
            const size_t row_bytes = static_cast<size_t>(copy_width) * sizeof(uint32_t);
            if (dirty.left == 0 && copy_width == width
                && native_buffer.stride == width) {
                std::memcpy(output + static_cast<size_t>(dirty.top)
                                * native_buffer.stride,
                            input + static_cast<size_t>(dirty.top) * width,
                            row_bytes * copy_height);
            } else {
                for (int y = dirty.top; y < dirty.bottom; ++y) {
                    std::memcpy(output + static_cast<size_t>(y)
                                    * native_buffer.stride + dirty.left,
                                input + static_cast<size_t>(y) * width
                                    + dirty.left,
                                row_bytes);
                }
            }
        }

        state->m_InPost.store(true, std::memory_order_release);
        const int post_result = ANativeWindow_unlockAndPost(window);
        state->m_InPost.store(false, std::memory_order_release);
        bool geometry_repaired = true;
        if (!buffer_valid) {
            geometry_repaired = ANativeWindow_setBuffersGeometry(
                window, width, height, WINDOW_FORMAT_RGBA_8888) == 0;
        }
        ANativeWindow_release(window);

        if (buffer_valid && post_result == 0
            && state->m_Running.load(std::memory_order_acquire)) {
            consecutive_failures = 0;
            state->m_RetryAfterMs.store(0, std::memory_order_release);
            state->m_LastPostMs.store(NowMs(), std::memory_order_release);
            if (state->m_PresentationRate != nullptr)
                state->m_PresentationRate->Record();
            continue;
        }

        if (!geometry_repaired || post_result != 0 || !buffer_valid) {
            if (!BackOffSubmitFailure(state, &consecutive_failures))
                break;
        }
    }
    state->m_Running.store(false, std::memory_order_release);
    state->m_Exited.store(true, std::memory_order_release);
    state->m_Cond.notify_all();
}

} // namespace

struct CPUGraphics::FrameScratch {
    PreparedFrame frame;

    PreparedFrame& Reset() {
        frame.primitives.clear();
        for (auto& band : frame.bands)
            band.clear();
        frame.content_rect = {};
        return frame;
    }
};

RenderPool::RenderPool() {
    try {
        for (int i = 0; i < kWorkers; ++i)
            m_Threads[i] = std::thread(&RenderPool::worker, this, i);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Alive = false;
        }
        m_CondWork.notify_all();
        for (std::thread& thread : m_Threads) {
            if (thread.joinable())
                thread.join();
        }
        throw;
    }
}

RenderPool::~RenderPool() {
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Alive = false;
    }
    m_CondWork.notify_all();
    for (std::thread& thread : m_Threads) {
        if (thread.joinable())
            thread.join();
    }
}

void RenderPool::run(std::function<void()> t0, std::function<void()> t1,
                     std::function<void()> t2, std::function<void()> t3) {
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Tasks[0] = std::move(t0);
        m_Tasks[1] = std::move(t1);
        m_Tasks[2] = std::move(t2);
        m_Tasks[3] = std::move(t3);
        m_Done.store(0, std::memory_order_release);
        ++m_Generation;
    }
    m_CondWork.notify_all();
}

bool RenderPool::wait_for_ms(int ms) {
    std::unique_lock<std::mutex> lock(m_Mutex);
    return m_CondDone.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return m_Done.load(std::memory_order_acquire) >= kWorkers;
    });
}

void RenderPool::worker(int index) {
    int seen = 0;
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_CondWork.wait(lock, [&] {
                return !m_Alive || m_Generation > seen;
            });
            if (!m_Alive)
                return;
            seen = m_Generation;
            m_Seen[index] = seen;
            task = m_Tasks[index];
        }
        if (task)
            task();
        if (m_Done.fetch_add(1, std::memory_order_acq_rel) + 1 == kWorkers)
            m_CondDone.notify_one();
    }
}

CPUGraphics::CPUGraphics() {
    std::snprintf(RenderName, sizeof(RenderName), "CPU");
}

CPUGraphics::~CPUGraphics() {
    StopSubmitThread(true);
    m_Abort.store(true, std::memory_order_release);
    m_Pool.reset();
}

bool CPUGraphics::Create() {
    m_SurfaceRecoveryRequested->store(false, std::memory_order_release);
    m_SurfaceReady = false;
    m_GeometryFailures = 0;
    m_NextGeometryRetryMs = 0;

    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(m_WindowMutex);
        if (m_Window == nullptr)
            return false;
        width = ANativeWindow_getWidth(m_Window);
        height = ANativeWindow_getHeight(m_Window);
    }
    if (width <= 0 || height <= 0) {
        width = static_cast<int>(m_Width);
        height = static_cast<int>(m_Height);
    }
    if (width <= 0 || height <= 0)
        return false;

    size_t pixel_count = 0;
    if (!TryPixelCount(width, height, &pixel_count))
        return false;

    std::vector<uint32_t> buffer;
    try {
        buffer.assign(pixel_count, 0u);
    } catch (...) {
        return false;
    }

    int geometry_result = -1;
    {
        std::lock_guard<std::mutex> lock(m_WindowMutex);
        if (m_Window != nullptr) {
            geometry_result = ANativeWindow_setBuffersGeometry(
                m_Window, width, height, WINDOW_FORMAT_RGBA_8888);
        }
    }
    if (geometry_result != 0)
        return false;

    m_Width = static_cast<float>(width);
    m_Height = static_cast<float>(height);
    {
        std::lock_guard<std::mutex> lock(m_RenderMutex);
        m_BufWidth = width;
        m_BufHeight = height;
        m_BufferContentRect = {};
        m_LastSubmittedContentRect = {};
        m_Buffer = std::move(buffer);
    }
    m_Abort.store(false, std::memory_order_release);
    m_SurfaceReady = true;
    if (!StartSubmitThread()) {
        m_SurfaceReady = false;
        return false;
    }
    return true;
}

bool CPUGraphics::Setup() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset
                    | ImGuiBackendFlags_RendererHasTextures;
    io.BackendRendererName = "CPU";
    return true;
}

void CPUGraphics::PrepareFrame(bool resize) {
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(m_WindowMutex);
        if (m_Window == nullptr)
            return;
        width = ANativeWindow_getWidth(m_Window);
        height = ANativeWindow_getHeight(m_Window);
    }
    if (width <= 0 || height <= 0) {
        m_SurfaceReady = false;
        return;
    }

    m_Width = static_cast<float>(width);
    m_Height = static_cast<float>(height);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(m_Width, m_Height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    const bool changed = resize || !m_SurfaceReady
        || width != m_BufWidth || height != m_BufHeight;
    if (changed) {
        const int64_t now = NowMs();
        if (now < m_NextGeometryRetryMs)
            return;

        size_t pixel_count = 0;
        if (!TryPixelCount(width, height, &pixel_count)) {
            m_SurfaceReady = false;
            ScheduleGeometryRetry(
                &m_GeometryFailures, &m_NextGeometryRetryMs);
            return;
        }

        m_SurfaceReady = false;
        if (!StopSubmitThread(false)) {
            ScheduleGeometryRetry(
                &m_GeometryFailures, &m_NextGeometryRetryMs);
            return;
        }

        std::vector<uint32_t> buffer;
        try {
            buffer.assign(pixel_count, 0u);
        } catch (...) {
            ScheduleGeometryRetry(
                &m_GeometryFailures, &m_NextGeometryRetryMs);
            return;
        }

        int geometry_result = -1;
        {
            std::lock_guard<std::mutex> lock(m_WindowMutex);
            if (m_Window != nullptr) {
                geometry_result = ANativeWindow_setBuffersGeometry(
                    m_Window, width, height, WINDOW_FORMAT_RGBA_8888);
            }
        }
        if (geometry_result != 0) {
            ScheduleGeometryRetry(
                &m_GeometryFailures, &m_NextGeometryRetryMs);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_RenderMutex);
            m_BufWidth = width;
            m_BufHeight = height;
            m_BufferContentRect = {};
            m_LastSubmittedContentRect = {};
            m_Buffer = std::move(buffer);
        }
        m_GeometryFailures = 0;
        m_NextGeometryRetryMs = 0;
        m_SurfaceReady = true;
        if (!StartSubmitThread()) {
            m_SurfaceReady = false;
            ScheduleGeometryRetry(
                &m_GeometryFailures, &m_NextGeometryRetryMs);
            m_NextGeometryRetryMs = std::max(
                m_NextGeometryRetryMs, NowMs() + kSubmitRestartDelayMs);
        }
    }
}

bool CPUGraphics::StartSubmitThread() {
    std::lock_guard<std::mutex> lock(m_ThreadMutex);
    const int64_t now = NowMs();
    if (m_Submit != nullptr) {
        if (m_Submit->m_Exited.load(std::memory_order_acquire)) {
            if (m_SubmitThread.joinable())
                m_SubmitThread.join();
            const int64_t retry_after =
                m_Submit->m_RetryAfterMs.load(std::memory_order_acquire);
            if (now < retry_after)
                return false;
            m_Submit.reset();
        } else if (!m_Submit->m_Running.load(std::memory_order_acquire)) {
            return false;
        } else {
            const int64_t last = m_Submit->m_LastPostMs.load(std::memory_order_acquire);
            const bool stuck = (m_Submit->m_InLock.load(std::memory_order_acquire)
                                || m_Submit->m_InPost.load(std::memory_order_acquire))
                && last > 0 && now - last > kWindowLockWatchdogMs;
            if (!stuck)
                return true;
            m_Submit->m_RetryAfterMs.store(
                now + kSubmitRestartDelayMs, std::memory_order_release);
            m_Submit->m_Running.store(false, std::memory_order_release);
            m_Submit->m_Cond.notify_all();
            m_SurfaceRecoveryRequested->store(
                true, std::memory_order_release);
            return false;
        }
    }
    if (m_SubmitThread.joinable())
        return false;

    try {
        std::shared_ptr<SubmitState> state = MakeSubmitState();
        {
            std::lock_guard<std::mutex> window_lock(m_WindowMutex);
            state->m_Window = m_Window;
            if (state->m_Window != nullptr)
                ANativeWindow_acquire(state->m_Window);
        }
        if (state->m_Window == nullptr)
            return false;

        state->m_Running.store(true, std::memory_order_release);
        state->m_Exited.store(false, std::memory_order_release);
        state->m_LastPostMs.store(now, std::memory_order_release);
        state->m_RetryAfterMs.store(0, std::memory_order_release);
        state->m_PresentationRate = GetPresentationRateTracker();
        state->m_SurfaceRecoveryRequested = m_SurfaceRecoveryRequested;
        m_Submit = state;
        try {
            m_SubmitThread = std::thread([state] {
                RunSubmitLoop(state);
            });
        } catch (...) {
            state->m_Running.store(false, std::memory_order_release);
            state->m_Exited.store(true, std::memory_order_release);
            state->m_RetryAfterMs.store(
                now + kSubmitRestartDelayMs, std::memory_order_release);
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool CPUGraphics::StopSubmitThread(bool abandonIfBlocked) {
    std::lock_guard<std::mutex> lock(m_ThreadMutex);
    std::shared_ptr<SubmitState> state = m_Submit;
    if (state == nullptr) {
        const bool thread_joinable = m_SubmitThread.joinable();
        if (thread_joinable && abandonIfBlocked)
            m_SubmitThread.detach();
        return !thread_joinable;
    }

    {
        std::lock_guard<std::mutex> state_lock(state->m_Mutex);
        state->m_Running.store(false, std::memory_order_release);
        state->m_HasWork = false;
    }
    state->m_Cond.notify_all();
    if (!state->m_Exited.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> state_lock(state->m_Mutex);
        state->m_Cond.wait_for(
            state_lock, std::chrono::milliseconds(kSubmitStopWaitMs), [&] {
                return state->m_Exited.load(std::memory_order_acquire);
            });
    }

    if (state->m_Exited.load(std::memory_order_acquire)) {
        if (m_SubmitThread.joinable())
            m_SubmitThread.join();
        if (m_Submit == state)
            m_Submit.reset();
        return true;
    }

    if (abandonIfBlocked && m_SubmitThread.joinable())
        m_SubmitThread.detach();
    return false;
}

void CPUGraphics::SubmitLoop(std::shared_ptr<SubmitState> state) {
    RunSubmitLoop(state);
}

bool CPUGraphics::ConsumeSurfaceRecoveryRequest() {
    return m_SurfaceRecoveryRequested->exchange(
        false, std::memory_order_acq_rel);
}

void CPUGraphics::Render(ImDrawData* draw_data) {
    if (draw_data == nullptr || !draw_data->Valid || !m_SurfaceReady)
        return;
    try {
        if (!UpdateManagedTextures(draw_data))
            return;
    } catch (...) {
        return;
    }
    if (!StartSubmitThread())
        return;

    std::shared_ptr<SubmitState> state;
    {
        std::lock_guard<std::mutex> lock(m_ThreadMutex);
        state = m_Submit;
    }
    if (state == nullptr || !state->m_Running.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lock(state->m_Mutex);
        if (state->m_HasWork)
            return;
    }

    std::lock_guard<std::mutex> render_lock(m_RenderMutex);
    if (m_BufWidth <= 0 || m_BufHeight <= 0)
        return;

    size_t pixel_count = 0;
    if (!TryPixelCount(m_BufWidth, m_BufHeight, &pixel_count))
        return;
    try {
        if (!m_Pool)
            m_Pool = std::make_unique<RenderPool>();
        if (!m_FrameScratch)
            m_FrameScratch = std::make_unique<FrameScratch>();
        if (m_Buffer.size() != pixel_count)
            m_Buffer.assign(pixel_count, 0u);
    } catch (...) {
        return;
    }

    const ImVec2 display_pos = draw_data->DisplayPos;
    const ImVec2 scale = draw_data->FramebufferScale;
    if (!std::isfinite(display_pos.x) || !std::isfinite(display_pos.y)
        || !std::isfinite(scale.x) || !std::isfinite(scale.y)
        || scale.x <= 0.0f || scale.y <= 0.0f)
        return;

    PreparedFrame& frame = m_FrameScratch->Reset();
    const PixelRect surface_bounds{0, 0, m_BufWidth, m_BufHeight};
    try {
        if (draw_data->TotalIdxCount > 0) {
            frame.primitives.reserve(
                static_cast<size_t>(draw_data->TotalIdxCount) / 3u);
        }
        for (const ImDrawList* draw_list : draw_data->CmdLists) {
            if (draw_list == nullptr)
                continue;
            for (const ImDrawCmd& command : draw_list->CmdBuffer) {
                if (command.UserCallback != nullptr)
                    continue;

                const double clip_x0 = static_cast<double>(
                    (command.ClipRect.x - display_pos.x) * scale.x);
                const double clip_y0 = static_cast<double>(
                    (command.ClipRect.y - display_pos.y) * scale.y);
                const double clip_x1 = static_cast<double>(
                    (command.ClipRect.z - display_pos.x) * scale.x);
                const double clip_y1 = static_cast<double>(
                    (command.ClipRect.w - display_pos.y) * scale.y);
                if (!std::isfinite(clip_x0) || !std::isfinite(clip_y0)
                    || !std::isfinite(clip_x1) || !std::isfinite(clip_y1))
                    continue;
                const PixelRect command_clip = ClampRect({
                    SaturatingInt(std::floor(clip_x0)),
                    SaturatingInt(std::floor(clip_y0)),
                    SaturatingInt(std::ceil(clip_x1)),
                    SaturatingInt(std::ceil(clip_y1)),
                }, m_BufWidth, m_BufHeight);
                if (!command_clip.valid())
                    continue;

                TextureView texture;
                const ImTextureID texture_id = command.GetTexID();
                if (IsManagedTextureId(texture_id)) {
                    const ManagedTexture* data = ManagedTextureFromId(texture_id);
                    if (data == nullptr || data->width <= 0 || data->height <= 0)
                        continue;
                    if (data->format == ImTextureFormat_Alpha8) {
                        if (data->alpha.empty())
                            continue;
                        texture.kind = TextureKind::FontAlpha;
                        texture.alpha = data->alpha.data();
                    } else {
                        if (data->rgba.empty())
                            continue;
                        texture.kind = TextureKind::Rgba;
                        texture.rgba = data->rgba.data();
                    }
                    texture.width = data->width;
                    texture.height = data->height;
                } else {
                    const CpuTextureData* data = nullptr;
                    for (const BaseTexData* candidate : m_Textures) {
                        if (candidate != nullptr
                            && TextureIdBits(candidate->DS)
                                == TextureIdBits(texture_id)) {
                            data = static_cast<const CpuTextureData*>(candidate);
                            break;
                        }
                    }
                    if (data == nullptr || data->Width <= 0 || data->Height <= 0
                        || data->pixels.empty())
                        continue;
                    texture.kind = TextureKind::Rgba;
                    texture.rgba = data->pixels.data();
                    texture.width = data->Width;
                    texture.height = data->Height;
                }

                if (command.IdxOffset
                    > static_cast<unsigned int>(draw_list->IdxBuffer.Size)
                    || draw_list->IdxBuffer.Data == nullptr
                    || draw_list->VtxBuffer.Data == nullptr)
                    continue;
                const unsigned int available_indices =
                    static_cast<unsigned int>(draw_list->IdxBuffer.Size)
                    - command.IdxOffset;
                const unsigned int element_count =
                    std::min(command.ElemCount, available_indices);
                if (element_count < 3)
                    continue;
                const ImDrawIdx* indices =
                    draw_list->IdxBuffer.Data + command.IdxOffset;
                auto vertex_at = [&](unsigned int index,
                                     RasterVertex* output) -> bool {
                    if (output == nullptr)
                        return false;
                    const size_t resolved =
                        static_cast<size_t>(command.VtxOffset) + index;
                    if (resolved >= static_cast<size_t>(draw_list->VtxBuffer.Size))
                        return false;
                    const ImDrawVert& vertex =
                        draw_list->VtxBuffer[static_cast<int>(resolved)];
                    *output = {
                        (vertex.pos.x - display_pos.x) * scale.x,
                        (vertex.pos.y - display_pos.y) * scale.y,
                        vertex.uv.x,
                        vertex.uv.y,
                        vertex.col,
                    };
                    return IsFiniteVertex(*output);
                };

                unsigned int index_pos = 0;
                while (index_pos + 2 < element_count) {
                    if (index_pos + 5 < element_count
                        && indices[index_pos] == indices[index_pos + 3]
                        && indices[index_pos + 2] == indices[index_pos + 4]) {
                        RasterVertex quad[4];
                        if (vertex_at(indices[index_pos], &quad[0])
                            && vertex_at(indices[index_pos + 1], &quad[1])
                            && vertex_at(indices[index_pos + 2], &quad[2])
                            && vertex_at(indices[index_pos + 5], &quad[3])) {
                            AppendPreparedPrimitive(
                                &frame, quad, 4, texture, command_clip,
                                surface_bounds);
                            index_pos += 6;
                            continue;
                        }
                    }

                    RasterVertex triangle[3];
                    if (vertex_at(indices[index_pos], &triangle[0])
                        && vertex_at(indices[index_pos + 1], &triangle[1])
                        && vertex_at(indices[index_pos + 2], &triangle[2])) {
                        AppendPreparedPrimitive(
                            &frame, triangle, 3, texture, command_clip,
                            surface_bounds);
                    }
                    index_pos += 3;
                }
            }
        }
        BuildPrimitiveBands(&frame, m_BufHeight);
    } catch (...) {
        return;
    }

    const PixelRect submit_dirty = ClampRect(
        UnionRect(m_LastSubmittedContentRect, frame.content_rect),
        m_BufWidth, m_BufHeight);
    if (!submit_dirty.valid())
        return;
    const PixelRect clear_rect = ClampRect(
        UnionRect(m_BufferContentRect, frame.content_rect),
        m_BufWidth, m_BufHeight);

    m_Abort.store(false, std::memory_order_release);
    const ImVec2 white_uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
    auto make_task = [&, this](int band) {
        return [&, this, band] {
            const int band_y0 = m_BufHeight * band / RenderPool::kWorkers;
            const int band_y1 =
                m_BufHeight * (band + 1) / RenderPool::kWorkers;
            const int clear_y0 = std::max(band_y0, clear_rect.y0);
            const int clear_y1 = std::min(band_y1, clear_rect.y1);
            if (clear_rect.valid()) {
                for (int y = clear_y0; y < clear_y1; ++y) {
                    std::memset(
                        m_Buffer.data() + static_cast<size_t>(y) * m_BufWidth
                            + clear_rect.x0,
                        0,
                        static_cast<size_t>(clear_rect.x1 - clear_rect.x0)
                            * sizeof(uint32_t));
                }
            }
            RasterPreparedBand(frame, band, m_Buffer.data(), m_BufWidth,
                               m_BufHeight, white_uv.x, white_uv.y, m_Abort);
        };
    };
    try {
        m_Pool->run(make_task(0), make_task(1), make_task(2), make_task(3));
    } catch (...) {
        return;
    }
    if (!m_Pool->wait_for_ms(250)) {
        m_Abort.store(true, std::memory_order_release);
        if (!m_Pool->wait_for_ms(500)) {
            // Tasks execute bounded loops and observe m_Abort.
            while (!m_Pool->wait_for_ms(50)) {
            }
        }
        std::fill(m_Buffer.begin(), m_Buffer.end(), 0u);
        m_BufferContentRect = {};
        return;
    }
    m_BufferContentRect = frame.content_rect;

    {
        std::lock_guard<std::mutex> lock(state->m_Mutex);
        if (!state->m_Running.load(std::memory_order_acquire)
            || state->m_HasWork)
            return;
        state->buffer.swap(m_Buffer);
        std::swap(state->m_ContentRect, m_BufferContentRect);
        state->m_BufW = m_BufWidth;
        state->m_BufH = m_BufHeight;
        state->m_DirtyRect = submit_dirty;
        state->m_HasWork = true;
    }
    m_LastSubmittedContentRect = frame.content_rect;
    state->m_Cond.notify_one();
    if (m_Buffer.size() != pixel_count) {
        try {
            m_Buffer.assign(pixel_count, 0u);
            m_BufferContentRect = {};
        } catch (...) {
            return;
        }
    }
}

void CPUGraphics::PrepareShutdown() {
    DestroyManagedTextures();
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset
                           | ImGuiBackendFlags_RendererHasTextures);
        io.BackendRendererName = nullptr;
    }
}

void CPUGraphics::Cleanup() {
    m_SurfaceReady = false;
    StopSubmitThread(true);
    m_Abort.store(true, std::memory_order_release);
    m_Pool.reset();
    m_FrameScratch.reset();
    m_Buffer.clear();
    m_BufferContentRect = {};
    m_LastSubmittedContentRect = {};
    m_BufWidth = 0;
    m_BufHeight = 0;
    m_GeometryFailures = 0;
    m_NextGeometryRetryMs = 0;
    m_SurfaceRecoveryRequested->store(false, std::memory_order_release);
}

BaseTexData* CPUGraphics::LoadTexture(BaseTexData* texture, void* pixel_data) {
    if (texture == nullptr)
        return nullptr;

    size_t pixel_count = 0;
    if (!TryPixelCount(texture->Width, texture->Height, &pixel_count))
        return nullptr;

    try {
        auto result = std::make_unique<CpuTextureData>();
        result->Width = texture->Width;
        result->Height = texture->Height;
        result->Channels = texture->Channels;
        result->pixels.resize(pixel_count);
        if (pixel_data != nullptr) {
            std::memcpy(result->pixels.data(), pixel_data,
                        pixel_count * sizeof(uint32_t));
        }
        result->DS = result.get();
        return result.release();
    } catch (...) {
        return nullptr;
    }
}

void CPUGraphics::RemoveTexture(BaseTexData* texture) {
    delete static_cast<CpuTextureData*>(texture);
}
