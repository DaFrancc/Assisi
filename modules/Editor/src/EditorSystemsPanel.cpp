/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorSystemsPanel.cpp
/// @brief The Systems panel: which systems the edited file requires, which it
///        inherits from the blueprints placed in it, and which are running.
///
/// Three things sit in one panel because an author reaches for them together.
///
/// The **list** is the file's own — added and removed names are what a save
/// writes back. The **icons** are not saved and cannot be: a blueprint's systems
/// belong to the blueprint's file, so they are worked out again from the
/// instances present every frame. Writing them into the level would merge the two
/// sources beyond telling apart, and then the level would go on demanding them
/// after the instance that brought them was deleted.
///
/// The **tick** is not saved either. It silences a system for the session so its
/// effect can be seen by its absence, and a file that remembered that would be a
/// file missing behaviour nobody asked it to drop.

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Editor/SystemSearch.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::Editor
{
namespace
{

/// How many suggestions the search field offers at once. The same count the
/// Inspector's Add Component field uses — the two read as one control in two
/// places, and a different length would say they were not.
constexpr std::size_t kMaxSuggestions = 8;

/// A required name this build does not declare. Amber rather than red: the file
/// is not corrupt, it was authored against a build that had the system.
constexpr ImVec4 kUndeclaredColor{0.9f, 0.8f, 0.3f, 1.f};

/// Inherited from a blueprint and not asked for by this file.
constexpr ImVec4 kInheritedColor{0.4f, 0.85f, 0.45f, 1.f};
constexpr const char *kInheritedGlyph = "\xef\x84\x91"; // U+F111, a filled circle

/// Asked for by this file *and* inherited — the two sources agreeing.
constexpr ImVec4 kSharedColor{0.95f, 0.8f, 0.25f, 1.f};
constexpr const char *kSharedGlyph = "\xef\x80\x85"; // U+F005, a star

/// Columns of the list: tick, remove, name, provenance. The icon is last and
/// fixed-width behind a stretching name, which is what right-aligns it without
/// any cursor arithmetic.
constexpr int32_t kSystemColumnCount = 4;

/// The provenance glyph, relative to the row's text. Large enough that a star
/// and a circle are told apart at a glance down the column rather than read.
constexpr float kIconScale = 1.5f;

/// Blank kept to the right of the glyph, in multiples of the row's text size, so
/// the icon does not sit against the panel edge. Part of the icon column's fixed
/// width, which leaves the glyph drawn at the column's start and the gap after it.
constexpr float kIconRightPad = 0.25f;

/// Alternating row backgrounds come from the table itself, so the zebra tracks
/// whatever theme is loaded instead of a colour pinned here.
constexpr ImGuiTableFlags kSystemTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

/// @brief One row of the list, as the walk over the catalog worked it out.
struct SystemRow
{
    /// Borrowed from the catalog, the file's own list, or the blueprint tally —
    /// all of which outlive the frame this is drawn in.
    const std::string *name;
    /// How many distinct blueprint files placed in this world name it, directly
    /// or through their own nested instances.
    int32_t blueprintCount;
    /// The edited file asks for this itself, rather than only inheriting it.
    bool inLevel;
    /// This build declares it. False for a name only some other build had.
    bool declared;
    /// The X is live: this file asks for the system itself, and no session is
    /// running. True even when blueprints also require it — removing it there
    /// drops only *this file's* claim, and the row stays behind as inherited.
    /// False for a row nothing but a blueprint asks for: there is nothing of the
    /// file's to take away, and the next spawn would reinstate it regardless.
    bool removable;
};

/// @brief How many distinct blueprint files placed in @p instances require each
/// system, keyed by system name.
///
/// Counts **files, not instances**: ten crates of one blueprint are one
/// blueprint's worth of requirement, which is what an author means by "required
/// by 2 blueprints". Only authored rows count, so a spawn made during play does
/// not change what the file is said to need.
///
/// `BlueprintDefinition::systems` is already the whole nested closure, so a file
/// that instances another contributes that one's systems too without this having
/// to recurse.
// What the blueprints in this level require. Shared with WorldManager::ApplySystems,
// which installs exactly this set on top of the level's own names — the panel and
// the installer have to agree, or a row reads "required" while nothing runs it.
using Assisi::App::BlueprintSystemCounts;

/// @brief Whether the edited file's own list names @p name.
bool Requires(const std::vector<std::string> &required, std::string_view name)
{
    return std::find(required.begin(), required.end(), name) != required.end();
}

/// @brief How many placed blueprints require @p name. Zero for one nothing does.
int32_t InheritedCount(const std::map<std::string, int32_t, std::less<>> &inherited, std::string_view name)
{
    const auto found = inherited.find(name);
    return found != inherited.end() ? found->second : 0;
}

/// @brief Everything the panel knows about @p name, worked out in one place.
///
/// `declared` comes from the catalog rather than from the caller: whether this
/// build has the system is a property of the name, not of which list it was
/// found in, and deriving it here is what keeps the three walks in
/// BuildSystemRows from having to agree about it.
SystemRow MakeSystemRow(const std::string &name, const std::vector<std::string> &required,
                        const std::map<std::string, int32_t, std::less<>> &inherited, bool listEditable)
{
    const int32_t count  = InheritedCount(inherited, name);
    const bool inFile = Requires(required, name);
    return SystemRow{.name           = &name,
                     .blueprintCount = count,
                     .inLevel        = inFile,
                     .declared       = Assisi::App::SystemCatalog::Instance().Find(name) != nullptr,
                     .removable      = listEditable && inFile};
}

/// @brief The list as it should be drawn: every system the edited file asks for
/// or inherits, each row carrying where it came from.
///
/// Declaration order, not the file's and not alphabetical: it is the one order
/// that reads the same in every level, and it does not rearrange itself when a
/// name is added. Names this build does not declare come last — they have no
/// place in that order, and the panel is the only place they can be found.
///
/// Every row borrows its name from @p catalog, @p required or @p inherited, so
/// none of the three may be touched while the result is alive.
std::vector<SystemRow> BuildSystemRows(std::span<const Assisi::App::SystemDefinition> catalog,
                                       const std::vector<std::string> &required,
                                       const std::map<std::string, int32_t, std::less<>> &inherited,
                                       bool listEditable)
{
    std::vector<SystemRow> rows;
    rows.reserve(required.size() + inherited.size());

    for (const Assisi::App::SystemDefinition &definition : catalog)
    {
        if (Requires(required, definition.name) || InheritedCount(inherited, definition.name) > 0)
            rows.push_back(MakeSystemRow(definition.name, required, inherited, listEditable));
    }

    const Assisi::App::SystemCatalog &declarations = Assisi::App::SystemCatalog::Instance();
    for (const std::string &name : required)
    {
        if (declarations.Find(name) == nullptr)
            rows.push_back(MakeSystemRow(name, required, inherited, listEditable));
    }
    for (const auto &[name, count] : inherited)
    {
        if (declarations.Find(name) == nullptr && !Requires(required, name))
            rows.push_back(MakeSystemRow(name, required, inherited, listEditable));
    }
    return rows;
}

/// @brief One row: the session tick, the remove button, the name, and the icon
/// saying where the requirement came from.
///
/// @p selfNoun names the edited file in the tooltip — "this level" or "this
/// blueprint" — because the panel is the same one in both modes and only the
/// wording differs.
///
/// Writes @p pendingRemove rather than removing: the caller is walking the very
/// list a removal would shift, so it applies this after the walk.
void DrawSystemRow(Assisi::App::SystemRegistry &systems, const SystemRow &row, const char *selfNoun,
                   std::string &pendingRemove)
{
    const std::string &name = *row.name;
    ImGui::PushID(name.c_str());
    ImGui::TableNextRow();

    // An undeclared name has no entry to silence, so its tick is dead.
    ImGui::TableSetColumnIndex(0);
    const bool wasEnabled = systems.IsEnabled(name);
    bool enabled    = wasEnabled;
    ImGui::BeginDisabled(!row.declared);
    if (ImGui::Checkbox("##enabled", &enabled))
        systems.SetEnabled(name, enabled);
    ImGui::EndDisabled();
    if (row.declared && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(wasEnabled ? "Running. Unticking stops it for this session only."
                                     : "Stopped for this session. Still saved with the file.");
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::BeginDisabled(!row.removable);
    const bool remove = ImGui::SmallButton("X");
    ImGui::EndDisabled();
    if (remove)
        pendingRemove = name;
    else if (ImGui::IsItemHovered())
    {
        // Three different rows reach this: one the file alone asks for, one it
        // asks for alongside a blueprint (removable — the blueprints go on
        // requiring it, and the row stays, inherited), and one only a blueprint
        // asks for, where there is nothing of the file's to remove.
        if (!row.inLevel)
            ImGui::SetTooltip("Required by a blueprint placed here, not by this file.");
        else if (row.blueprintCount > 0)
            ImGui::SetTooltip("Remove from this file's required systems. Blueprints here still need it.");
        else
            ImGui::SetTooltip("Remove from this file's required systems");
    }

    ImGui::TableSetColumnIndex(2);
    if (row.declared)
        ImGui::TextUnformatted(name.c_str());
    else
        ImGui::TextColored(kUndeclaredColor, "%s (not declared by this build)", name.c_str());

    // Nothing inherited it: the ordinary case, and the one that should stay quiet.
    if (row.blueprintCount == 0)
    {
        ImGui::PopID();
        return;
    }

    ImGui::TableSetColumnIndex(3);
    const char *const plural = row.blueprintCount == 1 ? "blueprint" : "blueprints";

    // Scaled for this one glyph and popped straight after, so an oversized row
    // height cannot leak into the rows below.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kIconScale);
    if (row.inLevel)
        ImGui::TextColored(kSharedColor, "%s", kSharedGlyph);
    else
        ImGui::TextColored(kInheritedColor, "%s", kInheritedGlyph);
    ImGui::PopFont();

    // Hover-tested after the pop: the item's rectangle is the one just drawn, and
    // the tooltip should read at the panel's own size.
    if (ImGui::IsItemHovered())
    {
        if (row.inLevel)
        {
            ImGui::SetTooltip("This system is required by %s and %d %s.", selfNoun, row.blueprintCount,
                              plural);
        }
        else
        {
            ImGui::SetTooltip("This system is required by %d %s.", row.blueprintCount, plural);
        }
    }

    ImGui::PopID();
}

} // namespace

void EditorApp::MarkSystemsEdited()
{
    if (InBlueprintMode())
        _blueprintSystemsEdited = true;
    else
        _systemsEdited = true;
}

void EditorApp::AddRequiredSystem(const std::string &name)
{
    if (_world == nullptr)
        return;

    std::vector<std::string> &required = _world->systemNames;
    if (std::find(required.begin(), required.end(), name) != required.end())
        return;
    required.push_back(name);
    MarkSystemsEdited();

    // Queued, not installed here: registering appends to the phase and invalidates
    // its cached execution order, and this runs from inside the frame's panel walk.
    // The editor drains the queue at the next safe point, before systems run.
    const std::vector<std::string> one{name};
    Assisi::App::QueueSystemInstall(*_world, one, _world->levelPath);
}

void EditorApp::RemoveRequiredSystem(const std::string &name)
{
    if (_world == nullptr)
        return;

    std::vector<std::string> &required = _world->systemNames;
    const auto found = std::find(required.begin(), required.end(), name);
    if (found == required.end())
        return;
    required.erase(found);
    MarkSystemsEdited();

    // A blueprint placed here still needs it, so the file's claim is all that was
    // dropped: the row stays, now inherited, and the system goes on running. Muting
    // it would stop behaviour those blueprints are relying on.
    if (InheritedCount(BlueprintSystemCounts(_world->instances), name) > 0)
        return;

    // Muted rather than unregistered, so the removal is visible immediately. Taking
    // the entry out would rebind every After()/Before() edge that names it, and the
    // mute goes away with the entry the next time this world's systems are applied
    // — by which point the name is no longer in the list that installs them.
    _world->systems.SetEnabled(name, false);
}

void EditorApp::DrawRequiredSystemsWindow()
{
    ImGui::Begin("Systems");

    if (_world == nullptr)
    {
        ImGui::TextDisabled("(no world)");
        ImGui::End();
        return;
    }

    // Inspect-only while a world other than the edited one is shown, as in every
    // panel that writes to a file: the save binds to the edited world alone.
    ImGui::BeginDisabled(!IsEditable());

    // Adding and removing are authoring, and a running session is not where that
    // happens: Stop reinstalls the list the session started with, so a name added
    // here mid-play would vanish with no dirty marker left to explain it. Muting
    // stays live throughout — see DrawSystemRow.
    const bool listEditable = IsEditable() && _playState == PlayState::Editing;

    const std::span<const Assisi::App::SystemDefinition> catalog =
        Assisi::App::SystemCatalog::Instance().All();
    const std::vector<std::string> &required = _world->systemNames;
    const std::map<std::string, int32_t, std::less<>> inherited = BlueprintSystemCounts(_world->instances);

    // --- The search field ----------------------------------------------------

    ImGui::BeginDisabled(!listEditable);
    ImGui::TextUnformatted("Add System");
    ImGui::SetNextItemWidth(-1.f);
    ImGuiSuggestionNav nav;
    const bool entered = ImGui::InputText("##addsystem", _addSystemBuf, sizeof(_addSystemBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue | kImGuiSuggestionNavFlags,
                                          ImGuiSuggestionNavCallback, &nav);

    // Everything this build declares, minus what the file already asks for. An
    // inherited name is still offered: the file does not require it yet, and
    // adding it is how an author says the level needs it in its own right —
    // which is what keeps it after the blueprint that brought it is deleted.
    std::vector<std::string_view> candidates;
    candidates.reserve(catalog.size());
    for (const Assisi::App::SystemDefinition &definition : catalog)
    {
        if (!Requires(required, definition.name))
            candidates.push_back(definition.name);
    }

    const std::vector<SystemSearchHit> hits = RankSystemMatches(candidates, _addSystemBuf);
    const std::size_t shown = std::min(hits.size(), kMaxSuggestions);
    _addSystemSelected = ImGuiAdvanceSuggestion(_addSystemSelected, nav, shown);

    if (_addSystemBuf[0] == '\0')
    {
        // Nothing to offer until the field has text, and nothing to say about it:
        // a line of placeholder here would push the list below down on every frame
        // the field is empty, which is most of them.
    }
    else if (shown == 0)
    {
        ImGui::TextDisabled("(no matching system)");
    }
    else if (entered)
    {
        AddRequiredSystem(std::string(hits[static_cast<std::size_t>(_addSystemSelected)].name));
        _addSystemBuf[0]   = '\0';
        _addSystemSelected = 0;
        // -1 re-focuses the previous widget, the field itself, so a second system
        // can be added without clicking back into it.
        ImGui::SetKeyboardFocusHere(-1);
    }
    else
    {
        for (std::size_t i = 0; i < shown; ++i)
        {
            ImGui::PushID(static_cast<int32_t>(i));
            const std::string label(hits[i].name);
            if (ImGui::Selectable(label.c_str(), static_cast<int32_t>(i) == _addSystemSelected))
            {
                AddRequiredSystem(label);
                _addSystemBuf[0]   = '\0';
                _addSystemSelected = 0;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndDisabled();

    // --- The list ------------------------------------------------------------

    ImGui::Separator();
    ImGui::TextUnformatted("Systems");

    const std::vector<SystemRow> rows = BuildSystemRows(catalog, required, inherited, listEditable);
    if (rows.empty())
        ImGui::TextDisabled("(none)");

    // Collected during the walk and applied after it: removing a name mid-walk
    // would shift the very list the rows are being read from.
    std::string pendingRemove;

    if (ImGui::BeginTable("##systems", kSystemColumnCount, kSystemTableFlags))
    {
        ImGui::TableSetupColumn("##tick", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        // Wide enough for the scaled glyph plus the gap that keeps it off the
        // panel edge. The glyph draws at the column's start, so the surplus is the
        // padding, and it is the same on every row whichever icon lands there.
        const float iconWidth = ImGui::GetFontSize() * (kIconScale + kIconRightPad);
        ImGui::TableSetupColumn("##origin", ImGuiTableColumnFlags_WidthFixed, iconWidth);

        const char *const selfNoun = InBlueprintMode() ? "this blueprint" : "this level";
        for (const SystemRow &row : rows)
            DrawSystemRow(_world->systems, row, selfNoun, pendingRemove);

        ImGui::EndTable();
    }

    if (!pendingRemove.empty())
        RemoveRequiredSystem(pendingRemove);

    ImGui::EndDisabled();
    ImGui::End();
}

} // namespace Assisi::Editor
