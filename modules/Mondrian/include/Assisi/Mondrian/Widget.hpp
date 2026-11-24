/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Widget.hpp
/// @brief What a kind of control is: a few plain functions and the parts it is
/// made of, registered once and named by a node's behaviour.
///
/// The UI does what every control shares — hit testing, keeping a press while
/// the pointer wanders, focus, key repeat — and hands a control the gestures
/// that came of it. A control therefore says only what it does with a press, a
/// drag or a direction, which is what makes the built-ins short and a game's
/// own control no harder to write than they are.
///
/// What a control shows is made of parts: a slider's track, fill and thumb are
/// child nodes like any other, which a theme or a screen styles by name and
/// can put content in. The control only says where each goes.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/NodeId.hpp>
#include <Assisi/Mondrian/Style.hpp>

#include <Assisi/Core/CursorShape.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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
    Count_
};

inline constexpr std::size_t kUiActionCount = static_cast<std::size_t>(UiAction::Count_);

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
    Count_
};

inline constexpr std::size_t kEditKeyCount = static_cast<std::size_t>(EditKey::Count_);

/// @brief Whether a movement brings the selection along.
enum class TextReach : uint8_t
{
    Moves,   ///< the caret goes alone, and any selection is dropped
    Extends, ///< the selection grows or shrinks to where the caret lands
    Count_
};

/// @brief How far a movement or a deletion reaches at once.
enum class TextStep : uint8_t
{
    Character,
    Word,
    Count_
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
    Count_
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
    UiAction action = UiAction::Count_;   ///< Action only
    EditKey key = EditKey::Count_;        ///< Edit only
    TextReach reach = TextReach::Moves;  ///< Action and Edit
    TextStep step = TextStep::Character; ///< Action and Edit
};

/// @brief Which of a scrolling node's bars a player has hold of.
enum class ScrollGrab : uint8_t
{
    None,
    Horizontal,
    Vertical,
    Count_
};

/// The most parts one kind of control may have.
inline constexpr std::size_t kMaxWidgetParts = 8;

/// What a node that is no control's part carries as its part.
inline constexpr uint8_t kNoPart = UINT8_MAX;

/// @brief One part of a kind of control: a child node of each such control,
/// which the control places and a file styles by its name.
///
/// Two parts may share a name, which a file then styles as one: a scroll's
/// two bars each have a track and a thumb.
struct WidgetPart
{
    std::string_view name;
    /// How it looks before any theme says anything.
    DefaultLook look;
};

/// @brief Where a control puts one of its parts, in device pixels.
struct PartPlace
{
    Rect rect;
    /// Whether it shows at all: a scroll bar with nothing to scroll does not.
    bool shown = true;
};

/// @brief What a control sees of the node it is on.
struct WidgetView
{
    /// Where each of its parts was placed this frame, by part; empty for one
    /// that was not, being hidden or not shown.
    std::array<Rect, kMaxWidgetParts> parts{};
    const Node *node = nullptr;
    const LayoutNode *layout = nullptr;
    /// The node's text as this frame laid it out, which is what a control
    /// mapping between text and the screen reads. Null when the node has none.
    const TextLayout *text = nullptr;
    /// How the UI reaches the system clipboard. Null while drawing, which is
    /// not a moment anything should be reading or writing it.
    const Clipboard *clipboard = nullptr;
    void *context = nullptr; ///< whatever the type was registered with
    float scale = 1.f;       ///< device pixels per UI pixel
    NodeId id;
    bool focused = false;
    bool pressed = false;
    bool hovered = false;
    /// Whether a blinking caret shows this frame.
    bool caretShown = true;
};

/// @brief What a control did with a gesture.
enum class WidgetResponse : uint8_t
{
    Ignored,   ///< pass it on: a direction moves focus, a wheel goes to the parent
    Handled,   ///< the control took it, and nothing changed
    Changed,   ///< the control took it and its value moved, so it announces
    Submitted, ///< the control took it, and what it holds is finished
    Count_
};

/// @brief One kind of control. Every callback may be null.
struct WidgetType
{
    /// What a file calls it, and so the name of the theme style every such
    /// control wears under whatever style it names itself. Empty for none.
    std::string_view name{};
    /// What it is made of. Each control of this kind gets one child node per
    /// part, in this order, which draw in this order over its other children.
    std::span<const WidgetPart> parts{};
    /// How the control itself looks before any theme says anything, or null.
    const DefaultLook *look = nullptr;
    /// The size the control wants, in UI pixels, before padding and sizing
    /// are applied. Null takes the size from the node's text and children.
    Point (*measure)(const Node &node, void *context) = nullptr;
    /// Where each part goes, once the control's own box is final. @p parts
    /// arrives with each part at the size its own style gives it, at the
    /// corner inside the control's padding; the control moves each, and may
    /// resize it, from what it holds. Null leaves them there.
    void (*place)(const WidgetView &view, std::span<PartPlace> parts) = nullptr;
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
AENUM()
enum class BuiltinWidget : uint32_t
{
    None,
    Button,
    Toggle,
    ContinuousSlider,
    SteppedSlider,
    Scroll,
    TextField,
    /// Plain text a player may select and copy: a field's selecting without its
    /// editing. No file names it, and it wears no theme style: a `text_field`
    /// style's box is for fields.
    SelectableText,
    Count_
};

/// @brief Registers the built-in controls into @p registry, which must be empty.
void RegisterBuiltinWidgets(WidgetRegistry &registry);

/// @brief What a file calls @p widget, which is also the theme style each one
/// wears: "slider" for a ContinuousSlider. Empty for None.
[[nodiscard]] std::string_view BuiltinWidgetName(BuiltinWidget widget);

/// @brief The names of @p widget's parts, each once, in the order it has them:
/// what a file may style on it, and what an error lists when it names another.
[[nodiscard]] std::vector<std::string_view> BuiltinPartNames(BuiltinWidget widget);

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

/// @brief What a slider's ends mean, and how far one press of a key moves it. A
/// stepped slider moves a step at a time and ignores @p step.
///
/// @p step has no usable default: no one size suits every range, and one sized
/// for 0 to 1 is a thousand presses end to end on 0 to 100. Left out it is zero,
/// which a continuous slider refuses, so a range written without one fails
/// where it is used rather than moving a slider almost nowhere.
ASTRUCT()
struct SliderRange
{
    AFIELD() float min = 0.f;
    AFIELD() float max = 1.f;
    AFIELD() float step = 0.f;

    [[nodiscard]] friend constexpr bool operator==(const SliderRange &, const SliderRange &) = default;
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
