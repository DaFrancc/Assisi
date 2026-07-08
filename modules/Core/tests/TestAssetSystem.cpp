/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <filesystem>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Errors.hpp>

using namespace Assisi::Core;

namespace
{
// A real, existing directory to anchor the asset root — SetRoot requires one.
std::filesystem::path TempRoot()
{
    return std::filesystem::temp_directory_path();
}
} // namespace

// These exercise the (private) NormalizeVirtualPath + root-escape logic through
// the public Resolve() surface. Resolve does not require the target file to
// exist, so no fixtures are needed on disk.
TEST_CASE("AssetSystem::Resolve normalizes and joins a valid virtual path")
{
    REQUIRE(AssetSystem::SetRoot(TempRoot()).has_value());

    auto resolved = AssetSystem::Resolve("textures/white.png");
    REQUIRE(resolved.has_value());
    CHECK(resolved->is_absolute());
    // The tail must be preserved under the root.
    CHECK(resolved->generic_string().ends_with("textures/white.png"));
}

TEST_CASE("AssetSystem::Resolve accepts Windows-style separators")
{
    REQUIRE(AssetSystem::SetRoot(TempRoot()).has_value());

    auto resolved = AssetSystem::Resolve("shaders\\cube.frag");
    REQUIRE(resolved.has_value());
    CHECK(resolved->generic_string().ends_with("shaders/cube.frag"));
}

TEST_CASE("AssetSystem::Resolve collapses interior '..' that cancels out")
{
    REQUIRE(AssetSystem::SetRoot(TempRoot()).has_value());

    // "a/../b" lexically normalizes to "b" — no surviving parent traversal.
    auto resolved = AssetSystem::Resolve("a/../b");
    REQUIRE(resolved.has_value());
    CHECK(resolved->generic_string().ends_with("/b"));
}

TEST_CASE("AssetSystem::Resolve rejects escaping and malformed paths")
{
    REQUIRE(AssetSystem::SetRoot(TempRoot()).has_value());

    auto expectInvalid = [](const char *vpath)
    {
        auto r = AssetSystem::Resolve(vpath);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == AssetError::InvalidVirtualPath);
    };

    expectInvalid("");           // empty
    expectInvalid("/etc/passwd"); // absolute
    expectInvalid("C:/Windows");  // drive-qualified
    expectInvalid("../secret");   // surviving parent traversal escapes the root
    expectInvalid("../../secret");
}
