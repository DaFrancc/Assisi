/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorMaterialPanel.cpp
/// @brief The Material panel: a reflection-driven property editor for `.amat`
/// assets, and the create / duplicate / rename / delete actions over them.
///
/// The panel holds no per-field state and knows no material parameter by name.
/// It walks MaterialData's reflected field table and hands each field to the
/// same widget renderer the Inspector uses, so a factor added to MaterialData
/// appears here with nothing edited in this file.
///
/// This is the only home for material authoring. The asset browser picks assets
/// to fill a field and offers no way to create one, so choosing a mesh cannot
/// put a "new material" button in front of the author.
///
/// Edits apply to the live scene as they are made, before any save. Which of the
/// two live paths runs is decided by whether a texture channel changed — see
/// ApplyMaterialEdit — because only that invalidates the resolved bindless slots
/// the material's table row carries.

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Editor
{
namespace
{

/// The folder a material goes in when the panel has no open material to take a
/// folder from. Not a lookup path — nothing resolves through it — just where a
/// first material lands so New always has an answer.
constexpr std::string_view kDefaultMaterialDir = "materials";

/// @brief MaterialData's reflected field table, or null if the generated
/// registration was not linked into this build.
const Assisi::Core::Reflect::AssetTypeMeta *MaterialMeta()
{
    return Assisi::Core::Reflect::AssetTypeRegistry::Instance().Find("MaterialData");
}

bool IsMaterialPath(std::string_view vpath)
{
    return vpath.size() > 5 && vpath.substr(vpath.size() - 5) == ".amat";
}

/// @brief The directory part of a virtual path, empty for one at the asset root.
std::string_view DirectoryOf(std::string_view vpath)
{
    const std::string_view::size_type slash = vpath.find_last_of('/');
    return slash == std::string_view::npos ? std::string_view{} : vpath.substr(0, slash);
}

/// @brief The filename of a virtual path without its extension.
std::string StemOf(std::string_view vpath)
{
    return std::filesystem::path(std::string{vpath}).stem().string();
}

} // namespace

void EditorApp::RefreshMaterialList()
{
    _materialList.clear();
    for (const auto &[id, path] : _assetDatabase.Assets())
    {
        if (IsMaterialPath(path))
        {
            _materialList.emplace_back(std::string_view{path});
        }
    }
    std::sort(_materialList.begin(), _materialList.end(),
              [](const Assisi::Core::AssetPath &a, const Assisi::Core::AssetPath &b) { return a.View() < b.View(); });
    _materialListDirty = false;
}

void EditorApp::ApplyAssetState(std::string_view typeName, const Assisi::Core::AssetPath &path,
                                const nlohmann::json &state)
{
    const Assisi::Core::Reflect::AssetTypeMeta *meta =
        Assisi::Core::Reflect::AssetTypeRegistry::Instance().Find(typeName);
    if (meta == nullptr || typeName != "MaterialData")
    {
        return; // MaterialData is the only asset anything edits today
    }

    Assisi::Geometry::MaterialData restored;
    if (!meta->deserialize(state, &restored))
    {
        Assisi::Core::Log::Warn("Material editor: could not replay an edit to '{}'.", path.View());
        return;
    }

    // The panel, when it is that material. Both copies would otherwise disagree
    // with the renderer, and the dirty marker with both.
    if (_materialEditorPath == path)
    {
        _materialEditorData = restored;
    }

    // The renderer, always. Undoing an edit to a material that has since been
    // closed still has to change what is on screen.
    if (const std::optional<Assisi::Core::AssetId> id = _assetDatabase.IdFor(path.View()); id.has_value())
    {
        // The channel ids may have moved, and a replay is rare, so this takes the
        // rebuilding path rather than deciding which half of the contract applies.
        _assetCache.ReloadMaterial(*id, restored);
    }
}

void EditorApp::CommitMaterialGesture()
{
    if (!_materialGestureOpen)
    {
        return;
    }
    _materialGestureOpen = false;

    const Assisi::Core::Reflect::AssetTypeMeta *meta = MaterialMeta();
    Assisi::Editor::EditHistory *history             = ActiveHistory();
    if (meta == nullptr || history == nullptr || _materialEditorPath.Empty())
    {
        return;
    }

    nlohmann::json after = meta->serialize(&_materialEditorData);
    if (after == _materialGestureBefore)
    {
        return; // a drag that ended where it started is not an edit
    }

    Assisi::Editor::Transaction txn;
    txn.label = "Edit " + StemOf(_materialEditorPath.View());
    txn.cmds.push_back(Assisi::Editor::AssetDelta{_materialEditorPath, "MaterialData",
                                                  std::move(_materialGestureBefore), std::move(after)});
    history->Push(std::move(txn));
    _materialGestureBefore = nlohmann::json{};
}

void EditorApp::ClearMaterialPreviewTarget()
{
    EndMaterialPreview(true);
    _materialPreviewEntity      = Assisi::ECS::NullEntity;
    _materialPreviewFieldOffset = 0;
    _materialPreviewSlot        = -1;
}

void EditorApp::OpenMaterialEditorForSlot(std::string_view vpath, Assisi::ECS::Entity entity,
                                          std::size_t fieldOffset, int32_t slot)
{
    OpenMaterialEditor(vpath);
    if (_materialEditorPath.View() != vpath)
    {
        return; // the open failed and said why; do not arm a target for it
    }
    _materialPreviewEntity      = entity;
    _materialPreviewFieldOffset = fieldOffset;
    _materialPreviewSlot        = slot;
}

std::vector<Assisi::Core::AssetId> *EditorApp::MaterialPreviewSlots()
{
    if (_materialPreviewSlot < 0 || _scene == nullptr || !_scene->IsAlive(_materialPreviewEntity))
    {
        return nullptr;
    }
    auto *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(_materialPreviewEntity);
    if (mrc == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<std::vector<Assisi::Core::AssetId> *>(reinterpret_cast<char *>(mrc) +
                                                                  _materialPreviewFieldOffset);
}

void EditorApp::BeginMaterialPreview()
{
    std::vector<Assisi::Core::AssetId> *slots      = MaterialPreviewSlots();
    const std::optional<Assisi::Core::AssetId> id = _assetDatabase.IdFor(_materialEditorPath.View());
    if (slots == nullptr || !id.has_value())
    {
        return;
    }

    _materialPreviewRestore = *slots;
    const std::size_t slot  = static_cast<std::size_t>(_materialPreviewSlot);
    if (slots->size() <= slot)
        slots->resize(slot + 1);
    (*slots)[slot]         = *id;
    _materialPreviewActive = true;

    // Make the working copy resident *before* the resolve goes looking for it.
    // Left to the ordinary path the resolve finds nothing, kicks an async load,
    // and binds the fallback — so the object would sit white until the first edit.
    _assetCache.ReloadMaterial(*id, _materialEditorData);
    ReresolveEntityAssets(_materialPreviewEntity);
}

void EditorApp::EndMaterialPreview(bool restore)
{
    if (!_materialPreviewActive)
    {
        return;
    }
    _materialPreviewActive = false;

    if (restore)
    {
        if (std::vector<Assisi::Core::AssetId> *slots = MaterialPreviewSlots(); slots != nullptr)
        {
            *slots = _materialPreviewRestore;
            ReresolveEntityAssets(_materialPreviewEntity);
        }
    }
    _materialPreviewRestore.clear();
}

void EditorApp::OpenMaterialEditor(std::string_view vpath)
{
    const std::expected<std::string, Assisi::Core::AssetError> text = Assisi::Core::AssetSystem::ReadText(vpath);
    if (!text)
    {
        Assisi::Core::Log::Warn("Material editor: cannot read '{}'.", vpath);
        return;
    }
    const std::expected<Assisi::Geometry::MaterialData, Assisi::Geometry::MaterialFileError> data =
        Assisi::Geometry::DeserializeMaterial(*text);
    if (!data)
    {
        Assisi::Core::Log::Warn("Material editor: '{}' failed to parse ({}).", vpath,
                                Assisi::Geometry::ToString(data.error()));
        return;
    }

    // Close out an edit still in progress on the outgoing material, while the
    // path it belongs to is still the open one.
    CommitMaterialGesture();

    if (!_materialEditorPath.Assign(vpath))
    {
        Assisi::Core::Log::Warn("Material editor: path '{}' is too long to hold.", vpath);
        return;
    }
    _materialEditorData  = *data;
    _materialEditorSaved = *data;
    _materialEditorOpen  = true;

    // No object context by default. OpenMaterialEditorForSlot arms one straight
    // after; everything else (the picker, the browser menu) genuinely has none,
    // and a target left over from a previous material would preview this one
    // onto an unrelated slot.
    ClearMaterialPreviewTarget();

    const std::string stem = StemOf(vpath);
    std::snprintf(_materialEditorNameBuf, sizeof(_materialEditorNameBuf), "%s", stem.c_str());
}

void EditorApp::CloseMaterialEditor()
{
    CommitMaterialGesture();
    ClearMaterialPreviewTarget();
    _materialEditorOpen       = false;
    _materialEditorPath       = {};
    _materialEditorNameBuf[0] = '\0';
}

void EditorApp::ApplyMaterialEdit(bool channelsChanged)
{
    // Before the reconcile pass has minted a sidecar there is no id to address
    // the material by, so nothing is resident to update. Saving and reimporting
    // is what brings it in.
    const std::optional<Assisi::Core::AssetId> id = _assetDatabase.IdFor(_materialEditorPath.View());
    if (!id.has_value())
    {
        return;
    }

    // The cheap path first — a factor edit only rewrites the constants row. It
    // declines when the material is not resident yet, which is the normal state
    // for one just created, so the full build is the fallback rather than an
    // error path.
    if (channelsChanged || !_assetCache.UpdateMaterialFactors(*id, _materialEditorData))
    {
        _assetCache.ReloadMaterial(*id, _materialEditorData);
    }
}

bool EditorApp::SaveOpenMaterial()
{
    if (_materialEditorPath.Empty())
    {
        return false;
    }

    const std::expected<void, Assisi::Geometry::MaterialWriteError> result =
        Assisi::Geometry::SaveMaterial(_materialEditorPath.View(), _materialEditorData);
    if (!result)
    {
        Assisi::Core::Log::Warn("Material editor: could not save '{}' ({}).", _materialEditorPath.View(),
                                Assisi::Geometry::ToString(result.error()));
        return false;
    }

    // Only now is the working copy the saved copy — on a failed write the panel
    // stays dirty, so the unsaved edit is still visibly unsaved.
    _materialEditorSaved = _materialEditorData;
    Assisi::Core::Log::Info("Material editor: saved '{}'.", _materialEditorPath.View());
    return true;
}

bool EditorApp::RenameOpenMaterial(std::string_view stem)
{
    if (_materialEditorPath.Empty() || stem.empty())
    {
        return false;
    }

    const std::string_view dir = DirectoryOf(_materialEditorPath.View());
    const std::string target = (dir.empty() ? std::string{} : std::string{dir} + "/") + std::string{stem} + ".amat";
    if (target == _materialEditorPath.View())
    {
        return true;
    }

    const std::expected<void, Assisi::Geometry::MaterialWriteError> result =
        Assisi::Geometry::RenameMaterial(_materialEditorPath.View(), target);
    if (!result)
    {
        Assisi::Core::Log::Warn("Material editor: cannot rename '{}' to '{}' ({}).", _materialEditorPath.View(),
                                target, Assisi::Geometry::ToString(result.error()));
        return false;
    }

    _materialEditorPath.Assign(target);

    // The database is keyed by path, and so is the asset cache. Reimport
    // re-registers the (unchanged) GUID against the new path; re-resolving the
    // scene then re-points every entity at the cache entry now under that path,
    // without which live edits would stop reaching the renderer.
    ReimportAssets();
    if (_scene != nullptr)
    {
        Assisi::Runtime::ResolveSceneAssets(*_scene, _assetCache, _assetDatabase);
    }
    Assisi::Core::Log::Info("Material editor: renamed to '{}'.", target);
    return true;
}

void EditorApp::CreateMaterial(std::string_view dirVirtualPath, std::string_view stem,
                               const Assisi::Geometry::MaterialData &seed)
{
    const std::string vpath = Assisi::Geometry::UniqueMaterialPath(dirVirtualPath, stem);
    const std::expected<void, Assisi::Geometry::MaterialWriteError> result =
        Assisi::Geometry::SaveMaterial(vpath, seed);
    if (!result)
    {
        Assisi::Core::Log::Warn("Material editor: could not create '{}' ({}).", vpath,
                                Assisi::Geometry::ToString(result.error()));
        return;
    }

    // A material created while authoring for an object is still for that object,
    // so the slot survives the open (which otherwise clears it, having no way to
    // know the new material belongs to the same job).
    const Assisi::ECS::Entity slotEntity = _materialPreviewEntity;
    const std::size_t slotOffset         = _materialPreviewFieldOffset;
    const int32_t slot                   = _materialPreviewSlot;

    // The new file has no `.aast` yet; the reconcile pass inside ReimportAssets
    // mints one, which is what makes the material addressable by GUID and so
    // assignable to a mesh slot. Nothing here mints ids of its own.
    ReimportAssets();
    OpenMaterialEditor(vpath);

    // Preview on by default here, and only here. A material created against a
    // mesh slot has nothing to show it on until it is assigned, so an author who
    // just made one is looking at sliders that move nothing; opening an
    // *existing* material has no such gap and must not silently redress an
    // object.
    if (slot >= 0)
    {
        _materialPreviewEntity      = slotEntity;
        _materialPreviewFieldOffset = slotOffset;
        _materialPreviewSlot        = slot;
        BeginMaterialPreview();
    }
}

void EditorApp::DeleteMaterial(std::string_view vpath)
{
    if (_materialEditorPath.View() == vpath)
    {
        // Close first: the preview points at a material that is about to stop
        // existing, and a slot left holding it would draw the fallback.
        CloseMaterialEditor();
    }

    if (!Assisi::Geometry::DeleteMaterialFile(vpath))
    {
        Assisi::Core::Log::Warn("Material editor: could not delete '{}'.", vpath);
        return;
    }
    Assisi::Core::Log::Info("Material editor: deleted '{}'.", vpath);
    ReimportAssets();
}

void EditorApp::DrawMaterialEditor()
{
    if (!_materialEditorOpen)
    {
        // Dismissed by its own X since the last frame: the preview belongs to the
        // panel being up, so it goes with it.
        EndMaterialPreview(true);
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(440.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material", &_materialEditorOpen))
    {
        ImGui::End();
        return;
    }

    if (_materialListDirty)
        RefreshMaterialList();

    const Assisi::Core::Reflect::AssetTypeMeta *meta = MaterialMeta();
    if (meta == nullptr)
    {
        ImGui::TextDisabled("MaterialData reflection is not linked into this build.");
        ImGui::End();
        return;
    }

    const bool readOnly = IsRestrictedViewer();
    const bool hasOpen  = !_materialEditorPath.Empty();

    // New goes beside the picker rather than in the asset browser: creating a
    // material is authoring, and the browser is where you choose an existing
    // asset — offering "new material" while someone picks a mesh is an action
    // for a different job.
    ImGui::BeginDisabled(readOnly);
    if (ImGui::Button("New"))
    {
        // Alongside the open material, else a default folder — a new material
        // usually belongs with the one being worked on.
        const std::string_view dir =
            hasOpen ? DirectoryOf(_materialEditorPath.View()) : kDefaultMaterialDir;
        CreateMaterial(dir, "Material", Assisi::Geometry::MaterialData{});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasOpen);
    if (ImGui::Button("Duplicate"))
    {
        CreateMaterial(DirectoryOf(_materialEditorPath.View()), StemOf(_materialEditorPath.View()),
                       _materialEditorData);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        DeleteMaterial(std::string{_materialEditorPath.View()});
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (readOnly)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(read-only session)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This viewer shares another editor's asset tree and never writes to it.");
    }

    // The picker. Every `.amat` the database knows, so a material is reachable
    // whether or not anything in the scene references it.
    // View() is not NUL-terminated (a TrivialString is length-counted over a
    // fixed buffer), so ImGui gets an owned string rather than its data pointer.
    const std::string openLabel{_materialEditorPath.View()};
    if (ImGui::BeginCombo("Material", hasOpen ? openLabel.c_str() : "(none)"))
    {
        for (const Assisi::Core::AssetPath &path : _materialList)
        {
            const std::string label{path.View()};
            const bool selected = (path == _materialEditorPath);
            if (ImGui::Selectable(label.c_str(), selected))
                OpenMaterialEditor(path.View());
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        if (_materialList.empty())
            ImGui::TextDisabled("(no materials — press New)");
        ImGui::EndCombo();
    }

    if (!hasOpen)
    {
        ImGui::Separator();
        ImGui::TextDisabled("Pick a material above, or press New.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // One serialize each, reused for the dirty marker and as the "before" of an
    // edit gesture that starts this frame — the field widgets write straight into
    // _materialEditorData, so this is the last chance to see the old value.
    const nlohmann::json frameStart = meta->serialize(&_materialEditorData);
    const bool dirty                = frameStart != meta->serialize(&_materialEditorSaved);
    if (dirty)
    {
        ImGui::TextColored(ImVec4(1.f, 0.82f, 0.4f, 1.f), "Unsaved changes");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Edits are already live in the scene; saving makes them durable.");
    }

    // The name row. A material has no name of its own — the filename is the name
    // — so this renames the file, and the move only happens on the button.
    // Renaming per keystroke would leave a trail of one-letter materials.
    const std::string currentStem = StemOf(_materialEditorPath.View());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputText("##name", _materialEditorNameBuf, sizeof(_materialEditorNameBuf));
    ImGui::SameLine();

    // Resolved every frame while typing, not on the button: a name that cannot
    // be applied has to *look* unavailable before it is pressed. Pressing a live
    // button and having nothing happen reads as the editor being broken.
    const bool nameChanged = _materialEditorNameBuf[0] != '\0' && currentStem != _materialEditorNameBuf;
    const std::string_view dir = DirectoryOf(_materialEditorPath.View());
    const std::string candidate =
        (dir.empty() ? std::string{} : std::string{dir} + "/") + _materialEditorNameBuf + ".amat";
    const bool nameTaken = nameChanged && Assisi::Core::AssetSystem::Exists(candidate);

    ImGui::BeginDisabled(readOnly || !nameChanged || nameTaken);
    if (ImGui::Button("Rename"))
    {
        if (!RenameOpenMaterial(_materialEditorNameBuf))
        {
            // Put the box back to the truth rather than leaving a name that was
            // refused sitting there looking applied.
            std::snprintf(_materialEditorNameBuf, sizeof(_materialEditorNameBuf), "%s", currentStem.c_str());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("Name");
    if (nameTaken)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.f), "'%s' already exists in this folder.",
                           _materialEditorNameBuf);
    }

    ImGui::BeginDisabled(readOnly || !dirty);
    if (ImGui::Button("Save"))
        SaveOpenMaterial();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert"))
    {
        // Undoable like any other edit: a revert that could not itself be undone
        // would be the one destructive button in the panel.
        _materialGestureOpen   = true;
        _materialGestureBefore = frameStart;
        _materialEditorData    = _materialEditorSaved;
        // Channels may have moved back, so take the rebuilding path rather than
        // reasoning about which fields the revert touched.
        ApplyMaterialEdit(true);
        CommitMaterialGesture();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close"))
    {
        CloseMaterialEditor();
        ImGui::End();
        return;
    }

    // Live preview: put this material on the slot the panel was opened from, so
    // a material that is not assigned to anything yet still has something on
    // screen to judge. Provisional — unticking puts the slot back.
    const bool hasTarget = MaterialPreviewSlots() != nullptr;
    ImGui::BeginDisabled(!hasTarget);
    bool previewOn = _materialPreviewActive;
    if (ImGui::Checkbox("Live preview", &previewOn))
    {
        if (previewOn)
            BeginMaterialPreview();
        else
            EndMaterialPreview(true);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (hasTarget)
            ImGui::SetTooltip("Show this material on %s, slot %d while you edit.\nUnticking puts the slot back; "
                              "pick the material in the browser to assign it for real.",
                              DescribeEntity(_materialPreviewEntity).c_str(), _materialPreviewSlot);
        else
            ImGui::SetTooltip("Open a material from a mesh's material slot to preview it on that mesh.");
    }

    ImGui::Separator();

    // The whole editor: one pass over the reflected fields. A texture channel is
    // the only kind that needs anything of its own here — a browse button whose
    // target is this panel — and everything else is decided by the field's type.
    bool edited          = false;
    bool channelsChanged = false;
    for (const Assisi::Core::Reflect::FieldMeta &field : meta->fields)
    {
        if (field.transient)
            continue;

        void *fp = reinterpret_cast<char *>(&_materialEditorData) + field.offset;
        ImGui::PushID(field.name.c_str());

        if (field.type == Assisi::Core::Reflect::FieldType::AssetId)
        {
            const std::string inputId = "##" + field.name;
            const bool rowEdited = AssetIdPathField(inputId.c_str(), *static_cast<Assisi::Core::AssetId *>(fp));
            ImGui::SameLine();
            if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0.f)))
                OpenAssetBrowserForMaterialField(field.offset);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse textures. Clear the path to make the channel factor-only.");
            ImGui::SameLine();
            ImGui::TextUnformatted(field.name.c_str());
            if (rowEdited)
            {
                edited          = true;
                channelsChanged = true;
            }
        }
        else if (EditFieldValue(fp, field))
        {
            edited = true;
        }

        ImGui::PopID();
    }

    if (edited)
    {
        // Open the gesture on the first edited frame, against the value as it was
        // before this frame's widgets ran. Held open through the drag; the branch
        // below closes it once nothing is being manipulated.
        if (!_materialGestureOpen)
        {
            _materialGestureOpen   = true;
            _materialGestureBefore = frameStart;
        }
        ApplyMaterialEdit(channelsChanged);
    }
    else if (_materialGestureOpen && !ImGui::IsAnyItemActive())
    {
        CommitMaterialGesture();
    }

    ImGui::End();
}

void EditorApp::OpenAssetBrowserForMaterialField(std::size_t fieldOffset)
{
    _assetBrowserOpen         = true;
    _assetBrowserTarget       = AssetBrowserTarget::MaterialField;
    _assetBrowserFilter       = AssetBrowserFilter::Textures;
    _assetBrowserMeta         = nullptr; // no component is involved
    _assetBrowserFieldOffset  = fieldOffset;
    _assetBrowserMaterialPath = _materialEditorPath;
    _assetBrowserVectorSlot   = -1;
    _assetBrowserDir.clear();
    _assetBrowserDirty = true;
}

} // namespace Assisi::Editor
