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

#include <Assisi/Core/Assert.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

    /// @brief Where the UI pushes its events: each bound node's when it is
    /// clicked or accepted, and UiBack. Until set, nothing is pushed.
    void SetEvents(Core::EventQueue *events) { _events = events; }

    /// @brief Makes @p node push a copy of @p event each time it is clicked or
    /// accepted, replacing whatever it pushed before. Game code reads it with
    /// EventQueue::Read<E>() in any phase after the UI's input step.
    template <typename E> void OnActivate(NodeId node, E event)
    {
        _tree.SetOnActivate(node, [event](Core::EventQueue &events) { events.Push(event); });
    }

    /// @brief A button under @p parent showing @p label, which activates on a
    /// click or on Accept while focused. Bind it with OnActivate.
    ButtonId AddButton(NodeId parent, std::string_view label);

    /// @brief A toggle under @p parent, starting @p on.
    ToggleId AddToggle(NodeId parent, bool on);

    /// @brief A slider under @p parent, starting at @p value, a fraction of its
    /// length from 0 to 1.
    /// @brief A slider under @p parent over @p range, starting at @p value,
    /// which rests anywhere along it.
    ContinuousSliderId AddContinuousSlider(NodeId parent, SliderRange range, float value);

    /// @brief A slider under @p parent over @p range that rests only on whole
    /// steps, with @p steps of them spread evenly along it, starting on
    /// @p step. It carries which step it is on, counted from zero.
    SteppedSliderId AddSteppedSlider(NodeId parent, SliderRange range, int32_t steps, int32_t step);

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

    /// @brief A container under @p parent, styled by @p style, that scrolls on
    /// the @p axes asked for rather than shrinking its content.
    NodeId AddScroll(NodeId parent, const Style &style, std::array<bool, kAxisCount> axes);

    /// @brief What a slider shows. Ignored while the player is dragging it: the
    /// control is what the value means for as long as it is held.
    void SetValue(ContinuousSliderId slider, float value);
    void SetValue(ToggleId toggle, bool on);
    /// @brief Which step a stepped slider rests on, clamped to the steps it has.
    void SetValue(SteppedSliderId slider, int32_t step);

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
        _tree.SetOnChange(node, [recipe](Core::EventQueue &events, const WidgetValue &value)
                          { events.Push(recipe(Held<T>(value))); });
    }

    /// Adds the sample screen's controls, which stand in until screens exist.
    void AddSampleControls();

    /// Carries every scrolling node that takes its time closer to where it is
    /// headed, over @p seconds since the last frame.
    void AdvanceScrolling(double seconds);

    /// Moves hover, presses and focus for @p input.
    InputResult Interact(const UiInput &input);
    /// Pushes the events this frame's interaction calls for.
    void Announce();
    /// Hands @p event to the control on @p node, if it is one, and remembers a
    /// value that changed so Announce can tell the game.
    WidgetResponse Dispatch(NodeId node, const WidgetEvent &event);
    /// The wheel, to the control under the pointer or the nearest above it that
    /// takes it. True when one did.
    bool DispatchWheel(NodeId hit, const UiInput &input);
    /// Which actions act this frame: those pressed, and the held one whose
    /// repeat has come round.
    std::array<bool, kUiActionCount> FiredActions(const UiInput &input);
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
    /// Nodes whose value moved this frame, announced once each.
    std::vector<NodeId> _changed;
    Interaction _interaction;
    const Font *_font = nullptr;
    Core::EventQueue *_events = nullptr;

    double _repeatAt = 0.0; ///< when the held direction next moves focus
    double _lastTime = 0.0; ///< the previous frame's clock, for what moves over time
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
