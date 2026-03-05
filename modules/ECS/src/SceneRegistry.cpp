#include <Assisi/ECS/SceneRegistry.hpp>

namespace Assisi::ECS
{

std::expected<Scene*, SceneError> SceneRegistry::Create(std::string_view name)
{
    if (Has(name))
        return std::unexpected(SceneError::NameAlreadyTaken);

    auto [it, _] = _scenes.emplace(std::string(name), std::make_unique<Scene>());
    return it->second.get();
}

void SceneRegistry::Destroy(std::string_view name)
{
    auto it = _scenes.find(std::string(name));
    if (it == _scenes.end())
        return;
    if (_active == it->second.get())
        _active = nullptr;
    _scenes.erase(it);
}

Scene* SceneRegistry::Get(std::string_view name)
{
    auto it = _scenes.find(std::string(name));
    return it != _scenes.end() ? it->second.get() : nullptr;
}

const Scene* SceneRegistry::Get(std::string_view name) const
{
    auto it = _scenes.find(std::string(name));
    return it != _scenes.end() ? it->second.get() : nullptr;
}

bool SceneRegistry::Has(std::string_view name) const
{
    return _scenes.contains(std::string(name));
}

std::expected<void, SceneError> SceneRegistry::SetActive(std::string_view name)
{
    auto it = _scenes.find(std::string(name));
    if (it == _scenes.end())
        return std::unexpected(SceneError::NotFound);

    _active = it->second.get();
    return {};
}

Scene*       SceneRegistry::Active()       { return _active; }
const Scene* SceneRegistry::Active() const { return _active; }

} // namespace Assisi::ECS
