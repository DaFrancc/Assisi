/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/StructRegistry.hpp
/// @brief Every ASTRUCT this build declares, by the name a file uses for one.
///
/// Code that has a struct's type reaches its table through StructTraits, which
/// the compiler checks. A file has only a name, such as a screen binding
/// `{OptionsView.volume}`, and this turns that name into the table.
///
/// Filled by the generated files before main runs, each entry holding the
/// function that builds its table rather than the table. A table is built on
/// first use and can reach other tables, so nothing here depends on the order
/// files initialise in.

#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Core::Reflect
{

class StructRegistry
{
  public:
    /// @brief What builds a struct's table.
    using SpecBuilder = const StructSpec *(*)();

    /// @brief The process-wide table the generated registrations fill.
    static StructRegistry &Instance();

    /// @brief Adds the struct called @p name. A second struct of one name is
    /// refused with an error, and the first stays: a file could not say which
    /// of the two it meant.
    void Register(std::string_view name, SpecBuilder spec);

    /// @brief The table of the struct called @p name, without its namespaces,
    /// or null when none is.
    [[nodiscard]] const StructSpec *Find(std::string_view name) const;

  private:
    struct Entry
    {
        std::string name;
        SpecBuilder spec = nullptr;
    };

    std::vector<Entry> _entries;
};

} // namespace Assisi::Core::Reflect
