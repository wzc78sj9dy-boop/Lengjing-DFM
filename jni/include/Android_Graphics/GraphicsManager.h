#ifndef ANDROIDIMGUI_GRAPHICSMANAGER_H
#define ANDROIDIMGUI_GRAPHICSMANAGER_H

#include "Android_draw/AndroidImgui.h"
#include <memory>

class GraphicsManager {
public:
    enum GraphicsAPI {
        CPU = 0,
        VULKAN = 1,
        OPENGL = 2,
    };

    static std::unique_ptr<AndroidImgui> getGraphicsInterface(GraphicsAPI api);
};

#endif
