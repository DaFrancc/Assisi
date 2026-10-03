/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EntityRef.hpp
/// @brief How a reflected entity field is written to and read from JSON.
///
/// An entity handle means nothing outside the process that holds it, so what one
/// is written as depends on who is writing: a level file names its target, an
/// entity migration numbers it within the moved set, an undo payload keeps the
/// raw handle. The ECS knows none of those vocabularies, so the serializer that
/// does installs an EntityRefCodec for as long as it runs, and the code reflectgen
/// generates for every entity field goes through EntityRefToJson and
/// EntityRefFromJson.
///
/// With no codec installed a reference writes null and reads back as NullEntity:
/// there is nothing it could be addressed by.

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{

/// @brief The two halves of one way of writing entity references.
struct EntityRefCodec
{
    nlohmann::json (*toJson)(Entity entity) = nullptr;
    Entity (*fromJson)(const nlohmann::json &value) = nullptr;
};

/// @brief Installs @p codec on this thread for the scope's lifetime.
///
/// Restores whichever codec was installed before it rather than clearing, so a
/// serializer reached from inside another one hands the outer one back intact.
class ScopedEntityRefCodec
{
public:
    explicit ScopedEntityRefCodec(EntityRefCodec codec);
    ~ScopedEntityRefCodec();

    ScopedEntityRefCodec(const ScopedEntityRefCodec &) = delete;
    ScopedEntityRefCodec &operator=(const ScopedEntityRefCodec &) = delete;
    ScopedEntityRefCodec(ScopedEntityRefCodec &&) = delete;
    ScopedEntityRefCodec &operator=(ScopedEntityRefCodec &&) = delete;

private:
    EntityRefCodec _outer;
};

/// @brief @p entity as the installed codec writes it; null for NullEntity or
/// when no codec is installed.
[[nodiscard]] nlohmann::json EntityRefToJson(Entity entity);

/// @brief The entity @p value names under the installed codec; NullEntity for
/// null or when no codec is installed.
[[nodiscard]] Entity EntityRefFromJson(const nlohmann::json &value);

} // namespace Assisi::ECS
