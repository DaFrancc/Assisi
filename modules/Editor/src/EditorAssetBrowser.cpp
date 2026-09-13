/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Assisi::Editor
{

namespace
{
// Defined in the anonymous namespace below; declared here so the hello window,
// which precedes that block, can play the spinner too.
void DrawLoadingFrame(const ImVec2 &origin, float size);
bool LoadingSpinnerAvailable();
} // namespace

void EditorApp::DrawHelloImageWindow()
{
    if (ImGui::Begin("hello.png"))
    {
        if (_helloTexture.IsValid())
        {
            const ImTextureID id = Assisi::Debug::DebugUI::GetOrCreateTextureId(_helloTexture.NativeTexture());
            ImGui::Image(id, ImVec2(256.f, 256.f));

            if (LoadingSpinnerAvailable())
            {
                constexpr float kSpin  = 224.f; // nearly fills the 256-wide image column
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const ImVec2 origin(cursor.x + (256.f - kSpin) * 0.5f, cursor.y);
                ImGui::Dummy(ImVec2(256.f, kSpin)); // advance the cursor past the hand-drawn row
                DrawLoadingFrame(origin, kSpin);
            }
        }
        else
        {
            ImGui::TextDisabled("textures/hello.png failed to load.");
        }
    }
    ImGui::End();
}

namespace
{
/// @brief Lowercased extension of @p path (including the leading dot), e.g. ".glb".
std::string LowerExtension(const std::filesystem::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// @brief True for image extensions stb_image (Texture::LoadFromAssets) decodes.
bool IsThumbnailableImage(const std::filesystem::path &path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

bool IsMaterialFile(const std::filesystem::path &path)
{
    return LowerExtension(path) == ".amat";
}

/// @brief True for mesh-file extensions Geometry::ImportMesh can load.
bool IsMeshFile(const std::filesystem::path &path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".glb" || ext == ".gltf";
}

/// @brief Paints a tabbed folder glyph filling a @p size square at screen-space
/// @p origin. Every tile icon here is drawn, so the browser needs no icon font
/// and no image assets of its own.
void DrawFolderIcon(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const ImU32 body     = IM_COL32(236, 202, 122, 255);
    const ImU32 tab      = IM_COL32(212, 178, 96, 255);
    const float left     = origin.x + size * 0.16f;
    const float right    = origin.x + size * 0.84f;
    const float tabTop   = origin.y + size * 0.30f;
    const float bodyTop  = origin.y + size * 0.42f;
    const float bottom   = origin.y + size * 0.84f;
    const float tabRight = left + (right - left) * 0.45f;
    const float rounding = size * 0.03f;

    drawList->AddRectFilled(ImVec2(left, tabTop), ImVec2(tabRight, bodyTop + rounding), tab, rounding);
    drawList->AddRectFilled(ImVec2(left, bodyTop), ImVec2(right, bottom), body, rounding);
}

/// @brief Paints an isometric cube filling a @p size square at @p origin — the
/// mesh-file tile. Cool tones, against the folder's amber.
void DrawMeshIcon(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const float centerX = origin.x + size * 0.5f;
    const float centerY = origin.y + size * 0.5f;
    const float halfW   = size * 0.26f;
    const float halfH   = size * 0.30f;

    // A hexagonal silhouette plus its centre: three quads, three visible faces.
    const ImVec2 top(centerX, centerY - halfH);
    const ImVec2 upperRight(centerX + halfW, centerY - halfH * 0.5f);
    const ImVec2 lowerRight(centerX + halfW, centerY + halfH * 0.5f);
    const ImVec2 bottom(centerX, centerY + halfH);
    const ImVec2 lowerLeft(centerX - halfW, centerY + halfH * 0.5f);
    const ImVec2 upperLeft(centerX - halfW, centerY - halfH * 0.5f);
    const ImVec2 center(centerX, centerY);

    const ImU32 topFace   = IM_COL32(150, 172, 204, 255);
    const ImU32 rightFace = IM_COL32(92, 112, 142, 255);
    const ImU32 leftFace  = IM_COL32(62, 78, 104, 255);

    drawList->AddQuadFilled(top, upperRight, center, upperLeft, topFace);
    drawList->AddQuadFilled(upperRight, lowerRight, bottom, center, rightFace);
    drawList->AddQuadFilled(upperLeft, center, bottom, lowerLeft, leftFace);
}

/// @brief Paints a shaded sphere filling a @p size square at @p origin — the
/// .amat tile, following the usual material-preview convention. Warm tones,
/// against the mesh cube's cool ones.
void DrawMaterialIcon(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float radius = size * 0.30f;

    // Base disc, then a smaller offset highlight disc for a lit-sphere read.
    drawList->AddCircleFilled(center, radius, IM_COL32(150, 120, 96, 255), 32);
    drawList->AddCircleFilled(ImVec2(center.x - radius * 0.30f, center.y - radius * 0.30f), radius * 0.55f,
                              IM_COL32(214, 188, 150, 255), 32);
}

// Thumbnail loading spinner. Two interchangeable backends, chosen at compile time
// by DebugUI::kUseWebpSpinner (flip that one bool and rebuild):
//   * WebP — an animated .webp decoded to one texture per frame (DebugUI::LoadingWebpFrame).
//   * TTF  — a font whose consecutive glyphs are the frames (DebugUI::LoadingFont).
// Both drive a time-based counter at kLoadingFps and loop; the frame count comes
// from the active backend — the WebP's decoded frames, or the TTF range below.
// Re-authoring the TTF means updating both constants below: the frame count, and
// the first frame's codepoint ('a' for an a..z sequence, or a Private-Use one).
constexpr uint32_t kTtfFirstFrame = 0xF000;     // Spinner.ttf frames: U+F000..U+F12B
constexpr int32_t kTtfFrameCount = 300;         // one full spin loop; must match the font
constexpr double kLoadingFps    = 30.0;         // 300 frames / 30fps = 10 s per loop.
                                                // Drives BOTH backends -- the WebP path ignores the
                                                // durations baked into the file -- so it must match
                                                // whichever spinner asset is shipped.
                                                // double: its only use multiplies ImGui::GetTime(), which is double.

/// @brief Is the active backend's spinner loaded and drawable? Gate on this
/// before reserving a spinner row, and before calling DrawLoadingFrame.
bool LoadingSpinnerAvailable()
{
    return Assisi::Debug::DebugUI::kUseWebpSpinner ? Assisi::Debug::DebugUI::LoadingWebpFrameCount() > 0
                                                   : Assisi::Debug::DebugUI::LoadingFont() != nullptr;
}

/// @brief Draws the current WebP spinner frame as a textured quad, centred and
/// inset to 85% of a @p size square at @p origin (matching the TTF glyph).
///
/// **Requires LoadingSpinnerAvailable()** — it divides by the frame count.
void DrawWebpLoadingFrame(const ImVec2 &origin, float size)
{
    const std::size_t frameCount = Assisi::Debug::DebugUI::LoadingWebpFrameCount();
    const std::size_t frame      = static_cast<std::size_t>(ImGui::GetTime() * kLoadingFps) % frameCount;
    const ImTextureID id         = Assisi::Debug::DebugUI::LoadingWebpFrame(frame);
    if (id == ImTextureID_Invalid)
        return;

    const float quad = size * 0.85f;
    const ImVec2 pos(origin.x + (size - quad) * 0.5f, origin.y + (size - quad) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(id, pos, ImVec2(pos.x + quad, pos.y + quad));
}

/// @brief Draws the current TTF spinner glyph centred in a @p size square at
/// @p origin — one glyph, so one textured quad, picked by a time-based counter.
void DrawTtfLoadingFrame(const ImVec2 &origin, float size)
{
    ImFont *font = Assisi::Debug::DebugUI::LoadingFont();
    if (font == nullptr)
        return;

    const int32_t frame = static_cast<int32_t>(ImGui::GetTime() * kLoadingFps) % kTtfFrameCount;
    const uint32_t cp    = kTtfFirstFrame + static_cast<uint32_t>(frame);

    // Encode the frame codepoint as UTF-8, up to U+FFFF — enough for both mappings
    // a spinner font is likely to use, ASCII and BMP Private-Use.
    char utf8[4] = {0, 0, 0, 0};
    if (cp < 0x80)
    {
        utf8[0] = static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        utf8[0] = static_cast<char>(0xC0 | (cp >> 6));
        utf8[1] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        utf8[0] = static_cast<char>(0xE0 | (cp >> 12));
        utf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = static_cast<char>(0x80 | (cp & 0x3F));
    }

    ImDrawList *drawList  = ImGui::GetWindowDrawList();
    const float glyphSize = size * 0.85f;
    const ImU32 color     = IM_COL32(226, 222, 210, 255);

    // Frames differ in extent, so measure at the scaled size rather than assuming.
    const ImVec2 extent = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, utf8);
    const ImVec2 pos(origin.x + (size - extent.x) * 0.5f, origin.y + (size - extent.y) * 0.5f);
    drawList->AddText(font, glyphSize, pos, color, utf8);
}

/// @brief Draws the current spinner frame centred in a @p size square at
/// @p origin, via whichever backend kUseWebpSpinner selects.
///
/// **Call only when LoadingSpinnerAvailable()** — the WebP path does not check.
/// Callers that cannot draw a spinner leave a plain placeholder instead.
void DrawLoadingFrame(const ImVec2 &origin, float size)
{
    if (Assisi::Debug::DebugUI::kUseWebpSpinner)
        DrawWebpLoadingFrame(origin, size);
    else
        DrawTtfLoadingFrame(origin, size);
}

/// @brief Paints a small amber "!" badge over the top-right corner of a @p size
/// tile at @p origin, marking an asset whose source changed since import and
/// could not be auto-reconciled.
void DrawStaleBadge(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const float radius = std::max(6.f, size * 0.12f);
    const ImVec2 center(origin.x + size - radius - 4.f, origin.y + radius + 4.f);
    const ImU32 amber = IM_COL32(230, 160, 40, 255);
    const ImU32 dark  = IM_COL32(40, 30, 10, 255);

    drawList->AddCircleFilled(center, radius, amber, 20);
    drawList->AddCircle(center, radius, dark, 20, 1.5f);
    // The exclamation mark: a short stem above a dot.
    drawList->AddLine(ImVec2(center.x, center.y - radius * 0.45f), ImVec2(center.x, center.y + radius * 0.10f), dark,
                      std::max(1.5f, radius * 0.18f));
    drawList->AddCircleFilled(ImVec2(center.x, center.y + radius * 0.45f), std::max(1.f, radius * 0.12f), dark, 8);
}
} // namespace

// The write target is pinned as (entity, component meta, field offset), never as a
// pointer to the field. The browser stays open across frames, and a component pool
// can reallocate in between, which would leave a raw pointer dangling by the time
// the user picks a file. SelectAsset re-resolves the address at write time.
void EditorApp::OpenAssetBrowserFor(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset)
{
    _assetBrowserOpen        = true;
    _assetBrowserTarget      = AssetBrowserTarget::ComponentField;
    _assetBrowserFilter      = AssetBrowserFilter::All;
    _assetBrowserEntity      = _selectedEntity;
    _assetBrowserMeta        = &meta;
    _assetBrowserFieldOffset = fieldOffset;
    _assetBrowserVectorSlot  = -1; // a scalar AssetId field, not an element of a vector
    _assetBrowserDir.clear(); // every open starts at the asset root
    _assetBrowserDirty = true;
}

void EditorApp::OpenAssetBrowserForSlot(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset,
                                        int32_t slot)
{
    OpenAssetBrowserFor(meta, fieldOffset);
    _assetBrowserVectorSlot = slot;
    _assetBrowserFilter     = AssetBrowserFilter::Materials;
}

void EditorApp::SelectAsset(std::string_view vpath)
{
    // A material's texture channel: the target is this object's own working copy,
    // which cannot move, so there is nothing to re-resolve. Handled before the
    // component path, which would otherwise reject it for having no meta.
    if (_assetBrowserTarget == AssetBrowserTarget::MaterialField)
    {
        // Dropped, not misapplied, if the panel moved to another material while
        // the browser sat open: the offset would name the right field of the
        // wrong file.
        if (_materialEditorPath == _assetBrowserMaterialPath && !_materialEditorPath.Empty())
        {
            auto *field = reinterpret_cast<Assisi::Core::AssetId *>(reinterpret_cast<char *>(&_materialEditorData) +
                                                                    _assetBrowserFieldOffset);
            *field = _assetDatabase.IdFor(vpath).value_or(Assisi::Core::AssetId{});
            ApplyMaterialEdit(true); // a channel changed: rebuild, don't rewrite the row
        }
        _assetBrowserOpen = false;
        _assetBrowserMeta = nullptr;
        return;
    }

    // An assignment to the slot being previewed supersedes the preview: the
    // author has now really chosen this material, so dropping the binding without
    // restoring is what keeps the choice. Restoring here would silently undo it.
    if (_materialPreviewActive && _assetBrowserEntity == _materialPreviewEntity &&
        _assetBrowserFieldOffset == _materialPreviewFieldOffset && _assetBrowserVectorSlot == _materialPreviewSlot)
    {
        EndMaterialPreview(false);
    }

    // Re-resolve the target from (entity, meta, offset): the component pool may have
    // moved since the browser was opened. The inspector's eyedropper pins its target
    // the same way, for the same reason.
    if (_assetBrowserMeta != nullptr && _scene != nullptr && _scene->IsAlive(_assetBrowserEntity))
    {
        const void *ptr =
            _assetBrowserMeta->getByEntity(_scene, _assetBrowserEntity.index, _assetBrowserEntity.generation);
        if (ptr != nullptr)
        {
            // A one-frame undo capture around the write. This edit site goes through a
            // raw offset, so the inspector's own record-before-write never sees it.
            Assisi::Editor::EditHistory *history = ActiveHistory();
            if (history != nullptr)
                history->RecordBefore(_assetBrowserEntity, _assetBrowserMeta->id,
                                      EditLabel("Assign asset", _assetBrowserEntity), _assetBrowserEntity);

            char *fieldPtr = const_cast<char *>(static_cast<const char *>(ptr)) + _assetBrowserFieldOffset;
            // The browser picks a file path, but the field stores an AssetId, so
            // translate through the database — nil when the path has no sidecar.
            const Assisi::Core::AssetId id = _assetDatabase.IdFor(vpath).value_or(Assisi::Core::AssetId{});
            if (_assetBrowserVectorSlot < 0)
            {
                *reinterpret_cast<Assisi::Core::AssetId *>(fieldPtr) = id;
            }
            else
            {
                // The override list is sparse: pad with nil ids up to the slot,
                // then assign. A short list means "the rest use the mesh defaults".
                auto &overrides = *reinterpret_cast<std::vector<Assisi::Core::AssetId> *>(fieldPtr);
                const std::size_t slot      = static_cast<std::size_t>(_assetBrowserVectorSlot);
                if (overrides.size() <= slot)
                    overrides.resize(slot + 1);
                overrides[slot] = id;
            }
            ReresolveEntityAssets(_assetBrowserEntity);

            if (history != nullptr)
                history->CommitGesture(_assetBrowserEntity, _assetBrowserMeta->id);
        }
    }
    _assetBrowserOpen = false;
    _assetBrowserMeta = nullptr;
}

void EditorApp::ReresolveEntityAssets(Assisi::ECS::Entity entity)
{
    if (_scene == nullptr || !_scene->IsAlive(entity))
        return;
    Assisi::Runtime::MeshRenderer *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(entity);
    if (mrc == nullptr)
        return;
    Assisi::Runtime::ResolveMeshRendererAssets(*mrc, _assetCache, _assetDatabase);
}

void EditorApp::RescanAssetBrowser()
{
    // Drop the previous directory's thumbnails, so browsing many folders does not
    // grow VRAM without bound. ClearThumbnails waits for the GPU to idle before
    // freeing, which is what makes releasing the ImGui binding here safe.
    _thumbnailCache.ClearThumbnails(
        [](nvrhi::ITexture *texture) { Assisi::Debug::DebugUI::ReleaseTexture(texture); });

    _assetBrowserDirs.clear();
    _assetBrowserImages.clear();
    _assetBrowserMeshes.clear();
    _assetBrowserMaterials.clear();
    _assetBrowserReadError = false;

    const std::filesystem::path root   = Assisi::Core::AssetSystem::GetRoot();
    const std::filesystem::path curDir = _assetBrowserDir.empty() ? root : root / _assetBrowserDir;

    std::error_code ec;
    std::filesystem::directory_iterator dirIt(curDir, ec);
    if (ec)
    {
        _assetBrowserReadError = true;
        return;
    }
    for (const std::filesystem::directory_entry &entry : dirIt)
    {
        std::error_code entryEc;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(entryEc))
            _assetBrowserDirs.push_back(name);
        else if (IsThumbnailableImage(entry.path()))
            _assetBrowserImages.push_back(name);
        else if (IsMeshFile(entry.path()))
            _assetBrowserMeshes.push_back(name);
        else if (IsMaterialFile(entry.path()))
            _assetBrowserMaterials.push_back(name);
    }
    std::sort(_assetBrowserDirs.begin(), _assetBrowserDirs.end());
    std::sort(_assetBrowserImages.begin(), _assetBrowserImages.end());
    std::sort(_assetBrowserMeshes.begin(), _assetBrowserMeshes.end());
    std::sort(_assetBrowserMaterials.begin(), _assetBrowserMaterials.end());
}

void EditorApp::DrawAssetBrowser()
{
    if (!_assetBrowserOpen)
        return;

    // Bounds and step for the tile size the zoom buttons below drive.
    static constexpr float kMinThumb  = 64.f;
    static constexpr float kMaxThumb  = 512.f;
    static constexpr float kThumbStep = 32.f;

    ImGui::SetNextWindowSize(ImVec2(720.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Browser", &_assetBrowserOpen))
    {
        ImGui::End();
        return;
    }

    const std::string header = "assets/" + _assetBrowserDir;
    ImGui::TextUnformatted(header.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(_assetBrowserDir.empty());
    if (ImGui::Button("Up"))
    {
        const std::string::size_type slash = _assetBrowserDir.find_last_of('/');
        if (slash == std::string::npos)
            _assetBrowserDir.clear();
        else
            _assetBrowserDir.erase(slash);
        _assetBrowserDirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        _assetBrowserDirty = true;

    // Reimport runs the editor reconcile pass: `.aast` sidecars for newly added
    // assets, then a rebuild of the id database. It marks the browser dirty itself.
    ImGui::SameLine();
    if (ImGui::Button("Reimport"))
        ReimportAssets();

    ImGui::SameLine();
    ImGui::TextUnformatted("Size");
    ImGui::SameLine();
    ImGui::BeginDisabled(_assetBrowserThumbSize <= kMinThumb);
    if (ImGui::Button("-"))
        _assetBrowserThumbSize = std::max(kMinThumb, _assetBrowserThumbSize - kThumbStep);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(_assetBrowserThumbSize >= kMaxThumb);
    if (ImGui::Button("+"))
        _assetBrowserThumbSize = std::min(kMaxThumb, _assetBrowserThumbSize + kThumbStep);
    ImGui::EndDisabled();
    ImGui::Separator();

    // Only on navigation / open / Refresh / Reimport. Never per frame.
    if (_assetBrowserDirty)
    {
        RescanAssetBrowser();
        _assetBrowserDirty = false;
    }

    if (_assetBrowserReadError)
    {
        ImGui::TextDisabled("Cannot read this directory.");
        ImGui::End();
        return;
    }

    const float thumb = _assetBrowserThumbSize;

    ImGui::BeginChild("browser_entries");

    const float cell = thumb + ImGui::GetStyle().ItemSpacing.x;
    const int32_t cols = std::max(1, static_cast<int32_t>(ImGui::GetContentRegionAvail().x / cell));
    int32_t col  = 0;

    // Folders first. Every tile loop below shares this grid flow and `col` with it.
    for (const std::string &dir : _assetBrowserDirs)
    {
        ImGui::PushID(dir.c_str());
        ImGui::BeginGroup();
        const ImVec2 tile    = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Button("##folder", ImVec2(thumb, thumb));
        DrawFolderIcon(tile, thumb);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb);
        ImGui::TextWrapped("%s", dir.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();

        if (clicked)
        {
            if (!_assetBrowserDir.empty())
                _assetBrowserDir += '/';
            _assetBrowserDir += dir;
            _assetBrowserDirty = true;
        }

        if (++col % cols != 0)
            ImGui::SameLine();
    }

    // Images and meshes, hidden while picking for a material slot, where a .amat
    // is the only valid choice.
    if (_assetBrowserFilter != AssetBrowserFilter::Materials)
    {
        for (const std::string &img : _assetBrowserImages)
        {
            const std::string vpath = _assetBrowserDir.empty() ? img : _assetBrowserDir + "/" + img;

            ImGui::PushID(img.c_str());
            ImGui::BeginGroup();
            bool clicked = false;
            // Resolve on-screen tiles only; off-screen rows get a plain placeholder
            // button. A folder of hundreds of images must not kick hundreds of decodes,
            // nor exhaust ImGui's 256-set descriptor pool in one frame. Visible tiles
            // decode on a worker — ResolveThumbnail returns null until one lands.
            const bool visible = ImGui::IsRectVisible(ImVec2(thumb, thumb));
            const Assisi::Render::Texture *tex =
                visible ? _thumbnailCache.ResolveThumbnail(Assisi::Core::AssetPath{std::string_view{vpath}}) : nullptr;
            if (tex != nullptr && tex->IsValid())
            {
                const ImTextureID id = Assisi::Debug::DebugUI::GetOrCreateTextureId(tex->NativeTexture());
                clicked = ImGui::ImageButton("thumb", id, ImVec2(thumb, thumb));
            }
            else if (visible &&
                     _thumbnailCache.IsThumbnailLoading(Assisi::Core::AssetPath{std::string_view{vpath}}) &&
                     LoadingSpinnerAvailable())
            {
                // Still decoding: a blank tile under the spinner, so it reads as
                // "loading" rather than as the dead text button of the branch below.
                const ImVec2 tile = ImGui::GetCursorScreenPos();
                clicked           = ImGui::Button("##loading", ImVec2(thumb, thumb));
                DrawLoadingFrame(tile, thumb);
            }
            else
            {
                clicked = ImGui::Button(img.c_str(), ImVec2(thumb, thumb));
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb);
            ImGui::TextWrapped("%s", img.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();

            if (clicked)
                SelectAsset(vpath);

            if (++col % cols != 0)
                ImGui::SameLine();
        }

        // Mesh files have no thumbnail: a cube icon drawn over a full-tile click target.
        for (const std::string &mesh : _assetBrowserMeshes)
        {
            const std::string vpath = _assetBrowserDir.empty() ? mesh : _assetBrowserDir + "/" + mesh;

            ImGui::PushID(mesh.c_str());
            ImGui::BeginGroup();
            const ImVec2 tile    = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::Button("##mesh", ImVec2(thumb, thumb));
            const bool hovered = ImGui::IsItemHovered();
            DrawMeshIcon(tile, thumb);
            const bool stale = IsAssetStale(vpath);
            if (stale)
                DrawStaleBadge(tile, thumb);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb);
            ImGui::TextWrapped("%s", mesh.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::PopID();

            if (stale && hovered)
                ImGui::SetTooltip("Source changed since import — click to resolve.\nMaterials were left untouched "
                                  "(no auto-resolve).");

            if (clicked)
            {
                // A stale mesh cannot be assigned until it is resolved, so a click opens
                // the resolution prompt rather than selecting it.
                if (stale)
                    OpenStaleResolution(vpath);
                else
                    SelectAsset(vpath);
            }

            if (++col % cols != 0)
                ImGui::SameLine();
        }
    } // end (filter != Materials)

    // Materials are listed in both modes, and are the only tiles left when the
    // browser was opened for a material slot.
    for (const std::string &material : _assetBrowserMaterials)
    {
        const std::string vpath = _assetBrowserDir.empty() ? material : _assetBrowserDir + "/" + material;

        ImGui::PushID(material.c_str());
        ImGui::BeginGroup();
        const ImVec2 tile    = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Button("##material", ImVec2(thumb, thumb));
        DrawMaterialIcon(tile, thumb);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb);
        ImGui::TextWrapped("%s", material.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // A shortcut into the Material panel, not a second home for authoring:
        // creating, duplicating and deleting all live there, so browsing for a
        // mesh never puts a material-authoring action in front of the author.
        if (ImGui::BeginPopupContextItem("##materialactions"))
        {
            if (ImGui::MenuItem("Edit in Material panel"))
                OpenMaterialEditor(vpath);
            ImGui::EndPopup();
        }
        ImGui::PopID();

        // A left click always assigns: the browser is only ever opened to fill a
        // field. Editing a material is the right-click action above, so the two
        // cannot be confused with each other.
        if (clicked)
            SelectAsset(vpath);

        if (++col % cols != 0)
            ImGui::SameLine();
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace Assisi::Editor
