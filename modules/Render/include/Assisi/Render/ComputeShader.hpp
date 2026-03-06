/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ComputeShader.hpp
/// @brief Single-stage compute shader wrapper with asset-system integration.
///
/// `ComputeShader` compiles a compute stage sourced through `AssetSystem`,
/// links it into an OpenGL program, and exposes typed uniform setters.
/// Move-only; copying is disabled.

#include <glad/glad.h>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Math/GLM.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace Assisi::Render
{

/// @brief Owns a linked OpenGL compute shader program.
class ComputeShader
{
  public:
    ComputeShader() = default;

    /// @brief Builds a compute program from a virtual asset path.
    explicit ComputeShader(std::string_view computeVPath)
    {
        (void)LoadFromAssets(computeVPath);
    }

    /// @brief (Re)loads, compiles, and links the compute shader from a virtual asset path.
    std::expected<void, Assisi::Core::AssetError> LoadFromAssets(std::string_view computeVPath) noexcept
    {
        Destroy();

        auto src = Assisi::Core::AssetSystem::ReadText(computeVPath);
        if (!src)
            return std::unexpected(src.error());

        const char *csrc = src->c_str();
        const unsigned int cs = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(cs, 1, &csrc, nullptr);
        glCompileShader(cs);

        int ok = 0;
        glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char buf[1024];
            glGetShaderInfoLog(cs, 1024, nullptr, buf);
            Assisi::Core::Log::Error("ComputeShader: Compilation failed for '{}'\n{}", computeVPath, buf);
            glDeleteShader(cs);
            return {};
        }

        _program = glCreateProgram();
        glAttachShader(_program, cs);
        glLinkProgram(_program);
        glDeleteShader(cs);

        glGetProgramiv(_program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char buf[1024];
            glGetProgramInfoLog(_program, 1024, nullptr, buf);
            Assisi::Core::Log::Error("ComputeShader: Linking failed for '{}'\n{}", computeVPath, buf);
            Destroy();
        }

        return {};
    }

    void         Use()     const { glUseProgram(_program); }
    bool         IsValid() const { return _program != 0u; }
    unsigned int ProgramIdentifier() const { return _program; }

    /// @brief Dispatches the compute shader with the given workgroup counts.
    void Dispatch(unsigned int x, unsigned int y, unsigned int z) const
    {
        glUseProgram(_program);
        glDispatchCompute(x, y, z);
    }

    /// @name Uniform setters (program must be bound via Use() first, or call Dispatch which binds it)
    ///@{
    void SetInt(const std::string &name, int v) const
    {
        glUniform1i(glGetUniformLocation(_program, name.c_str()), v);
    }

    void SetUInt(const std::string &name, unsigned int v) const
    {
        glUniform1ui(glGetUniformLocation(_program, name.c_str()), v);
    }

    void SetFloat(const std::string &name, float v) const
    {
        glUniform1f(glGetUniformLocation(_program, name.c_str()), v);
    }

    void SetVec2(const std::string &name, float x, float y) const
    {
        glUniform2f(glGetUniformLocation(_program, name.c_str()), x, y);
    }

    void SetUVec3(const std::string &name, unsigned int x, unsigned int y, unsigned int z) const
    {
        glUniform3ui(glGetUniformLocation(_program, name.c_str()), x, y, z);
    }

    void SetMat4(const std::string &name, const glm::mat4 &v) const
    {
        glUniformMatrix4fv(glGetUniformLocation(_program, name.c_str()), 1, GL_FALSE, &v[0][0]);
    }
    ///@}

    ~ComputeShader() { Destroy(); }

    ComputeShader(const ComputeShader &) = delete;
    ComputeShader &operator=(const ComputeShader &) = delete;

    ComputeShader(ComputeShader &&o) noexcept { *this = std::move(o); }
    ComputeShader &operator=(ComputeShader &&o) noexcept
    {
        if (this != &o)
        {
            Destroy();
            _program   = o._program;
            o._program = 0u;
        }
        return *this;
    }

  private:
    void Destroy() noexcept
    {
        if (_program != 0u)
        {
            glDeleteProgram(_program);
            _program = 0u;
        }
    }

    unsigned int _program = 0u;
};

} // namespace Assisi::Render