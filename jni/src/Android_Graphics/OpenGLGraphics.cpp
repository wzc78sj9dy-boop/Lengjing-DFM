#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <cstdint>
#include <cstdio>

#include "OpenGLGraphics.h"
#include "imgui_impl_opengl3.h"

bool OpenGLGraphics::Create() {
    std::snprintf(RenderName, sizeof(RenderName), "OpenGL");
    m_RenderingDisabled = false;
    m_ImGuiBackendInitialized = false;
    m_SurfaceRecoveryRequested.store(false, std::memory_order_release);
    if (m_Window == nullptr)
        return false;

    const EGLint egl_attributes[] = {EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
                                     EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                     EGL_WINDOW_BIT, EGL_NONE};

    m_EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_EglDisplay == EGL_NO_DISPLAY ||
        eglInitialize(m_EglDisplay, nullptr, nullptr) != EGL_TRUE) {
        Cleanup();
        return false;
    }
    EGLint num_configs = 0;
    if (eglChooseConfig(m_EglDisplay, egl_attributes, nullptr, 0, &num_configs) != EGL_TRUE ||
        num_configs <= 0 ||
        eglChooseConfig(m_EglDisplay, egl_attributes, &m_EglConfig, 1, &num_configs) != EGL_TRUE) {
        Cleanup();
        return false;
    }
    EGLint egl_format = 0;
    if (eglGetConfigAttrib(m_EglDisplay, m_EglConfig, EGL_NATIVE_VISUAL_ID,
                           &egl_format) != EGL_TRUE ||
        ANativeWindow_setBuffersGeometry(m_Window, 0, 0, egl_format) != 0) {
        Cleanup();
        return false;
    }

    const EGLint egl_context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    m_EglContext = eglCreateContext(m_EglDisplay, m_EglConfig, EGL_NO_CONTEXT,
                                    egl_context_attributes);
    if (m_EglContext == EGL_NO_CONTEXT) {
        Cleanup();
        return false;
    }
    m_EglSurface = eglCreateWindowSurface(m_EglDisplay, m_EglConfig, m_Window, nullptr);
    if (m_EglSurface == EGL_NO_SURFACE ||
        eglMakeCurrent(m_EglDisplay, m_EglSurface, m_EglSurface, m_EglContext) != EGL_TRUE) {
        Cleanup();
        return false;
    }
    if (eglSwapInterval(m_EglDisplay, 1) != EGL_TRUE) {
        fprintf(stderr, "[opengl] eglSwapInterval(1) failed: EGL error = 0x%x\n",
                eglGetError());
    }
    glClearColor(0.0, 0.0, 0.0, 0.0);
    return true;
}

bool OpenGLGraphics::Setup() {
    if (m_ImGuiBackendInitialized)
        return true;
    if (!EnsureCurrentContext())
        return false;
    m_ImGuiBackendInitialized =
        ImGui_ImplOpenGL3_Init("#version 300 es");
    return m_ImGuiBackendInitialized;
}

void OpenGLGraphics::PrepareFrame(bool resize) {
    (void)resize;
    if (m_RenderingDisabled || !m_ImGuiBackendInitialized)
        return;
    ImGui_ImplOpenGL3_NewFrame();
}

void OpenGLGraphics::Render(ImDrawData *drawData) {
    if (m_RenderingDisabled || !m_ImGuiBackendInitialized ||
        drawData == nullptr)
        return;

    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
    if (eglSwapBuffers(m_EglDisplay, m_EglSurface) == EGL_TRUE) {
        RecordPresentedFrame();
        return;
    }

    const EGLint error = eglGetError();
    switch (error) {
        case EGL_BAD_SURFACE:
        case EGL_BAD_NATIVE_WINDOW:
            RequestSurfaceRecovery("swap surface lost", error);
            break;
        case EGL_CONTEXT_LOST:
            RequestSurfaceRecovery("context lost", error);
            break;
        default:
            RequestSurfaceRecovery("eglSwapBuffers", error);
            break;
    }
}

bool OpenGLGraphics::EnsureCurrentContext() {
    if (m_RenderingDisabled)
        return false;
    if (m_EglDisplay == EGL_NO_DISPLAY ||
        m_EglSurface == EGL_NO_SURFACE ||
        m_EglContext == EGL_NO_CONTEXT) {
        RequestSurfaceRecovery("invalid EGL state", EGL_BAD_CONTEXT);
        return false;
    }
    if (eglGetCurrentDisplay() == m_EglDisplay &&
        eglGetCurrentContext() == m_EglContext &&
        eglGetCurrentSurface(EGL_DRAW) == m_EglSurface) {
        return true;
    }
    if (eglMakeCurrent(
            m_EglDisplay, m_EglSurface, m_EglSurface, m_EglContext) ==
        EGL_TRUE) {
        return true;
    }
    RequestSurfaceRecovery("eglMakeCurrent", eglGetError());
    return false;
}

void OpenGLGraphics::RequestSurfaceRecovery(
    const char* operation,
    EGLint error) {
    if (!m_SurfaceRecoveryRequested.exchange(
            true, std::memory_order_acq_rel)) {
        fprintf(
            stderr,
            "[opengl] %s failed: EGL error = 0x%x; requesting surface recovery\n",
            operation,
            error);
    }
    m_RenderingDisabled = true;
}

bool OpenGLGraphics::ConsumeSurfaceRecoveryRequest() {
    if (!m_SurfaceRecoveryRequested.load(std::memory_order_acquire))
        return false;
    return m_SurfaceRecoveryRequested.exchange(
        false, std::memory_order_acq_rel);
}

void OpenGLGraphics::PrepareShutdown() {
    if (!m_ImGuiBackendInitialized)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    m_ImGuiBackendInitialized = false;
}

void OpenGLGraphics::Cleanup() {
    if (m_EglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_EglSurface != EGL_NO_SURFACE)
            eglDestroySurface(m_EglDisplay, m_EglSurface);
        if (m_EglContext != EGL_NO_CONTEXT)
            eglDestroyContext(m_EglDisplay, m_EglContext);
        eglTerminate(m_EglDisplay);
    }
    m_EglDisplay = EGL_NO_DISPLAY;
    m_EglSurface = EGL_NO_SURFACE;
    m_EglContext = EGL_NO_CONTEXT;
    m_EglConfig = nullptr;
    m_RenderingDisabled = false;
    m_ImGuiBackendInitialized = false;
    m_SurfaceRecoveryRequested.store(false, std::memory_order_release);
}

BaseTexData *OpenGLGraphics::LoadTexture(BaseTexData *texture,
                                        void *pixelData) {
    if (texture == nullptr || pixelData == nullptr ||
        texture->Width <= 0 || texture->Height <= 0 ||
        m_RenderingDisabled || !m_ImGuiBackendInitialized ||
        !EnsureCurrentContext())
        return nullptr;

    auto textureData = new OpenGlTextureData();
    textureData->Width = texture->Width;
    textureData->Height = texture->Height;
    textureData->Channels = texture->Channels;

    GLuint textureId = 0;
    glGenTextures(1, &textureId);
    if (textureId == 0) {
        delete textureData;
        return nullptr;
    }
    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureData->Width,
                 textureData->Height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixelData);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &textureId);
        delete textureData;
        return nullptr;
    }
    textureData->DS = reinterpret_cast<void*>(
        static_cast<std::intptr_t>(textureId));
    return textureData;
}

void OpenGLGraphics::RemoveTexture(BaseTexData *texture) {
    auto* textureData = static_cast<OpenGlTextureData*>(texture);
    if (textureData == nullptr) {
        return;
    }
    auto textureId = static_cast<GLuint>(
        reinterpret_cast<std::intptr_t>(textureData->DS));
    if (textureId != 0 && EnsureCurrentContext())
        glDeleteTextures(1, &textureId);
    delete textureData;
}
