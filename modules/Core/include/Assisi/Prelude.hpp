#pragma once

/*
  Prelude — included by every Assisi header.
  Keep this file lean: only zero-cost inclusions (macros, namespace
  forward-declarations).  Never pull in GLM, nlohmann, or other heavy
  headers here — a change to Prelude forces a full rebuild of everything.
*/

/* Reflection annotations: ACOMP(), AFIELD().  Compile to nothing. */
#include <Assisi/Core/Reflect/Annotations.hpp>

/* Ensure the namespaces exist (forward declarations). */
namespace Assisi
{
namespace Core
{
namespace Reflect
{
}
} // namespace Core
namespace Math
{
}
namespace Render
{
}
namespace Window
{
}
namespace ECS
{
}
namespace Runtime
{
}
namespace Physics
{
}
namespace Input
{
}
} // namespace Assisi