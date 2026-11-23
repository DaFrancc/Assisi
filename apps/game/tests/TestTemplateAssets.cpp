/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTemplateAssets.cpp
/// @brief Every asset the first-person template carries is the one the sample
///        game plays.
///
/// The engine reads assets from one folder, so the sample game keeps a copy of
/// each template asset in assets/. Two copies drift the moment someone edits
/// one, and the template would then hand new projects a player nobody has
/// played. This walks the template's files rather than a list, so an asset
/// added to the template is held to the rule without touching this test.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace
{

/// The whole of @p path, or nothing when it cannot be read.
std::optional<std::string> ReadAll(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// Every file under @p root, as paths relative to it.
std::vector<std::filesystem::path> FilesUnder(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(root, error))
    {
        if (entry.is_regular_file())
        {
            files.push_back(std::filesystem::relative(entry.path(), root));
        }
    }
    REQUIRE_FALSE(error);
    return files;
}

} // namespace

TEST_CASE("Every asset the first-person template carries is the sample game's copy, byte for byte")
{
    const std::filesystem::path templateRoot{ASSISI_TEMPLATE_ASSETS};
    const std::filesystem::path gameRoot{ASSISI_GAME_ASSETS};
    const std::vector<std::filesystem::path> files = FilesUnder(templateRoot);

    // The player's blueprint and its animator at least, or the walk found
    // nothing and every check below passed by having nothing to check.
    const auto has = [&files](const char *path)
    { return std::find(files.begin(), files.end(), std::filesystem::path{path}) != files.end(); };
    REQUIRE(has("blueprints/Player.abp"));
    REQUIRE(has("animation/Player.sgl"));

    for (const std::filesystem::path &relative : files)
    {
        CAPTURE(relative.generic_string());
        const std::optional<std::string> fromTemplate = ReadAll(templateRoot / relative);
        const std::optional<std::string> fromGame = ReadAll(gameRoot / relative);
        REQUIRE(fromTemplate.has_value());
        CHECK_MESSAGE(fromGame.has_value(), "assets/", relative.generic_string(),
                      " is missing: copy it from templates/first-person/assets/");
        CHECK_MESSAGE((fromGame.has_value() && *fromGame == *fromTemplate), "assets/", relative.generic_string(),
                      " differs from templates/first-person/assets/", relative.generic_string(),
                      ": copy whichever was edited over the other");
    }
}
