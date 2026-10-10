/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetKind.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <format>

namespace Assisi::Core
{

namespace
{

/// Bytes on their own: no other file to read, so a step that asks finds none.
class StandaloneCookContext final : public AssetCookContext
{
  public:
    [[nodiscard]] std::string_view Path() const override { return {}; }

    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> Read(std::string_view vpath) const override
    {
        return std::unexpected(std::format("\"{}\" can't be read: this cook has no other files", vpath));
    }

    [[nodiscard]] std::optional<AssetId> IdFor(std::string_view) const override { return std::nullopt; }

    [[nodiscard]] std::optional<std::string> KindNameOf(AssetId) const override { return std::nullopt; }

    void Report(std::string message) const override { Log::Warn("AssetKind: {}", message); }
};

AssetFormat ReadFormat(std::string extension)
{
    return AssetFormat{.extension = std::move(extension), .role = FormatRole::Reads, .preferred = true};
}

AssetFormat ConsumeFormat(std::string extension)
{
    return AssetFormat{.extension = std::move(extension), .role = FormatRole::Consumes, .preferred = false};
}

/// One of the engine's own kinds: named here so every file can say it is one,
/// loaded by the engine's own readers rather than through AssetStore.
AssetKind BuiltIn(AssetKindId id, std::vector<AssetFormat> formats)
{
    AssetKind kind;
    kind.name = std::string{BuiltInKindName(id)};
    kind.formats = std::move(formats);
    kind.id = id;
    return kind;
}

} // namespace

AssetKindRegistry::AssetKindRegistry()
{
    _kinds.push_back(BuiltIn(kTextureKind, {ReadFormat(".png"), ReadFormat(".jpg"), ReadFormat(".jpeg")}));
    _kinds.push_back(BuiltIn(kMeshKind, {ReadFormat(".gltf"), ReadFormat(".glb"), ConsumeFormat(".bin")}));
    _kinds.push_back(BuiltIn(kReflectedKind, {ReadFormat(".amat"), ReadFormat(".json")}));
    _kinds.push_back(BuiltIn(kSceneKind, {ReadFormat(".alvl"), ReadFormat(".abp")}));
    _kinds.push_back(
        BuiltIn(kShaderKind, {AssetFormat{.extension = ".spv", .role = FormatRole::Generated}, ConsumeFormat(".vert"),
                              ConsumeFormat(".frag"), ConsumeFormat(".comp"), ConsumeFormat(".glsl")}));
    _kinds.push_back(BuiltIn(
        kFontKind, {ReadFormat(".afont"), ConsumeFormat(".ttf"), ConsumeFormat(".otf"), ConsumeFormat(".txt")}));
    _kinds.push_back(BuiltIn(kScreenKind, {ReadFormat(".amdn"), ConsumeFormat(".amdt")}));
    _kinds.push_back(BuiltIn(kStringTableKind, {ReadFormat(".csv")}));
    _kinds.push_back(BuiltIn(kVerbatimKind, {ReadFormat(".webp")}));
}

AssetKindRegistry &AssetKindRegistry::Instance()
{
    static AssetKindRegistry instance;
    return instance;
}

bool AssetKindRegistry::Register(AssetKind kind)
{
    if (Find(kind.id) != nullptr)
    {
        Log::Error("AssetKind: '{}' is already a kind; the registration is ignored.", kind.name);
        return false;
    }
    if (!kind.load)
    {
        Log::Error("AssetKind: '{}' has no load function; the registration is ignored.", kind.name);
        return false;
    }
    for (const AssetFormat &format : kind.formats)
    {
        if (format.role == FormatRole::Generated)
        {
            Log::Error("AssetKind: '{}' declares '{}' as generated, which only the engine's shaders are; the "
                       "registration is ignored.",
                       kind.name, format.extension);
            return false;
        }
    }
    _kinds.push_back(std::move(kind));
    return true;
}

bool AssetKindRegistry::RegisterCookStep(AssetCookStep step)
{
    if (CookStepFor(step.kind) != nullptr)
    {
        Log::Error("AssetKind: {} has two cook steps; the second is ignored.", DescribeKind(step.kind));
        return false;
    }
    _cookSteps.push_back(std::move(step));
    return true;
}

const AssetKind *AssetKindRegistry::Find(AssetKindId id) const
{
    for (const AssetKind &kind : _kinds)
    {
        if (kind.id == id)
        {
            return &kind;
        }
    }
    return nullptr;
}

const AssetKind *AssetKindRegistry::FindByName(std::string_view name) const
{
    return Find(AssetKindId{name});
}

bool AssetKindRegistry::Reads(AssetKindId id, std::string_view extension) const
{
    const AssetKind *kind = Find(id);
    if (kind == nullptr)
    {
        return false;
    }
    return std::ranges::any_of(kind->formats, [extension](const AssetFormat &format)
                               { return format.role == FormatRole::Reads && format.extension == extension; });
}

std::vector<const AssetKind *> AssetKindRegistry::KindsReading(std::string_view extension) const
{
    std::vector<const AssetKind *> readers;
    for (const AssetKind &kind : _kinds)
    {
        if (Reads(kind.id, extension))
        {
            readers.push_back(&kind);
        }
    }
    // By name, so the order is the same whichever module's static initialiser
    // happened to run first.
    std::ranges::sort(readers, [](const AssetKind *a, const AssetKind *b) { return a->name < b->name; });
    return readers;
}

const AssetKind *AssetKindRegistry::KindForNewFile(std::string_view extension) const
{
    const std::vector<const AssetKind *> readers = KindsReading(extension);
    for (const AssetKind *kind : readers)
    {
        const bool prefers = std::ranges::any_of(kind->formats, [extension](const AssetFormat &format)
                                                 { return format.extension == extension && format.preferred; });
        if (prefers)
        {
            return kind;
        }
    }
    return readers.empty() ? nullptr : readers.front();
}

const AssetKind *AssetKindRegistry::ConsumerOf(std::string_view extension) const
{
    return WithRole(extension, FormatRole::Consumes);
}

const AssetKind *AssetKindRegistry::GeneratorOf(std::string_view extension) const
{
    return WithRole(extension, FormatRole::Generated);
}

const AssetKind *AssetKindRegistry::WithRole(std::string_view extension, FormatRole role) const
{
    for (const AssetKind &kind : _kinds)
    {
        for (const AssetFormat &format : kind.formats)
        {
            if (format.role == role && format.extension == extension)
            {
                return &kind;
            }
        }
    }
    return nullptr;
}

const AssetCookStep *AssetKindRegistry::CookStepFor(AssetKindId id) const
{
    for (const AssetCookStep &step : _cookSteps)
    {
        if (step.kind == id)
        {
            return &step;
        }
    }
    return nullptr;
}

std::string_view ExtensionOf(std::string_view vpath)
{
    const std::string_view::size_type slash = vpath.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? vpath : vpath.substr(slash + 1);
    const std::string_view::size_type dot = name.find_last_of('.');
    // A name that is only an extension (".assisiignore") has none.
    if (dot == std::string_view::npos || dot == 0)
    {
        return {};
    }
    return name.substr(dot);
}

const AssetCookContext &NoCookContext()
{
    static const StandaloneCookContext context;
    return context;
}

std::expected<std::vector<std::byte>, AssetError> CookAssetBytes(const AssetKind &kind,
                                                                 std::span<const std::byte> source)
{
    return CookAssetBytes(kind, source, NoCookContext());
}

std::expected<std::vector<std::byte>, AssetError> CookAssetBytes(const AssetKind &kind,
                                                                 std::span<const std::byte> source,
                                                                 const AssetCookContext &context)
{
    BitWriter writer;
    WriteCookedHeader(writer, kind.id);

    const AssetCookStep *step = AssetKindRegistry::Instance().CookStepFor(kind.id);
    if (step == nullptr)
    {
        writer.WriteBytes(source);
    }
    else
    {
        const std::expected<std::vector<std::byte>, AssetError> cooked = step->cook(source, context);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        writer.WriteBytes(*cooked);
    }

    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace Assisi::Core
