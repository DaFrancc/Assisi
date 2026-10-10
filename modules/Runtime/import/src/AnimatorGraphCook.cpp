/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file AnimatorGraphCook.cpp
/// @brief The animator kind's cook step: a `.sgl` file compiled into the graph
///        the game runs, refused with every mistake in it listed, and cooked
///        again when a library it imports or the model it names changes.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/Import/AnimatorCompiler.hpp>
#include <Assisi/Sigil/Compile/Compile.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Runtime::Import
{

namespace
{

/// Raise when the step would cook the same file differently.
constexpr uint32_t kAnimatorGraphCookVersion = 1;

constexpr std::string_view kRefused = "the file has mistakes, listed with it";

std::string_view TextOf(std::span<const std::byte> bytes)
{
    return std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::expected<std::vector<std::byte>, Core::AssetError> CookAnimatorGraph(std::span<const std::byte> source,
                                                                          const Core::AssetCookContext &context)
{
    const std::expected<AnimatorGraph, std::string> graph = CompileAnimator(TextOf(source), context);
    if (!graph)
    {
        context.Report(graph.error());
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset, kRefused});
    }
    return WriteAnimatorGraph(*graph);
}

/// Every library the file imports, and those they import, and the model it
/// names as its skeleton.
std::vector<std::string> AnimatorGraphDependencies(std::span<const std::byte> source,
                                                   const Core::AssetCookContext &context)
{
    std::vector<std::string> dependencies;
    const std::vector<std::string> imports = Sigil::Compile::ListImports(TextOf(source));
    std::deque<std::string> unread{imports.begin(), imports.end()};
    while (!unread.empty())
    {
        std::string path = std::move(unread.front());
        unread.pop_front();
        if (std::ranges::find(dependencies, path) != dependencies.end())
        {
            continue;
        }
        const std::expected<std::vector<std::byte>, std::string> library = context.Read(path);
        dependencies.push_back(std::move(path));
        if (library)
        {
            for (std::string &imported : Sigil::Compile::ListImports(TextOf(*library)))
            {
                unread.push_back(std::move(imported));
            }
        }
    }
    if (std::optional<std::string> skeleton = Sigil::Compile::FileClauseString(TextOf(source), "skeleton"))
    {
        dependencies.push_back(std::move(*skeleton));
    }
    return dependencies;
}

[[maybe_unused]] const bool kRegistered =
    Core::AssetKindRegistry::Instance().RegisterCookStep(Core::MakeContextCookStep(
        kAnimatorGraphKind, kAnimatorGraphCookVersion, CookAnimatorGraph, AnimatorGraphDependencies));

} // namespace

} // namespace Assisi::Runtime::Import
