/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShortNames.hpp
/// @brief Opt-in short namespace aliases (`Core`, `Math`, `Render`, ...) for
/// use inside a translation unit.
///
/// CPP ONLY. These aliases are injected at global scope, so including this from
/// a header leaks them into every translation unit that transitively includes
/// that header, producing global-namespace collisions far from their cause.
/// Include it only from a .cpp, ideally after the other includes.
///
/// Enforced, not merely documented: `__INCLUDE_LEVEL__` — a GCC/Clang
/// preprocessor builtin — is 1 when a file is included directly by a .cpp and
/// >= 2 when reached through another header, so a stray header include trips the
/// `#error` below. MSVC doesn't define the builtin, so the guard is a no-op
/// there; the clang/gcc presets and CI catch the misuse instead.
#if defined(__INCLUDE_LEVEL__) && (__INCLUDE_LEVEL__ > 1)
#error "ShortNames.hpp is .cpp-only: it injects global namespace aliases, so including it from a header leaks them into every downstream translation unit. Include it directly from a .cpp instead."
#endif

#include <Assisi/Prelude.hpp>

/* Short aliases (opt-in). */
namespace A = Assisi;

namespace Core = A::Core;
namespace Math = A::Math;
namespace Render = A::Render;
namespace Window = A::Window;
namespace ECS = A::ECS;
namespace Runtime = A::Runtime;
namespace Physics = A::Physics;
namespace Input = A::Input;