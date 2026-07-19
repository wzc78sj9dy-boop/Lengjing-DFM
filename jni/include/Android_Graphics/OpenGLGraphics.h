#ifndef ANDROIDIMGUI_OPENGLGRAPHICS_H
#define ANDROIDIMGUI_OPENGLGRAPHICS_H

#include <EGL/egl.h>

#include <atomic>

#include "AndroidImgui.h"

class OpenGLGraphics final : public AndroidImgui {
public:
    bool ConsumeSurfaceRecoveryRequest() override;

private:
    struct OpenGlTextureData final : BaseTexData {
    };

    EGLDisplay m_EglDisplay = EGL_NO_DISPLAY;
    EGLSurface m_EglSurface = EGL_NO_SURFACE;
    EGLContext m_EglContext = EGL_NO_CONTEXT;
    EGLConfig m_EglConfig = nullptr;
    bool m_RenderingDisabled = false;
    bool m_ImGuiBackendInitialized = false;
    std::atomic<bool> m_SurfaceRecoveryRequested{false};

    bool EnsureCurrentContext();
    void RequestSurfaceRecovery(const char* operation, EGLint error);

    bool Create() override;

    bool Setup() override;

    void PrepareFrame(bool resize) override;

    void Render(ImDrawData *drawData) override;

    void PrepareShutdown() override;

    void Cleanup() override;

    BaseTexData *LoadTexture(BaseTexData *texture, void *pixelData) override;

    void RemoveTexture(BaseTexData *texture) override;
};

#endif
