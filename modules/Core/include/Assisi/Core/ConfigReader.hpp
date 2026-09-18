/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ConfigReader.hpp
/// @brief Where shipped config files are read from: one seam, installed per
///        executable.
///
/// A config is a reflected asset type stored at a fixed virtual path. The editor
/// reads it as the JSON document an author edits; a game reading a pak reads the
/// reflected blob the cook wrote. The loaders ask this seam for a filled instance
/// and never see which it was.

#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>
#include <typeindex>

namespace Assisi::Core
{

class AssetProvider;

/// @brief Why a config was not read.
enum class ConfigError : std::uint8_t
{
    Missing,    ///< Nothing is stored at the path. A loader falls back to defaults.
    Unreadable, ///< Something is stored and could not be read, or no reader is installed.
    Malformed,  ///< Read, and not a document of the type asked for.
};

[[nodiscard]] std::string_view ToString(ConfigError error) noexcept;

/// @brief Fills @p instance, of reflected type @p type, from the config at @p vpath.
///
/// Fields the stored config omits keep the value @p instance already holds. On
/// Malformed the instance may be partly written.
using ConfigReader = std::function<std::expected<void, ConfigError>(std::string_view vpath, std::type_index type,
                                                                    void *instance)>;

/// @brief Install the reader every config read goes through, returning the one it
///        replaces.
///
/// Install at startup, before a config is read on another thread: the reader is
/// read without a lock.
ConfigReader SetConfigReader(ConfigReader reader);

/// @brief The config at @p vpath, through the installed reader.
///
/// Unreadable, logged, when no reader is installed. There is no fallback to
/// reading text, because a game that quietly opened a loose file would hide a pak
/// that is missing its config.
[[nodiscard]] std::expected<void, ConfigError> ReadConfig(std::string_view vpath, std::type_index type,
                                                          void *instance);

/// @brief ReadConfig for a type known to the compiler.
template <typename T> [[nodiscard]] std::expected<void, ConfigError> ReadConfig(std::string_view vpath, T &instance)
{
    return ReadConfig(vpath, std::type_index(typeid(T)), &instance);
}

/// @brief The reader for configs stored as JSON documents under the asset root.
[[nodiscard]] std::expected<void, ConfigError> ReadTextConfig(std::string_view vpath, std::type_index type,
                                                              void *instance);

/// @brief The reader for configs stored as cooked reflected blobs in @p provider.
[[nodiscard]] std::expected<void, ConfigError> ReadCookedConfig(const AssetProvider &provider, std::string_view vpath,
                                                                std::type_index type, void *instance);

} // namespace Assisi::Core
