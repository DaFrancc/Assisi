/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/MeshImporter.hpp>

#include <Assisi/Geometry/DefaultMeshes.hpp> // ComputeTangents (fallback when a primitive lacks TANGENT)
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Math/GLM.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Assisi::Geometry
{
namespace
{

/* Case-insensitive check that @p path ends with @p suffix (an ASCII extension). */
bool EndsWithNoCase(std::string_view path, std::string_view suffix) noexcept
{
    if (path.size() < suffix.size())
    {
        return false;
    }
    const std::string_view tail = path.substr(path.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(tail[i])));
        if (lhs != suffix[i])
        {
            return false;
        }
    }
    return true;
}

bool IsGltfPath(std::string_view path) noexcept
{
    return EndsWithNoCase(path, ".gltf") || EndsWithNoCase(path, ".glb");
}

/* Virtual-path directory containing @p vpath, or "" if it has no directory part.
   Sibling buffers are resolved relative to this (kept as a virtual path so the
   read still goes through AssetSystem's escape protection). */
std::string ParentDir(std::string_view vpath)
{
    const size_t slash = vpath.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string{vpath.substr(0, slash)};
}

glm::mat4 ToGlm(fastgltf::math::fmat4x4 matrix) noexcept
{
    glm::mat4 result(1.0f);
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            result[col][row] = matrix[static_cast<size_t>(col)][static_cast<size_t>(row)];
        }
    }
    return result;
}

/* Replaces every external-file buffer (left as a URI because we deliberately did
   NOT pass Options::LoadExternalBuffers) with bytes read through AssetSystem, so
   glTF's sibling .bin files stay inside the escape-protected asset root. Buffers
   that are already resolved (GLB binary chunk, embedded base64 data URIs decoded
   at parse time) are left untouched. Returns false if any external read fails. */
bool ResolveExternalBuffers(fastgltf::Asset &asset, std::string_view virtualPath)
{
    const std::string parent = ParentDir(virtualPath);

    for (fastgltf::Buffer &buffer : asset.buffers)
    {
        auto *uriSource = std::get_if<fastgltf::sources::URI>(&buffer.data);
        if (uriSource == nullptr)
        {
            continue; // already have bytes (GLB chunk / decoded data URI)
        }
        if (uriSource->uri.isDataUri())
        {
            // An undecoded embedded data URI: nothing to read from disk, but we
            // can't feed it to the accessor tools either. Treat as unsupported.
            Core::Log::Warn("MeshImporter: '{}' has an undecoded embedded buffer; skipping.", virtualPath);
            return false;
        }

        const std::string relative{uriSource->uri.path()};
        const std::string sibling = parent.empty() ? relative : parent + "/" + relative;

        std::expected<std::vector<std::byte>, Core::AssetError> bytes = Core::AssetSystem::ReadBinary(sibling);
        if (!bytes)
        {
            Core::Log::Warn("MeshImporter: '{}' references buffer '{}' that could not be read.", virtualPath,
                            sibling);
            return false;
        }

        // A buffer's data may begin partway into its file (glTF fileByteOffset).
        std::vector<std::byte> owned;
        if (uriSource->fileByteOffset < bytes->size())
        {
            owned.assign(bytes->begin() + static_cast<std::ptrdiff_t>(uriSource->fileByteOffset), bytes->end());
        }
        buffer.data = fastgltf::sources::Vector{std::move(owned)};
    }
    return true;
}

/* Appends one primitive's geometry to @p out, baking @p model / @p normalMatrix
   into positions and directions. Updates @p allHaveTangents / @p allHaveUv so the
   caller knows whether tangents must be regenerated after the merge. */
void AppendPrimitive(const fastgltf::Asset &asset, const fastgltf::Primitive &primitive, const glm::mat4 &model,
                     const glm::mat3 &normalMatrix, MeshData &out, bool &allHaveTangents, bool &allHaveUv)
{
    const fastgltf::Attribute *positionAttr = primitive.findAttribute("POSITION");
    if (positionAttr == primitive.attributes.end())
    {
        return; // POSITION is mandatory in glTF; a primitive without it is not drawable.
    }

    const size_t              baseVertex   = out.Vertices.size();
    const fastgltf::Accessor &posAccessor  = asset.accessors[positionAttr->accessorIndex];
    out.Vertices.resize(baseVertex + posAccessor.count);

    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        asset, posAccessor, [&](fastgltf::math::fvec3 position, size_t index)
        {
            const glm::vec4 world = model * glm::vec4(position[0], position[1], position[2], 1.0f);
            out.Vertices[baseVertex + index].Position = glm::vec3(world);
        });

    if (const fastgltf::Attribute *normalAttr = primitive.findAttribute("NORMAL");
        normalAttr != primitive.attributes.end())
    {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            asset, asset.accessors[normalAttr->accessorIndex], [&](fastgltf::math::fvec3 normal, size_t index)
            {
                out.Vertices[baseVertex + index].Normal =
                    glm::normalize(normalMatrix * glm::vec3(normal[0], normal[1], normal[2]));
            });
    }

    if (const fastgltf::Attribute *uvAttr = primitive.findAttribute("TEXCOORD_0");
        uvAttr != primitive.attributes.end())
    {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
            asset, asset.accessors[uvAttr->accessorIndex], [&](fastgltf::math::fvec2 uv, size_t index)
            {
                out.Vertices[baseVertex + index].TextureCoordinates = glm::vec2(uv[0], uv[1]);
            });
    }
    else
    {
        allHaveUv = false;
    }

    if (const fastgltf::Attribute *tangentAttr = primitive.findAttribute("TANGENT");
        tangentAttr != primitive.attributes.end())
    {
        const glm::mat3 tangentMatrix(model);
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            asset, asset.accessors[tangentAttr->accessorIndex], [&](fastgltf::math::fvec4 tangent, size_t index)
            {
                const glm::vec3 direction =
                    glm::normalize(tangentMatrix * glm::vec3(tangent[0], tangent[1], tangent[2]));
                out.Vertices[baseVertex + index].Tangent = glm::vec4(direction, tangent[3]);
            });
    }
    else
    {
        allHaveTangents = false;
    }

    if (primitive.indicesAccessor.has_value())
    {
        const fastgltf::Accessor &indexAccessor = asset.accessors[*primitive.indicesAccessor];
        out.Indices.reserve(out.Indices.size() + indexAccessor.count);
        fastgltf::iterateAccessor<std::uint32_t>(
            asset, indexAccessor, [&](std::uint32_t index)
            {
                out.Indices.push_back(static_cast<unsigned int>(baseVertex) + index);
            });
    }
}

} // namespace

std::string_view ToString(MeshImportError error) noexcept
{
    switch (error)
    {
    case MeshImportError::UnsupportedFormat:
        return "unsupported format";
    case MeshImportError::ReadFailed:
        return "file read failed";
    case MeshImportError::ParseFailed:
        return "parse failed";
    case MeshImportError::ExternalDataFailed:
        return "external buffer read failed";
    case MeshImportError::NoGeometry:
        return "no geometry";
    }
    return "unknown error";
}

std::expected<MeshData, MeshImportError> ImportMesh(std::string_view virtualPath)
{
    if (!IsGltfPath(virtualPath))
    {
        return std::unexpected(MeshImportError::UnsupportedFormat);
    }

    // Read the file through AssetSystem so root-escape protection applies.
    std::expected<std::vector<std::byte>, Core::AssetError> fileBytes = Core::AssetSystem::ReadBinary(virtualPath);
    if (!fileBytes)
    {
        return std::unexpected(MeshImportError::ReadFailed);
    }

    fastgltf::Expected<fastgltf::GltfDataBuffer> dataBuffer =
        fastgltf::GltfDataBuffer::FromBytes(fileBytes->data(), fileBytes->size());
    if (dataBuffer.error() != fastgltf::Error::None)
    {
        return std::unexpected(MeshImportError::ParseFailed);
    }

    // No LoadExternalBuffers: fastgltf must not touch the filesystem itself.
    // Sibling .bin files are resolved by ResolveExternalBuffers() via AssetSystem.
    fastgltf::Parser              parser;
    constexpr fastgltf::Options   options = fastgltf::Options::GenerateMeshIndices;
    fastgltf::Expected<fastgltf::Asset> assetResult =
        parser.loadGltf(dataBuffer.get(), std::filesystem::path{}, options);
    if (assetResult.error() != fastgltf::Error::None)
    {
        return std::unexpected(MeshImportError::ParseFailed);
    }
    fastgltf::Asset &asset = assetResult.get();

    if (!ResolveExternalBuffers(asset, virtualPath))
    {
        return std::unexpected(MeshImportError::ExternalDataFailed);
    }

    const size_t sceneIndex = asset.defaultScene.value_or(0);
    if (sceneIndex >= asset.scenes.size())
    {
        return std::unexpected(MeshImportError::NoGeometry);
    }

    MeshData merged;
    bool     allHaveTangents = true;
    bool     allHaveUv       = true;

    fastgltf::iterateSceneNodes(
        asset, sceneIndex, fastgltf::math::fmat4x4(),
        [&](fastgltf::Node &node, fastgltf::math::fmat4x4 worldMatrix)
        {
            if (!node.meshIndex.has_value())
            {
                return;
            }
            const glm::mat4 model        = ToGlm(worldMatrix);
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));
            for (const fastgltf::Primitive &primitive : asset.meshes[*node.meshIndex].primitives)
            {
                AppendPrimitive(asset, primitive, model, normalMatrix, merged, allHaveTangents, allHaveUv);
            }
        });

    if (merged.Vertices.empty() || merged.Indices.empty())
    {
        return std::unexpected(MeshImportError::NoGeometry);
    }

    // If any primitive shipped without tangents, regenerate them for the whole
    // merged mesh — but only when UVs exist (Lengyel's method needs them).
    if (!allHaveTangents && allHaveUv)
    {
        ComputeTangents(merged);
    }

    return merged;
}

} /* namespace Assisi::Geometry */
