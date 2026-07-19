#include "GraphicsManager.h"
#include "CPUGraphics.h"
#include "OpenGLGraphics.h"
#include "VulkanGraphics.h"

std::unique_ptr<AndroidImgui> GraphicsManager::getGraphicsInterface(GraphicsAPI api) {
    switch (api) {
        case CPU:
            return std::make_unique<CPUGraphics>();
        case VULKAN:
            return std::make_unique<VulkanGraphics>();
        case OPENGL:
            return std::make_unique<OpenGLGraphics>();
    }
    return nullptr;
}
