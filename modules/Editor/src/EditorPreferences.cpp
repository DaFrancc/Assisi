/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorPreferences.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>

#include <nlohmann/json.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Errors.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::Editor
{
namespace
{

/// The file, in the user root beside options.json.
constexpr std::string_view kPreferencesFile = "editor.json";

/// Spaces per level of indent in the written file, which is meant to be read.
constexpr std::int32_t kIndent = 2;

/// Channels in a colour: red, green, blue and opacity.
constexpr std::size_t kColorChannels = 4;

void ReadColor(const nlohmann::json &splines, glm::vec4 &color)
{
    const nlohmann::json::const_iterator found = splines.find("color");
    if (found == splines.end())
    {
        return;
    }
    if (!found->is_array() || found->size() != kColorChannels ||
        !std::all_of(found->begin(), found->end(), [](const nlohmann::json &channel) { return channel.is_number(); }))
    {
        Core::Log::Warn("{}: splines.color is not four numbers; keeping the default.", kPreferencesFile);
        return;
    }
    for (std::size_t i = 0; i < kColorChannels; ++i)
    {
        color[static_cast<glm::length_t>(i)] = std::clamp((*found)[i].get<float>(), 0.f, 1.f);
    }
}

void ReadThickness(const nlohmann::json &splines, float &thickness)
{
    const nlohmann::json::const_iterator found = splines.find("thickness");
    if (found == splines.end())
    {
        return;
    }
    if (!found->is_number())
    {
        Core::Log::Warn("{}: splines.thickness is not a number; keeping the default.", kPreferencesFile);
        return;
    }
    thickness = std::clamp(found->get<float>(), kMinSplineThickness, kMaxSplineThickness);
}

} // namespace

EditorPreferences ParseEditorPreferences(std::string_view text)
{
    EditorPreferences preferences;
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/ false);
    if (document.is_discarded() || !document.is_object())
    {
        Core::Log::Warn("{} is not a JSON object; using the defaults.", kPreferencesFile);
        return preferences;
    }

    const nlohmann::json::const_iterator splines = document.find("splines");
    if (splines != document.end() && splines->is_object())
    {
        ReadColor(*splines, preferences.splines.color);
        ReadThickness(*splines, preferences.splines.thickness);
    }
    return preferences;
}

std::string FormatEditorPreferences(const EditorPreferences &preferences)
{
    const glm::vec4 &color = preferences.splines.color;
    const nlohmann::json document = {
        {"splines",
         {{"color", {color.r, color.g, color.b, color.a}}, {"thickness", preferences.splines.thickness}}}};
    return document.dump(kIndent) + "\n";
}

EditorPreferences LoadEditorPreferences()
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadUserText(kPreferencesFile);
    return text ? ParseEditorPreferences(*text) : EditorPreferences{};
}

void SaveEditorPreferences(const EditorPreferences &preferences)
{
    const std::expected<void, Core::AssetError> result =
        Core::AssetSystem::WriteText(kPreferencesFile, FormatEditorPreferences(preferences));
    if (!result)
    {
        Core::Log::Warn("Could not write {} ({}).", kPreferencesFile, Core::ToString(result.error()));
    }
}

} // namespace Assisi::Editor
