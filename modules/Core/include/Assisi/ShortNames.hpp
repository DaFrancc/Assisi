#pragma once

#include <Assisi/Prelude.hpp>

/*
    CPP ONLY!!! Never include in header files as it will pollute the global namespace and cause conflicts.
*/

/* Short aliases (opt-in). */
namespace A = Assisi;

namespace Core		= A::Core;
namespace Math		= A::Math;
namespace Render	= A::Render;
namespace Window	= A::Window;
namespace ECS		= A::ECS;
namespace Runtime	= A::Runtime;
namespace Physics	= A::Physics;
namespace Input		= A::Input;