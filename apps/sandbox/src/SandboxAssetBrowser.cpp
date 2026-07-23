/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Debug/DebugUI.hpp>
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

namespace
{
// Defined below in this file's anonymous namespace; forward-declared so the hello
// window (above that block) can play the spinner too.
void DrawLoadingFrame(const ImVec2 &origin, float size);
bool LoadingSpinnerAvailable();
} // namespace

void SandboxApp::DrawHelloImageWindow()
{
    if (ImGui::Begin("hello.png"))
    {
        if (_helloTexture.IsValid())
        {
            const ImTextureID id = Assisi::Debug::DebugUI::GetOrCreateTextureId(_helloTexture.NativeTexture());
            ImGui::Image(id, ImVec2(256.f, 256.f));

            // Play the loading spinner below the image, centred to its width.
            if (LoadingSpinnerAvailable())
            {
                constexpr float kSpin  = 224.f; // nearly fills the 256-wide image column
                const ImVec2    cursor = ImGui::GetCursorScreenPos();
                const ImVec2    origin(cursor.x + (256.f - kSpin) * 0.5f, cursor.y);
                ImGui::Dummy(ImVec2(256.f, kSpin)); // reserve the row
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

/// @brief True for .amat material files.
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

/// @brief Paints a classic folder glyph (a tabbed body) filling a @p size square
/// at screen-space @p origin, so a folder tile reads at a glance without needing
/// an emoji font or an image asset.
void DrawFolderIcon(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const ImU32  body     = IM_COL32(236, 202, 122, 255);
    const ImU32  tab      = IM_COL32(212, 178, 96, 255);
    const float  left     = origin.x + size * 0.16f;
    const float  right    = origin.x + size * 0.84f;
    const float  tabTop   = origin.y + size * 0.30f;
    const float  bodyTop  = origin.y + size * 0.42f;
    const float  bottom   = origin.y + size * 0.84f;
    const float  tabRight = left + (right - left) * 0.45f;
    const float  rounding = size * 0.03f;

    drawList->AddRectFilled(ImVec2(left, tabTop), ImVec2(tabRight, bodyTop + rounding), tab, rounding);
    drawList->AddRectFilled(ImVec2(left, bodyTop), ImVec2(right, bottom), body, rounding);
}

/// @brief Paints a simple isometric cube filling a @p size square at @p origin,
/// so a mesh-file tile reads as "3D model" without an image asset — the mesh
/// counterpart to DrawFolderIcon, in cool tones to distinguish it from a folder.
void DrawMeshIcon(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const float  centerX = origin.x + size * 0.5f;
    const float  centerY = origin.y + size * 0.5f;
    const float  halfW   = size * 0.26f;
    const float  halfH   = size * 0.30f;

    // Hexagonal silhouette of a cube plus its centre, yielding three visible faces.
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
/// material-preview convention — so a .amat tile reads as "material" without an
/// image asset. Warm tones distinguish it from the cool mesh cube.
void DrawMaterialIcon(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float  radius = size * 0.30f;

    // Base disc, then a smaller offset highlight disc for a lit-sphere read.
    drawList->AddCircleFilled(center, radius, IM_COL32(150, 120, 96, 255), 32);
    drawList->AddCircleFilled(ImVec2(center.x - radius * 0.30f, center.y - radius * 0.30f), radius * 0.55f,
                              IM_COL32(214, 188, 150, 255), 32);
}

// Thumbnail loading spinner. Two interchangeable backends, chosen at compile time by
// DebugUI::kUseWebpSpinner (flip that one bool and rebuild):
//   * WebP — an animated .webp decoded to one texture per frame (DebugUI::LoadingWebpFrame).
//   * TTF  — a font whose consecutive glyphs are the frames (DebugUI::LoadingFont),
//            advanced one glyph per tick and looped.
// Either path advances a time-driven counter at kLoadingFps and loops; the frame
// count comes from the active backend (the WebP's decoded frames, or the TTF glyph
// range below). To match a re-authored TTF, set kTtfFrameCount to the number of
// frames and kTtfFirstFrame to the first frame's codepoint ('a' for an a..z
// sequence, or a Private-Use codepoint like 0xE000).
constexpr uint32_t     kTtfFirstFrame = 0xF000; // Spinner.ttf frames: U+F000..U+F12B
constexpr int32_t      kTtfFrameCount = 300;    // one full v4-30 atom-spin loop (must match the font)
constexpr double       kLoadingFps    = 30.0;   // 300 frames / 30fps = 10 s per loop (matches the design).
                                                // double: its only use multiplies ImGui::GetTime(), which is double.
                                                // NOTE: this drives BOTH backends -- the WebP path ignores
                                                // the durations baked into the file -- so it must match
                                                // whichever v4 variant is shipped as the asset.

/// @brief True when a loading spinner (in whichever backend kUseWebpSpinner selects)
/// is loaded and can be drawn. Callers gate on this before reserving a spinner row.
bool LoadingSpinnerAvailable()
{
    return Assisi::Debug::DebugUI::kUseWebpSpinner ? Assisi::Debug::DebugUI::LoadingWebpFrameCount() > 0
                                                   : Assisi::Debug::DebugUI::LoadingFont() != nullptr;
}

/// @brief Draws the current WebP spinner frame as a textured quad, centred and inset
/// (to 85%, matching the TTF glyph) in a @p size square at @p origin. Assumes frames
/// are loaded (LoadingSpinnerAvailable() was true).
void DrawWebpLoadingFrame(const ImVec2 &origin, float size)
{
    const std::size_t frameCount = Assisi::Debug::DebugUI::LoadingWebpFrameCount();
    const std::size_t frame      = static_cast<std::size_t>(ImGui::GetTime() * kLoadingFps) % frameCount;
    const ImTextureID id         = Assisi::Debug::DebugUI::LoadingWebpFrame(frame);
    if (id == ImTextureID_Invalid)
        return;

    const float  quad = size * 0.85f;
    const ImVec2 pos(origin.x + (size - quad) * 0.5f, origin.y + (size - quad) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(id, pos, ImVec2(pos.x + quad, pos.y + quad));
}

/// @brief Draws the current TTF spinner glyph centred in a @p size square at
/// @p origin. Cheap: one glyph (a single textured quad) picked by a time-driven
/// counter. Assumes the spinner font is loaded.
void DrawTtfLoadingFrame(const ImVec2 &origin, float size)
{
    ImFont *font = Assisi::Debug::DebugUI::LoadingFont();
    if (font == nullptr)
        return;

    const int32_t  frame = static_cast<int32_t>(ImGui::GetTime() * kLoadingFps) % kTtfFrameCount;
    const uint32_t cp    = kTtfFirstFrame + static_cast<uint32_t>(frame);

    // Encode the frame codepoint as UTF-8 (covers ASCII 'a'.. and BMP Private-Use
    // up to U+FFFF — the two mappings a spinner font is likely to use).
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

    // Centre the glyph in the tile using its measured extent at the scaled size.
    const ImVec2 extent = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, utf8);
    const ImVec2 pos(origin.x + (size - extent.x) * 0.5f, origin.y + (size - extent.y) * 0.5f);
    drawList->AddText(font, glyphSize, pos, color, utf8);
}

/// @brief Draws the current loading-spinner frame centred in a @p size square at
/// @p origin, over a thumbnail tile still decoding on a worker thread. Dispatches to
/// whichever backend kUseWebpSpinner selects. No-op if no spinner is loaded (the
/// caller then leaves a plain placeholder).
void DrawLoadingFrame(const ImVec2 &origin, float size)
{
    if (Assisi::Debug::DebugUI::kUseWebpSpinner)
        DrawWebpLoadingFrame(origin, size);
    else
        DrawTtfLoadingFrame(origin, size);
}

/// @brief Paints a small amber "!" badge in the top-right corner of a @p size
/// tile at @p origin, marking an asset whose source changed but couldn't be
/// auto-reconciled (S4). Drawn over the tile icon, so it reads at a glance.
void DrawStaleBadge(const ImVec2 &origin, float size)
{
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    const float  radius = std::max(6.f, size * 0.12f);
    const ImVec2 center(origin.x + size - radius - 4.f, origin.y + radius + 4.f);
    const ImU32  amber = IM_COL32(230, 160, 40, 255);
    const ImU32  dark  = IM_COL32(40, 30, 10, 255);

    drawList->AddCircleFilled(center, radius, amber, 20);
    drawList->AddCircle(center, radius, dark, 20, 1.5f);
    // The exclamation mark: a short stem above a dot.
    drawList->AddLine(ImVec2(center.x, center.y - radius * 0.45f), ImVec2(center.x, center.y + radius * 0.10f), dark,
                      std::max(1.5f, radius * 0.18f));
    drawList->AddCircleFilled(ImVec2(center.x, center.y + radius * 0.45f), std::max(1.f, radius * 0.12f), dark, 8);
}
} // namespace

void SandboxApp::OpenAssetBrowserFor(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset)
{
    _assetBrowserOpen        = true;
    _assetBrowserEntity      = _selectedEntity;
    _assetBrowserMeta        = &meta;
    _assetBrowserFieldOffset = fieldOffset;
    _assetBrowserVectorSlot  = -1; // plain AssetPath field
    _assetBrowserDir.clear(); // always start at the asset root
    _assetBrowserDirty = true; // re-read on open
}

void SandboxApp::OpenAssetBrowserForSlot(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset,
                                         int32_t slot)
{
    OpenAssetBrowserFor(meta, fieldOffset);
    _assetBrowserVectorSlot = slot; // element [slot] of an AssetPathVector
}

void SandboxApp::SelectAsset(std::string_view vpath)
{
    // Re-resolve the target from (entity, meta, offset) at write time — the
    // component pool may have moved since the browser was opened (see eyedropper).
    if (_assetBrowserMeta != nullptr && _scene != nullptr && _scene->IsAlive(_assetBrowserEntity))
    {
        const void *ptr =
            _assetBrowserMeta->getByEntity(_scene, _assetBrowserEntity.index, _assetBrowserEntity.generation);
        if (ptr != nullptr)
        {
            // One-frame capture around this raw-offset asset write (another
            // off-inspector edit site the inspector's record-before-write misses).
            Sandbox::EditHistory *history = ActiveHistory();
            if (history != nullptr)
                history->RecordBefore(_assetBrowserEntity, _assetBrowserMeta->id,
                                      EditLabel("Assign asset", _assetBrowserEntity), _assetBrowserEntity);

            char *fieldPtr = const_cast<char *>(static_cast<const char *>(ptr)) + _assetBrowserFieldOffset;
            // The browser picks a file path; the stored reference is a GUID, so
            // translate through the database (nil if the path has no sidecar).
            const Assisi::Core::AssetId id = _assetDatabase.IdFor(vpath).value_or(Assisi::Core::AssetId{});
            if (_assetBrowserVectorSlot < 0)
            {
                *reinterpret_cast<Assisi::Core::AssetId *>(fieldPtr) = id;
            }
            else
            {
                // Element [slot] of an AssetIdVector: grow the sparse override
                // list with nil entries up to the slot, then assign.
                auto              &overrides = *reinterpret_cast<std::vector<Assisi::Core::AssetId> *>(fieldPtr);
                const std::size_t  slot      = static_cast<std::size_t>(_assetBrowserVectorSlot);
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

void SandboxApp::ReresolveEntityAssets(Assisi::ECS::Entity entity)
{
    if (_scene == nullptr || !_scene->IsAlive(entity))
        return;
    Assisi::Runtime::MeshRenderer *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(entity);
    if (mrc == nullptr)
        return;
    Assisi::Runtime::ResolveMeshRendererAssets(*mrc, _assetCache, _assetDatabase);
}

void SandboxApp::RescanAssetBrowser()
{
    // Leaving this directory: drop its thumbnails so browsing many folders doesn't
    // grow VRAM without bound. ClearThumbnails waits for the GPU to idle, so it's
    // safe to release each texture's ImGui binding here before it's freed.
    _thumbnailCache.ClearThumbnails(
        [](nvrhi::ITexture *texture) { Assisi::Debug::DebugUI::ReleaseTexture(texture); });

    _assetBrowserDirs.clear();
    _assetBrowserImages.clear();
    _assetBrowserMeshes.clear();
    _assetBrowserMaterials.clear();
    _assetBrowserReadError = false;

    const std::filesystem::path root   = Assisi::Core::AssetSystem::GetRoot();
    const std::filesystem::path curDir = _assetBrowserDir.empty() ? root : root / _assetBrowserDir;

    std::error_code                     ec;
    std::filesystem::directory_iterator dirIt(curDir, ec);
    if (ec)
    {
        _assetBrowserReadError = true;
        return;
    }
    for (const std::filesystem::directory_entry &entry : dirIt)
    {
        std::error_code   entryEc;
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

void SandboxApp::DrawAssetBrowser()
{
    if (!_assetBrowserOpen)
        return;

    // Thumbnail tile size, clamped and stepped by the zoom buttons below.
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

    // Re-run the editor reconcile pass: generate `.aast` sidecars for any newly
    // added assets and rebuild the GUID database. Also marks the browser dirty.
    ImGui::SameLine();
    if (ImGui::Button("Reimport"))
        ReimportAssets();

    // Zoom controls for the icon size.
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

    // Re-read the directory only when it changed (navigation / open / Refresh),
    // never per frame.
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
    int32_t       col  = 0;

    // Folders first, as icon tiles in the same grid as the assets. Clicking one
    // navigates into it (marks the listing dirty so it re-reads next frame).
    for (const std::string &dir : _assetBrowserDirs)
    {
        ImGui::PushID(dir.c_str());
        ImGui::BeginGroup();
        const ImVec2 tile    = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::Button("##folder", ImVec2(thumb, thumb));
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

    // Then the thumbnailable assets and mesh files, continuing the same grid
    // flow — but hidden while picking a material for a slot, where only a .amat
    // is a valid choice.
    if (_assetBrowserVectorSlot < 0)
    {
    for (const std::string &img : _assetBrowserImages)
    {
        const std::string vpath = _assetBrowserDir.empty() ? img : _assetBrowserDir + "/" + img;

        ImGui::PushID(img.c_str());
        ImGui::BeginGroup();
        bool clicked = false;
        // Only resolve tiles actually on screen: an off-screen row draws a plain
        // placeholder button, so a folder of hundreds of images neither kicks
        // hundreds of decodes nor exhausts the 256-set ImGui descriptor pool in one
        // frame. Visible tiles decode asynchronously (ResolveThumbnail returns null
        // while loading), so entering a texture-heavy folder no longer hitches.
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
            // Still decoding on a worker: a blank tile with the animated loading
            // spinner over it (the filename still shows below). Reads as "loading"
            // rather than a dead text button.
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

    // Finally the mesh files (.glb/.gltf). They have no thumbnail, so each shows a
    // cube icon over a click target, continuing the same grid flow as folders.
    for (const std::string &mesh : _assetBrowserMeshes)
    {
        const std::string vpath = _assetBrowserDir.empty() ? mesh : _assetBrowserDir + "/" + mesh;

        ImGui::PushID(mesh.c_str());
        ImGui::BeginGroup();
        const ImVec2 tile    = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::Button("##mesh", ImVec2(thumb, thumb));
        const bool   hovered = ImGui::IsItemHovered();
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
            // A stale mesh can't be picked into a field until it is resolved:
            // clicking one opens the resolution prompt instead of selecting it.
            if (stale)
                OpenStaleResolution(vpath);
            else
                SelectAsset(vpath);
        }

        if (++col % cols != 0)
            ImGui::SameLine();
    }
    } // end (_assetBrowserVectorSlot < 0)

    // Material files (.amat), always listed, shown as a shaded-sphere tile over a
    // click target — the only pickable asset while browsing for a material slot.
    for (const std::string &material : _assetBrowserMaterials)
    {
        const std::string vpath = _assetBrowserDir.empty() ? material : _assetBrowserDir + "/" + material;

        ImGui::PushID(material.c_str());
        ImGui::BeginGroup();
        const ImVec2 tile    = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::Button("##material", ImVec2(thumb, thumb));
        DrawMaterialIcon(tile, thumb);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumb);
        ImGui::TextWrapped("%s", material.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();

        if (clicked)
            SelectAsset(vpath);

        if (++col % cols != 0)
            ImGui::SameLine();
    }

    ImGui::EndChild();
    ImGui::End();
}
