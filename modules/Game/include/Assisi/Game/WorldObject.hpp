#pragma once

#include <Assisi/Game/Transform.hpp>
#include <Assisi/Render/DefaultResources.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>

namespace Assisi::Game
{
class WorldObject
{
  public:
    WorldObject() = default;

    explicit WorldObject(const Assisi::Render::OpenGL::MeshBuffer &meshBuffer) : _meshBuffer(&meshBuffer) {}

    explicit WorldObject(const Assisi::Render::OpenGL::MeshBuffer &meshBuffer, unsigned int diffuseTextureIdentifier)
        : _meshBuffer(&meshBuffer), _customDiffuseTextureIdentifier(diffuseTextureIdentifier)
    {
    }

    Assisi::Game::Transform &Transform() { return _transform; }
    const Assisi::Game::Transform &Transform() const { return _transform; }

    bool HasMeshBuffer() const { return _meshBuffer != nullptr; }

    /* Preconditions: HasMeshBuffer() == true. */
    const Assisi::Render::OpenGL::MeshBuffer &MeshBuffer() const { return *_meshBuffer; }

    void SetMeshBuffer(const Assisi::Render::OpenGL::MeshBuffer *meshBuffer) { _meshBuffer = meshBuffer; }
    void ClearMeshBuffer() { _meshBuffer = nullptr; }

    unsigned int DiffuseTextureIdentifier() const
    {
        if (_customDiffuseTextureIdentifier != 0)
        {
            return _customDiffuseTextureIdentifier;
        }

        return Assisi::Render::DefaultResources::WhiteTextureId();
    }

    void SetDiffuseTextureIdentifier(unsigned int diffuseTextureIdentifier)
    {
        _customDiffuseTextureIdentifier = diffuseTextureIdentifier;
    }
    void ClearDiffuseTexture() { _customDiffuseTextureIdentifier = 0; }

    bool HasCustomDiffuseTexture() const { return _customDiffuseTextureIdentifier != 0; }
    unsigned int CustomDiffuseTextureIdentifier() const { return _customDiffuseTextureIdentifier; }

  private:
    Assisi::Game::Transform _transform;

    /* Non-owning pointer. The referenced MeshBuffer must outlive this WorldObject. */
    const Assisi::Render::OpenGL::MeshBuffer *_meshBuffer = nullptr;

    unsigned int _customDiffuseTextureIdentifier = 0;
};
} /* namespace Assisi::Game */
