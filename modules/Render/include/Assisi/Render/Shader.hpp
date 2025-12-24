/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

#ifndef ASSISI_RENDER_SHADER_HPP
#define ASSISI_RENDER_SHADER_HPP

#include <glad/glad.h>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Math/GLM.hpp>

#include <expected>
#include <iostream>
#include <string>
#include <string_view>

namespace Assisi::Render
{
class Shader
{
  public:
    Shader() = default;

    /**
     * @brief Builds a shader program from virtual asset paths.
     *
     * @param vertexVPath Virtual path under the asset root (e.g., "shaders/basic.vert").
     * @param fragmentVPath Virtual path under the asset root (e.g., "shaders/basic.frag").
     *
     * @warning Preconditions:
     *  - Assisi::Core::AssetSystem is initialized.
     *  - Files exist and contain valid GLSL for the active context/profile.
     */
    explicit Shader(std::string_view vertexVPath, std::string_view fragmentVPath)
    {
        (void)LoadFromAssets(vertexVPath, fragmentVPath);
    }

    /**
     * @brief Loads/compiles/links from virtual asset paths.
     *
     * @return std::expected<void, Assisi::Core::AssetError>
     *   - Success: the program is ready.
     *   - Failure: an AssetError if reading/resolving fails. GLSL failures are logged.
     */
    std::expected<void, Assisi::Core::AssetError> LoadFromAssets(std::string_view vertexVPath,
                                                                 std::string_view fragmentVPath) noexcept
    {
        /* Reset any existing program. */
        Destroy();

        /* Read shader sources via the asset system. */
        auto vert = Assisi::Core::AssetSystem::ReadText(vertexVPath);
        if (!vert)
        {
            return std::unexpected(vert.error());
        }

        auto frag = Assisi::Core::AssetSystem::ReadText(fragmentVPath);
        if (!frag)
        {
            return std::unexpected(frag.error());
        }

        const char *vsrc = vert->c_str();
        const char *fsrc = frag->c_str();

        /* Compile stages. */
        const unsigned int vs = CompileStage(GL_VERTEX_SHADER, vsrc, "VERTEX");
        if (vs == 0u)
        {
            return {};
        }

        const unsigned int fs = CompileStage(GL_FRAGMENT_SHADER, fsrc, "FRAGMENT");
        if (fs == 0u)
        {
            glDeleteShader(vs);
            return {};
        }

        /* Link program. */
        _programIdentifier = glCreateProgram();
        glAttachShader(_programIdentifier, vs);
        glAttachShader(_programIdentifier, fs);
        glLinkProgram(_programIdentifier);

        /* Stages are no longer needed after linking. */
        glDeleteShader(vs);
        glDeleteShader(fs);

        if (!PrintProgramLinkErrors(_programIdentifier))
        {
            Destroy();
        }

        return {};
    }

    void Use() const { glUseProgram(_programIdentifier); }

    unsigned int ProgramIdentifier() const { return _programIdentifier; }

    void SetBool(const std::string &uniformName, bool value) const
    {
        glUniform1i(glGetUniformLocation(_programIdentifier, uniformName.c_str()), static_cast<int>(value));
    }

    void SetInt(const std::string &uniformName, int value) const
    {
        glUniform1i(glGetUniformLocation(_programIdentifier, uniformName.c_str()), value);
    }

    void SetFloat(const std::string &uniformName, float value) const
    {
        glUniform1f(glGetUniformLocation(_programIdentifier, uniformName.c_str()), value);
    }

    void SetVec2(const std::string &uniformName, const glm::vec2 &value) const
    {
        glUniform2fv(glGetUniformLocation(_programIdentifier, uniformName.c_str()), 1, &value[0]);
    }

    void SetVec2(const std::string &uniformName, float xValue, float yValue) const
    {
        glUniform2f(glGetUniformLocation(_programIdentifier, uniformName.c_str()), xValue, yValue);
    }

    void SetVec3(const std::string &uniformName, const glm::vec3 &value) const
    {
        glUniform3fv(glGetUniformLocation(_programIdentifier, uniformName.c_str()), 1, &value[0]);
    }

    void SetVec3(const std::string &uniformName, float xValue, float yValue, float zValue) const
    {
        glUniform3f(glGetUniformLocation(_programIdentifier, uniformName.c_str()), xValue, yValue, zValue);
    }

    void SetVec4(const std::string &uniformName, const glm::vec4 &value) const
    {
        glUniform4fv(glGetUniformLocation(_programIdentifier, uniformName.c_str()), 1, &value[0]);
    }

    void SetVec4(const std::string &uniformName, float xValue, float yValue, float zValue, float wValue) const
    {
        glUniform4f(glGetUniformLocation(_programIdentifier, uniformName.c_str()), xValue, yValue, zValue, wValue);
    }

    void SetMat2(const std::string &uniformName, const glm::mat2 &value) const
    {
        glUniformMatrix2fv(glGetUniformLocation(_programIdentifier, uniformName.c_str()), 1, GL_FALSE, &value[0][0]);
    }

    void SetMat3(const std::string &uniformName, const glm::mat3 &value) const
    {
        glUniformMatrix3fv(glGetUniformLocation(_programIdentifier, uniformName.c_str()), 1, GL_FALSE, &value[0][0]);
    }

    void SetMat4(const std::string &uniformName, const glm::mat4 &value) const
    {
        glUniformMatrix4fv(glGetUniformLocation(_programIdentifier, uniformName.c_str()), 1, GL_FALSE, &value[0][0]);
    }

    ~Shader() { Destroy(); }

    Shader(const Shader &) = delete;
    Shader &operator=(const Shader &) = delete;

    Shader(Shader &&other) noexcept { *this = std::move(other); }

    Shader &operator=(Shader &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            _programIdentifier = other._programIdentifier;
            other._programIdentifier = 0u;
        }
        return *this;
    }

  private:
    static unsigned int CompileStage(unsigned int stage, const char *source, const std::string &stageName)
    {
        const unsigned int shader = glCreateShader(stage);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        if (!PrintShaderCompileErrors(shader, stageName))
        {
            glDeleteShader(shader);
            return 0u;
        }

        return shader;
    }

    static bool PrintShaderCompileErrors(unsigned int shaderIdentifier, const std::string &shaderStageName)
    {
        int ok = 0;
        glGetShaderiv(shaderIdentifier, GL_COMPILE_STATUS, &ok);

        if (ok == 1)
        {
            return true;
        }

        char buffer[1024];
        glGetShaderInfoLog(shaderIdentifier, 1024, nullptr, buffer);

        std::cout << "Shader: Compilation failed for stage: " << shaderStageName << std::endl;
        std::cout << buffer << std::endl;

        return false;
    }

    static bool PrintProgramLinkErrors(unsigned int programIdentifier)
    {
        int ok = 0;
        glGetProgramiv(programIdentifier, GL_LINK_STATUS, &ok);

        if (ok == 1)
        {
            return true;
        }

        char buffer[1024];
        glGetProgramInfoLog(programIdentifier, 1024, nullptr, buffer);

        std::cout << "Shader: Program linking failed." << std::endl;
        std::cout << buffer << std::endl;

        return false;
    }

    void Destroy() noexcept
    {
        if (_programIdentifier != 0u)
        {
            glDeleteProgram(_programIdentifier);
            _programIdentifier = 0u;
        }
    }

  private:
    unsigned int _programIdentifier = 0u;
};
} // namespace Assisi::Render

#endif /* ASSISI_RENDER_SHADER_HPP */
