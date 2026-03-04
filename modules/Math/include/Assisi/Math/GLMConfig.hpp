#pragma once

/// @file GLMConfig.hpp
/// @brief GLM compile-time configuration.
///
/// Must be included (directly or via GLM.hpp) before any raw GLM header to
/// ensure consistent behaviour across all translation units.
///
/// Active settings:
/// - `GLM_FORCE_RADIANS`          — all angle parameters are in radians.
/// - `GLM_FORCE_DEPTH_ZERO_TO_ONE` — clip-space depth maps to [0, 1] (Vulkan/D3D convention).
/// - `GLM_ENABLE_EXPERIMENTAL`    — enables GLM extension headers (gtx/*).

/* These must be defined before any GLM header is included anywhere. */
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL 1
/* #define GLM_ENABLE_EXPERIMENTAL */
