/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Ui.hpp
/// @brief The game UI: one per windowed application, driven by two steps a frame.
///
/// The application calls ProcessInput directly after polling input, before any
/// fixed update can read what the UI means to consume, and Sync directly before
/// rendering, so what is drawn reflects the frame's final state rather than the
/// previous one's. Each step expects the other to have run in between; calling
/// either twice in a row is a bug in the host loop and asserts.

#include <Assisi/Mondrian/Clipboard.hpp>
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/Input.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/Navigation.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <cstdint>
#include <utility>

namespace Assisi::Mondrian
{

class Ui
{
  public:
    /// @brief A UI showing the sample screen, which stands in until screens exist.
    Ui();

    /// @brief Moves hover, presses and focus for this frame's input, and says
    /// what of it the UI used. Must precede Sync.
    ///
    /// Runs against the last Sync's layout, since this frame's has not been
    /// made yet: the first frame hits nothing, and the frame after a resize
    /// hits where things were.
    InputResult ProcessInput(const UiInput &input);

    /// @brief Hover, presses and focus as the last ProcessInput left them.
    [[nodiscard]] const Interaction &GetInteraction() const { return _interaction; }

    /// @brief Moves focus to @p id, or clears it with a null id.
    void SetFocus(NodeId id) { _interaction.focused = id; }

    /// @brief Whether the pointer moving over a node focuses it, so the keys act
    /// on what is pointed at. Off until set; never while keys were used last.
    void SetHoverFocuses(bool focuses) { _hoverFocuses = focuses; }
    [[nodiscard]] bool GetHoverFocuses() const { return _hoverFocuses; }

    /// @brief Whether moving past the last node in a direction comes round to
    /// the first. On until set.
    void SetNavWrap(NavWrap wrap) { _navWrap = wrap; }

    /// @brief Lay the tree out against @p viewport and rebuild the draw list
    /// from scratch. Must follow ProcessInput.
    void Sync(Extent viewport);

    /// @brief What the last Sync produced, finalized. Stays valid until the next
    /// Sync, so a redraw between frames shows the same thing without laying out
    /// again.
    [[nodiscard]] const DrawList &GetDrawList() const { return _drawList; }

    /// @brief Where the last Sync placed every node.
    [[nodiscard]] const LayoutResult &GetLayout() const { return _layout; }

    [[nodiscard]] NodeTree &Tree() { return _tree; }
    [[nodiscard]] const NodeTree &Tree() const { return _tree; }

    /// @brief The font every text node is set in, and the texture its atlas was
    /// registered as. The font must outlive the Ui or be replaced first. Until
    /// one is set, text takes no space and draws nothing.
    void SetFont(const Font *font, TextureId atlas)
    {
        _font = font;
        _fontAtlas = atlas;
    }

    /// @brief The texture the sample screen's picture shows; the engine
    /// registers one and hands it over. Until then the picture is white.
    void SetPlaceholderTexture(TextureId texture);

    /// @brief How the UI reaches the system clipboard. Until set, it reads as
    /// empty and writes go nowhere.
    void SetClipboard(Clipboard clipboard) { _clipboard = std::move(clipboard); }
    [[nodiscard]] const Clipboard &GetClipboard() const { return _clipboard; }

    /// @brief The player's UI size, multiplying the scale the viewport gives.
    void SetUserScale(float scale) { _userScale = scale; }
    [[nodiscard]] float GetUserScale() const { return _userScale; }

  private:
    /// Which step the host loop owes next.
    enum class FrameStep : uint8_t
    {
        AwaitingInput,
        AwaitingSync,
        Count
    };

    /// Moves focus for @p action, one of the directions or Tab order.
    void Move(UiAction action);
    /// Moves focus for the directions pressed this frame, and again for one
    /// still held once it has been held long enough.
    void Navigate(const UiInput &input);
    void DrawFocusRing();

    NodeTree _tree;
    Clipboard _clipboard;
    LayoutResult _layout;
    DrawList _drawList;
    Interaction _interaction;
    const Font *_font = nullptr;
    double _repeatAt = 0.0; ///< when the held direction next moves focus
    NodeId _picture;
    Point _lastPointer;
    TextureId _fontAtlas = kWhiteTexture;
    float _userScale = 1.f;
    FrameStep _nextStep = FrameStep::AwaitingInput;
    UiAction _repeating = UiAction::Count; ///< the direction held, or Count for none
    NavWrap _navWrap = NavWrap::Around;
    bool _hoverFocuses = false;
};

} // namespace Assisi::Mondrian
