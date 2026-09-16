/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Cook/Cooker.hpp>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Geometry/MaterialChannels.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>
#include <Assisi/Geometry/MeshValidate.hpp>
#include <Assisi/Image/Compress.hpp>
#include <Assisi/Image/Decode.hpp>
#include <Assisi/Runtime/CookedScene.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>

namespace Assisi::Cook
{

namespace
{

/// Whether @p vpath ends with @p extension, case-sensitively — the asset tree is
/// authored, so a `.PNG` is a different file rather than the same one shouted.
bool HasExtension(std::string_view vpath, std::string_view extension)
{
    return vpath.size() > extension.size() && vpath.ends_with(extension);
}

bool HasAnyExtension(std::string_view vpath, std::span<const std::string_view> extensions)
{
    return std::ranges::any_of(extensions, [vpath](std::string_view ext) { return HasExtension(vpath, ext); });
}

CookError Failure(std::string_view vpath, std::string reason)
{
    return CookError{.vpath = std::string{vpath}, .reason = std::move(reason)};
}

/// Reads a source file, reporting as a cook failure rather than an AssetError.
std::expected<std::vector<std::byte>, CookError> ReadSource(std::string_view vpath)
{
    std::expected<std::vector<std::byte>, Core::AssetError> bytes = Core::AssetSystem::ReadBinary(vpath);
    if (!bytes)
    {
        return std::unexpected(Failure(vpath, "could not be read"));
    }
    return std::move(*bytes);
}

// ── Reflected assets ──────────────────────────────────────────────────────────

/// `.amat` and the config files: a `{version, type, fields}` document whose type
/// this build knows how to walk.
class ReflectedCooker final : public Cooker
{
public:
    [[nodiscard]] std::string_view Name() const override { return "reflected"; }
    [[nodiscard]] Core::CookedKind Kind() const override { return Core::CookedKind::Reflected; }

    [[nodiscard]] Claim Claims(std::string_view vpath) const override
    {
        static constexpr std::array kExtensions{std::string_view{".amat"}, std::string_view{".json"}};
        return HasAnyExtension(vpath, kExtensions) ? Claim::Output : Claim::None;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId, const CookContext &) const override
    {
        const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(vpath);
        if (!text)
        {
            return std::unexpected(Failure(vpath, "could not be read"));
        }

        // The envelope names the type; the registry says whether this build has
        // it. A document naming a type nothing registered is a file the engine
        // could not load either, so it fails here rather than shipping.
        const nlohmann::json document = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/ false);
        if (document.is_discarded() || !document.is_object())
        {
            return std::unexpected(Failure(vpath, "is not a JSON object"));
        }
        const auto typeIt = document.find("type");
        if (typeIt == document.end() || !typeIt->is_string())
        {
            return std::unexpected(Failure(vpath, "has no 'type' in its envelope"));
        }
        const std::string typeName = typeIt->get<std::string>();

        const Core::Reflect::AssetTypeMeta *meta = Core::Reflect::AssetTypeRegistry::Instance().Find(typeName);
        if (meta == nullptr)
        {
            return std::unexpected(Failure(vpath, std::format("names type '{}', which this build does not have",
                                                              typeName)));
        }
        if (!meta->construct || !meta->destroy || !meta->deserialize)
        {
            return std::unexpected(Failure(vpath, std::format("type '{}' has no construct/deserialize hooks",
                                                              typeName)));
        }

        // The caller knows the type only by that name, so the instance has to come
        // from the registry and go back to it.
        void *instance = meta->construct();
        if (instance == nullptr)
        {
            return std::unexpected(Failure(vpath, "could not be allocated"));
        }

        const std::expected<void, Core::Reflect::AssetDocumentError> applied =
            Core::Reflect::ApplyAssetDocument(*text, meta->typeIndex, instance);
        if (!applied)
        {
            const std::string reason{Core::Reflect::ToString(applied.error())};
            meta->destroy(instance);
            return std::unexpected(Failure(vpath, reason));
        }

        Core::BitWriter writer;
        Core::WriteCookedHeader(writer, Core::CookedKind::Reflected);
        writer.WriteString(typeName);
        const bool encoded = Core::Reflect::WriteAsset(*meta, instance, writer);
        meta->destroy(instance);

        if (!encoded)
        {
            return std::unexpected(Failure(vpath, "holds a field the codec cannot encode"));
        }

        const std::span<const std::byte> bytes = writer.Data();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }
};

// ── Levels and blueprints ─────────────────────────────────────────────────────

/// `.alvl` and `.abp`, cooked by loading them the way the engine does and
/// re-emitting through the codec.
///
/// Loading rather than re-parsing is the point: every rule the format has —
/// unique names, references that resolve, a version this build reads — is
/// already in `SceneSerializer::Load`, and a second reader here would be a second
/// opinion about what a level is.
class SceneCooker final : public Cooker
{
public:
    [[nodiscard]] std::string_view Name() const override { return "scene"; }
    [[nodiscard]] Core::CookedKind Kind() const override { return Core::CookedKind::Scene; }

    [[nodiscard]] std::uint64_t KeyVariant(const CookContext &) const override { return Runtime::kScenePayloadVersion; }

    [[nodiscard]] Claim Claims(std::string_view vpath) const override
    {
        static constexpr std::array kExtensions{std::string_view{".alvl"}, std::string_view{".abp"}};
        return HasAnyExtension(vpath, kExtensions) ? Claim::Output : Claim::None;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId, const CookContext &context) const override
    {
        if (context.database == nullptr)
        {
            return std::unexpected(Failure(vpath, "was cooked with no asset database"));
        }
        const Core::AssetDatabase &database = *context.database;
        const Runtime::BlueprintIdOf idOf = [&database](std::string_view source)
                                            {
                                                return database.IdFor(source).value_or(Core::AssetId{});
                                            };

        ECS::Scene scene;
        Runtime::InstanceTable instances;
        Runtime::LevelHeader header;

        Runtime::LoadOptions options;
        options.header    = &header;
        options.instances = &instances;

        const Runtime::LevelResult loaded = Runtime::SceneSerializer::LoadFromFile(scene, vpath, options);
        if (!loaded)
        {
            return std::unexpected(Failure(vpath, std::string{Runtime::Describe(loaded.error())}));
        }

        const std::expected<std::vector<std::byte>, Runtime::LevelError> cooked =
            Runtime::SaveCookedScene(scene, header, &instances, idOf);
        if (!cooked)
        {
            return std::unexpected(Failure(vpath, std::string{Runtime::Describe(cooked.error())}));
        }
        return *cooked;
    }
};

// ── Meshes ────────────────────────────────────────────────────────────────────

/// Version of the mesh payload's own framing, separate from the blob envelope's.
constexpr std::uint8_t kMeshPayloadVersion = 1;

/// `.gltf` / `.glb`, cooked to the arrays the GPU path already wants so no
/// import runs at load.
class MeshCooker final : public Cooker
{
public:
    [[nodiscard]] std::string_view Name() const override { return "mesh"; }
    [[nodiscard]] Core::CookedKind Kind() const override { return Core::CookedKind::Mesh; }

    [[nodiscard]] std::uint64_t KeyVariant(const CookContext &) const override { return kMeshPayloadVersion; }

    [[nodiscard]] Claim Claims(std::string_view vpath) const override
    {
        static constexpr std::array kExtensions{std::string_view{".gltf"}, std::string_view{".glb"}};
        if (HasAnyExtension(vpath, kExtensions))
        {
            return Claim::Output;
        }
        // A glTF's external buffer. Content, and consumed whole into the mesh
        // blob beside it rather than cooked on its own — the same relationship a
        // shader source has to its .spv.
        return HasExtension(vpath, ".bin") ? Claim::SourceOnly : Claim::None;
    }

    [[nodiscard]] std::vector<std::string> Dependencies(std::string_view vpath) const override
    {
        // A `.gltf` reads its vertex data from a sibling `.bin`, so editing that
        // changes the cooked mesh without touching the file this is keyed on.
        // Named by convention rather than by parsing the document: getting it
        // wrong costs a spurious re-cook, and parsing here would mean reading
        // every mesh twice on every walk.
        if (!HasExtension(vpath, ".gltf"))
        {
            return {};
        }
        std::string sibling{vpath};
        sibling.replace(sibling.size() - 5, 5, ".bin");
        return {sibling};
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId id, const CookContext &context) const override
    {
        if (context.database == nullptr)
        {
            return std::unexpected(Failure(vpath, "was cooked with no asset database"));
        }

        const Core::AssetDatabase &database = *context.database;
        const Geometry::AssetIdResolver resolve = [&database](std::string_view texturePath) -> Core::AssetId
                                                  {
                                                      const std::optional<Core::AssetId> found =
                                                          database.IdFor(texturePath);
                                                      return found.has_value() ? *found : Core::AssetId{};
                                                  };

        std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Geometry::ImportMesh(vpath, resolve);
        if (!mesh)
        {
            return std::unexpected(Failure(vpath, std::string{Geometry::ToString(mesh.error())}));
        }

        // Normalized before validation, because the degenerate "no tables means
        // one whole-mesh submesh" form is a convenience the validator refuses
        // rather than re-implements.
        Geometry::EnsureSubMeshTables(*mesh);
        Geometry::EnsureMeshBounds(*mesh);

        if (const std::expected<void, Geometry::MeshValidationError> valid = Geometry::ValidateMesh(*mesh); !valid)
        {
            return std::unexpected(Failure(vpath, std::string{Geometry::ToString(valid.error())}));
        }

        Core::BitWriter writer;
        Core::WriteCookedHeader(writer, Core::CookedKind::Mesh);
        writer.WriteUInt8(kMeshPayloadVersion);

        writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh->Vertices.size()));
        for (const Geometry::Vertex &vertex : mesh->Vertices)
        {
            WriteVec3(writer, vertex.Position);
            WriteVec3(writer, vertex.Normal);
            writer.WriteFloat(vertex.TextureCoordinates.x);
            writer.WriteFloat(vertex.TextureCoordinates.y);
            writer.WriteFloat(vertex.Tangent.x);
            writer.WriteFloat(vertex.Tangent.y);
            writer.WriteFloat(vertex.Tangent.z);
            writer.WriteFloat(vertex.Tangent.w);
        }

        writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh->Indices.size()));
        for (const std::uint32_t index : mesh->Indices)
        {
            writer.WriteVarUInt32(index);
        }

        writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh->SubMeshes.size()));
        for (const Geometry::SubMesh &submesh : mesh->SubMeshes)
        {
            writer.WriteVarUInt32(submesh.IndexOffset);
            writer.WriteVarUInt32(submesh.IndexCount);
            writer.WriteVarUInt32(submesh.MaterialSlot);
            WriteVec3(writer, submesh.LocalBounds.center);
            writer.WriteFloat(submesh.LocalBounds.radius);
            WriteVec3(writer, submesh.LocalAabb.min);
            WriteVec3(writer, submesh.LocalAabb.max);
        }

        writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh->Lods.size()));
        for (const Geometry::LodRange &lod : mesh->Lods)
        {
            writer.WriteVarUInt32(lod.FirstSubMesh);
            writer.WriteVarUInt32(lod.SubMeshCount);
            writer.WriteFloat(lod.ScreenSizeThreshold);
        }

        WriteVec3(writer, mesh->LocalBounds.center);
        writer.WriteFloat(mesh->LocalBounds.radius);
        WriteVec3(writer, mesh->LocalAabb.min);
        WriteVec3(writer, mesh->LocalAabb.max);

        // The slot table, from the sidecar manifest rather than from the import.
        // That is what the renderer reads at runtime: the import's own
        // MaterialData is the default a `.amat` was exploded from, and the
        // sidecar is where the binding actually lives afterwards.
        const auto slotCount = static_cast<std::uint32_t>(mesh->Materials.size());
        writer.WriteVarUInt32(slotCount);
        for (std::uint32_t slot = 0; slot < slotCount; ++slot)
        {
            Core::WriteAssetId(writer, database.SlotMaterial(id, slot));
        }

        const std::span<const std::byte> bytes = writer.Data();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }

private:
    static void WriteVec3(Core::BitWriter &writer, const glm::vec3 &value)
    {
        writer.WriteFloat(value.x);
        writer.WriteFloat(value.y);
        writer.WriteFloat(value.z);
    }
};

// ── Textures ──────────────────────────────────────────────────────────────────

constexpr std::uint8_t kTexturePayloadVersion = 1;

/// Images, cooked to the block-compressed mips a device uploads directly.
///
/// The format is not a property of the file: the same pixels are a colour map in
/// one material and a normal map in another, and those want different formats in
/// different colour spaces. So the role comes from the walk over every `.amat`,
/// and an image no material references is a cook failure — a texture nothing
/// binds is authoring residue, and shipping it would be shipping a guess about
/// what it is for.
class TextureCooker final : public Cooker
{
public:
    [[nodiscard]] std::string_view Name() const override { return "texture"; }
    [[nodiscard]] Core::CookedKind Kind() const override { return Core::CookedKind::Texture; }

    [[nodiscard]] std::uint64_t KeyVariant(const CookContext &context) const override
    {
        // The tier fits in its low byte, so the version above it cannot collide
        // with a tier.
        constexpr std::uint32_t kTierBits = 8;
        return (static_cast<std::uint64_t>(kTexturePayloadVersion) << kTierBits) |
               static_cast<std::uint64_t>(context.textureQuality);
    }

    [[nodiscard]] Claim Claims(std::string_view vpath) const override
    {
        static constexpr std::array kExtensions{std::string_view{".png"}, std::string_view{".jpg"},
                                                std::string_view{".jpeg"}};
        return HasAnyExtension(vpath, kExtensions) ? Claim::Output : Claim::None;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId id, const CookContext &context) const override
    {
        if (context.roles == nullptr)
        {
            return std::unexpected(Failure(vpath, "was cooked with no material channel map"));
        }

        const std::optional<Geometry::MaterialChannel> channel = context.roles->ChannelOf(id);
        if (!channel)
        {
            const auto [first, second] = context.roles->Conflict(id);
            if (!first.empty())
            {
                return std::unexpected(Failure(
                                           vpath, std::format("is bound as two different channels, by '{}' and '{}' — one GUID is one "
                                                              "blob, so copy the file if both are wanted",
                                                              first, second)));
            }
        }

        // Unbound textures are loaded straight from their path — the sky's moon —
        // and are colour. A normal map reaches the GPU through a material slot or
        // not at all, so there is no unbound one to get wrong.
        const Geometry::MaterialChannelFormat wanted =
            Geometry::FormatFor(channel.value_or(Geometry::MaterialChannel::BaseColor));

        std::expected<Image::DecodedImage, Core::AssetError> decoded = Image::DecodeImage(vpath, wanted.space);
        if (!decoded)
        {
            return std::unexpected(Failure(vpath, "could not be decoded"));
        }

        const std::expected<Image::DecodedImage, Core::AssetError> compressed =
            Image::Compress(*decoded, wanted.format, context.textureQuality);
        if (!compressed)
        {
            return std::unexpected(Failure(vpath, "could not be block compressed"));
        }
        if (!Image::ValidateMipChain(*compressed))
        {
            return std::unexpected(Failure(vpath, "compressed to a mip chain that does not match its format"));
        }

        Core::BitWriter writer;
        Core::WriteCookedHeader(writer, Core::CookedKind::Texture);
        writer.WriteUInt8(kTexturePayloadVersion);
        writer.WriteUInt32(compressed->width);
        writer.WriteUInt32(compressed->height);
        writer.WriteUInt8(static_cast<std::uint8_t>(compressed->format));
        writer.WriteUInt8(static_cast<std::uint8_t>(compressed->colorSpace));
        writer.WriteVarUInt32(static_cast<std::uint32_t>(compressed->mips.size()));
        for (const std::vector<unsigned char> &mip : compressed->mips)
        {
            writer.WriteVarUInt32(static_cast<std::uint32_t>(mip.size()));
            writer.WriteBytes(std::as_bytes(std::span{mip}));
        }

        const std::span<const std::byte> bytes = writer.Data();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }
};

// ── Shaders ───────────────────────────────────────────────────────────────────

/// SPIR-V, copied; GLSL, claimed and dropped.
class ShaderCooker final : public Cooker
{
public:
    [[nodiscard]] std::string_view Name() const override { return "shader"; }
    [[nodiscard]] Core::CookedKind Kind() const override { return Core::CookedKind::Shader; }

    [[nodiscard]] Claim Claims(std::string_view vpath) const override
    {
        if (HasExtension(vpath, ".spv"))
        {
            return Claim::Output;
        }
        // Source, which the build compiles and a shipped tree never carries. A
        // `.glsl` is a fragment another shader includes and is not compiled on
        // its own, so it has no `.spv` of its own either.
        static constexpr std::array kSources{std::string_view{".vert"}, std::string_view{".frag"},
                                             std::string_view{".comp"}, std::string_view{".glsl"}};
        return HasAnyExtension(vpath, kSources) ? Claim::SourceOnly : Claim::None;
    }

    [[nodiscard]] std::vector<std::string> Dependencies(std::string_view vpath) const override
    {
        // A compiled stage depends on its source, so editing the GLSL re-cooks
        // the `.spv` even though the build wrote that file itself.
        if (!HasExtension(vpath, ".spv"))
        {
            return {};
        }
        std::string source{vpath};
        source.resize(source.size() - 4);
        return {source};
    }

    [[nodiscard]] std::expected<void, CookError> CheckSource(std::string_view vpath) const override
    {
        // A `.glsl` is included into another stage and compiles to nothing of its
        // own, so it is the one source with no output to look for.
        if (HasExtension(vpath, ".glsl"))
        {
            return {};
        }

        const std::string compiled = std::string{vpath} + ".spv";
        if (!Core::AssetSystem::Exists(compiled))
        {
            return std::unexpected(Failure(vpath, std::format("has no compiled '{}' beside it, so this stage "
                                                              "would be missing from the cooked tree",
                                                              compiled)));
        }
        return {};
    }

    [[nodiscard]] Core::AssetId DerivedId(std::string_view vpath) const override
    {
        return HasExtension(vpath, ".spv") ? Core::DerivedAssetId(vpath) : Core::AssetId{};
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId, const CookContext &) const override
    {
        const std::expected<std::vector<std::byte>, CookError> spirv = ReadSource(vpath);
        if (!spirv)
        {
            return std::unexpected(spirv.error());
        }

        Core::BitWriter writer;
        Core::WriteCookedHeader(writer, Core::CookedKind::Shader);
        writer.WriteVarUInt32(static_cast<std::uint32_t>(spirv->size()));
        writer.WriteBytes(*spirv);

        const std::span<const std::byte> bytes = writer.Data();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }
};

// ── Everything else that is still content ─────────────────────────────────────

/// Fonts and animated WebP: bytes with no transformation to apply, wrapped in the
/// envelope so a cooked tree is uniformly readable.
///
/// Last in the list and explicit about what it takes. A catch-all that claimed
/// anything would turn the cook's best property — a file nobody handles is a
/// build failure — into a silent copy.
class VerbatimCooker final : public Cooker
{
public:
    [[nodiscard]] std::string_view Name() const override { return "verbatim"; }
    [[nodiscard]] Core::CookedKind Kind() const override { return Core::CookedKind::Verbatim; }

    [[nodiscard]] Claim Claims(std::string_view vpath) const override
    {
        static constexpr std::array kExtensions{std::string_view{".ttf"}, std::string_view{".webp"},
                                                std::string_view{".txt"}};
        return HasAnyExtension(vpath, kExtensions) ? Claim::Output : Claim::None;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, CookError>
    Cook(std::string_view vpath, Core::AssetId, const CookContext &) const override
    {
        const std::expected<std::vector<std::byte>, CookError> source = ReadSource(vpath);
        if (!source)
        {
            return std::unexpected(source.error());
        }

        Core::BitWriter writer;
        Core::WriteCookedHeader(writer, Core::CookedKind::Verbatim);
        writer.WriteVarUInt32(static_cast<std::uint32_t>(source->size()));
        writer.WriteBytes(*source);

        const std::span<const std::byte> bytes = writer.Data();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }
};

} // namespace

bool TextureRoles::Bind(Core::AssetId texture, Geometry::MaterialChannel channel, std::string_view material)
{
    if (texture.IsNil())
    {
        return true; // a factor-only channel binds nothing
    }

    Binding fresh;
    fresh.material = std::string{material};
    fresh.channel  = channel;

    const auto [slot, inserted] = _bindings.try_emplace(texture, std::move(fresh));
    if (inserted || slot->second.channel == channel)
    {
        return true;
    }

    // Recorded rather than reported here: the cook of that texture is where the
    // path is known, and this walk has only ids.
    if (slot->second.conflictingMaterial.empty())
    {
        slot->second.conflictingMaterial = std::string{material};
    }
    return false;
}

std::optional<Geometry::MaterialChannel> TextureRoles::ChannelOf(Core::AssetId texture) const
{
    const auto found = _bindings.find(texture);
    if (found == _bindings.end() || !found->second.conflictingMaterial.empty())
    {
        return std::nullopt;
    }
    return found->second.channel;
}

std::pair<std::string, std::string> TextureRoles::Conflict(Core::AssetId texture) const
{
    const auto found = _bindings.find(texture);
    if (found == _bindings.end() || found->second.conflictingMaterial.empty())
    {
        return {};
    }
    return {found->second.material, found->second.conflictingMaterial};
}

std::vector<std::unique_ptr<Cooker>> MakeCookers()
{
    std::vector<std::unique_ptr<Cooker>> cookers;
    cookers.push_back(std::make_unique<ReflectedCooker>());
    cookers.push_back(std::make_unique<SceneCooker>());
    cookers.push_back(std::make_unique<MeshCooker>());
    cookers.push_back(std::make_unique<TextureCooker>());
    cookers.push_back(std::make_unique<ShaderCooker>());
    cookers.push_back(std::make_unique<VerbatimCooker>());
    return cookers;
}

} // namespace Assisi::Cook
