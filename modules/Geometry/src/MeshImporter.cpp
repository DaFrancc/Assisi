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

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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
   Sibling buffers/images are resolved relative to this (kept as a virtual path so
   the read still goes through AssetSystem's escape protection). */
std::string ParentDir(std::string_view vpath)
{
    const size_t slash = vpath.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string{vpath.substr(0, slash)};
}

glm::mat4 ToGlm(fastgltf::math::fmat4x4 matrix) noexcept
{
    glm::mat4 result(1.0f);
    for (int32_t col = 0; col < 4; ++col)
    {
        for (int32_t row = 0; row < 4; ++row)
        {
            result[col][row] = matrix[static_cast<size_t>(col)][static_cast<size_t>(row)];
        }
    }
    return result;
}

/* Parses the authored-LOD naming convention: a name ending in "_LOD<n>"
   (case-insensitive, digits to the end) yields n. Anything else is nullopt. */
std::optional<uint32_t> ParseLodSuffix(std::string_view name) noexcept
{
    constexpr std::string_view kTag = "_lod";

    // The suffix must be the *last* underscore group: "Cube_LOD1" matches,
    // "Cube_LOD1_old" does not (its tail is "_old").
    const size_t underscore = name.find_last_of('_');
    if (underscore == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view tail = name.substr(underscore); // "_lod<digits>" expected
    if (tail.size() <= kTag.size())
    {
        return std::nullopt;
    }
    for (size_t i = 0; i < kTag.size(); ++i)
    {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(tail[i]))) != kTag[i])
        {
            return std::nullopt;
        }
    }

    uint32_t level = 0;
    for (size_t i = kTag.size(); i < tail.size(); ++i)
    {
        const char c = tail[i];
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        level = level * 10 + static_cast<uint32_t>(c - '0');
    }
    return level;
}

/* LOD level for a node's mesh: the node name carries the suffix, falling back
   to the mesh name (DCC exporters differ on which one they stamp). No suffix
   anywhere means LOD 0. */
uint32_t LodLevelFor(const fastgltf::Node &node, const fastgltf::Asset &asset, size_t meshIndex) noexcept
{
    if (const auto fromNode = ParseLodSuffix(std::string_view{node.name}))
    {
        return *fromNode;
    }
    if (const auto fromMesh = ParseLodSuffix(std::string_view{asset.meshes[meshIndex].name}))
    {
        return *fromMesh;
    }
    return 0;
}

/* One warning of each kind per file, so a 200-primitive model doesn't log 200
   identical lines. */
struct ImportWarnings
{
    bool secondUvSet = false;
    bool vertexColor = false;
    bool skinning = false;
    bool alphaBlend = false;
    bool doubleSided = false;
    bool embeddedImage = false;
    bool secondaryTexCoord = false;
    bool negativeIor = false;
    bool specularTexture = false;
};

/* The KHR_materials_* extensions ExtractMaterial reads into MaterialData. */
constexpr std::array<std::string_view, 3> kMappedMaterialExtensions = {
    "KHR_materials_ior",
    "KHR_materials_specular",
    "KHR_materials_emissive_strength",
};

/* Every KHR_materials_* extension fastgltf knows, which is wider than the set
   above on purpose. glTF lets a file mark an extension *required*, and fastgltf
   aborts the whole parse when a required one is outside the mask — so masking to
   only what we read would drop an entire model on the floor over a material
   parameter, with no Asset left to name the reason from. Parsing them all and
   discarding what we cannot express is what keeps a required extension a
   warning instead of a dead import.
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

/* Warns once for every KHR_materials_* extension the file declares that the
   material model cannot express — whether the parser kept it or not, since
   extensionsUsed is filled either way. */
void WarnUnmappedMaterialExtensions(const fastgltf::Asset &asset, std::string_view virtualPath)
{
    for (const auto &used : asset.extensionsUsed)
    {
        const std::string_view name{used};
        if (!name.starts_with("KHR_materials_") ||
            std::ranges::find(kMappedMaterialExtensions, name) != kMappedMaterialExtensions.end())
        {
            continue;
        }
        Core::Log::Warn("MeshImporter: '{}' uses {}, which is not supported; its material parameters are dropped.",
                        virtualPath, name);
    }
}

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

/* GUID for the image behind texture @p textureIndex. Resolves the image URI to a
   virtual path relative to the .gltf (like external buffers), then maps it to a
   stable id via @p resolveId. Embedded images (GLB chunk / data URIs) and an
   absent resolver return the nil id — the unpack-to-separate-files stance: warn
   once naming the fix and import the channel factor-only. */
Core::AssetId ResolveImageId(const fastgltf::Asset &asset, size_t textureIndex, std::string_view virtualPath,
                             const std::string &parent, const AssetIdResolver &resolveId, ImportWarnings &warnings)
{
    if (textureIndex >= asset.textures.size())
    {
        return {};
    }
    const fastgltf::Texture &texture = asset.textures[textureIndex];
    if (!texture.imageIndex.has_value() || *texture.imageIndex >= asset.images.size())
    {
        return {};
    }

    const fastgltf::Image &image = asset.images[*texture.imageIndex];
    const auto *uriSource = std::get_if<fastgltf::sources::URI>(&image.data);
    if (uriSource == nullptr || uriSource->uri.isDataUri())
    {
        if (!warnings.embeddedImage)
        {
            warnings.embeddedImage = true;
            Core::Log::Warn("MeshImporter: '{}' embeds image data; textures import factor-only. Unpack with "
                            "`gltf-pipeline -i model.glb -o model.gltf --separate`.",
                            virtualPath);
        }
        return {};
    }

    const std::string relative{uriSource->uri.path()};
    const std::string sibling = parent.empty() ? relative : parent + "/" + relative;
    // No resolver (shipped/test) → nil id (factor-only). Otherwise the database
    // maps the resolved path to its stable GUID; an unknown path yields nil too.
    return resolveId ? resolveId(sibling) : Core::AssetId{};
}

/* Maps one glTF material to CPU MaterialData: pbrMetallicRoughness factors, the
   KHR_materials_* extensions in kMappedMaterialExtensions, and virtual asset
   paths for each texture channel. Unsupported authoring (alpha modes,
   double-sided, secondary UV sets) is flattened with a warning — never
   silently. */
MaterialData ExtractMaterial(const fastgltf::Asset &asset, size_t materialIndex, std::string_view virtualPath,
                             const std::string &parent, const AssetIdResolver &resolveId, ImportWarnings &warnings)
{
    MaterialData data;
    const fastgltf::Material &material = asset.materials[materialIndex];
    data.Name = std::string{std::string_view{material.name}};

    const auto &pbr = material.pbrData;
    data.BaseColorFactor =
        glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
    data.MetallicFactor = pbr.metallicFactor;
    data.RoughnessFactor = pbr.roughnessFactor;

    // KHR_materials_emissive_strength scales the emissive factor, and past 1 on
    // purpose: an emissive brighter than white is the whole point of the
    // extension, so this must not clamp.
    data.EmissiveFactor =
        glm::vec3(material.emissiveFactor[0], material.emissiveFactor[1], material.emissiveFactor[2]) *
        static_cast<float>(material.emissiveStrength);

    // KHR_materials_ior, passed through as authored. SpecularIor's [1, 3] is an
    // authoring range, not a validity bound: F0 = ((ior-1)/(ior+1))^2 squares
    // away the sign, so it is continuous and within [0, 1] for every positive
    // ior. glTF's ior of 0 needs no special case either — that expression
    // already yields the Fresnel of 1 the extension requires of it.
    //
    // A negative ior means nothing, and the pole at -1 sits inside that range,
    // so it flattens to the default rather than reaching the shader.
    const float ior = static_cast<float>(material.ior);
    if (ior < 0.f)
    {
        if (!warnings.negativeIor)
        {
            warnings.negativeIor = true;
            Core::Log::Warn("MeshImporter: '{}' has a material ('{}') whose KHR_materials_ior is {}; an index of "
                            "refraction cannot be negative — importing the default {}.",
                            virtualPath, data.Name, ior, data.SpecularIor);
        }
    }
    else
    {
        data.SpecularIor = ior;
    }

    // KHR_materials_specular. Absent when the file does not use the extension,
    // in which case MaterialData's defaults (weight 1, white) already mean the
    // same thing the extension's own defaults do.
    if (material.specular != nullptr)
    {
        const fastgltf::MaterialSpecular &specular = *material.specular;
        data.SpecularWeight = static_cast<float>(specular.specularFactor);
        data.SpecularColor  = glm::vec3(specular.specularColorFactor[0], specular.specularColorFactor[1],
                                        specular.specularColorFactor[2]);
        if ((specular.specularTexture.has_value() || specular.specularColorTexture.has_value()) &&
            !warnings.specularTexture)
        {
            warnings.specularTexture = true;
            Core::Log::Warn("MeshImporter: '{}' has a material ('{}') with a KHR_materials_specular texture; there is "
                            "no specular texture channel — importing its factors only.",
                            virtualPath, data.Name);
        }
    }

    const auto texCoordCheck = [&](size_t texCoordIndex)
                               {
                                   if (texCoordIndex != 0 && !warnings.secondaryTexCoord)
                                   {
                                       warnings.secondaryTexCoord = true;
                                       Core::Log::Warn("MeshImporter: '{}' has a material sampling TEXCOORD_{} — only TEXCOORD_0 is "
                                                       "supported; that channel will sample the wrong UVs.",
                                                       virtualPath, texCoordIndex);
                                   }
                               };

    if (pbr.baseColorTexture.has_value())
    {
        texCoordCheck(pbr.baseColorTexture->texCoordIndex);
        data.BaseColorTexture =
            ResolveImageId(asset, pbr.baseColorTexture->textureIndex, virtualPath, parent, resolveId, warnings);
    }
    if (pbr.metallicRoughnessTexture.has_value())
    {
        texCoordCheck(pbr.metallicRoughnessTexture->texCoordIndex);
        data.MetallicRoughnessTexture =
            ResolveImageId(asset, pbr.metallicRoughnessTexture->textureIndex, virtualPath, parent, resolveId, warnings);
    }
    if (material.normalTexture.has_value())
    {
        texCoordCheck(material.normalTexture->texCoordIndex);
        data.NormalTexture =
            ResolveImageId(asset, material.normalTexture->textureIndex, virtualPath, parent, resolveId, warnings);
        data.NormalScale = material.normalTexture->scale;
    }
    if (material.occlusionTexture.has_value())
    {
        texCoordCheck(material.occlusionTexture->texCoordIndex);
        data.OcclusionTexture =
            ResolveImageId(asset, material.occlusionTexture->textureIndex, virtualPath, parent, resolveId, warnings);
        data.OcclusionStrength = material.occlusionTexture->strength;
    }
    if (material.emissiveTexture.has_value())
    {
        texCoordCheck(material.emissiveTexture->texCoordIndex);
        data.EmissiveTexture =
            ResolveImageId(asset, material.emissiveTexture->textureIndex, virtualPath, parent, resolveId, warnings);
    }

    if (material.alphaMode == fastgltf::AlphaMode::Mask)
    {
        data.Alpha = AlphaMode::Mask;
        data.AlphaCutoff = static_cast<float>(material.alphaCutoff);
    }
    else if (material.alphaMode == fastgltf::AlphaMode::Blend && !warnings.alphaBlend)
    {
        // Blending has no pass to draw in. Opaque is the honest fallback: importing
        // it as a cutout instead would punch holes in a surface authored to fade.
        warnings.alphaBlend = true;
        Core::Log::Warn("MeshImporter: '{}' has an alphaMode BLEND material ('{}'); blended transparency is not "
                        "supported yet — importing as opaque.",
                        virtualPath, data.Name);
    }
    if (material.doubleSided && !warnings.doubleSided)
    {
        warnings.doubleSided = true;
        Core::Log::Warn("MeshImporter: '{}' has a double-sided material ('{}'); rendering single-sided.", virtualPath,
                        data.Name);
    }

    return data;
}

/* Warns (once per file) about vertex attributes the engine's vertex format
   cannot carry yet — data that would otherwise be dropped silently. */
void WarnDroppedAttributes(const fastgltf::Primitive &primitive, std::string_view virtualPath,
                           ImportWarnings &warnings)
{
    if (!warnings.secondUvSet && primitive.findAttribute("TEXCOORD_1") != primitive.attributes.end())
    {
        warnings.secondUvSet = true;
        Core::Log::Warn("MeshImporter: '{}' has TEXCOORD_1+; only TEXCOORD_0 is imported.", virtualPath);
    }
    if (!warnings.vertexColor && primitive.findAttribute("COLOR_0") != primitive.attributes.end())
    {
        warnings.vertexColor = true;
        Core::Log::Warn("MeshImporter: '{}' has vertex colors (COLOR_0); they are not imported.", virtualPath);
    }
    if (!warnings.skinning && primitive.findAttribute("JOINTS_0") != primitive.attributes.end())
    {
        warnings.skinning = true;
        Core::Log::Warn("MeshImporter: '{}' has skinning data (JOINTS_0/WEIGHTS_0); skinning is not supported — "
                        "importing the bind pose.",
                        virtualPath);
    }
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

    const size_t baseVertex   = out.Vertices.size();
    const fastgltf::Accessor &posAccessor  = asset.accessors[positionAttr->accessorIndex];
    out.Vertices.resize(baseVertex + posAccessor.count);

    // A node with a negative-determinant transform (mirror / negative scale) maps a
    // CCW triangle to CW once its positions are baked through `model`. With fixed
    // back-face culling and CCW-front, those faces would be culled and the mesh would
    // render inside-out. Flag it so the index winding is swapped back to CCW and the
    // tangent handedness (w) is negated to match the mirrored basis below.
    const bool mirrored = glm::determinant(glm::mat3(model)) < 0.0f;

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
                    // A mirrored node flips tangent-space handedness; negate w to keep the
                    // TBN basis consistent with the winding swap below.
                    out.Vertices[baseVertex + index].Tangent =
                        glm::vec4(direction, mirrored ? -tangent[3] : tangent[3]);
                });
    }
    else
    {
        allHaveTangents = false;
    }

    if (primitive.indicesAccessor.has_value())
    {
        const fastgltf::Accessor &indexAccessor = asset.accessors[*primitive.indicesAccessor];
        const size_t firstIndex    = out.Indices.size();
        out.Indices.reserve(firstIndex + indexAccessor.count);
        fastgltf::iterateAccessor<std::uint32_t>(
            asset, indexAccessor, [&](std::uint32_t index)
                {
                    out.Indices.push_back(static_cast<uint32_t>(baseVertex) + index);
                });
        if (mirrored)
        {
            // Swap the 2nd/3rd index of every triangle we just appended to restore CCW
            // winding in world space. glTF primitives imported here are triangle lists,
            // so the appended range is a whole number of triangles.
            for (size_t i = firstIndex; i + 2 < out.Indices.size(); i += 3)
                std::swap(out.Indices[i + 1], out.Indices[i + 2]);
        }
    }
}

/* One primitive occurrence in the scene: which primitive, with what baked
   transform, in which LOD, using which material. Collected in traversal order
   (phase 1), then bucketed by (lod, material slot) into submeshes (phase 2). */
struct PrimitiveRecord
{
    const fastgltf::Primitive *primitive = nullptr;
    glm::mat4 model{1.f};
    uint32_t lodIndex = 0;     // dense index into the sorted distinct LOD levels
    uint32_t materialSlot = 0; // dense slot in first-appearance order
};

constexpr size_t kNoMaterial = static_cast<size_t>(-1);

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
    case MeshImportError::Cancelled:
        return "cancelled (superseded)";
    }
    return "unknown error";
}

std::expected<MeshData, MeshImportError> ImportMesh(std::string_view virtualPath, const AssetIdResolver &resolveId)
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
    fastgltf::Parser parser{kMaterialExtensionMask};
    constexpr fastgltf::Options options = fastgltf::Options::GenerateMeshIndices;
    fastgltf::Expected<fastgltf::Asset> assetResult =
        parser.loadGltf(dataBuffer.get(), std::filesystem::path{}, options);
    if (assetResult.error() != fastgltf::Error::None)
    {
        return std::unexpected(MeshImportError::ParseFailed);
    }
    fastgltf::Asset &asset = assetResult.get();

    WarnUnmappedMaterialExtensions(asset, virtualPath);

    if (!ResolveExternalBuffers(asset, virtualPath))
    {
        return std::unexpected(MeshImportError::ExternalDataFailed);
    }

    const size_t sceneIndex = asset.defaultScene.value_or(0);
    if (sceneIndex >= asset.scenes.size())
    {
        return std::unexpected(MeshImportError::NoGeometry);
    }

    // --- Phase 1: collect (primitive, world transform, LOD level, material) records
    // in traversal order, plus the raw LOD levels and glTF material keys seen. ---
    std::vector<PrimitiveRecord> records;
    std::vector<uint32_t>        rawLodLevels; // parallel to records until densified
    std::vector<size_t>          slotKeys;     // slot -> glTF material index (or kNoMaterial)

    fastgltf::iterateSceneNodes(
        asset, sceneIndex, fastgltf::math::fmat4x4(),
        [&](fastgltf::Node &node, fastgltf::math::fmat4x4 worldMatrix)
        {
            if (!node.meshIndex.has_value())
            {
                return;
            }
            const glm::mat4 model = ToGlm(worldMatrix);
            const uint32_t lodLevel = LodLevelFor(node, asset, *node.meshIndex);
            for (const fastgltf::Primitive &primitive : asset.meshes[*node.meshIndex].primitives)
            {
                const size_t materialKey =
                    primitive.materialIndex.has_value() ? *primitive.materialIndex : kNoMaterial;
                uint32_t slot = 0;
                if (const auto found = std::ranges::find(slotKeys, materialKey); found != slotKeys.end())
                {
                    slot = static_cast<uint32_t>(found - slotKeys.begin());
                }
                else
                {
                    slot = static_cast<uint32_t>(slotKeys.size());
                    slotKeys.push_back(materialKey);
                }
                records.push_back(PrimitiveRecord{.primitive = &primitive, .model = model, .lodIndex = lodLevel,
                                                  .materialSlot = slot});
                rawLodLevels.push_back(lodLevel);
            }
        });

    if (records.empty())
    {
        return std::unexpected(MeshImportError::NoGeometry);
    }

    // Densify LOD levels: distinct authored levels, ascending, become Lods[0..n).
    std::vector<uint32_t> distinctLods = rawLodLevels;
    std::ranges::sort(distinctLods);
    distinctLods.erase(std::unique(distinctLods.begin(), distinctLods.end()), distinctLods.end());
    if (distinctLods.size() > 1 &&
        (distinctLods.front() != 0 || distinctLods.back() != distinctLods.size() - 1))
    {
        Core::Log::Warn("MeshImporter: '{}' has non-contiguous LOD levels (lowest {}, highest {}); they are used "
                        "in ascending order.",
                        virtualPath, distinctLods.front(), distinctLods.back());
    }
    for (PrimitiveRecord &record : records)
    {
        const auto found = std::ranges::lower_bound(distinctLods, record.lodIndex);
        record.lodIndex = static_cast<uint32_t>(found - distinctLods.begin());
    }

    // --- Phase 2: bucket by (LOD, material slot). Stable sort preserves traversal
    // order within a bucket, so same-material primitives merge in authoring
    // order. ---
    std::ranges::stable_sort(records, [](const PrimitiveRecord &a, const PrimitiveRecord &b)
        {
            if (a.lodIndex != b.lodIndex)
            {
                return a.lodIndex < b.lodIndex;
            }
            return a.materialSlot < b.materialSlot;
        });

    MeshData merged;
    ImportWarnings warnings;
    bool allHaveTangents = true;
    bool allHaveUv       = true;

    // One LodRange per dense LOD level, indexed directly by the (dense) lod value.
    // Pre-sized rather than push_back'd: a LOD whose buckets are all non-drawable
    // is skipped below, which would otherwise desynchronize Lods.size() from the
    // lod index and write out of bounds.
    merged.Lods.assign(distinctLods.size(), LodRange{});

    for (size_t i = 0; i < records.size();)
    {
        const uint32_t lod = records[i].lodIndex;
        const uint32_t slot = records[i].materialSlot;
        const uint32_t indexOffset = static_cast<uint32_t>(merged.Indices.size());

        for (; i < records.size() && records[i].lodIndex == lod && records[i].materialSlot == slot; ++i)
        {
            const PrimitiveRecord &record = records[i];
            WarnDroppedAttributes(*record.primitive, virtualPath, warnings);
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(record.model)));
            AppendPrimitive(asset, *record.primitive, record.model, normalMatrix, merged, allHaveTangents,
                            allHaveUv);
        }

        const uint32_t indexCount = static_cast<uint32_t>(merged.Indices.size()) - indexOffset;
        if (indexCount == 0)
        {
            continue; // bucket held only non-drawable primitives (no POSITION)
        }

        SubMesh subMesh;
        subMesh.IndexOffset = indexOffset;
        subMesh.IndexCount = indexCount;
        subMesh.MaterialSlot = slot;
        subMesh.LocalBounds = ComputeBoundingSphere(merged, indexOffset, indexCount);
        subMesh.LocalAabb = ComputeAabb(merged, indexOffset, indexCount);
        merged.SubMeshes.push_back(subMesh);

        // `lod` is a dense index into merged.Lods (pre-sized above). The first
        // drawable submesh of a level fixes its FirstSubMesh; the rest just count.
        LodRange &range = merged.Lods[lod];
        if (range.SubMeshCount == 0)
        {
            range.FirstSubMesh = static_cast<uint32_t>(merged.SubMeshes.size()) - 1;
        }
        ++range.SubMeshCount;
    }

    if (merged.Vertices.empty() || merged.Indices.empty())
    {
        return std::unexpected(MeshImportError::NoGeometry);
    }

    // Material slot table: extract each used glTF material; a primitive with no
    // material gets the spec-default MaterialData (metallic=1, roughness=1).
    const std::string parent = ParentDir(virtualPath);
    merged.Materials.reserve(slotKeys.size());
    for (const size_t key : slotKeys)
    {
        if (key == kNoMaterial || key >= asset.materials.size())
        {
            MaterialData fallback;
            fallback.Name = "default";
            merged.Materials.push_back(std::move(fallback));
        }
        else
        {
            merged.Materials.push_back(ExtractMaterial(asset, key, virtualPath, parent, resolveId, warnings));
        }
    }

    // If any primitive shipped without tangents, regenerate them for the whole
    // merged mesh — but only when UVs exist (Lengyel's method needs them).
    // Buckets never share vertices, so accumulation cannot bleed across
    // material seams.
    if (!allHaveTangents && allHaveUv)
    {
        ComputeTangents(merged);
    }

    // Fit the whole-mesh bounds here, on the import worker — after tangents, so
    // the vertex array is final. The main-thread publish (MeshBuffer::Upload)
    // then only reads them instead of re-walking a large mesh's vertices.
    EnsureMeshBounds(merged);

    return merged;
}

} /* namespace Assisi::Geometry */
