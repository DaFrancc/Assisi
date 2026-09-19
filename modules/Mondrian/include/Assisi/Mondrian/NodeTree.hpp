/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NodeTree.hpp
/// @brief The retained tree of UI nodes, changed by mutation and addressed by id.
///
/// Nodes live in one array of slots, linked by index to their parent, first
/// child and next sibling. An id carries its slot's generation, which moves on
/// every destroy, so an id that outlived its node matches nothing: every call
/// given one does nothing and every read returns empty. Main thread only.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Style.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief A node, by slot and generation.
struct NodeId
{
    static constexpr uint32_t kNullIndex = std::numeric_limits<uint32_t>::max();

    uint32_t index = kNullIndex;
    uint32_t generation = 0;

    bool operator==(const NodeId &) const = default;
    [[nodiscard]] explicit operator bool() const { return index != kNullIndex; }
};

/// @brief The four ways focus moves on the screen.
enum class NavDirection : uint8_t
{
    Up,
    Down,
    Left,
    Right,
    Count
};

inline constexpr std::size_t kNavDirectionCount = static_cast<std::size_t>(NavDirection::Count);

/// @brief One slot: a box with optional text or image.
struct Node
{
    Style style;
    std::string text;
    std::string name;
    /// Where focus goes from here in each direction, overriding the nearest
    /// node there; null to take the nearest.
    std::array<NodeId, kNavDirectionCount> navOverride{};
    Rect imageUv{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};
    Point scrollOffset; ///< logical pixels scrolled into the content, per axis
    NodeId parent;
    NodeId firstChild;
    NodeId nextSibling;
    TextureId image = kWhiteTexture;
    uint32_t behaviour = 0; ///< what a widget does with this node; zero for none
    uint32_t generation = 0;
    bool alive = false;
    bool visible = true;
    bool hasImage = false;
    bool focusable = false;     ///< takes focus, hover and presses; stops the pointer
    bool enabled = true;        ///< a disabled focusable node still stops the pointer, and does nothing else
    bool blocksPointer = false; ///< stops the pointer without taking focus
    bool takesKeyboard = false; ///< while focused, has the keyboard even when the game has it
};

class NodeTree
{
  public:
    /// @brief A tree holding only its root, which is never destroyed.
    NodeTree();

    [[nodiscard]] NodeId Root() const { return _root; }

    /// @brief A new node, last among @p parent's children, or a null id when
    /// @p parent is not alive.
    NodeId Create(NodeId parent, std::string_view name = {});

    /// @brief Destroys @p id and everything under it. The root may not be destroyed.
    void Destroy(NodeId id);

    void SetText(NodeId id, std::string_view text);
    void SetVisible(NodeId id, bool visible);
    void SetStyle(NodeId id, const Style &style);
    /// @brief Draws @p texture over @p uv across the node, over its background.
    void SetImage(NodeId id, TextureId texture, const Rect &uv);
    void ClearImage(NodeId id);
    void SetScrollOffset(NodeId id, Point offset);
    void SetBehaviour(NodeId id, uint32_t behaviour);
    void SetFocusable(NodeId id, bool focusable);
    void SetEnabled(NodeId id, bool enabled);
    /// @brief Whether a press on @p id is the UI's even though it does nothing,
    /// so it never reaches the game: a panel's background, or an invisible node
    /// over the whole screen.
    void SetBlocksPointer(NodeId id, bool blocks);
    void SetTakesKeyboard(NodeId id, bool takes);
    /// @brief Sends focus moving @p direction from @p id to @p target; a null
    /// target takes the override off.
    void SetNavOverride(NodeId id, NavDirection direction, NodeId target);

    /// @brief The first live node named @p name, or a null id. A scan: look a
    /// name up once and keep the id.
    [[nodiscard]] NodeId Find(std::string_view name) const;

    [[nodiscard]] bool IsAlive(NodeId id) const { return Get(id) != nullptr; }
    /// @brief The node @p id names, or null when it names none.
    [[nodiscard]] const Node *Get(NodeId id) const;

    /// @brief Every slot, live or free, by index. What layout walks.
    [[nodiscard]] std::span<const Node> Slots() const { return _slots; }

    /// @brief The id of the live node in slot @p index.
    [[nodiscard]] NodeId IdOf(uint32_t index) const { return {.index = index, .generation = _slots[index].generation}; }

  private:
    Node *GetMutable(NodeId id);
    void Unlink(NodeId id);

    std::vector<Node> _slots;
    std::vector<uint32_t> _free;
    NodeId _root;
};

} // namespace Assisi::Mondrian
