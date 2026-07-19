#pragma once

#include "Android_Graphics/GraphicsManager.h"
#include "ui/UiModel.h"

namespace lengjing::app {

constexpr ui::RenderBackend NormalizeRenderBackend(
    ui::RenderBackend backend) noexcept {
    switch (backend) {
        case ui::RenderBackend::Cpu:
        case ui::RenderBackend::Vulkan:
        case ui::RenderBackend::OpenGl:
            return backend;
    }
    return ui::RenderBackend::Cpu;
}

constexpr GraphicsManager::GraphicsAPI GraphicsApiForRenderBackend(
    ui::RenderBackend backend) noexcept {
    switch (NormalizeRenderBackend(backend)) {
        case ui::RenderBackend::Vulkan:
            return GraphicsManager::VULKAN;
        case ui::RenderBackend::OpenGl:
            return GraphicsManager::OPENGL;
        case ui::RenderBackend::Cpu:
            return GraphicsManager::CPU;
    }
    return GraphicsManager::CPU;
}

}  // namespace lengjing::app
