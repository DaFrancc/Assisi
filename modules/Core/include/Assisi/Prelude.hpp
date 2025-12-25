#pragma once

/*
  Prelude is intentionally dependency-free:
  it only declares namespaces and provides aliases.
*/

/* Ensure the namespaces exist (forward declarations). */
namespace Assisi
{
namespace Core
{
}
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
namespace Game
{
}
namespace Physics
{
}
namespace Input
{
}
} // namespace Assisi

/* Short aliases (opt-in). */
namespace A = Assisi;

namespace Core = Assisi::Core;
namespace Math = Assisi::Math;
namespace Render = Assisi::Render;
namespace Window = Assisi::Window;
namespace ECS = Assisi::ECS;
namespace Game = Assisi::Game;
namespace Physics = Assisi::Physics;
namespace Input = Assisi::Input;
