/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/TextRules.hpp>

#include <Assisi/Core/Reflect/AssetDocument.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace Assisi::Mondrian::Import
{

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
        return std::unexpected(MarkupError{.message = "the UI settings do not read (" +
                                                      std::string{Core::Reflect::ToString(applied.error())} + ").",
                                           .file = std::string{kUiConfigPath}});
    }
    return config;
}

EmptyText EmptyTextFor(const UiConfig &config)
{
    return config.allowEmptyStrings ? EmptyText::Allow : EmptyText::Refuse;
}

std::expected<TextRules, MarkupError> LoadTextRules(const SourceReader &read)
{
    const std::expected<UiConfig, MarkupError> config = LoadUiConfig(read);
    if (!config)
    {
        return std::unexpected(config.error());
    }

    TextRules rules;
    rules.literals = config->requireStringKeys ? LiteralText::RequiresKey : LiteralText::Allowed;

    // Which path each name came from, so a second table with the name can say
    // which two collide.
    std::unordered_map<std::string, std::string> pathOf;
    for (const Core::AssetPath &listed : config->stringTables)
    {
        const std::string vpath{listed.View()};
        const std::string name{TableName(vpath)};
        if (const std::unordered_map<std::string, std::string>::const_iterator taken = pathOf.find(name);
            taken != pathOf.end())
        {
            return std::unexpected(MarkupError{.message = "'" + taken->second + "' and '" + vpath +
                                                          "' are both the table '" + name +
                                                          "', so a key naming it could mean either. Rename one.",
                                               .file = std::string{kUiConfigPath}});
        }
        pathOf.emplace(name, vpath);

        const std::expected<std::string, std::string> text = read(vpath);
        if (!text)
        {
            return std::unexpected(MarkupError{.message = "'" + vpath +
                                                          "' is listed as a string table and could not "
                                                          "be read (" +
                                                          text.error() + ").",
                                               .file = std::string{kUiConfigPath}});
        }
        std::expected<StringTable, MarkupError> table = CompileStringTable(*text, EmptyTextFor(*config));
        if (!table)
        {
            MarkupError error = table.error();
            error.file = vpath;
            return std::unexpected(std::move(error));
        }
        rules.tables.byName.emplace(name, std::move(*table));
    }
    return rules;
}

} // namespace Assisi::Mondrian::Import
