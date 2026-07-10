/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShortNames.hpp
/// @brief Opt-in short namespace aliases (`Core`, `Math`, `Render`, ...) for
/// use inside a translation unit.
///
/// CPP ONLY. These aliases are injected at global scope, so including this from
/// a header would leak them into every translation unit that transitively
/// includes that header — exactly the kind of baffling global-namespace
/// collision the review flagged. Include it only from a .cpp, ideally after the
/// other includes.
///
/// Enforcement (not just a comment): `__INCLUDE_LEVEL__` — a GCC/Clang
/// preprocessor builtin — is 1 when a file is included directly by a .cpp and
/// >= 2 when reached through another header, so a stray header include trips the
/// `#error` below. MSVC doesn't define the builtin, so the guard is a no-op
/// there; but the clang/gcc build presets (and CI) catch a misuse for the whole
/// team. Guarding is better than a comment even if it isn't every compiler.
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