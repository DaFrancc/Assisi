/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Widget.hpp
/// @brief What a kind of control is: three plain functions, registered once and
/// named by a node's behaviour.
///
/// The UI does what every control shares — hit testing, keeping a press while
/// the pointer wanders, focus, key repeat — and hands a control the gestures
/// that came of it. A control therefore says only what it does with a press, a
/// drag or a direction, which is what makes the built-ins short and a game's
/// own control no harder to write than they are.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/NodeId.hpp>

#include <Assisi/Core/CursorShape.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace Assisi::Mondrian
{

struct Node;
struct LayoutNode;
struct TextLayout;
struct Clipboard;

/// @brief What a control holds: nothing for a button, on or off for a toggle,
/// a fraction of its length for a slider, which step it rests on for a stepped
/// one.
using WidgetValue = std::variant<std::monostate, bool, float, int32_t>;

/// @brief What the keyboard, or later a gamepad, asks of the UI.
enum class UiAction : uint8_t
{
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,
    Next,     ///< Tab order forward
    Previous, ///< Tab order backward
    Count
};

inline constexpr std::size_t kUiActionCount = static_cast<std::size_t>(UiAction::Count);

/// @brief The editing keys a field answers to.
///
/// Separate from UiAction because these are the platform's conventions rather
/// than a game's choices: nobody rebinds what Backspace does. The directions a
/// caret moves in are missing on purpose — they are UiActions, so a field is
/// worked by the same keys, and the same gamepad stick, as every other control.
enum class EditKey : uint8_t
{
    LineStart,
    LineEnd,
    Backspace,
    Delete,
    SelectAll,
    Copy,
    Cut,
    Paste,
    Count
};

inline constexpr std::size_t kEditKeyCount = static_cast<std::size_t>(EditKey::Count);

/// @brief Whether a movement brings the selection along.
enum class TextReach : uint8_t
{
    Moves,   ///< the caret goes alone, and any selection is dropped
    Extends, ///< the selection grows or shrinks to where the caret lands
    Count
};

/// @brief How far a movement or a deletion reaches at once.
enum class TextStep : uint8_t
{
    Character,
    Word,
    Count
};

/// @brief What happened to a control, once the UI has made sense of the input.
enum class WidgetGesture : uint8_t
{
    Press,    ///< the pointer went down on it, and it keeps the pointer until Release
    Drag,     ///< the pointer moved while it was held, wherever the pointer now is
    Release,  ///< the pointer came up
    Activate, ///< clicked, or accepted while focused
    Action,   ///< a direction, Tab, Accept or Back, while focused
    Wheel,    ///< the wheel turned over it or over something inside it
    Type,     ///< characters were typed while it had focus
    Edit,     ///< an editing key was pressed while it had focus
    Count
};

/// @brief One gesture's details. Only the fields its gesture names are set.
struct WidgetEvent
{
    Point pointer;          ///< device pixels
    Point pointerDelta;     ///< device pixels moved since the last frame, Drag only
    Point wheel;            ///< notches, y away from the player and x sideways, Wheel only
    std::string_view typed; ///< what was typed, UTF-8, Type only; lives only for the call
    uint32_t clicks = 1;    ///< presses in quick succession on this node, Press only
    WidgetGesture gesture = WidgetGesture::Press;
    UiAction action = UiAction::Count;   ///< Action only
    EditKey key = EditKey::Count;        ///< Edit only
    TextReach reach = TextReach::Moves;  ///< Action and Edit
    TextStep step = TextStep::Character; ///< Action and Edit
};

/// @brief Which of a scrolling node's bars a player has hold of.
enum class ScrollGrab : uint8_t
{
    None,
    Horizontal,
    Vertical,
    Count
};

/// @brief What a control sees of the node it is on.
struct WidgetView
{
    const Node *node = nullptr;
    const LayoutNode *layout = nullptr;
    /// The node's text as this frame laid it out, which is what a control
    /// mapping between text and the screen reads. Null when the node has none.
    const TextLayout *text = nullptr;
    /// How the UI reaches the system clipboard. Null while drawing, which is
    /// not a moment anything should be reading or writing it.
    const Clipboard *clipboard = nullptr;
    void *context = nullptr; ///< whatever the type was registered with
    float scale = 1.f;       ///< device pixels per logical pixel
    NodeId id;
    bool focused = false;
    bool pressed = false;
    bool hovered = false;
};

/// @brief What a control did with a gesture.
enum class WidgetResponse : uint8_t
{
    Ignored,   ///< pass it on: a direction moves focus, a wheel goes to the parent
    Handled,   ///< the control took it, and nothing changed
    Changed,   ///< the control took it and its value moved, so it announces
    Submitted, ///< the control took it, and what it holds is finished
    Count
};

/// @brief One kind of control. Every callback may be null.
struct WidgetType
{
    /// The size the control wants, in logical pixels, before padding and sizing
    /// are applied. Null takes the size from the node's text and children.
    Point (*measure)(const Node &node, void *context) = nullptr;
    /// Whether the control takes the pointer at this point even over whatever
    /// it contains — a scroll bar drawn across its own content. Null leaves the
    /// pointer to the children, which is what most controls want.
    bool (*claims)(const WidgetView &view, Point point) = nullptr;
    /// Draws under the node's own text, over its box and image: a selection
    /// highlight, which has to sit behind the words it marks.
    void (*underlay)(const WidgetView &view, DrawList &list) = nullptr;
    /// Draws over the node's own box, image and text, under its children.
    void (*draw)(const WidgetView &view, DrawList &list) = nullptr;
    /// Reacts to one gesture, and may change @p node's value.
    WidgetResponse (*input)(const WidgetView &view, Node &node, const WidgetEvent &event) = nullptr;
    /// What the pointer looks like over this control, which may depend on
    /// where it is and on what the control is doing: a field shows the I-beam
    /// over its text and the moving shape while it carries a selection. Null
    /// leaves the pointer as the platform's own.
    Core::CursorShape (*cursor)(const WidgetView &view, Point point) = nullptr;
    void *context = nullptr;
};

/// @brief Every kind of control this UI knows, by the id a node carries.
class WidgetRegistry
{
  public:
    /// @brief Adds @p type and returns the id nodes name it by, which is never
    /// zero: zero is the node that is no control at all.
    uint32_t Register(WidgetType type);

    /// @brief The type @p behaviour names, or null for zero and for an id this
    /// registry never handed out.
    [[nodiscard]] const WidgetType *Get(uint32_t behaviour) const;

  private:
    std::vector<WidgetType> _types;
};

/// @brief The controls every UI has, registered first so their ids are fixed.
enum class BuiltinWidget : uint32_t
{
    None,
    Button,
    Toggle,
    ContinuousSlider,
    SteppedSlider,
    Scroll,
    TextField,
    Count
};

/// @brief Registers the built-in controls into @p registry, which must be empty.
void RegisterBuiltinWidgets(WidgetRegistry &registry);

/// @brief Where a slider's handle sits, in device pixels, with its thumb
/// @p fraction of the way along its track. Null layout gives an empty rect.
[[nodiscard]] Rect SliderThumbRect(const Node &node, const LayoutNode *layout, float scale, float fraction);

/// @brief A button, from Ui::AddButton. Naming the kind of control keeps a
/// binding that expects another kind from compiling.
struct ButtonId
{
    NodeId node;
};

struct ToggleId
{
    NodeId node;
};

/// @brief What a slider's ends mean, and how far one press of a key or a button
/// moves it. A stepped slider moves a step at a time and ignores @p step.
struct SliderRange
{
    float min = 0.f;
    float max = 1.f;
    float step = 0.1f;
};

/// @brief Whether a slider carries a button at each end to step it.
enum class SliderButtons : uint8_t
{
    Hidden,
    Shown,
    Count
};

/// @brief A slider that rests anywhere along its length, and so carries how far
/// along it is.
struct ContinuousSliderId
{
    NodeId node;
};

/// @brief A slider that rests only on whole steps, and so carries which step it
/// is on rather than how far along it is.
struct SteppedSliderId
{
    NodeId node;
};

/// @brief A text field, from Ui::AddTextField. What it holds is text, so its
/// recipes take a std::string_view where a slider's take a float.
struct TextFieldId
{
    NodeId node;
};

} // namespace Assisi::Mondrian
