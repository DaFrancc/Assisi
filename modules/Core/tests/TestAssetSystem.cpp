/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

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
        CHECK(r.error() == AssetErrorCode::InvalidVirtualPath);
    };

    expectInvalid("");            // empty
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

namespace
{
// The names directly inside @p dir, sorted.
std::vector<std::string> Entries(const std::filesystem::path &dir)
{
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir))
    {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

// What @p path holds right now, read in one go.
std::string ReadWhole(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
} // namespace

TEST_CASE("AssetSystem::WriteText replaces a file whole and leaves nothing beside it")
{
    const std::filesystem::path root = MakeUserRoot("replace");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());

    // Longer first, so a write that kept any of the old file would show it.
    REQUIRE(AssetSystem::WriteText("options.json", "{\"old\":\"and longer than the new\"}").has_value());
    REQUIRE(AssetSystem::WriteText("options.json", "{\"new\":1}").has_value());

    CHECK(ReadWhole(root / "options.json") == "{\"new\":1}");
    CHECK(Entries(root) == std::vector<std::string>{"options.json"});
}

TEST_CASE("AssetSystem::WriteText takes the place of what a crash mid-save left behind")
{
    const std::filesystem::path root = MakeUserRoot("leftover");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());
    std::ofstream{root / "options.json.tmp"} << "half a file";

    REQUIRE(AssetSystem::WriteText("options.json", "{}").has_value());

    CHECK(ReadWhole(root / "options.json") == "{}");
    CHECK(Entries(root) == std::vector<std::string>{"options.json"});
}

TEST_CASE("AssetSystem::WriteText that cannot put its file in place leaves the target and nothing else")
{
    // A directory where the file goes: the new contents are written in full
    // beside it, and replacing a directory with them is refused.
    const std::filesystem::path root = MakeUserRoot("blocked");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());
    std::filesystem::create_directories(root / "options.json");
    std::ofstream{root / "options.json" / "kept.txt"} << "kept";

    CHECK_FALSE(AssetSystem::WriteText("options.json", "{}").has_value());

    CHECK(ReadWhole(root / "options.json" / "kept.txt") == "kept");
    CHECK(Entries(root) == std::vector<std::string>{"options.json"});
}

TEST_CASE("AssetSystem::WriteText that cannot write its new contents leaves the old file as it was")
{
    // A directory holding something where the new contents would be written
    // first: the write fails before the file it replaces is touched.
    const std::filesystem::path root = MakeUserRoot("unwritable");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());
    REQUIRE(AssetSystem::WriteText("options.json", "{\"old\":1}").has_value());
    std::filesystem::create_directories(root / "options.json.tmp");
    std::ofstream{root / "options.json.tmp" / "in-the-way.txt"} << "in the way";

    CHECK_FALSE(AssetSystem::WriteText("options.json", "{\"new\":1}").has_value());

    CHECK(ReadWhole(root / "options.json") == "{\"old\":1}");
}

// Windows refuses to replace a file another handle has open, so a reader
// there makes the save fail instead of showing it anything.
#if !defined(_WIN32)
namespace
{
// Large enough that writing it takes many reads' worth of time.
constexpr std::size_t kLargeFileBytes = std::size_t{1} << 20;
constexpr std::int32_t kRewrites = 40;

// Reads @p path until @p done, counting the reads that were neither @p first
// nor @p second whole.
std::int32_t CountTornReads(const std::filesystem::path &path, const std::string &first, const std::string &second,
                            const std::atomic<bool> &done)
{
    std::int32_t torn = 0;
    while (!done.load())
    {
        const std::string read = ReadWhole(path);
        if (read != first && read != second)
        {
            ++torn;
        }
    }
    return torn;
}
} // namespace

TEST_CASE("AssetSystem::WriteText never shows a reader a file part written")
{
    // Written in place, the file is cut to nothing and grows back while it is
    // written, which a reader polling it all but certainly catches.
    const std::filesystem::path root = MakeUserRoot("torn");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());
    const std::string first(kLargeFileBytes, 'a');
    const std::string second(kLargeFileBytes, 'b');
    REQUIRE(AssetSystem::WriteText("save.sav", first).has_value());

    std::atomic<bool> done{false};
    std::int32_t torn = 0;
    std::thread reader([&] { torn = CountTornReads(root / "save.sav", first, second, done); });
    for (std::int32_t rewrite = 0; rewrite < kRewrites; ++rewrite)
    {
        REQUIRE(AssetSystem::WriteText("save.sav", rewrite % 2 == 0 ? second : first).has_value());
    }
    done.store(true);
    reader.join();

    CHECK(torn == 0);
}
#endif

TEST_CASE("AssetSystem::ResolveUser rejects paths escaping the user root")
{
    REQUIRE(AssetSystem::SetUserRoot(MakeUserRoot("escape")).has_value());

    const std::expected<std::filesystem::path, AssetError> escaped = AssetSystem::ResolveUser("../outside.txt");
    REQUIRE_FALSE(escaped.has_value());
    CHECK(escaped.error() == AssetErrorCode::InvalidVirtualPath);

    // A write that would escape must fail without touching the filesystem.
    CHECK_FALSE(AssetSystem::WriteText("../outside.txt", "nope").has_value());
}

TEST_CASE("AssetSystem::ReadUserText reports a clean error for a missing file")
{
    REQUIRE(AssetSystem::SetUserRoot(MakeUserRoot("missing")).has_value());

    const std::expected<std::string, AssetError> read = AssetSystem::ReadUserText("does-not-exist.json");
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == AssetErrorCode::FileNotFound);
    CHECK_FALSE(AssetSystem::UserExists("does-not-exist.json"));
}

TEST_CASE("An asset error describes its code, and its detail when it has one")
{
    CHECK(ToString(AssetErrorCode::FileNotFound) == "the file does not exist");
    CHECK(ToString(AssetError{AssetErrorCode::CorruptAsset, "the audio could not be decoded"}) ==
          "the file's contents are corrupt (the audio could not be decoded)");
}

TEST_CASE("AssetSystem::ReadUserText says a path is a directory, not that it is missing")
{
    const std::filesystem::path root = MakeUserRoot("directory");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());
    std::filesystem::create_directories(root / "saves");

    const std::expected<std::string, AssetError> read = AssetSystem::ReadUserText("saves");
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == AssetErrorCode::IsDirectory);
}

TEST_CASE("AssetSystem::ReadUserText says a file it may not open is refused, not missing")
{
    const std::filesystem::path root = MakeUserRoot("permission");
    REQUIRE(AssetSystem::SetUserRoot(root).has_value());
    REQUIRE(AssetSystem::WriteText("locked.json", "{}").has_value());
    std::filesystem::permissions(root / "locked.json", std::filesystem::perms::none);

    // A process that can open it anyway (root, or a filesystem that ignores
    // permissions) has nothing to report.
    if (std::ifstream{root / "locked.json"})
    {
        std::filesystem::permissions(root / "locked.json", std::filesystem::perms::owner_all);
        return;
    }

    const std::expected<std::string, AssetError> read = AssetSystem::ReadUserText("locked.json");
    std::filesystem::permissions(root / "locked.json", std::filesystem::perms::owner_all);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == AssetErrorCode::PermissionDenied);
}

// ---------------------------------------------------------------------------
// Roots that cannot be set.
//
// SetRoot / SetUserRoot are noexcept, so they must use std::filesystem's
// error_code overloads — the throwing ones would make an OS-level failure
// std::terminate rather than a returned error.
//
// **These cases do not reproduce that failure.** A path that merely does not
// exist is not an OS failure: is_directory answers false with a clear
// error_code. The real thing needs permission denied on a parent or an
// unmounted volume, neither portably constructible in a unit test and both
// skipped on CI running as root. What these pin is the contract around it: the
// documented rejection, by value, with nothing escaping.
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
    CHECK(result.error() == AssetErrorCode::InvalidRoot);
}

TEST_CASE("AssetSystem::SetUserRoot refuses a path that does not exist, and does not throw")
{
    const std::filesystem::path missing = TempRoot() / "assisi_no_such_root_9f3a" / "deeper";

    std::expected<void, AssetError> result;
    CHECK_NOTHROW(result = AssetSystem::SetUserRoot(missing));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == AssetErrorCode::InvalidRoot);
}

TEST_CASE("AssetSystem::Exists answers false for an unreadable path rather than throwing")
{
    REQUIRE(AssetSystem::SetRoot(TempRoot()).has_value());

    // A path with an interior component that is a *file*, so the OS reports
    // ENOTDIR when it stats it — an error_code here, where the throwing overload
    // of fs::exists would raise.
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
