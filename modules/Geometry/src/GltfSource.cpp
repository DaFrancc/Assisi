/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "GltfSource.hpp"

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <fastgltf/core.hpp>

#include <cctype>
#include <cstddef>
#include <filesystem>
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

/* Every KHR_materials_* extension fastgltf knows, which is wider than the set
   the material import reads on purpose. glTF lets a file mark an extension
* required*, and fastgltf aborts the whole parse when a required one is outside
   the mask — so masking to only what we read would drop an entire model on the
   floor over a material parameter, with no Asset left to name the reason from.
   Parsing them all and discarding what we cannot express is what keeps a
   required extension a warning instead of a dead import.
   Geometry extensions (Draco, meshopt, quantization) stay out: those change how
   vertices decode, so accepting one we cannot decode yields garbage geometry
   rather than a dropped material parameter. Failing the parse is right there. */
constexpr fastgltf::Extensions kMaterialExtensionMask =
    fastgltf::Extensions::KHR_materials_ior | fastgltf::Extensions::KHR_materials_specular |
    fastgltf::Extensions::KHR_materials_emissive_strength | fastgltf::Extensions::KHR_materials_iridescence |
    fastgltf::Extensions::KHR_materials_volume | fastgltf::Extensions::KHR_materials_transmission |
    fastgltf::Extensions::KHR_materials_clearcoat | fastgltf::Extensions::KHR_materials_sheen |
    fastgltf::Extensions::KHR_materials_unlit | fastgltf::Extensions::KHR_materials_anisotropy |
    fastgltf::Extensions::KHR_materials_dispersion | fastgltf::Extensions::KHR_materials_variants |
    fastgltf::Extensions::KHR_materials_diffuse_transmission;

/* Replaces every external-file buffer (left as a URI because the parser is not
   given Options::LoadExternalBuffers) with bytes read through AssetSystem, so
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
            Core::Log::Warn("glTF: '{}' has an undecoded embedded buffer; skipping.", virtualPath);
            return false;
        }

        const std::string relative{uriSource->uri.path()};
        const std::string sibling = parent.empty() ? relative : parent + "/" + relative;

        std::expected<std::vector<std::byte>, Core::AssetError> bytes = Core::AssetSystem::ReadBinary(sibling);
        if (!bytes)
        {
            Core::Log::Warn("glTF: '{}' references buffer '{}' that could not be read.", virtualPath, sibling);
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

} // namespace

bool IsGltfPath(std::string_view path) noexcept
{
    return EndsWithNoCase(path, ".gltf") || EndsWithNoCase(path, ".glb");
}

std::string ParentDir(std::string_view vpath)
{
    const size_t slash = vpath.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string{vpath.substr(0, slash)};
}

std::expected<fastgltf::Asset, MeshImportError> LoadGltfSource(std::string_view virtualPath)
{
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
    fastgltf::Parser parser{kMaterialExtensionMask};
    constexpr fastgltf::Options options = fastgltf::Options::GenerateMeshIndices;
    fastgltf::Expected<fastgltf::Asset> assetResult =
        parser.loadGltf(dataBuffer.get(), std::filesystem::path{}, options);
    if (assetResult.error() != fastgltf::Error::None)
    {
        return std::unexpected(MeshImportError::ParseFailed);
    }

    if (!ResolveExternalBuffers(assetResult.get(), virtualPath))
    {
        return std::unexpected(MeshImportError::ExternalDataFailed);
    }
    return std::move(assetResult.get());
}

} // namespace Assisi::Geometry
