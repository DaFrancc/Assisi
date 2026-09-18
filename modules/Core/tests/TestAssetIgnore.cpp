/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <Assisi/Core/AssetIgnore.hpp>

using namespace Assisi::Core;

namespace
{
namespace fs = std::filesystem;

void WriteIgnoreFile(const fs::path &path, std::string_view contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// A list holding one ignore file's rules, as if it sat in @p baseDir.
AssetIgnoreList ListOf(std::string_view text, std::string_view baseDir = "")
{
    AssetIgnoreList list;
    list.AddRules(text, baseDir);
    return list;
}

/// A fresh temp asset root, emptied first so a previous run leaves nothing behind.
fs::path MakeRoot()
{
    const fs::path root = fs::temp_directory_path() / "assisi_assetignore_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}
} // namespace

TEST_CASE("An unanchored pattern matches a name at any depth")
{
    const AssetIgnoreList list = ListOf("*.zip\n");

    CHECK(list.IsFileIgnored("a.zip"));
    CHECK(list.IsFileIgnored("models/a.zip"));
    CHECK(list.IsFileIgnored("models/Coffee_Machine/a.zip"));
    CHECK_FALSE(list.IsFileIgnored("models/a.gltf"));
}

TEST_CASE("A pattern containing a slash is anchored, and * does not cross one")
{
    const AssetIgnoreList list = ListOf("models/*.zip\n");

    CHECK(list.IsFileIgnored("models/a.zip"));
    // Anchored: only directly under the ignore file's own directory.
    CHECK_FALSE(list.IsFileIgnored("x/models/a.zip"));
    // '*' stops at a separator, so it cannot reach into a subdirectory.
    CHECK_FALSE(list.IsFileIgnored("models/sub/a.zip"));
}

TEST_CASE("A trailing slash restricts a rule to directories, and reaches what is under them")
{
    const AssetIgnoreList list = ListOf("editor/\n");

    CHECK(list.IsDirectoryIgnored("editor"));
    CHECK(list.IsDirectoryIgnored("editor/shaders"));
    CHECK(list.IsFileIgnored("editor/entity_icon.png"));
    CHECK(list.IsFileIgnored("editor/shaders/line.vert"));
    // A regular file that happens to be named "editor" is not a directory.
    CHECK_FALSE(list.IsFileIgnored("editor"));
    CHECK_FALSE(list.IsFileIgnored("textures/hello.png"));
}

TEST_CASE("A negation re-includes one file inside an excluded directory")
{
    const AssetIgnoreList list = ListOf("/editor/\n"
                                        "!/editor/loading/Spinner.webp\n");

    // The whole directory is out...
    CHECK(list.IsFileIgnored("editor/entity_icon.png"));
    CHECK(list.IsFileIgnored("editor/loading/Spinner.ttf"));
    // ...except the one file a later rule names. Git cannot express this, because
    // it prunes the excluded directory and never reaches the negation.
    CHECK_FALSE(list.IsFileIgnored("editor/loading/Spinner.webp"));
}

TEST_CASE("The last matching rule decides, so rule order is load-bearing")
{
    // The same two rules the other way round: the exclusion now comes last and
    // takes the file back.
    const AssetIgnoreList list = ListOf("!/editor/loading/Spinner.webp\n"
                                        "/editor/\n");

    CHECK(list.IsFileIgnored("editor/loading/Spinner.webp"));
}

TEST_CASE("A ** segment matches zero or more path segments")
{
    const AssetIgnoreList doubleStar = ListOf("a/**/b\n");
    CHECK(doubleStar.IsFileIgnored("a/b"));
    CHECK(doubleStar.IsFileIgnored("a/x/b"));
    CHECK(doubleStar.IsFileIgnored("a/x/y/b"));
    CHECK_FALSE(doubleStar.IsFileIgnored("a/x/y/c"));
    CHECK_FALSE(doubleStar.IsFileIgnored("z/a/b"));

    const AssetIgnoreList scratch = ListOf("**/scratch/**\n");
    CHECK(scratch.IsFileIgnored("m/scratch/f"));
    CHECK(scratch.IsFileIgnored("scratch/f"));
    CHECK(scratch.IsFileIgnored("m/n/scratch/o/f"));
    CHECK_FALSE(scratch.IsFileIgnored("m/scratchy/f"));
}

TEST_CASE("A ? matches exactly one character and never a separator")
{
    const AssetIgnoreList list = ListOf("foo?\n");

    CHECK(list.IsFileIgnored("fooa"));
    CHECK_FALSE(list.IsFileIgnored("foo"));
    CHECK_FALSE(list.IsFileIgnored("fooab"));
    // "foo/x" must not be read as "foo" + one character.
    CHECK_FALSE(list.IsFileIgnored("foo/x"));
}

TEST_CASE("An ignore file is always ignored, whatever it says")
{
    const AssetIgnoreList empty;
    REQUIRE(empty.RuleCount() == 0);

    // Minting a sidecar for the list would make the list itself an asset.
    CHECK(empty.IsFileIgnored(".assisiignore"));
    CHECK(empty.IsFileIgnored("models/.assisiignore"));
    // Nothing else is ignored by an empty list.
    CHECK_FALSE(empty.IsFileIgnored("models/a.gltf"));
}

TEST_CASE("Blank lines, comments and trailing whitespace produce no rules")
{
    const AssetIgnoreList list = ListOf("\n"
                                        "# a comment\n"
                                        "   \n"
                                        "\t\n");

    CHECK(list.RuleCount() == 0);
    CHECK_FALSE(list.IsFileIgnored("a.zip"));
}

TEST_CASE("Trailing whitespace is stripped from a pattern")
{
    const AssetIgnoreList list = ListOf("*.zip   \n");

    CHECK(list.RuleCount() == 1);
    CHECK(list.IsFileIgnored("models/a.zip"));
}

TEST_CASE("A line using an unsupported escape or character class is dropped")
{
    // Dropped rather than misread: a rule that silently matched nothing would
    // leave the author believing a file was excluded when it was being minted.
    const AssetIgnoreList list = ListOf("*.[oa]\n"
                                        "\\#literal\n");

    CHECK(list.RuleCount() == 0);
    CHECK_FALSE(list.IsFileIgnored("a.o"));
}

TEST_CASE("A rule never reaches outside the directory its ignore file sits in")
{
    const AssetIgnoreList list = ListOf("*.blend\n", "models");

    CHECK(list.IsFileIgnored("models/a.blend"));
    CHECK(list.IsFileIgnored("models/sub/a.blend"));
    CHECK_FALSE(list.IsFileIgnored("textures/a.blend"));
    CHECK_FALSE(list.IsFileIgnored("a.blend"));
}

TEST_CASE("Load collects nested ignore files, deepest last so it wins")
{
    const fs::path root = MakeRoot();
    WriteIgnoreFile(root / ".assisiignore", "*.blend\n");
    WriteIgnoreFile(root / "models" / ".assisiignore", "!*.blend\n");

    const AssetIgnoreList list = AssetIgnoreList::Load(root);
    REQUIRE(list.RuleCount() == 2);

    // The nested file re-includes what the root file excluded, but only beneath
    // itself — which is the whole point of nesting.
    CHECK_FALSE(list.IsFileIgnored("models/a.blend"));
    CHECK(list.IsFileIgnored("textures/a.blend"));
}

TEST_CASE("Load on a tree with no ignore file ignores nothing")
{
    const fs::path root = MakeRoot();
    fs::create_directories(root / "textures");

    const AssetIgnoreList list = AssetIgnoreList::Load(root);

    CHECK(list.RuleCount() == 0);
    CHECK_FALSE(list.IsFileIgnored("textures/crate.png"));
}
