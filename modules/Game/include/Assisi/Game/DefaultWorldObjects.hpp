#pragma once

#include <Assisi/Game/WorldObject.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>

namespace Assisi::Game
{
inline WorldObject CreateDefaultCube()
{
    Assisi::Render::MeshData cubeMesh = Assisi::Render::CreateUnitCubeMesh();
    Assisi::Render::OpenGL::MeshBuffer cubeMeshBuffer(cubeMesh);

    WorldObject cube(cubeMeshBuffer);

    /* Identity transform by default (centered at origin). */
    cube.Transform().SetWorldPosition({0.0f, 0.0f, 0.0f});
    cube.Transform().SetWorldScale({1.0f, 1.0f, 1.0f});

    return cube;
}
} /* namespace Assisi::Scene */
