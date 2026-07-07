#pragma once

/// @file ShaderModule.hpp
/// @brief Compiles GLSL source to SPIR-V at runtime and creates an NVRHI shader from it.

#include <string_view>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
/// @brief Reads GLSL source via `Core::AssetSystem` and compiles it to SPIR-V with
/// glslang (targeting Vulkan 1.0 / SPIR-V 1.0, matching this engine's baseline),
/// then creates an NVRHI shader from the result.
///
/// Compiling at runtime (rather than shipping a separate build-time
/// glslang-standalone + custom-command step) means shader source lives under
/// `assets/` like every other asset — no build product to fall out of sync with
/// its source (see docs/nvrhi-migration-todo.md section 4 for why this replaced
/// the earlier build-time approach).
///
/// @param glslVirtualPath  Virtual asset path to the `.vert`/`.frag`/`.comp` source,
/// e.g. "shaders/cube_min.vert".
/// @return nullptr if the source couldn't be read, failed to compile/link, or
/// shader creation failed (logged either way).
nvrhi::ShaderHandle CompileGlslShader(nvrhi::IDevice *device, std::string_view glslVirtualPath,
                                      nvrhi::ShaderType stage);

} /* namespace Assisi::Render */
