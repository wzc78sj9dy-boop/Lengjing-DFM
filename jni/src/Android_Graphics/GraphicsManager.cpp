#include "GraphicsManager.h"
#include "CPUGraphics.h"

std::unique_ptr<AndroidImgui> GraphicsManager::getGraphicsInterface(GraphicsAPI api) {
    switch (api) {
        case CPU:
            return std::make_unique<CPUGraphics>();
    }
    return nullptr;
}
