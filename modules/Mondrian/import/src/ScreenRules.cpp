/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/ScreenRules.hpp>

#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// An error about the UI settings, which list the file it is about.
MarkupError InSettings(std::string message)
{
    return MarkupError{.message = std::move(message), .file = std::string{kUiConfigPath}};
}

/// Remembers that @p vpath is the @p kind called @p name, refusing a second
/// file with the name: a reference naming it could mean either.
std::expected<void, MarkupError> ClaimName(std::unordered_map<std::string, std::string> &pathOf,
                                           std::string_view kind, std::string_view name, std::string_view vpath)
{
    if (const std::unordered_map<std::string, std::string>::const_iterator taken = pathOf.find(std::string{name});
        taken != pathOf.end())
    {
        return std::unexpected(InSettings("'" + taken->second + "' and '" + std::string{vpath} + "' are both the " +
                                          std::string{kind} + " '" + std::string{name} +
                                          "', so a name using it could mean either. Rename one."));
    }
    pathOf.emplace(std::string{name}, std::string{vpath});
    return {};
}

std::expected<void, MarkupError> LoadTables(const UiConfig &config, const SourceReader &read, ScreenRules &rules)
{
    std::unordered_map<std::string, std::string> pathOf;
    for (const Core::AssetPath &listed : config.stringTables)
    {
        const std::string vpath{listed.View()};
        const std::string_view name = TableName(vpath);
        if (const std::expected<void, MarkupError> claimed = ClaimName(pathOf, "table", name, vpath); !claimed)
        {
            return claimed;
        }
        const std::expected<std::string, std::string> text = read(vpath);
        if (!text)
        {
            return std::unexpected(
                InSettings(std::format("'{}' is listed as a string table and could not be read ({}).", vpath,
                                       text.error())));
        }
        std::expected<StringTable, MarkupError> table = CompileStringTable(*text, EmptyTextFor(config));
        if (!table)
        {
            MarkupError error = table.error();
            error.file = vpath;
            return std::unexpected(std::move(error));
        }
        rules.tables.byName.emplace(std::string{name}, std::move(*table));
    }
    return {};
}

std::expected<void, MarkupError> LoadThemes(const UiConfig &config, const SourceReader &read, ScreenRules &rules)
{
    for (const Core::AssetPath &listed : config.themes)
    {
        const std::string vpath{listed.View()};
        const std::expected<std::string, std::string> text = read(vpath);
        if (!text)
        {
            return std::unexpected(
                InSettings("'" + vpath + "' is listed as a theme and could not be read (" + text.error() + ")."));
        }
        std::expected<CompiledTheme, MarkupError> theme = CompileThemeSource(*text);
        if (!theme)
        {
            MarkupError error = theme.error();
            error.file = vpath;
            return std::unexpected(std::move(error));
        }
        rules.themes.push_back(ProjectTheme{.vpath = vpath, .compiled = std::move(*theme)});
    }
    return {};
}

std::expected<void, MarkupError> LoadSheets(const UiConfig &config, const SourceReader &read, ScreenRules &rules)
{
    std::unordered_map<std::string, std::string> pathOf;
    for (const Core::AssetPath &listed : config.spriteSheets)
    {
        const std::string vpath{listed.View()};
        if (const std::expected<void, MarkupError> claimed = ClaimName(pathOf, "sheet", SheetName(vpath), vpath);
            !claimed)
        {
            return claimed;
        }
        const std::expected<std::string, std::string> text = read(vpath);
        if (!text)
        {
            return std::unexpected(InSettings("'" + vpath + "' is listed as a sprite sheet and could not be read (" +
                                              text.error() + ")."));
        }
        std::expected<SpriteSheetDocument, MarkupError> sheet = CompileSpriteSheetSource(*text, read);
        if (!sheet)
        {
            MarkupError error = sheet.error();
            error.file = vpath;
            return std::unexpected(std::move(error));
        }
        rules.sheets.push_back(ProjectSheet{.vpath = vpath, .document = std::move(*sheet)});
    }
    return {};
}

/// Whether the background image @p look writes itself, if it writes one, is a
/// sprite @p rules has. @p where says where the look is, for the message.
std::expected<void, MarkupError> CheckLookSprite(const ScreenRules &rules, const ThemedPatch &look,
                                                 std::string_view where, std::string_view vpath)
{
    const bool tokened = std::ranges::any_of(
        look.tokens, [](const TokenUse &use) { return use.property == StyleProperty::BackgroundImage; });
    if (!look.patch.Writes(StyleProperty::BackgroundImage) || tokened)
    {
        return {};
    }
    const std::string_view sprite = look.patch.values.backgroundImage.View();
    if (const std::expected<void, std::string> checked = CheckSprite(rules, sprite); !sprite.empty() && !checked)
    {
        return std::unexpected(
            MarkupError{.message = std::string{where} + " " + checked.error(), .file = std::string{vpath}});
    }
    return {};
}

/// Whether every sprite each listed theme names, in a token's value in any
/// mode or in a style's look, is in a listed sheet: the game may wear any of
/// them.
std::expected<void, MarkupError> CheckThemeSprites(const ScreenRules &rules)
{
    for (const ProjectTheme &theme : rules.themes)
    {
        const ThemeDocument &document = theme.compiled.document;
        for (const ThemeToken &token : document.tokens)
        {
            for (std::size_t mode = 0; mode < token.values.size(); ++mode)
            {
                const ThemedPatch value{.patch = token.values[mode], .tokens = {}};
                const std::string where =
                    std::format("the token ${}, in the mode '{}',", token.name.View(), document.modes[mode].View());
                if (const std::expected<void, MarkupError> checked = CheckLookSprite(rules, value, where, theme.vpath);
                    !checked)
                {
                    return checked;
                }
            }
        }
        for (const ThemeStyle &style : document.styles)
        {
            const std::string where = "the style '" + std::string{style.name.View()} + "'";
            if (const std::expected<void, MarkupError> checked =
                    CheckLookSprite(rules, style.resting, where, theme.vpath);
                !checked)
            {
                return checked;
            }
            for (const ThemedPatch &state : style.states)
            {
                if (const std::expected<void, MarkupError> checked = CheckLookSprite(rules, state, where, theme.vpath);
                    !checked)
                {
                    return checked;
                }
            }
        }
    }
    return {};
}

} // namespace

std::expected<UiConfig, MarkupError> LoadUiConfig(const SourceReader &read)
{
    UiConfig config;
    const std::expected<std::string, std::string> text = read(kUiConfigPath);
    if (!text)
    {
        return config;
    }
    const std::expected<void, Core::Reflect::AssetDocumentError> applied =
        Core::Reflect::ApplyAssetDocument(*text, config);
    if (!applied)
    {
        return std::unexpected(InSettings("the UI settings do not read (" +
                                          std::string{Core::Reflect::ToString(applied.error())} + ")."));
    }
    return config;
}

EmptyText EmptyTextFor(const UiConfig &config)
{
    return config.allowEmptyStrings ? EmptyText::Allow : EmptyText::Refuse;
}

std::expected<void, std::string> CheckSprite(const ScreenRules &rules, std::string_view qualified)
{
    const std::optional<TableKey> split = SplitKey(qualified);
    if (!split)
    {
        return std::unexpected("names '" + std::string{qualified} +
                               "', which is no sprite. Write the sheet before the sprite, such as icons:heart.");
    }
    for (const ProjectSheet &sheet : rules.sheets)
    {
        if (SheetName(sheet.vpath) != split->table)
        {
            continue;
        }
        if (sheet.document.Find(split->key) == nullptr)
        {
            return std::unexpected("names the sprite '" + std::string{qualified} + "', and the sheet '" +
                                   sheet.vpath + "' has no sprite '" + std::string{split->key} + "'.");
        }
        return {};
    }
    return std::unexpected("names the sprite '" + std::string{qualified} + "', and there is no sprite sheet '" +
                           std::string{split->table} + "'. List its file in " + std::string{kUiConfigPath} +
                           " under \"spriteSheets\".");
}

std::expected<ScreenRules, MarkupError> LoadScreenRules(const SourceReader &read)
{
    const std::expected<UiConfig, MarkupError> config = LoadUiConfig(read);
    if (!config)
    {
        return std::unexpected(config.error());
    }

    ScreenRules rules;
    rules.read = read;
    // The components this program reflects, which in the cook and the editor
    // are every component the game has.
    rules.components = [](std::string_view name) { return Core::Reflect::ComponentRegistry::Instance().Find(name); };
    rules.literals = config->requireStringKeys ? LiteralText::RequiresKey : LiteralText::Allowed;
    if (const std::expected<void, MarkupError> tables = LoadTables(*config, read, rules); !tables)
    {
        return std::unexpected(tables.error());
    }
    if (const std::expected<void, MarkupError> themes = LoadThemes(*config, read, rules); !themes)
    {
        return std::unexpected(themes.error());
    }
    if (const std::expected<void, MarkupError> sheets = LoadSheets(*config, read, rules); !sheets)
    {
        return std::unexpected(sheets.error());
    }
    if (const std::expected<void, MarkupError> sprites = CheckThemeSprites(rules); !sprites)
    {
        return std::unexpected(sprites.error());
    }
    return rules;
}

} // namespace Assisi::Mondrian::Import
