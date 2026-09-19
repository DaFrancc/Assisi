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
#include <Assisi/Mondrian/Layout.hpp>
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

    /// @brief Consume this frame's input. Must precede Sync.
    void ProcessInput();

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

    NodeTree _tree;
    Clipboard _clipboard;
    LayoutResult _layout;
    DrawList _drawList;
    const Font *_font = nullptr;
    NodeId _picture;
    TextureId _fontAtlas = kWhiteTexture;
    float _userScale = 1.f;
    FrameStep _nextStep = FrameStep::AwaitingInput;
};

} // namespace Assisi::Mondrian
