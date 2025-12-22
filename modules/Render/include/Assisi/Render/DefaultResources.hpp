/*
 * Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")
 */

#pragma once

namespace Assisi::Render
{
class DefaultResources
{
  public:
    /* Returns the engine-wide default white texture identifier.
       Preconditions: Render backend has been initialized. */
    static unsigned int WhiteTextureId();
};
} /* namespace Assisi::Render */
