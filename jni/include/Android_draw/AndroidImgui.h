#ifndef ANDROIDIMGUI_ANDROIDIMGUI_H
#define ANDROIDIMGUI_ANDROIDIMGUI_H

#include "render/PresentationRateTracker.h"

#include <memory>
#include <functional>
#include <vector>


struct ANativeWindow;
struct ImDrawData;

struct BaseTexData {
    void *DS = nullptr;
    int Width = 0;
    int Height = 0;
    int Channels = 0;

    BaseTexData() = default;

    BaseTexData(BaseTexData &other) = default;
};

class AndroidImgui {
protected:
    ANativeWindow *m_Window = nullptr;
    float m_Width = 0.0f;
    float m_Height = 0.0f;

    std::vector<BaseTexData *> m_Textures;
    std::shared_ptr<lengjing::render::PresentationRateTracker>
        m_PresentationRate =
            std::make_shared<lengjing::render::PresentationRateTracker>();

    std::shared_ptr<lengjing::render::PresentationRateTracker>
    GetPresentationRateTracker() const;
public:
    char RenderName[16];

public:
    AndroidImgui() = default;

    virtual ~AndroidImgui() = default;

    bool Init_Render(ANativeWindow *window, float width, float height);

    void NewFrame(bool resize = false);

    void EndFrame();

    void Shutdown();

    float PresentedFrameRate() const;

    virtual bool ConsumeSurfaceRecoveryRequest() {
        return false;
    }

    BaseTexData *LoadTextureFromFile(const char *filepath);

    BaseTexData *LoadTextureFromMemory(void *data, int len);

    void DeleteTexture(BaseTexData *tex_data);
    
private:
    bool m_Initialized = false;

    BaseTexData *LoadTextureData(const std::function<unsigned char *(BaseTexData *)> &loadFunc);

    virtual bool Create() = 0;

    virtual void Setup() = 0;

    virtual void PrepareFrame(bool resize) = 0;

    virtual void Render(ImDrawData *drawData) = 0;

    virtual void PrepareShutdown() = 0;

    virtual void Cleanup() = 0;

    virtual BaseTexData *LoadTexture(BaseTexData *tex_data, void *pixel_data) = 0;

    virtual void RemoveTexture(BaseTexData *tex_data) = 0;
};


#endif //ANDROIDIMGUI_ANDROIDIMGUI_H
