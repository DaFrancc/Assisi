/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Screen.hpp
/// @brief One screen: a tree of nodes, a kind that decides how it behaves and a
/// sort key that decides where it draws.
///
/// A pause menu, a HUD, a tooltip and a loading screen are all screens; what
/// separates them is the kind. The screen is what nodes are built on, so a node
/// id names a slot in one screen's tree and means nothing in another's — which
/// is why no call takes a node from one screen and a tree from another.

#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Mondrian/Pattern.hpp>
#include <Assisi/Mondrian/TextEdit.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <Assisi/Core/Assert.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Assisi::Mondrian
{

class Ui;

/// @brief What a screen does with the pointer and the keys.
///
/// Only the topmost screen that consumes them gets them, and while it does the
/// game sees neither. Back is the one way a player hands them back without the
/// game saying so, which is why it is answered here rather than on its own: a
/// screen that consumes nothing has nothing for Back to return.
enum class ScreenInput : uint8_t
{
    NoConsume,          ///< the game reads them as though nothing were shown
    LockedConsumeInput, ///< the screen has them until code hides it; Back does nothing
    ConsumeInput,       ///< the screen has them until Back, which hides it
    Count
};

/// @brief Whether the screens below this one on its target are still drawn.
///
/// Answered among the screens sharing a target and nowhere else: a screen drawn
/// onto a surface in the world hides nothing on the player's display, however
/// it sorts.
enum class ScreenBeneath : uint8_t
{
    NoHide,       ///< what is below is laid out and drawn as usual
    HidesBeneath, ///< what is below cannot be seen, so it is neither laid out nor drawn
    Count
};

/// @brief What happens to the world while a screen is shown.
///
/// The UI runs either way — it is stepped by the application rather than by a
/// world phase — so the menu that stopped the world is still worked while it is
/// stopped.
enum class ScreenPause : uint8_t
{
    Pause, ///< the world's fixed step is skipped, and its physics with it
    Run,   ///< the world's fixed step runs as usual
    Count
};

/// @brief What a screen does, as three answers rather than a name for one
/// common set of them.
///
/// A menu is {ConsumeInput, HidesBeneath, Pause}; a HUD is the default; a
/// dialog over live play is {ConsumeInput, NoHide, Run}. The combinations a
/// name would not have reached are the point: an inventory that consumes input
/// without stopping the world, a cutscene letterbox that hides what is beneath
/// and consumes nothing.
struct ScreenTraits
{
    ScreenInput input = ScreenInput::NoConsume;
    ScreenBeneath beneath = ScreenBeneath::NoHide;
    ScreenPause pause = ScreenPause::Run;

    [[nodiscard]] friend constexpr bool operator==(const ScreenTraits &, const ScreenTraits &) = default;
};

/// The gap between the layers the engine names, so a game can slot a screen of
/// its own between two of them without renumbering anything.
inline constexpr int32_t kSortLayerSpacing = 1000;

/// The layers the engine names, lowest drawn first.
inline constexpr int32_t kSortHud = 0;
inline constexpr int32_t kSortMenu = kSortLayerSpacing;
inline constexpr int32_t kSortPopup = 2 * kSortLayerSpacing;
inline constexpr int32_t kSortOverlay = 3 * kSortLayerSpacing;

class Screen
{
  public:
    /// @brief An empty screen behaving as @p traits say, drawn at @p sortKey,
    /// called @p name, hidden until it is shown.
    ///
    /// It joins @p ui here and leaves it when destroyed, so whatever owns the
    /// screen decides how long it lasts and the UI is never told separately.
    /// @p ui must outlive it, which a world's screens get for nothing: worlds
    /// are destroyed before the UI they were drawn in.
    Screen(Ui &ui, ScreenTraits traits, int32_t sortKey, std::string name);

    Screen(const Screen &) = delete;
    Screen &operator=(const Screen &) = delete;
    Screen(Screen &&) = delete;
    Screen &operator=(Screen &&) = delete;
    ~Screen();

    /// @brief Shows this screen, or hides it again keeping everything on it.
    ///
    /// Safe from a node's own callback: a button may hide the screen it is on.
    void Show();
    void Hide();

    [[nodiscard]] ScreenTraits Traits() const { return _traits; }
    [[nodiscard]] int32_t SortKey() const { return _sortKey; }
    [[nodiscard]] std::string_view Name() const { return _name; }
    [[nodiscard]] bool IsShown() const { return _shown; }

    [[nodiscard]] NodeTree &Tree() { return _tree; }
    [[nodiscard]] const NodeTree &Tree() const { return _tree; }
    [[nodiscard]] NodeId Root() const { return _tree.Root(); }

    /// @brief The first node on this screen named @p name, or a null id. A
    /// scan: look a name up once and keep the id.
    [[nodiscard]] NodeId Find(std::string_view name) const { return _tree.Find(name); }

    /// @brief Where the last Sync placed this screen's nodes.
    [[nodiscard]] const LayoutResult &GetLayout() const { return _layout; }

    /// @brief Makes @p node push a copy of @p event each time it is clicked or
    /// accepted, replacing whatever it pushed before. Game code reads it with
    /// EventQueue::Read<E>() in any phase after the UI's input step.
    ///
    /// For what reaches the world — quitting, loading, respawning — which the
    /// UI cannot do itself and a system must answer.
    template <typename E>
        requires(!std::invocable<E, Screen &>)
    void OnActivate(NodeId node, E event)
    {
        _tree.SetOnActivate(node, [event](Core::EventQueue &events) { events.Push(event); });
    }

    /// @brief Makes @p node run @p act on this screen each time it is clicked
    /// or accepted, replacing whatever it did before.
    ///
    /// For what touches only the UI — closing this screen, opening another —
    /// which needs no event, no system, and so nothing named in a level. A
    /// screen wired this way works wherever it is shown.
    ///
    /// Showing, hiding and popping from inside @p act are safe: the UI walks a
    /// copy of what it is announcing. Destroying a screen is not, and asserts —
    /// the tree being announced from would go with it.
    void OnActivate(NodeId node, std::function<void(Screen &)> act);

    /// @brief An unnamed node under @p parent styled by @p style.
    NodeId Add(NodeId parent, const Style &style);

    /// @brief The same, called @p name, which no other node on this screen may
    /// have. Refused before anything is made.
    [[nodiscard]] std::expected<NodeId, NameError> Add(NodeId parent, const Style &style, std::string_view name);

    /// @brief An unnamed node showing @p text.
    NodeId AddText(NodeId parent, const Style &style, std::string_view text);

    /// @brief The same, called @p name.
    [[nodiscard]] std::expected<NodeId, NameError> AddText(NodeId parent, const Style &style, std::string_view text,
                                                           std::string_view name);

    /// @brief An unnamed button under @p parent showing @p label, which
    /// activates on a click or on Accept while focused. Bind it with
    /// OnActivate.
    ///
    /// A label is what a player reads and never what the button is found by:
    /// two buttons may both read "Back".
    ButtonId AddButton(NodeId parent, std::string_view label);

    /// @brief The same, called @p name.
    [[nodiscard]] std::expected<ButtonId, NameError> AddButton(NodeId parent, std::string_view label,
                                                               std::string_view name);

    /// @brief A toggle under @p parent, starting @p on.
    ToggleId AddToggle(NodeId parent, bool on);

    /// @brief A slider under @p parent over @p range, starting at @p value,
    /// which rests anywhere along it.
    ContinuousSliderId AddContinuousSlider(NodeId parent, SliderRange range, float value);

    /// @brief A slider under @p parent over @p range that rests only on whole
    /// steps, with @p steps of them spread evenly along it, starting on
    /// @p step. It carries which step it is on, counted from zero.
    SteppedSliderId AddSteppedSlider(NodeId parent, SliderRange range, int32_t steps, int32_t step);

    /// @brief A container under @p parent, styled by @p style, that scrolls on
    /// the @p axes asked for rather than shrinking its content.
    NodeId AddScroll(NodeId parent, const Style &style, std::array<bool, kAxisCount> axes);

    /// @brief What a slider's ends mean, and how far a key or a button moves it.
    void SetRange(ContinuousSliderId slider, SliderRange range);
    void SetRange(SteppedSliderId slider, SliderRange range);

    /// @brief Whether a slider carries a button at each end to step it.
    void SetButtons(ContinuousSliderId slider, SliderButtons buttons);
    void SetButtons(SteppedSliderId slider, SliderButtons buttons);

    /// @brief What a slider's ends mean, for whatever draws them.
    [[nodiscard]] SliderRange GetRange(ContinuousSliderId slider) const;
    [[nodiscard]] SliderRange GetRange(SteppedSliderId slider) const;

    /// @brief Where a slider's handle sits along its track, from 0 to 1.
    [[nodiscard]] float GetFraction(ContinuousSliderId slider) const;
    [[nodiscard]] float GetFraction(SteppedSliderId slider) const;

    /// @brief How many steps a stepped slider has, and what value each one
    /// stands for, so a tick can be drawn and labelled where it falls.
    [[nodiscard]] int32_t GetSteps(SteppedSliderId slider) const;
    [[nodiscard]] float GetStepValue(SteppedSliderId slider, int32_t step) const;

    /// @brief What a slider reads now: its value within its range, or which
    /// step it rests on. For a game showing that value beside or over it.
    [[nodiscard]] float GetValue(ContinuousSliderId slider) const;
    [[nodiscard]] int32_t GetValue(SteppedSliderId slider) const;

    /// @brief Where a slider's handle is, in device pixels, as of the last
    /// Sync. What a value drawn on the handle is placed against.
    [[nodiscard]] Rect GetThumbRect(ContinuousSliderId slider) const;
    [[nodiscard]] Rect GetThumbRect(SteppedSliderId slider) const;

    /// @brief What a slider shows. Ignored while the player is dragging it: the
    /// control is what the value means for as long as it is held.
    void SetValue(ContinuousSliderId slider, float value);
    void SetValue(ToggleId toggle, bool on);
    /// @brief Which step a stepped slider rests on, clamped to the steps it has.
    void SetValue(SteppedSliderId slider, int32_t step);

    /// @brief Moves the slider @p slider as @p moves presses of an arrow key
    /// would, Right for positive and Left for negative, and announces the move
    /// as those presses would.
    ///
    /// One move is the slider's own: its step on a continuous slider, one
    /// position on a stepped one. A slider a player could not move — disabled,
    /// hidden, or on a screen that does not have the keys — is not moved.
    /// Announced in the frame's own announcing when called from a node's
    /// callback, and in the next frame's otherwise.
    void Step(NodeId slider, int32_t moves);

    /// @brief Makes @p slider push the event @p recipe builds from its new
    /// value, every time that value moves. A recipe that does not take a float
    /// does not compile.
    template <typename Recipe> void OnChange(ContinuousSliderId slider, Recipe recipe)
    {
        BindChange<float>(slider.node, std::move(recipe));
    }

    /// @brief The same for a toggle, whose recipe takes whether it is on.
    template <typename Recipe> void OnChange(ToggleId toggle, Recipe recipe)
    {
        BindChange<bool>(toggle.node, std::move(recipe));
    }

    /// @brief The same for a stepped slider, whose recipe takes which step it
    /// has reached.
    template <typename Recipe> void OnChange(SteppedSliderId slider, Recipe recipe)
    {
        BindChange<int32_t>(slider.node, std::move(recipe));
    }

    /// @brief A field under @p parent that a player types into, holding one
    /// line or many. Empty to begin with.
    TextFieldId AddTextField(NodeId parent, TextLines lines);

    /// @brief The text @p field holds. Valid until the field is next changed.
    [[nodiscard]] std::string_view GetText(TextFieldId field) const;

    /// @brief Replaces what @p field holds, putting the caret at the end.
    ///
    /// Taken as given: the filter and the length limit govern what a player
    /// types, not what the game puts there.
    void SetText(TextFieldId field, std::string_view text);

    /// @brief Whether a player may select, copy, cut or paste in @p field.
    /// Every one is allowed until it is turned off.
    void SetAbility(TextFieldId field, TextAbility ability, bool allowed);

    /// @brief What @p field shows while it is empty: what to type there, or an
    /// example of it, in fainter ink. Never part of the text — it is not
    /// returned by GetText, cannot be selected, and goes the moment anything
    /// is typed.
    void SetPlaceholder(TextFieldId field, std::string_view text);

    /// @brief Whether @p field shows what it holds or stands in marks for it.
    /// Masking a field also stops copy and cut, which can be turned back on
    /// afterwards for a field where seeing the text is the only worry.
    void SetMask(TextFieldId field, TextMask mask);

    /// @brief The most characters a player may put in @p field, counted as
    /// they see them rather than in bytes.
    void SetMaxLength(TextFieldId field, uint32_t characters);

    /// @brief How tall @p field is, in lines of its own text, on a field of
    /// many lines. @p lines is ignored by Unbounded.
    ///
    /// A field held to a number of lines refuses what would overflow it rather
    /// than scrolling; put it in a scrolling node to hold more than it shows.
    /// A length limit and a line limit hold at once, so whichever the text
    /// reaches first is the one that stops it: wide characters reach the lines,
    /// narrow ones the count.
    void SetHeight(TextFieldId field, TextHeight height, uint32_t lines);

    /// @brief What @p field's text must look like, and when being told matters.
    ///
    /// Returns what went wrong with @p pattern, in which case the field keeps
    /// the pattern it had. An empty pattern takes the rule off.
    std::expected<void, PatternError> SetPattern(TextFieldId field, std::string_view pattern, TextCheck check);

    /// @brief The same, given a pattern already compiled. Null takes the rule
    /// off.
    ///
    /// For a caller that compiled it earlier to find out whether it would: a
    /// screen loaded from a file settles every pattern before it builds a
    /// single node, so nothing can fail half way through building one.
    void SetPattern(TextFieldId field, std::shared_ptr<const Pattern> pattern, TextCheck check);

    /// @brief Whether @p field's text is acceptable, as of the last time its
    /// pattern was consulted. Unchecked until then, and for a field with no
    /// pattern.
    [[nodiscard]] TextValidity GetValidity(TextFieldId field) const;

    /// @brief Where @p field's caret stands, in device pixels, as of the last
    /// Sync: as tall as the line it is on. What anything that has to follow
    /// the caret is placed against.
    [[nodiscard]] Rect GetCaretRect(TextFieldId field) const;

    /// @brief Makes @p field push what @p recipe builds from its text, every
    /// time that text changes. The recipe takes a std::string_view.
    template <typename Recipe> void OnChange(TextFieldId field, Recipe recipe)
    {
        _tree.SetOnChange(field.node, TextPusher(std::move(recipe)));
    }

    /// @brief The same for the moment the text is finished rather than merely
    /// changed: Enter in a single-line field.
    template <typename Recipe> void OnSubmit(TextFieldId field, Recipe recipe)
    {
        _tree.SetOnSubmit(field.node, TextPusher(std::move(recipe)));
    }

    /// @brief Whether plain text on @p id may be selected and copied, which a
    /// player wants for an address or an error code and nowhere else. Making
    /// it selectable makes it focusable, since copying needs the keyboard.
    void SetSelectable(NodeId id, bool selectable);

  private:
    friend class Ui;

    /// What a control of type @p T holds, from the value it carries. A control
    /// only ever carries its own kind, so anything else is this UI's own bug.
    template <typename T> static T Held(const WidgetValue &value)
    {
        const T *held = std::get_if<T>(&value);
        ASSISI_ASSERT(held != nullptr, "a control was asked for a value of a kind it does not hold");
        return held != nullptr ? *held : T{};
    }

    /// Makes @p node push what @p recipe builds from its value, read as a @p T.
    template <typename T, typename Recipe> void BindChange(NodeId node, Recipe recipe)
    {
        static_assert(std::is_invocable_v<Recipe, T>,
                      "a control's OnChange recipe takes the value that control carries: a float for a "
                      "slider, a bool for a toggle");
        _tree.SetOnChange(node, [recipe](Core::EventQueue &events, const Node &changed)
                          { events.Push(recipe(Held<T>(changed.value))); });
    }

    /// What a field pushes from the text it holds. The text is the node's, so
    /// the recipe is handed a view of it and builds its event there and then;
    /// a recipe that keeps the view rather than copying from it outlives it.
    template <typename Recipe>
    [[nodiscard]] static std::function<void(Core::EventQueue &, const Node &)> TextPusher(Recipe recipe)
    {
        static_assert(std::is_invocable_v<Recipe, std::string_view>,
                      "a text field's recipe takes the text it holds, as a std::string_view");
        return [recipe](Core::EventQueue &events, const Node &node)
        { events.Push(recipe(std::string_view{node.text})); };
    }

    /// Whether the player has hold of @p id on this screen right now, which is
    /// what makes SetValue leave a control alone while it is being dragged.
    [[nodiscard]] bool Held(NodeId id) const;

    /// The text @p placed was laid out with, or null when it has none.
    [[nodiscard]] const TextLayout *TextOf(const LayoutNode &placed) const;

    Ui &_ui;
    NodeTree _tree;
    LayoutResult _layout;
    std::string _name;
    /// Which show this was, so screens sharing a sort key draw in the order
    /// they were shown. Zero until first shown.
    uint64_t _shownAt = 0;
    /// What had focus when this screen last gave the keys up, restored when it
    /// takes them back.
    NodeId _focused;
    int32_t _sortKey = kSortMenu;
    ScreenTraits _traits;
    bool _shown = false;
};

} // namespace Assisi::Mondrian
