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
#include <Assisi/Mondrian/NodeId.hpp>
#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/TextEdit.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <Assisi/Core/EventQueue.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

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

/// @brief Why a node could not take a name.
enum class NameError : uint8_t
{
    Taken, ///< another live node on the tree already has it
    Count
};

/// @brief One slot: a box with optional text or image.
struct Node
{
    Style style;
    std::string text;
    std::string name;
    /// What this node does when it is clicked or accepted; empty for none. Set
    /// through Screen::OnActivate, which is what knows whether it pushes an
    /// event or acts on the screen — one that acts on the screen ignores the
    /// queue it is handed.
    std::function<void(Core::EventQueue &)> onActivate;
    /// Pushes this node's event when its value changes; empty for none. Set
    /// through Ui::OnChange, which is what knows the event's type. Given the
    /// node rather than the value, because what a control holds is not always
    /// one: a field holds text, which lives in the node.
    std::function<void(Core::EventQueue &, const Node &)> onChange;
    /// Pushes this node's event when what it holds is finished rather than
    /// merely changed: Enter in a field. Empty for none.
    std::function<void(Core::EventQueue &, const Node &)> onSubmit;
    /// Where the caret is and what may be typed, on a node holding text.
    TextEdit edit;
    /// What the control on this node holds; nothing on a node that is none.
    WidgetValue value;
    /// What a slider's ends mean; unused on everything else.
    SliderRange range;
    /// How many positions a stepped control has; zero on everything else.
    int32_t steps = 0;
    /// Where focus goes from here in each direction, overriding the nearest
    /// node there; null to take the nearest.
    std::array<NodeId, kNavDirectionCount> navOverride{};
    Rect imageUv{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};
    Point scrollOffset; ///< UI pixels scrolled into the content, per axis
    /// Where the scrolling is headed, which the offset reaches at once unless
    /// the style asks it to take its time.
    Point scrollTarget;
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
    /// Which scroll bar is being dragged, while one is.
    ScrollGrab scrollGrab = ScrollGrab::None;
    /// Whether a slider carries a button at each end.
    SliderButtons sliderButtons = SliderButtons::Hidden;
    /// Whether a slider's held press began on its track rather than a button.
    bool slidingTrack = false;
};

class NodeTree
{
  public:
    /// @brief A tree holding only its root, which is never destroyed.
    NodeTree();

    [[nodiscard]] NodeId Root() const { return _root; }

    /// @brief A new unnamed node, last among @p parent's children, or a null id
    /// when @p parent is not alive.
    NodeId Create(NodeId parent);

    /// @brief The same, called @p name, which no other live node on this tree
    /// may have. Refused before anything is made; an empty name is no name.
    [[nodiscard]] std::expected<NodeId, NameError> Create(NodeId parent, std::string_view name);

    /// @brief Destroys @p id and everything under it. The root may not be destroyed.
    void Destroy(NodeId id);

    void SetText(NodeId id, std::string_view text);
    /// @brief What Find looks @p id up by, replacing the name it had. Refused,
    /// leaving the old name, when another live node has @p name; an empty name
    /// takes the name off.
    [[nodiscard]] std::expected<void, NameError> SetName(NodeId id, std::string_view name);
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
    /// @brief What @p id does when clicked or accepted, replacing what it did.
    void SetOnActivate(NodeId id, std::function<void(Core::EventQueue &)> push);
    /// @brief What @p id does when its value changes, replacing what it did.
    void SetOnChange(NodeId id, std::function<void(Core::EventQueue &, const Node &)> push);
    /// @brief What @p id does when what it holds is finished, replacing what it did.
    void SetOnSubmit(NodeId id, std::function<void(Core::EventQueue &, const Node &)> push);
    /// @brief Where the caret is and what may be typed on @p id.
    void SetTextEdit(NodeId id, const TextEdit &edit);
    /// @brief What the control on @p id holds.
    void SetValue(NodeId id, WidgetValue value);
    /// @brief How many positions the stepped control on @p id has.
    void SetSteps(NodeId id, int32_t steps);
    /// @brief What the ends of the slider on @p id mean.
    void SetRange(NodeId id, SliderRange range);
    /// @brief Whether the slider on @p id carries a button at each end.
    void SetSliderButtons(NodeId id, SliderButtons buttons);

    /// @brief The kinds of control this tree's nodes may name. Every tree has
    /// the built-ins; a game registers its own beside them.
    [[nodiscard]] WidgetRegistry &Widgets() { return _widgets; }
    [[nodiscard]] const WidgetRegistry &Widgets() const { return _widgets; }
    /// @brief The type @p id's behaviour names, or null when it is no control.
    [[nodiscard]] const WidgetType *WidgetOf(NodeId id) const;

    /// @brief @p id for a control to change, or null when it names none.
    ///
    /// What a control's input callback is given, so it can move its own value
    /// or scroll offset. The links between nodes are the tree's own: change
    /// those through the calls above, which keep them consistent.
    [[nodiscard]] Node *Editable(NodeId id) { return GetMutable(id); }

    /// @brief The live node named @p name, or a null id. A scan: look a name up
    /// once and keep the id.
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

    WidgetRegistry _widgets;
    std::vector<Node> _slots;
    std::vector<uint32_t> _free;
    NodeId _root;
};

} // namespace Assisi::Mondrian
