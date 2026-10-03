/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/EntityRef.hpp>

#include <utility>

namespace Assisi::ECS
{

namespace
{
thread_local EntityRefCodec t_codec;
} // namespace

ScopedEntityRefCodec::ScopedEntityRefCodec(EntityRefCodec codec) : _outer(std::exchange(t_codec, codec))
{
}

ScopedEntityRefCodec::~ScopedEntityRefCodec()
{
    t_codec = _outer;
}

nlohmann::json EntityRefToJson(Entity entity)
{
    // Null before the codec: every codec spells "no target" the same way.
    if (entity == NullEntity || t_codec.toJson == nullptr)
    {
        return nullptr;
    }
    return t_codec.toJson(entity);
}

Entity EntityRefFromJson(const nlohmann::json &value)
{
    if (value.is_null() || t_codec.fromJson == nullptr)
    {
        return NullEntity;
    }
    return t_codec.fromJson(value);
}

} // namespace Assisi::ECS
