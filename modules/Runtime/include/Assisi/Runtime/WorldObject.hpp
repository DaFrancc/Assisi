#pragma once

/// @file WorldObject.hpp
/// @brief A renderable scene object combining a transform, mesh reference, and diffuse texture.

#include <Assisi/Runtime/Transform.hpp>
#include <Assisi/Render/DefaultResources.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>

namespace Assisi::Runtime
{
/// @brief A renderable entity with a world transform, a mesh, and an optional diffuse texture.
///
/// Holds a **non-owning** pointer to a `MeshBuffer`; the referenced buffer
/// must outlive this object.  When no custom texture is set, the engine-wide
/// white texture is returned by DiffuseTextureIdentifier(), allowing shader
/// tint colour to pass through unmodified.
class WorldObject
{
  public:
    WorldObject() = default;

    /// @param meshBuffer  Mesh to render.  Must outlive this WorldObject.
    explicit WorldObject(const Assisi::Render::OpenGL::MeshBuffer &meshBuffer) : _meshBuffer(&meshBuffer) {}

    /// @param meshBuffer               Mesh to render.  Must outlive this WorldObject.
    /// @param diffuseTextureIdentifier OpenGL texture ID to use as the diffuse map.
    explicit WorldObject(const Assisi::Render::OpenGL::MeshBuffer &meshBuffer, unsigned int diffuseTextureIdentifier)
        : _meshBuffer(&meshBuffer), _customDiffuseTextureIdentifier(diffuseTextureIdentifier)
    {
    }

    Assisi::Runtime::Transform &Transform() { return _transform; }
    const Assisi::Runtime::Transform &Transform() const { return _transform; }

    bool HasMeshBuffer() const { return _meshBuffer != nullptr; }

    /// @pre HasMeshBuffer() == true.
    const Assisi::Render::OpenGL::MeshBuffer &MeshBuffer() const { return *_meshBuffer; }

    /// @brief Replaces the mesh reference.  Pass nullptr to clear it.
    void SetMeshBuffer(const Assisi::Render::OpenGL::MeshBuffer *meshBuffer) { _meshBuffer = meshBuffer; }
    void ClearMeshBuffer() { _meshBuffer = nullptr; }

    /// @brief Returns the diffuse texture ID to use when rendering.
    ///
    /// Returns the custom texture if one has been set, otherwise falls back
    /// to the engine-wide default white texture.
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

    /// @brief Clears the custom diffuse texture; DiffuseTextureIdentifier() will return the default.
    void ClearDiffuseTexture() { _customDiffuseTextureIdentifier = 0; }

    bool HasCustomDiffuseTexture() const { return _customDiffuseTextureIdentifier != 0; }
    unsigned int CustomDiffuseTextureIdentifier() const { return _customDiffuseTextureIdentifier; }

  private:
    Assisi::Runtime::Transform _transform;

    /// @brief Non-owning pointer.  The referenced MeshBuffer must outlive this WorldObject.
    const Assisi::Render::OpenGL::MeshBuffer *_meshBuffer = nullptr;

    /// @brief Custom diffuse texture ID.  0 means "use default".
    unsigned int _customDiffuseTextureIdentifier = 0;
};
} /* namespace Assisi::Runtime */