#pragma once

/// @file Framebuffer.hpp
/// @brief RAII OpenGL framebuffer object with optional MSAA support.
///
/// Non-MSAA framebuffers use a GL_TEXTURE_2D colour attachment so the result
/// can be sampled in a subsequent post-process pass (e.g. FXAA).
/// MSAA framebuffers use GL_RENDERBUFFER attachments and must be resolved via
/// BlitTo() or BlitToScreen() before sampling.

#include <glad/glad.h>

namespace Assisi::Render::OpenGL
{

/// @brief Owns a framebuffer object, colour attachment, and depth renderbuffer.
///
/// Move-only; copying is disabled.
class Framebuffer
{
  public:
    Framebuffer() = default;

    /// @brief Creates and validates the framebuffer immediately.
    /// @param width   Framebuffer width in pixels.
    /// @param height  Framebuffer height in pixels.
    /// @param samples Sample count: 1 = no MSAA, 2/4/8 = MSAA.
    explicit Framebuffer(int width, int height, int samples = 1) { Create(width, height, samples); }

    ~Framebuffer() { Destroy(); }

    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;

    Framebuffer(Framebuffer &&other) noexcept
        : _fbo(other._fbo), _colorTexture(other._colorTexture),
          _colorRenderbuffer(other._colorRenderbuffer), _depthRenderbuffer(other._depthRenderbuffer),
          _width(other._width), _height(other._height), _samples(other._samples)
    {
        other._fbo = other._colorTexture = other._colorRenderbuffer = other._depthRenderbuffer = 0;
        other._width = other._height = other._samples = 0;
    }

    Framebuffer &operator=(Framebuffer &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            _fbo                 = other._fbo;
            _colorTexture        = other._colorTexture;
            _colorRenderbuffer   = other._colorRenderbuffer;
            _depthRenderbuffer   = other._depthRenderbuffer;
            _width               = other._width;
            _height              = other._height;
            _samples             = other._samples;
            other._fbo = other._colorTexture = other._colorRenderbuffer = other._depthRenderbuffer = 0;
            other._width = other._height = other._samples = 0;
        }
        return *this;
    }

    /// @brief Binds this framebuffer as the active render target.
    void Bind() const { glBindFramebuffer(GL_FRAMEBUFFER, _fbo); }

    /// @brief Binds the default (screen) framebuffer.
    static void BindDefault() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    /// @brief Binds this framebuffer's colour texture to a texture unit (non-MSAA only).
    void BindColorTexture(unsigned int unit) const
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, _colorTexture);
    }

    /// @brief Resolves this MSAA framebuffer into @p target via glBlitFramebuffer.
    void BlitTo(const Framebuffer &target) const
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target._fbo);
        glBlitFramebuffer(0, 0, _width, _height, 0, 0, target._width, target._height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// @brief Resolves (or blits) this framebuffer to the default screen framebuffer.
    void BlitToScreen(int screenW, int screenH) const
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, _width, _height, 0, 0, screenW, screenH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /// @brief Destroys and recreates the framebuffer at the new size, keeping the same sample count.
    void Resize(int width, int height) { Destroy(); Create(width, height, _samples); }

    bool IsValid()  const { return _fbo != 0; }
    bool IsMsaa()   const { return _samples > 1; }
    int  Width()    const { return _width; }
    int  Height()   const { return _height; }
    int  Samples()  const { return _samples; }

  private:
    void Create(int width, int height, int samples)
    {
        _width   = width;
        _height  = height;
        _samples = samples;

        glGenFramebuffers(1, &_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, _fbo);

        if (samples > 1)
        {
            // MSAA: colour and depth as multisampled renderbuffers (not sampleable directly)
            glGenRenderbuffers(1, &_colorRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderbuffer);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGB8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, _colorRenderbuffer);

            glGenRenderbuffers(1, &_depthRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderbuffer);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, _depthRenderbuffer);
        }
        else
        {
            // Non-MSAA: colour as texture (sampleable in post-process passes)
            glGenTextures(1, &_colorTexture);
            glBindTexture(GL_TEXTURE_2D, _colorTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _colorTexture, 0);

            glGenRenderbuffers(1, &_depthRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, _depthRenderbuffer);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Destroy()
    {
        if (_colorTexture != 0)      { glDeleteTextures(1, &_colorTexture);       _colorTexture = 0; }
        if (_colorRenderbuffer != 0) { glDeleteRenderbuffers(1, &_colorRenderbuffer); _colorRenderbuffer = 0; }
        if (_depthRenderbuffer != 0) { glDeleteRenderbuffers(1, &_depthRenderbuffer); _depthRenderbuffer = 0; }
        if (_fbo != 0)               { glDeleteFramebuffers(1, &_fbo);            _fbo = 0; }
        _width = _height = _samples = 0;
    }

    unsigned int _fbo               = 0;
    unsigned int _colorTexture      = 0; ///< GL_TEXTURE_2D (non-MSAA only).
    unsigned int _colorRenderbuffer = 0; ///< GL_RENDERBUFFER (MSAA only).
    unsigned int _depthRenderbuffer = 0;
    int          _width             = 0;
    int          _height            = 0;
    int          _samples           = 1;
};

} // namespace Assisi::Render::OpenGL