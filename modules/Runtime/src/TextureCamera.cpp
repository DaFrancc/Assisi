/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/TextureCamera.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>

namespace Assisi::Runtime
{
namespace
{

/// A surface showing a camera that is not among the candidates.
constexpr uint32_t kNoCandidate = std::numeric_limits<uint32_t>::max();

/// The most MSAA samples a texture camera draws with, as for the game's own
/// setting: past 8, edges look no smoother and each sample still costs memory.
constexpr uint32_t kMostTextureCameraSamples = 8u;

/// Whether @p camera draws whether or not anything shows it.
bool DrawsRegardless(const TextureCameraCandidate &camera)
{
    if (camera.redraw)
    {
        return true;
    }
    switch (camera.update)
    {
    case TextureCameraUpdate::Always:
        return true;
    case TextureCameraUpdate::WhileOn:
        return camera.on;
    case TextureCameraUpdate::WhenAsked:
        return camera.asked;
    case TextureCameraUpdate::WhileSeen:
    case TextureCameraUpdate::Count_:
        return false;
    }
    return false;
}

/// What the choice works with: the candidates, the surfaces, and for each
/// surface the candidate it shows.
struct Choice
{
    std::span<const TextureCameraCandidate> cameras;
    std::span<const CameraSurfaceBounds> surfaces;
    std::vector<uint32_t> shown;
    std::vector<uint8_t> drawing;
    std::vector<uint8_t> placed;
};

/// The candidates whose surfaces @p seer sees, other than its own, through
/// @p frustum, called with each one's index.
template <typename Visit>
void ForEachSeen(const Choice &choice, const Render::Frustum &frustum, uint32_t seer, Visit &&visit)
{
    for (std::size_t surface = 0; surface < choice.surfaces.size(); ++surface)
    {
        const uint32_t shown = choice.shown[surface];
        if (shown == kNoCandidate || shown == seer)
        {
            continue;
        }
        if (frustum.IntersectsSphere(choice.surfaces[surface].bounds))
        {
            visit(shown);
        }
    }
}

/// Marks drawing every WhileSeen candidate @p seer sees, and those they see in
/// turn. A camera in any other mode draws by its mode alone.
void MarkSeen(Choice &choice, const Render::Frustum &frustum, uint32_t seer)
{
    std::vector<uint32_t> found;
    ForEachSeen(choice, frustum, seer, [&](uint32_t shown) { found.push_back(shown); });
    for (const uint32_t shown : found)
    {
        if (choice.drawing[shown] == 0u && choice.cameras[shown].update == TextureCameraUpdate::WhileSeen)
        {
            choice.drawing[shown] = 1u;
            MarkSeen(choice, choice.cameras[shown].frustum, shown);
        }
    }
}

/// Places @p camera in @p order after every drawing camera whose surface it
/// sees.
void Place(Choice &choice, uint32_t camera, std::vector<ECS::Entity> &order)
{
    choice.placed[camera] = 1u;
    std::vector<uint32_t> seen;
    ForEachSeen(choice, choice.cameras[camera].frustum, camera, [&](uint32_t shown) { seen.push_back(shown); });
    for (const uint32_t shown : seen)
    {
        if (choice.drawing[shown] != 0u && choice.placed[shown] == 0u)
        {
            Place(choice, shown, order);
        }
    }
    order.push_back(choice.cameras[camera].camera);
}

} // namespace

uint32_t TextureCameraSamples(uint32_t asked, uint32_t most)
{
    return std::bit_floor(std::clamp(asked, 1u, std::clamp(most, 1u, kMostTextureCameraSamples)));
}

void SwitchTextureCamera(ECS::Scene &scene, ECS::Entity camera, TextureCameraSwitch to)
{
    const TextureCamera *const held = scene.Get<TextureCamera>(camera);
    const bool on = to == TextureCameraSwitch::Start;
    if (held != nullptr && held->on != on)
    {
        scene.GetMut<TextureCamera>(camera)->on = on;
    }
}

void RequestTextureCameraPicture(ECS::Scene &scene, ECS::Entity camera)
{
    if (scene.Has<TextureCamera>(camera) && !scene.Has<TextureCameraRequest>(camera))
    {
        (void)scene.Add(camera, TextureCameraRequest{});
    }
}

void OrderTextureCameras(const Render::Frustum &main, std::span<const TextureCameraCandidate> cameras,
                         std::span<const CameraSurfaceBounds> surfaces, std::vector<ECS::Entity> &order)
{
    order.clear();
    Choice choice{.cameras = cameras,
                  .surfaces = surfaces,
                  .shown = std::vector<uint32_t>(surfaces.size(), kNoCandidate),
                  .drawing = std::vector<uint8_t>(cameras.size(), 0u),
                  .placed = std::vector<uint8_t>(cameras.size(), 0u)};
    for (std::size_t surface = 0; surface < surfaces.size(); ++surface)
    {
        for (std::size_t camera = 0; camera < cameras.size(); ++camera)
        {
            if (cameras[camera].camera == surfaces[surface].camera)
            {
                choice.shown[surface] = static_cast<uint32_t>(camera);
                break;
            }
        }
    }

    // What the main camera sees, then what each camera drawing anyway sees.
    MarkSeen(choice, main, kNoCandidate);
    for (std::size_t camera = 0; camera < cameras.size(); ++camera)
    {
        if (DrawsRegardless(cameras[camera]) && choice.drawing[camera] == 0u)
        {
            choice.drawing[camera] = 1u;
            MarkSeen(choice, cameras[camera].frustum, static_cast<uint32_t>(camera));
        }
    }

    for (std::size_t camera = 0; camera < cameras.size(); ++camera)
    {
        if (choice.drawing[camera] != 0u && choice.placed[camera] == 0u)
        {
            Place(choice, static_cast<uint32_t>(camera), order);
        }
    }
}

} // namespace Assisi::Runtime
