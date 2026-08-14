/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

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

// --- Writable user root -----------------------------------------------------

namespace
{
// A fresh, empty directory to anchor the writable user root for a test.
std::filesystem::path MakeUserRoot(const char *name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "assisi-user-tests" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("AssetSystem::WriteText round-trips through ReadUserText")
{
    REQUIRE(AssetSystem::SetUserRoot(MakeUserRoot("text")).has_value());

    REQUIRE(AssetSystem::WriteText("options.json", "{\"a\":1}").has_value());
    CHECK(AssetSystem::UserExists("options.json"));

    const std::expected<std::string, AssetError> read = AssetSystem::ReadUserText("options.json");
    REQUIRE(read.has_value());
    CHECK(*read == "{\"a\":1}");
}

TEST_CASE("AssetSystem::WriteBinary round-trips through ReadUserBinary")
{
    REQUIRE(AssetSystem::SetUserRoot(MakeUserRoot("binary")).has_value());

    const std::vector<std::byte> bytes = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}, std::byte{0x7F}};
    REQUIRE(AssetSystem::WriteBinary("saves/slot1.sav", bytes).has_value());

    const std::expected<std::vector<std::byte>, AssetError> read = AssetSystem::ReadUserBinary("saves/slot1.sav");
    REQUIRE(read.has_value());
    CHECK(*read == bytes);
}

TEST_CASE("AssetSystem::WriteText creates missing parent directories")
{
    const std::filesystem::path root = MakeUserRoot("nested");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());

    REQUIRE(AssetSystem::WriteText("a/b/c/note.txt", "hi").has_value());
    CHECK(std::filesystem::exists(root / "a" / "b" / "c" / "note.txt"));
}

TEST_CASE("AssetSystem::ResolveUser rejects paths escaping the user root")
{
    REQUIRE(AssetSystem::SetUserRoot(MakeUserRoot("escape")).has_value());

    const std::expected<std::filesystem::path, AssetError> escaped = AssetSystem::ResolveUser("../outside.txt");
    REQUIRE_FALSE(escaped.has_value());
    CHECK(escaped.error() == AssetError::InvalidVirtualPath);

    // A write that would escape must fail without touching the filesystem.
    CHECK_FALSE(AssetSystem::WriteText("../outside.txt", "nope").has_value());
}

TEST_CASE("AssetSystem::ReadUserText reports a clean error for a missing file")
{
    REQUIRE(AssetSystem::SetUserRoot(MakeUserRoot("missing")).has_value());

    const std::expected<std::string, AssetError> read = AssetSystem::ReadUserText("does-not-exist.json");
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == AssetError::FileOpenFailed);
    CHECK_FALSE(AssetSystem::UserExists("does-not-exist.json"));
}

// ---------------------------------------------------------------------------
// Roots that cannot be set.
//
// SetRoot / SetUserRoot are noexcept and used to call std::filesystem's
// *throwing* overloads with no handler, so an OS-level failure was
// std::terminate rather than a returned error. They now use the error_code
// overloads.
//
// **These cases do not reproduce that.** A path that merely does not exist is
// not an OS failure — is_directory reports false with a clear error_code and
// never threw. Provoking the real thing needs a genuine failure (permission
// denied on a parent, an unmounted volume), which is not portably constructible
// in a unit test and would be skipped on the CI that runs as root. What these
// pin is the contract around it: the documented rejection, by value, with
// nothing escaping.
// ---------------------------------------------------------------------------

TEST_CASE("AssetSystem::SetRoot refuses a path that is not a directory, and does not throw")
{
    // A regular file, not a directory: is_directory answers false rather than
    // failing, which is the ordinary rejection.
    const std::filesystem::path file = MakeUserRoot("notadir") / "afile.txt";
    {
        std::ofstream out(file);
        REQUIRE(out.good());
    }

    std::expected<void, AssetError> result;
    CHECK_NOTHROW(result = AssetSystem::SetRoot(file));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == AssetError::InvalidRoot);
}

TEST_CASE("AssetSystem::SetUserRoot refuses a path that does not exist, and does not throw")
{
    const std::filesystem::path missing = TempRoot() / "assisi_no_such_root_9f3a" / "deeper";

    std::expected<void, AssetError> result;
    CHECK_NOTHROW(result = AssetSystem::SetUserRoot(missing));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == AssetError::InvalidRoot);
}

TEST_CASE("AssetSystem::Exists answers false for an unreadable path rather than throwing")
{
    REQUIRE(AssetSystem::SetRoot(TempRoot()).has_value());

    // A path with an interior component that is a *file*, so the OS reports
    // ENOTDIR when it stats it — an error_code, where the throwing overload of
    // fs::exists would have raised.
    const std::filesystem::path file = TempRoot() / "assisi_notdir_probe.txt";
    {
        std::ofstream out(file);
        REQUIRE(out.good());
    }

    bool answer = true;
    CHECK_NOTHROW(answer = AssetSystem::Exists("assisi_notdir_probe.txt/below"));
    CHECK_FALSE(answer);

    std::error_code ec;
    std::filesystem::remove(file, ec);
}
