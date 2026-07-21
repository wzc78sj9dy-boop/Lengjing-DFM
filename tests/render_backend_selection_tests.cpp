#include "app/RenderBackendSelection.h"

#include "test_support.h"

void RunRenderBackendSelectionTests() {
    REQUIRE(
        lengjing::ui::UiModel{}.system.renderBackend ==
        lengjing::ui::RenderBackend::Cpu);
    REQUIRE(
        lengjing::app::GraphicsApiForRenderBackend(
            lengjing::ui::RenderBackend::Cpu) ==
        GraphicsManager::CPU);
    REQUIRE(
        lengjing::app::GraphicsApiForRenderBackend(
            lengjing::ui::RenderBackend::Vulkan) ==
        GraphicsManager::VULKAN);
    REQUIRE(
        lengjing::app::GraphicsApiForRenderBackend(
            lengjing::ui::RenderBackend::OpenGl) ==
        GraphicsManager::OPENGL);
    REQUIRE(
        lengjing::app::GraphicsApiForRenderBackend(
            static_cast<lengjing::ui::RenderBackend>(255)) ==
        GraphicsManager::CPU);
}
