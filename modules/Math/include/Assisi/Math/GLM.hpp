#pragma once

/// @file GLM.hpp
/// @brief Single include for GLM with the project-wide configuration applied.
///
/// Always include this header instead of raw GLM headers to guarantee that
/// GLMConfig.hpp macros are set before any GLM code is parsed.
/// Pulls in: core GLM, matrix transforms, and quaternion support (stable + experimental).

#include <Assisi/Math/GLMConfig.hpp>

/* This makes it much harder to include GLM "raw" without config. */
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>