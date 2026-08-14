/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InspectorFieldChrome.hpp
/// @brief Per-field chrome of the Inspector: whether a reflected field is drawn
/// at all, and the dimming an AFIELD(norep) field carries while it is.
///
/// The two are one unit because the order between them is the whole point. The
/// tint is an ImGui colour push, and the field it decorates may turn out not to
/// be drawn — so the verdict has to be in hand before anything is pushed.

#include <cstdint>

namespace Assisi::Editor
{

/// @brief Editor visibility of an AFIELD(radio) listener for the current data.
enum class RadioVisibility : uint8_t
{
    Active, ///< Source enum is at one of the field's values — edit normally.
    Greyed, ///< Not active; show disabled (radioBehavior = grey).
    Hidden, ///< Not active; omit entirely (radioBehavior = vanish).
};

/// @brief Scopes the ImGui colour an AFIELD(norep) field is dimmed with.
///
/// The visibility verdict is a constructor argument, so the tint cannot be
/// pushed before the caller knows whether the field is drawn: a Hidden field
/// pushes nothing, and the `continue` that skips it has nothing to unwind. The
/// destructor pops whatever is still outstanding, so no exit path can leave the
/// colour stack deeper than it found it.
class ScopedFieldChrome
{
  public:
    ScopedFieldChrome(RadioVisibility radio, bool norep);
    ~ScopedFieldChrome();

    ScopedFieldChrome(const ScopedFieldChrome &)            = delete;
    ScopedFieldChrome &operator=(const ScopedFieldChrome &) = delete;
    ScopedFieldChrome(ScopedFieldChrome &&)                 = delete;
    ScopedFieldChrome &operator=(ScopedFieldChrome &&)      = delete;

    /// @brief False for a Hidden field: skip it. Nothing was pushed.
    [[nodiscard]] bool Visible() const { return _visible; }

    /// @brief True for a listener drawn but not active; wrap it in BeginDisabled.
    [[nodiscard]] bool Greyed() const { return _greyed; }

    /// @brief Drop the tint before the trailing "(server-only)" tag, which draws
    /// in its own colour. Idempotent; the destructor covers the paths that never
    /// reach it.
    void EndTint();

  private:
    bool _visible;
    bool _greyed;
    bool _tinted;
};

} // namespace Assisi::Editor
