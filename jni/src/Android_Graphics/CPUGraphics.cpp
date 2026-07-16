#include "CPUGraphics.h"

#include <arm_neon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include "imgui.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kWindowWatchdogMs = 500;
constexpr int kRasterWaitMs = 250;
constexpr int kRasterAbortWaitMs = 500;
constexpr int kSubmitStopWaitMs = 100;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::uint8_t Scale255Exact(std::uint16_t value) {
    const std::uint16_t adjusted =
        static_cast<std::uint16_t>(value + (value >> 8));
    return static_cast<std::uint8_t>((adjusted + 0x80u) >> 8);
}

std::uint8_t Div255Floor(std::uint32_t value) {
    return static_cast<std::uint8_t>((value * 32897u) >> 23);
}

std::uint32_t BlendPixel(std::uint32_t destination, std::uint32_t source) {
    const std::uint32_t sourceAlpha = source >> 24;
    if (sourceAlpha == 0) {
        return destination;
    }
    if (sourceAlpha == 255) {
        return source;
    }

    const std::uint32_t inverseAlpha = sourceAlpha ^ 255u;
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const std::uint32_t destinationChannel =
            (destination >> shift) & 255u;
        const std::uint32_t sourceChannel = (source >> shift) & 255u;
        result |= static_cast<std::uint32_t>(Scale255Exact(
                      static_cast<std::uint16_t>(
                          destinationChannel * inverseAlpha +
                          sourceChannel * sourceAlpha)))
               << shift;
    }

    const std::uint32_t destinationAlpha = destination >> 24;
    const std::uint32_t outputAlpha =
        sourceAlpha + Div255Floor(destinationAlpha * inverseAlpha);
    return result | (outputAlpha << 24);
}

void BlendSpan(std::uint32_t* destination,
               int count,
               std::uint32_t source) {
    if (destination == nullptr || count <= 0 || (source >> 24) == 0) {
        return;
    }

    const std::uint32_t sourceAlpha = source >> 24;
    int offset = 0;
    if (sourceAlpha == 255) {
        const uint32x4_t fill = vdupq_n_u32(source);
        for (; offset + 4 <= count; offset += 4) {
            vst1q_u32(destination + offset, fill);
        }
        for (; offset < count; ++offset) {
            destination[offset] = source;
        }
        return;
    }

    const uint8x8_t inverse8 =
        vdup_n_u8(static_cast<std::uint8_t>(sourceAlpha ^ 255u));
    const uint8x8_t alpha8 =
        vdup_n_u8(static_cast<std::uint8_t>(sourceAlpha));
    const uint8x16_t sourceBytes =
        vreinterpretq_u8_u32(vdupq_n_u32(source));
    const uint8x8_t sourceLow = vget_low_u8(sourceBytes);
    const uint8x8_t sourceHigh = vget_high_u8(sourceBytes);
    const uint32x4_t rgbMask = vdupq_n_u32(0x00FFFFFFu);
    const uint32x4_t sourceAlpha4 = vdupq_n_u32(sourceAlpha);
    const std::uint32_t alphaCoefficient =
        (sourceAlpha ^ 255u) * 32897u;

    for (; offset + 4 <= count; offset += 4) {
        const uint32x4_t destinationWords =
            vld1q_u32(destination + offset);
        const uint8x16_t destinationBytes =
            vreinterpretq_u8_u32(destinationWords);

        uint16x8_t low =
            vmull_u8(vget_low_u8(destinationBytes), inverse8);
        uint16x8_t high =
            vmull_u8(vget_high_u8(destinationBytes), inverse8);
        low = vmlal_u8(low, sourceLow, alpha8);
        high = vmlal_u8(high, sourceHigh, alpha8);
        low = vsraq_n_u16(low, low, 8);
        high = vsraq_n_u16(high, high, 8);
        const uint8x16_t blendedBytes = vcombine_u8(
            vrshrn_n_u16(low, 8), vrshrn_n_u16(high, 8));

        const uint32x4_t destinationAlpha4 =
            vshrq_n_u32(destinationWords, 24);
        const uint32x4_t outputAlpha4 = vaddq_u32(
            sourceAlpha4,
            vshrq_n_u32(
                vmulq_n_u32(destinationAlpha4, alphaCoefficient), 23));
        const uint32x4_t output = vorrq_u32(
            vandq_u32(vreinterpretq_u32_u8(blendedBytes), rgbMask),
            vshlq_n_u32(outputAlpha4, 24));
        vst1q_u32(destination + offset, output);
    }

    for (; offset < count; ++offset) {
        destination[offset] = BlendPixel(destination[offset], source);
    }
}

std::uint32_t ModulateColor(std::uint32_t texture,
                            std::uint32_t tint) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const std::uint16_t product = static_cast<std::uint16_t>(
            ((texture >> shift) & 255u) * ((tint >> shift) & 255u));
        result |= static_cast<std::uint32_t>(Scale255Exact(product)) << shift;
    }
    return result;
}

struct RasterVertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t color = 0;
};

struct RasterClip {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
};

enum class TextureKind {
    Solid,
    FontAlpha,
    Rgba,
};

struct TextureView {
    TextureKind kind = TextureKind::Solid;
    const std::uint8_t* alpha = nullptr;
    const std::uint32_t* rgba = nullptr;
    int width = 0;
    int height = 0;
};

int ClampTextureCoordinate(int value, int size) {
    if (value < 0) {
        return 0;
    }
    if (value >= size) {
        return size - 1;
    }
    return value;
}

std::uint32_t InterpolateColor(const RasterVertex& first,
                               const RasterVertex& second,
                               const RasterVertex& third,
                               float firstWeight,
                               float secondWeight,
                               float thirdWeight) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const float value =
            static_cast<float>((first.color >> shift) & 255u) * firstWeight +
            static_cast<float>((second.color >> shift) & 255u) * secondWeight +
            static_cast<float>((third.color >> shift) & 255u) * thirdWeight;
        result |= static_cast<std::uint32_t>(value) << shift;
    }
    return result;
}

std::uint32_t ShadePixel(const TextureView& texture,
                         float u,
                         float v,
                         std::uint32_t color) {
    if (texture.kind == TextureKind::Solid) {
        return color;
    }
    if (texture.width <= 0 || texture.height <= 0) {
        return 0;
    }

    const int textureX = ClampTextureCoordinate(
        static_cast<int>(u * texture.width), texture.width);
    const int textureY = ClampTextureCoordinate(
        static_cast<int>(v * texture.height), texture.height);
    const std::size_t index =
        static_cast<std::size_t>(textureY) * texture.width + textureX;

    if (texture.kind == TextureKind::Rgba) {
        return texture.rgba == nullptr
            ? 0
            : ModulateColor(texture.rgba[index], color);
    }

    if (texture.alpha == nullptr) {
        return 0;
    }
    const std::uint32_t coverage = texture.alpha[index];
    if (coverage == 0) {
        return 0;
    }
    const std::uint32_t alpha =
        Div255Floor(coverage * (color >> 24));
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

bool IsWhiteUv(const RasterVertex& vertex,
               float whiteU,
               float whiteV,
               float tolerance) {
    return std::fabs(vertex.u - whiteU) < tolerance &&
           std::fabs(vertex.v - whiteV) < tolerance;
}

bool IsInside(float firstWeight,
              float secondWeight,
              float thirdWeight) {
    return firstWeight >= 0.0f &&
           secondWeight >= 0.0f &&
           thirdWeight >= 0.0f;
}

bool TryRasterQuad(const RasterVertex& first,
                   const RasterVertex& second,
                   const RasterVertex& third,
                   const RasterVertex& fourth,
                   const TextureView& texture,
                   const RasterClip& clip,
                   std::uint32_t* buffer,
                   int stride,
                   float whiteU,
                   float whiteV,
                   const std::atomic<bool>& abort) {
    if (std::fabs(first.x - fourth.x) > 0.01f ||
        std::fabs(second.x - third.x) > 0.01f ||
        std::fabs(first.y - second.y) > 0.01f ||
        std::fabs(third.y - fourth.y) > 0.01f) {
        return false;
    }
    if (first.color != second.color ||
        first.color != third.color ||
        first.color != fourth.color) {
        return false;
    }

    const float left =
        std::min({first.x, second.x, third.x, fourth.x});
    const float right =
        std::max({first.x, second.x, third.x, fourth.x});
    const float top =
        std::min({first.y, second.y, third.y, fourth.y});
    const float bottom =
        std::max({first.y, second.y, third.y, fourth.y});
    const int minX =
        std::max(clip.minX, static_cast<int>(std::floor(left)));
    const int minY =
        std::max(clip.minY, static_cast<int>(std::floor(top)));
    const int maxX =
        std::min(clip.maxX, static_cast<int>(std::ceil(right)));
    const int maxY =
        std::min(clip.maxY, static_cast<int>(std::ceil(bottom)));
    if (maxX <= minX || maxY <= minY) {
        return true;
    }

    const bool solid =
        texture.kind != TextureKind::Rgba &&
        IsWhiteUv(first, whiteU, whiteV, 0.001f) &&
        IsWhiteUv(second, whiteU, whiteV, 0.001f) &&
        IsWhiteUv(third, whiteU, whiteV, 0.001f) &&
        IsWhiteUv(fourth, whiteU, whiteV, 0.001f);
    if (solid) {
        for (int y = minY; y < maxY; ++y) {
            if ((y & 15) == 0 &&
                abort.load(std::memory_order_relaxed)) {
                return true;
            }
            BlendSpan(buffer + y * stride + minX,
                      maxX - minX,
                      first.color);
        }
        return true;
    }

    if (texture.kind != TextureKind::FontAlpha ||
        texture.width <= 0 ||
        texture.height <= 0) {
        return false;
    }

    const bool axisAlignedUv =
        std::fabs(first.u - fourth.u) <= 0.0005f &&
        std::fabs(second.u - third.u) <= 0.0005f &&
        std::fabs(first.v - second.v) <= 0.0005f &&
        std::fabs(third.v - fourth.v) <= 0.0005f;
    if (!axisAlignedUv) {
        return false;
    }

    const float deltaX = second.x - first.x;
    const float deltaY = fourth.y - first.y;
    if (std::fabs(deltaX) <= 0.001f ||
        std::fabs(deltaY) <= 0.001f) {
        return true;
    }

    const float uPerX = (second.u - first.u) / deltaX;
    const float vPerX = (second.v - first.v) / deltaX;
    const float uPerY = (fourth.u - first.u) / deltaY;
    const float vPerY = (fourth.v - first.v) / deltaY;

    for (int y = minY; y < maxY; ++y) {
        if ((y & 15) == 0 &&
            abort.load(std::memory_order_relaxed)) {
            return true;
        }
        const float pixelY = static_cast<float>(y) + 0.5f;
        float u = first.u + (pixelY - first.y) * uPerY +
                  (static_cast<float>(minX) + 0.5f - first.x) * uPerX;
        float v = first.v + (pixelY - first.y) * vPerY +
                  (static_cast<float>(minX) + 0.5f - first.x) * vPerX;
        std::uint32_t* destination = buffer + y * stride + minX;
        for (int x = minX; x < maxX; ++x, ++destination) {
            const std::uint32_t source =
                ShadePixel(texture, u, v, first.color);
            if ((source >> 24) != 0) {
                *destination = BlendPixel(*destination, source);
            }
            u += uPerX;
            v += vPerX;
        }
    }
    return true;
}

float EdgeXAtY(const RasterVertex& first,
               const RasterVertex& second,
               float y) {
    const float deltaY = second.y - first.y;
    if (std::fabs(deltaY) <= 0.000001f) {
        return first.x;
    }
    return first.x +
           (y - first.y) * (second.x - first.x) / deltaY;
}

void RasterTriangle(const RasterVertex& first,
                    const RasterVertex& second,
                    const RasterVertex& third,
                    const TextureView& texture,
                    const RasterClip& clip,
                    std::uint32_t* buffer,
                    int stride,
                    float whiteU,
                    float whiteV,
                    const std::atomic<bool>& abort) {
    const float firstSecondX = second.x - first.x;
    const float firstSecondY = second.y - first.y;
    const float firstThirdX = third.x - first.x;
    const float firstThirdY = third.y - first.y;
    const float determinant =
        firstSecondX * firstThirdY - firstThirdX * firstSecondY;
    if (std::fabs(determinant) < 0.5f) {
        return;
    }

    const RasterVertex* top = &first;
    const RasterVertex* middle = &second;
    const RasterVertex* bottom = &third;
    if (middle->y < top->y) {
        std::swap(top, middle);
    }
    if (bottom->y < middle->y) {
        std::swap(middle, bottom);
    }
    if (middle->y < top->y) {
        std::swap(top, middle);
    }

    const int minY =
        std::max(clip.minY, static_cast<int>(top->y));
    const int maxY =
        std::min(clip.maxY, static_cast<int>(bottom->y + 1.0f));
    if (maxY <= minY) {
        return;
    }

    const float inverseDeterminant = 1.0f / determinant;
    const float secondStep = firstThirdY * inverseDeterminant;
    const float thirdStep = -firstSecondY * inverseDeterminant;
    const float firstStep = -(secondStep + thirdStep);
    const bool sameColor =
        first.color == second.color && first.color == third.color;
    const bool solid =
        sameColor &&
        texture.kind != TextureKind::Rgba &&
        IsWhiteUv(first, whiteU, whiteV, 0.001f) &&
        IsWhiteUv(second, whiteU, whiteV, 0.001f) &&
        IsWhiteUv(third, whiteU, whiteV, 0.001f);

    for (int y = minY; y < maxY; ++y) {
        if ((y & 15) == 0 &&
            abort.load(std::memory_order_relaxed)) {
            return;
        }
        const float pixelY = static_cast<float>(y) + 0.5f;
        const float longX = EdgeXAtY(*top, *bottom, pixelY);
        const float shortX = pixelY < middle->y
            ? EdgeXAtY(*top, *middle, pixelY)
            : EdgeXAtY(*middle, *bottom, pixelY);
        const float left = std::min(longX, shortX);
        const float right = std::max(longX, shortX);
        const int minX =
            std::max(clip.minX, static_cast<int>(left));
        const int maxX =
            std::min(clip.maxX, static_cast<int>(right + 1.0f));
        if (maxX <= minX) {
            continue;
        }

        const float pixelX = static_cast<float>(minX) + 0.5f;
        float secondWeight =
            ((pixelX - first.x) * firstThirdY +
             (pixelY - first.y) * (first.x - third.x)) *
            inverseDeterminant;
        float thirdWeight =
            (firstSecondX * (pixelY - first.y) +
             (pixelX - first.x) * (first.y - second.y)) *
            inverseDeterminant;
        float firstWeight = 1.0f - secondWeight - thirdWeight;

        if (solid) {
            int runStart = -1;
            int runEnd = -1;
            if ((first.color >> 24) == 255u && maxX - minX >= 17) {
                runStart = minX;
                float leftFirst = firstWeight;
                float leftSecond = secondWeight;
                float leftThird = thirdWeight;
                while (runStart < maxX &&
                       !IsInside(leftFirst, leftSecond, leftThird)) {
                    ++runStart;
                    leftFirst += firstStep;
                    leftSecond += secondStep;
                    leftThird += thirdStep;
                }
                if (runStart < maxX) {
                    runEnd = maxX;
                    const float rightOffset =
                        static_cast<float>(maxX - minX - 1);
                    float rightFirst =
                        firstWeight + firstStep * rightOffset;
                    float rightSecond =
                        secondWeight + secondStep * rightOffset;
                    float rightThird =
                        thirdWeight + thirdStep * rightOffset;
                    while (runEnd > runStart &&
                           !IsInside(rightFirst,
                                     rightSecond,
                                     rightThird)) {
                        --runEnd;
                        rightFirst -= firstStep;
                        rightSecond -= secondStep;
                        rightThird -= thirdStep;
                    }
                }
            } else {
                for (int x = minX; x < maxX; ++x) {
                    if (IsInside(firstWeight,
                                 secondWeight,
                                 thirdWeight)) {
                        if (runStart < 0) {
                            runStart = x;
                        }
                        runEnd = x + 1;
                    } else if (runStart >= 0) {
                        break;
                    }
                    firstWeight += firstStep;
                    secondWeight += secondStep;
                    thirdWeight += thirdStep;
                }
            }

            if (runStart >= 0) {
                BlendSpan(buffer + y * stride + runStart,
                          runEnd - runStart,
                          first.color);
            }
            continue;
        }

        std::uint32_t* destination =
            buffer + y * stride + minX;
        for (int x = minX; x < maxX; ++x, ++destination) {
            if (IsInside(firstWeight,
                         secondWeight,
                         thirdWeight)) {
                const std::uint32_t color = sameColor
                    ? first.color
                    : InterpolateColor(first,
                                       second,
                                       third,
                                       firstWeight,
                                       secondWeight,
                                       thirdWeight);
                const float u =
                    first.u * firstWeight +
                    second.u * secondWeight +
                    third.u * thirdWeight;
                const float v =
                    first.v * firstWeight +
                    second.v * secondWeight +
                    third.v * thirdWeight;
                const std::uint32_t source =
                    ShadePixel(texture, u, v, color);
                if ((source >> 24) != 0) {
                    *destination =
                        BlendPixel(*destination, source);
                }
            }
            firstWeight += firstStep;
            secondWeight += secondStep;
            thirdWeight += thirdStep;
        }
    }
}

std::shared_ptr<CpuSubmitState> MakeSubmitState() {
    return std::shared_ptr<CpuSubmitState>(
        new CpuSubmitState(),
        [](CpuSubmitState* state) {
            if (state->window != nullptr) {
                ANativeWindow_release(state->window);
            }
            delete state;
        });
}

void RunSubmitLoop(const std::shared_ptr<CpuSubmitState>& state) {
    std::vector<std::uint32_t> localBuffer;

    while (state->running.load(std::memory_order_acquire)) {
        int width = 0;
        int height = 0;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait(lock, [&] {
                return !state->running.load(std::memory_order_acquire) ||
                       state->hasWork;
            });
            if (!state->running.load(std::memory_order_acquire)) {
                break;
            }
            localBuffer.swap(state->buffer);
            width = state->bufferWidth;
            height = state->bufferHeight;
            minX = state->dirtyMinX;
            minY = state->dirtyMinY;
            maxX = state->dirtyMaxX;
            maxY = state->dirtyMaxY;
            state->hasWork = false;
        }

        const std::size_t expectedPixels =
            width > 0 && height > 0
                ? static_cast<std::size_t>(width) *
                      static_cast<std::size_t>(height)
                : 0;
        if (localBuffer.size() != expectedPixels ||
            maxX <= minX ||
            maxY <= minY ||
            state->window == nullptr) {
            continue;
        }

        state->lastProgressMs.store(NowMs(), std::memory_order_release);
        ANativeWindow* window = state->window;
        ANativeWindow_acquire(window);

        const int windowWidth = ANativeWindow_getWidth(window);
        const int windowHeight = ANativeWindow_getHeight(window);
        if (windowWidth != width || windowHeight != height) {
            ANativeWindow_setBuffersGeometry(
                window, width, height, WINDOW_FORMAT_RGBA_8888);
            ANativeWindow_release(window);
            continue;
        }

        ARect dirty{
            std::clamp(minX, 0, width),
            std::clamp(minY, 0, height),
            std::clamp(maxX, 0, width),
            std::clamp(maxY, 0, height),
        };
        ANativeWindow_Buffer nativeBuffer{};
        state->insideWindowLock.store(true, std::memory_order_release);
        const int lockResult =
            ANativeWindow_lock(window, &nativeBuffer, &dirty);
        state->insideWindowLock.store(false, std::memory_order_release);
        if (lockResult != 0) {
            ANativeWindow_release(window);
            continue;
        }

        if (nativeBuffer.width != width ||
            nativeBuffer.height != height) {
            state->insideWindowPost.store(true, std::memory_order_release);
            ANativeWindow_unlockAndPost(window);
            state->insideWindowPost.store(false, std::memory_order_release);
            ANativeWindow_setBuffersGeometry(
                window, width, height, WINDOW_FORMAT_RGBA_8888);
            ANativeWindow_release(window);
            continue;
        }

        dirty.left = std::clamp(dirty.left, 0, width);
        dirty.top = std::clamp(dirty.top, 0, height);
        dirty.right = std::clamp(dirty.right, 0, width);
        dirty.bottom = std::clamp(dirty.bottom, 0, height);
        const int copyWidth = dirty.right - dirty.left;
        const int copyHeight = dirty.bottom - dirty.top;
        bool copiedFrame = false;
        if (nativeBuffer.bits != nullptr &&
            nativeBuffer.format == WINDOW_FORMAT_RGBA_8888 &&
            nativeBuffer.stride >= width &&
            copyWidth > 0 &&
            copyHeight > 0) {
            auto* output =
                static_cast<std::uint32_t*>(nativeBuffer.bits);
            const std::uint32_t* input = localBuffer.data();
            const std::size_t rowBytes =
                static_cast<std::size_t>(copyWidth) *
                sizeof(std::uint32_t);
            if (dirty.left == 0 &&
                copyWidth == width &&
                nativeBuffer.stride == width) {
                std::memcpy(
                    output +
                        static_cast<std::size_t>(dirty.top) *
                            nativeBuffer.stride,
                    input +
                        static_cast<std::size_t>(dirty.top) * width,
                    rowBytes * static_cast<std::size_t>(copyHeight));
            } else {
                for (int y = dirty.top; y < dirty.bottom; ++y) {
                    std::memcpy(
                        output +
                            static_cast<std::size_t>(y) *
                                nativeBuffer.stride +
                            dirty.left,
                        input +
                            static_cast<std::size_t>(y) * width +
                            dirty.left,
                        rowBytes);
                }
            }
            copiedFrame = true;
        }

        state->insideWindowPost.store(true, std::memory_order_release);
        const int postResult = ANativeWindow_unlockAndPost(window);
        state->insideWindowPost.store(false, std::memory_order_release);
        if (postResult == 0) {
            state->lastProgressMs.store(
                NowMs(), std::memory_order_release);
            if (copiedFrame &&
                state->running.load(std::memory_order_acquire) &&
                state->presentationRate != nullptr) {
                state->presentationRate->Record();
            }
        }
        ANativeWindow_release(window);
    }

    state->running.store(false, std::memory_order_release);
    state->exited.store(true, std::memory_order_release);
}

bool HasValidFrameSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    constexpr std::size_t kMaxPixels = 64u * 1024u * 1024u;
    const std::size_t unsignedWidth = static_cast<std::size_t>(width);
    const std::size_t unsignedHeight = static_cast<std::size_t>(height);
    return unsignedWidth <= kMaxPixels / unsignedHeight;
}

}  // namespace

CpuRasterPool::CpuRasterPool() {
    for (int index = 0; index < kWorkerCount; ++index) {
        threads_[index] =
            std::thread(&CpuRasterPool::WorkerMain, this, index);
    }
}

CpuRasterPool::~CpuRasterPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        alive_ = false;
    }
    workCondition_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void CpuRasterPool::Run(std::function<void()> first,
                        std::function<void()> second,
                        std::function<void()> third,
                        std::function<void()> fourth) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[0] = std::move(first);
        tasks_[1] = std::move(second);
        tasks_[2] = std::move(third);
        tasks_[3] = std::move(fourth);
        doneCount_.store(0, std::memory_order_release);
        ++generation_;
    }
    workCondition_.notify_all();
}

bool CpuRasterPool::WaitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return doneCondition_.wait_for(lock, timeout, [&] {
        return doneCount_.load(std::memory_order_acquire) >= kWorkerCount;
    });
}

void CpuRasterPool::WorkerMain(int index) {
    int seenGeneration = 0;
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workCondition_.wait(lock, [&] {
                return !alive_ || generation_ > seenGeneration;
            });
            if (!alive_) {
                return;
            }
            seenGeneration = generation_;
            seenGeneration_[index] = seenGeneration;
            task = tasks_[index];
        }
        if (task) {
            task();
        }
        if (doneCount_.fetch_add(1, std::memory_order_acq_rel) + 1 ==
            kWorkerCount) {
            doneCondition_.notify_one();
        }
    }
}

CPUGraphics::CPUGraphics() {
    std::snprintf(RenderName, sizeof(RenderName), "CPU");
}

CPUGraphics::~CPUGraphics() {
    StopSubmitThread();
    abortRaster_.store(true, std::memory_order_release);
    rasterPool_.reset();
}

bool CPUGraphics::Create() {
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (m_Window != nullptr) {
            width = ANativeWindow_getWidth(m_Window);
            height = ANativeWindow_getHeight(m_Window);
        }
    }
    if (width <= 0 || height <= 0) {
        width = static_cast<int>(m_Width);
        height = static_cast<int>(m_Height);
    }
    if (!HasValidFrameSize(width, height)) {
        return false;
    }

    m_Width = static_cast<float>(width);
    m_Height = static_cast<float>(height);
    bufferWidth_ = width;
    bufferHeight_ = height;
    previousMinX_ = width;
    previousMinY_ = height;
    previousMaxX_ = 0;
    previousMaxY_ = 0;
    frontBuffer_.assign(
        static_cast<std::size_t>(width) * height, 0u);
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (m_Window != nullptr) {
            ANativeWindow_setBuffersGeometry(
                m_Window,
                width,
                height,
                WINDOW_FORMAT_RGBA_8888);
        }
    }
    StartSubmitThread();
    return true;
}

void CPUGraphics::Setup() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendRendererName = "CPU";
    RefreshFontTexture();
}

void CPUGraphics::RefreshFontTexture() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        fontPixels_ = nullptr;
        fontWidth_ = 0;
        fontHeight_ = 0;
        return;
    }

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsAlpha8(&pixels, &width, &height);
    fontPixels_ = pixels;
    fontWidth_ = width;
    fontHeight_ = height;
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(pixels));
}

void CPUGraphics::PrepareFrame(bool resize) {
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (m_Window == nullptr) {
            return;
        }
        width = ANativeWindow_getWidth(m_Window);
        height = ANativeWindow_getHeight(m_Window);
    }
    if (!HasValidFrameSize(width, height)) {
        return;
    }

    const bool changed =
        resize || width != bufferWidth_ || height != bufferHeight_;
    if (changed) {
        StopSubmitThread();
        {
            std::lock_guard<std::mutex> lock(renderMutex_);
            bufferWidth_ = width;
            bufferHeight_ = height;
            previousMinX_ = width;
            previousMinY_ = height;
            previousMaxX_ = 0;
            previousMaxY_ = 0;
            frontBuffer_.assign(
                static_cast<std::size_t>(width) * height, 0u);
        }
        {
            std::lock_guard<std::mutex> lock(windowMutex_);
            if (m_Window != nullptr) {
                ANativeWindow_setBuffersGeometry(
                    m_Window,
                    width,
                    height,
                    WINDOW_FORMAT_RGBA_8888);
            }
        }
        StartSubmitThread();
    }

    m_Width = static_cast<float>(width);
    m_Height = static_cast<float>(height);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(m_Width, m_Height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    RefreshFontTexture();
}

void CPUGraphics::StartSubmitThread() {
    std::lock_guard<std::mutex> lock(threadMutex_);
    const std::int64_t now = NowMs();

    if (submitState_ != nullptr &&
        submitState_->running.load(std::memory_order_acquire)) {
        const std::int64_t lastProgress =
            submitState_->lastProgressMs.load(std::memory_order_acquire);
        const bool blocked =
            submitState_->insideWindowLock.load(std::memory_order_acquire) ||
            submitState_->insideWindowPost.load(std::memory_order_acquire);
        const bool stuck =
            blocked &&
            lastProgress > 0 &&
            now - lastProgress > kWindowWatchdogMs;
        if (!stuck) {
            return;
        }

        submitState_->running.store(false, std::memory_order_release);
        submitState_->condition.notify_all();
        if (submitThread_.joinable()) {
            submitThread_.detach();
        }
        submitState_.reset();
    } else if (submitThread_.joinable()) {
        if (submitState_ != nullptr &&
            !submitState_->exited.load(std::memory_order_acquire)) {
            submitThread_.detach();
        } else {
            submitThread_.join();
        }
        submitState_.reset();
    }

    std::shared_ptr<CpuSubmitState> state = MakeSubmitState();
    state->presentationRate = GetPresentationRateTracker();
    {
        std::lock_guard<std::mutex> windowLock(windowMutex_);
        state->window = m_Window;
        if (state->window != nullptr) {
            ANativeWindow_acquire(state->window);
        }
    }
    if (state->window == nullptr) {
        return;
    }

    state->exited.store(false, std::memory_order_release);
    state->running.store(true, std::memory_order_release);
    state->lastProgressMs.store(now, std::memory_order_release);
    submitState_ = state;
    submitThread_ = std::thread([state] {
        RunSubmitLoop(state);
    });
}

void CPUGraphics::StopSubmitThread() {
    std::lock_guard<std::mutex> lock(threadMutex_);
    std::shared_ptr<CpuSubmitState> state = submitState_;
    if (state != nullptr) {
        {
            std::lock_guard<std::mutex> stateLock(state->mutex);
            state->running.store(false, std::memory_order_release);
            state->hasWork = false;
        }
        state->condition.notify_all();
    }

    if (submitThread_.joinable()) {
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(kSubmitStopWaitMs);
        while (state != nullptr &&
               !state->exited.load(std::memory_order_acquire) &&
               Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (state == nullptr ||
            state->exited.load(std::memory_order_acquire)) {
            submitThread_.join();
        } else {
            submitThread_.detach();
        }
    }
    submitState_.reset();
}

void CPUGraphics::RenderBand(ImDrawData* drawData,
                             std::uint32_t* buffer,
                             int stride,
                             int bandMinY,
                             int dirtyMinX,
                             int dirtyMinY,
                             int bandMaxY,
                             int dirtyMaxX,
                             int dirtyMaxY) {
    if (drawData == nullptr ||
        buffer == nullptr ||
        stride <= 0 ||
        abortRaster_.load(std::memory_order_relaxed)) {
        return;
    }

    const ImVec2 displayPosition = drawData->DisplayPos;
    const ImVec2 scale = drawData->FramebufferScale;
    const ImVec2 whiteUv = ImGui::GetIO().Fonts->TexUvWhitePixel;

    for (int listIndex = 0;
         listIndex < drawData->CmdListsCount;
         ++listIndex) {
        if (abortRaster_.load(std::memory_order_relaxed)) {
            return;
        }
        const ImDrawList* drawList = drawData->CmdLists[listIndex];
        if (drawList == nullptr) {
            continue;
        }

        for (int commandIndex = 0;
             commandIndex < drawList->CmdBuffer.Size;
             ++commandIndex) {
            if (abortRaster_.load(std::memory_order_relaxed)) {
                return;
            }
            const ImDrawCmd& command =
                drawList->CmdBuffer[commandIndex];
            if (command.UserCallback != nullptr) {
                continue;
            }

            RasterClip clip{
                std::max(
                    0,
                    std::max(
                        dirtyMinX,
                        static_cast<int>(
                            (command.ClipRect.x - displayPosition.x) *
                            scale.x))),
                std::max(
                    bandMinY,
                    std::max(
                        dirtyMinY,
                        static_cast<int>(
                            (command.ClipRect.y - displayPosition.y) *
                            scale.y))),
                std::min(
                    bufferWidth_,
                    std::min(
                        dirtyMaxX,
                        static_cast<int>(
                            (command.ClipRect.z - displayPosition.x) *
                            scale.x))),
                std::min(
                    bandMaxY,
                    std::min(
                        dirtyMaxY,
                        static_cast<int>(
                            (command.ClipRect.w - displayPosition.y) *
                            scale.y))),
            };
            if (clip.maxX <= clip.minX ||
                clip.maxY <= clip.minY) {
                continue;
            }

            TextureView texture;
            const ImTextureID textureId = command.GetTexID();
            if (textureId == nullptr ||
                textureId ==
                    reinterpret_cast<ImTextureID>(
                        const_cast<std::uint8_t*>(fontPixels_))) {
                if (fontPixels_ == nullptr ||
                    fontWidth_ <= 0 ||
                    fontHeight_ <= 0) {
                    continue;
                }
                texture.kind = TextureKind::FontAlpha;
                texture.alpha = fontPixels_;
                texture.width = fontWidth_;
                texture.height = fontHeight_;
            } else {
                const CpuTextureData* textureData = nullptr;
                for (const BaseTexData* candidate : m_Textures) {
                    if (candidate != nullptr &&
                        candidate->DS == textureId) {
                        textureData =
                            static_cast<const CpuTextureData*>(candidate);
                        break;
                    }
                }
                if (textureData == nullptr ||
                    textureData->Width <= 0 ||
                    textureData->Height <= 0 ||
                    textureData->pixels.empty()) {
                    continue;
                }
                texture.kind = TextureKind::Rgba;
                texture.rgba = textureData->pixels.data();
                texture.width = textureData->Width;
                texture.height = textureData->Height;
            }

            if (command.IdxOffset >
                static_cast<unsigned int>(drawList->IdxBuffer.Size)) {
                continue;
            }
            const unsigned int availableIndices =
                static_cast<unsigned int>(drawList->IdxBuffer.Size) -
                command.IdxOffset;
            const unsigned int elementCount =
                std::min(command.ElemCount, availableIndices);
            if (elementCount < 3 || drawList->IdxBuffer.Data == nullptr) {
                continue;
            }
            const ImDrawIdx* indices =
                drawList->IdxBuffer.Data + command.IdxOffset;

            auto readVertex =
                [&](unsigned int index, RasterVertex& output) -> bool {
                const std::size_t resolved =
                    static_cast<std::size_t>(command.VtxOffset) + index;
                if (resolved >=
                    static_cast<std::size_t>(
                        drawList->VtxBuffer.Size)) {
                    return false;
                }
                const ImDrawVert& vertex =
                    drawList->VtxBuffer[
                        static_cast<int>(resolved)];
                output = {
                    (vertex.pos.x - displayPosition.x) * scale.x,
                    (vertex.pos.y - displayPosition.y) * scale.y,
                    vertex.uv.x,
                    vertex.uv.y,
                    vertex.col,
                };
                return true;
            };

            unsigned int indexPosition = 0;
            while (indexPosition + 2 < elementCount) {
                if (indexPosition + 5 < elementCount &&
                    indices[indexPosition] ==
                        indices[indexPosition + 3] &&
                    indices[indexPosition + 2] ==
                        indices[indexPosition + 4]) {
                    RasterVertex first{};
                    RasterVertex second{};
                    RasterVertex third{};
                    RasterVertex fourth{};
                    if (readVertex(indices[indexPosition], first) &&
                        readVertex(indices[indexPosition + 1], second) &&
                        readVertex(indices[indexPosition + 2], third) &&
                        readVertex(indices[indexPosition + 5], fourth) &&
                        TryRasterQuad(
                            first,
                            second,
                            third,
                            fourth,
                            texture,
                            clip,
                            buffer,
                            stride,
                            whiteUv.x,
                            whiteUv.y,
                            abortRaster_)) {
                        indexPosition += 6;
                        continue;
                    }
                }

                RasterVertex first{};
                RasterVertex second{};
                RasterVertex third{};
                if (readVertex(indices[indexPosition], first) &&
                    readVertex(indices[indexPosition + 1], second) &&
                    readVertex(indices[indexPosition + 2], third)) {
                    RasterTriangle(
                        first,
                        second,
                        third,
                        texture,
                        clip,
                        buffer,
                        stride,
                        whiteUv.x,
                        whiteUv.y,
                        abortRaster_);
                }
                indexPosition += 3;
            }
        }
    }
}

void CPUGraphics::Render(ImDrawData* drawData) {
    if (drawData == nullptr || !drawData->Valid) {
        return;
    }
    StartSubmitThread();

    std::shared_ptr<CpuSubmitState> state;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        state = submitState_;
    }
    if (state == nullptr ||
        !state->running.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->hasWork) {
            return;
        }
    }

    std::lock_guard<std::mutex> renderLock(renderMutex_);
    if (rasterPool_ == nullptr) {
        rasterPool_ = std::make_unique<CpuRasterPool>();
    }
    if (bufferWidth_ <= 0 || bufferHeight_ <= 0) {
        return;
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(bufferWidth_) * bufferHeight_;
    if (frontBuffer_.size() != pixelCount) {
        frontBuffer_.assign(pixelCount, 0u);
    }

    const ImVec2 displayPosition = drawData->DisplayPos;
    const ImVec2 scale = drawData->FramebufferScale;
    int minX = bufferWidth_;
    int minY = bufferHeight_;
    int maxX = 0;
    int maxY = 0;
    for (int listIndex = 0;
         listIndex < drawData->CmdListsCount;
         ++listIndex) {
        const ImDrawList* drawList = drawData->CmdLists[listIndex];
        if (drawList == nullptr) {
            continue;
        }
        for (int commandIndex = 0;
             commandIndex < drawList->CmdBuffer.Size;
             ++commandIndex) {
            const ImDrawCmd& command =
                drawList->CmdBuffer[commandIndex];
            if (command.UserCallback != nullptr) {
                continue;
            }
            const int commandMinX = static_cast<int>(
                (command.ClipRect.x - displayPosition.x) * scale.x);
            const int commandMinY = static_cast<int>(
                (command.ClipRect.y - displayPosition.y) * scale.y);
            const int commandMaxX = static_cast<int>(
                (command.ClipRect.z - displayPosition.x) * scale.x);
            const int commandMaxY = static_cast<int>(
                (command.ClipRect.w - displayPosition.y) * scale.y);
            if (commandMaxX <= commandMinX ||
                commandMaxY <= commandMinY) {
                continue;
            }
            minX = std::min(minX, commandMinX);
            minY = std::min(minY, commandMinY);
            maxX = std::max(maxX, commandMaxX);
            maxY = std::max(maxY, commandMaxY);
        }
    }

    const bool hasCurrentDirty = maxX > minX && maxY > minY;
    if (!hasCurrentDirty) {
        minX = previousMinX_;
        minY = previousMinY_;
        maxX = previousMaxX_;
        maxY = previousMaxY_;
        previousMinX_ = bufferWidth_;
        previousMinY_ = bufferHeight_;
        previousMaxX_ = 0;
        previousMaxY_ = 0;
    } else {
        if (previousMaxX_ > previousMinX_ &&
            previousMaxY_ > previousMinY_) {
            minX = std::min(minX, previousMinX_);
            minY = std::min(minY, previousMinY_);
            maxX = std::max(maxX, previousMaxX_);
            maxY = std::max(maxY, previousMaxY_);
        }
        previousMinX_ = minX;
        previousMinY_ = minY;
        previousMaxX_ = maxX;
        previousMaxY_ = maxY;
    }

    minX = std::clamp(minX, 0, bufferWidth_);
    minY = std::clamp(minY, 0, bufferHeight_);
    maxX = std::clamp(maxX, 0, bufferWidth_);
    maxY = std::clamp(maxY, 0, bufferHeight_);
    if (maxX <= minX || maxY <= minY) {
        return;
    }

    if (!hasCurrentDirty) {
        for (int y = minY; y < maxY; ++y) {
            std::memset(
                frontBuffer_.data() +
                    static_cast<std::size_t>(y) * bufferWidth_ +
                    minX,
                0,
                static_cast<std::size_t>(maxX - minX) *
                    sizeof(std::uint32_t));
        }
    } else {
        abortRaster_.store(false, std::memory_order_release);
        const int bandHeight =
            bufferHeight_ / CpuRasterPool::kWorkerCount;
        auto makeTask =
            [=](int bandMinY, int bandMaxY) {
                return [=] {
                    const int clearMinY =
                        std::max(bandMinY, minY);
                    const int clearMaxY =
                        std::min(bandMaxY, maxY);
                    for (int y = clearMinY; y < clearMaxY; ++y) {
                        if ((y & 15) == 0 &&
                            abortRaster_.load(std::memory_order_relaxed)) {
                            return;
                        }
                        std::memset(
                            frontBuffer_.data() +
                                static_cast<std::size_t>(y) *
                                    bufferWidth_ +
                                minX,
                            0,
                            static_cast<std::size_t>(maxX - minX) *
                                sizeof(std::uint32_t));
                    }
                    RenderBand(
                        drawData,
                        frontBuffer_.data(),
                        bufferWidth_,
                        bandMinY,
                        minX,
                        minY,
                        bandMaxY,
                        maxX,
                        maxY);
                };
            };
        rasterPool_->Run(
            makeTask(0, bandHeight),
            makeTask(bandHeight, bandHeight * 2),
            makeTask(bandHeight * 2, bandHeight * 3),
            makeTask(bandHeight * 3, bufferHeight_));
        if (!rasterPool_->WaitFor(
                std::chrono::milliseconds(kRasterWaitMs))) {
            abortRaster_.store(true, std::memory_order_release);
            if (!rasterPool_->WaitFor(
                    std::chrono::milliseconds(kRasterAbortWaitMs))) {
                while (!rasterPool_->WaitFor(
                    std::chrono::milliseconds(50))) {
                }
            }
            std::fill(frontBuffer_.begin(), frontBuffer_.end(), 0u);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->running.load(std::memory_order_acquire)) {
            return;
        }
        state->buffer.swap(frontBuffer_);
        state->bufferWidth = bufferWidth_;
        state->bufferHeight = bufferHeight_;
        state->dirtyMinX = minX;
        state->dirtyMinY = minY;
        state->dirtyMaxX = maxX;
        state->dirtyMaxY = maxY;
        state->hasWork = true;
    }
    if (frontBuffer_.size() != pixelCount) {
        frontBuffer_.assign(pixelCount, 0u);
    }
    state->condition.notify_one();
}

void CPUGraphics::PrepareShutdown() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.Fonts != nullptr &&
            io.Fonts->TexID ==
                reinterpret_cast<ImTextureID>(
                    const_cast<std::uint8_t*>(fontPixels_))) {
            io.Fonts->SetTexID(nullptr);
        }
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendRendererName = nullptr;
    }
    fontPixels_ = nullptr;
    fontWidth_ = 0;
    fontHeight_ = 0;
}

void CPUGraphics::Cleanup() {
    StopSubmitThread();
    abortRaster_.store(true, std::memory_order_release);
    rasterPool_.reset();
    frontBuffer_.clear();
    frontBuffer_.shrink_to_fit();
}

BaseTexData* CPUGraphics::LoadTexture(BaseTexData* texture,
                                     void* pixelData) {
    if (texture == nullptr ||
        pixelData == nullptr ||
        texture->Width <= 0 ||
        texture->Height <= 0) {
        return nullptr;
    }
    const std::size_t width =
        static_cast<std::size_t>(texture->Width);
    const std::size_t height =
        static_cast<std::size_t>(texture->Height);
    if (width >
        std::numeric_limits<std::size_t>::max() / height) {
        return nullptr;
    }

    auto* result = new CpuTextureData();
    result->Width = texture->Width;
    result->Height = texture->Height;
    result->Channels = 4;
    const std::size_t pixelCount = width * height;
    result->pixels.resize(pixelCount);
    std::memcpy(
        result->pixels.data(),
        pixelData,
        pixelCount * sizeof(std::uint32_t));
    result->DS = result;
    return result;
}

void CPUGraphics::RemoveTexture(BaseTexData* texture) {
    delete static_cast<CpuTextureData*>(texture);
}
