#pragma once

#ifndef SHADER_HPP
#define SHADER_HPP

#include <glad/glad.h>

#include <Assisi/Math/GLM.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace Assisi::Render
{
class Shader
{
  public:
    Shader() = default;

    explicit Shader(const char *vertexShaderFilePath, const char *fragmentShaderFilePath)
    {
        /* Read shader source code from files. */
        std::string vertexShaderSourceCode;
        std::string fragmentShaderSourceCode;

        std::ifstream vertexShaderFileStream;
        std::ifstream fragmentShaderFileStream;

        vertexShaderFileStream.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        fragmentShaderFileStream.exceptions(std::ifstream::failbit | std::ifstream::badbit);

        try
        {
            vertexShaderFileStream.open(vertexShaderFilePath);
            fragmentShaderFileStream.open(fragmentShaderFilePath);

            std::stringstream vertexShaderStringStream;
            std::stringstream fragmentShaderStringStream;

            vertexShaderStringStream << vertexShaderFileStream.rdbuf();
            fragmentShaderStringStream << fragmentShaderFileStream.rdbuf();

            vertexShaderFileStream.close();
            fragmentShaderFileStream.close();

            vertexShaderSourceCode = vertexShaderStringStream.str();
            fragmentShaderSourceCode = fragmentShaderStringStream.str();
        }
        catch (const std::exception &)
        {
            std::cout << "Shader: Failed to read shader files." << std::endl;
        }

        const char *vertexShaderSourceCodeCString = vertexShaderSourceCode.c_str();
        const char *fragmentShaderSourceCodeCString = fragmentShaderSourceCode.c_str();

        /* Compile the vertex shader. */
        unsigned int vertexShaderIdentifier = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShaderIdentifier, 1, &vertexShaderSourceCodeCString, nullptr);
        glCompileShader(vertexShaderIdentifier);
        PrintShaderCompileErrors(vertexShaderIdentifier, "VERTEX");

        /* Compile the fragment shader. */
        unsigned int fragmentShaderIdentifier = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShaderIdentifier, 1, &fragmentShaderSourceCodeCString, nullptr);
        glCompileShader(fragmentShaderIdentifier);
        PrintShaderCompileErrors(fragmentShaderIdentifier, "FRAGMENT");

        /* Link the program. */
        _programIdentifier = glCreateProgram();
        glAttachShader(_programIdentifier, vertexShaderIdentifier);
        glAttachShader(_programIdentifier, fragmentShaderIdentifier);
        glLinkProgram(_programIdentifier);
        PrintProgramLinkErrors(_programIdentifier);

        /* The individual shaders are no longer needed after linking. */
        glDeleteShader(vertexShaderIdentifier);
        glDeleteShader(fragmentShaderIdentifier);
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

  private:
    static void PrintShaderCompileErrors(unsigned int shaderIdentifier, const std::string &shaderStageName)
    {
        int compilationSucceeded = 0;
        glGetShaderiv(shaderIdentifier, GL_COMPILE_STATUS, &compilationSucceeded);

        if (compilationSucceeded != 1)
        {
            char errorMessageBuffer[1024];
            glGetShaderInfoLog(shaderIdentifier, 1024, nullptr, errorMessageBuffer);

            std::cout << "Shader: Compilation failed for stage: " << shaderStageName << std::endl;
            std::cout << errorMessageBuffer << std::endl;
        }
    }

    static void PrintProgramLinkErrors(unsigned int programIdentifier)
    {
        int linkingSucceeded = 0;
        glGetProgramiv(programIdentifier, GL_LINK_STATUS, &linkingSucceeded);

        if (linkingSucceeded != 1)
        {
            char errorMessageBuffer[1024];
            glGetProgramInfoLog(programIdentifier, 1024, nullptr, errorMessageBuffer);

            std::cout << "Shader: Program linking failed." << std::endl;
            std::cout << errorMessageBuffer << std::endl;
        }
    }

  private:
    unsigned int _programIdentifier = 0;
};
} /* namespace Assisi::Render */

#endif /* SHADER_HPP */