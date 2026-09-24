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
///
/// The UI owns screens and nothing else that a game builds. Nodes are built on
/// a screen, so everything here is either about the screens as a set — which is
/// shown, which has the keys, what order they draw in — or about the frame:
/// input, layout, the draw list, the font and the clipboard.

#include <Assisi/Mondrian/Clipboard.hpp>
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/Input.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/Navigation.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Mondrian/Screen.hpp>

#include <Assisi/Core/Assert.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{

class Ui
{
  public:
    /// @brief A UI holding no screens, pushing what its screens announce into
    /// @p events. A game makes the screens it needs.
    ///
    /// The queue is taken here rather than set afterwards so that there is no
    /// moment where the UI has nowhere to push: a callback firing then would
    /// have had to be skipped, and skipping one that only works the UI is a
    /// button that quietly does nothing. @p events must outlive the UI.
    explicit Ui(Core::EventQueue &events);
    ~Ui();

    Ui(const Ui &) = delete;
    Ui &operator=(const Ui &) = delete;
    Ui(Ui &&) = delete;
    Ui &operator=(Ui &&) = delete;

    // -------------------------------------------------------------------------
    // Screens
    // -------------------------------------------------------------------------

    /// @brief Shows @p screen. A screen that takes input becomes the one the
    /// keys and the pointer reach, and the one already there keeps its focus
    /// for when it comes back.
    void Show(Screen &screen);

    /// @brief Hides @p screen, keeping it and everything on it.
    void Hide(Screen &screen);

    /// @brief Hides the screen at the top of the back-stack, if its kind closes
    /// on Back. True when one was hidden.
    bool Back();

    /// @brief The screen the pointer and the keys reach: the topmost shown one
    /// whose kind takes input. Null when none does.
    [[nodiscard]] Screen *InputScreen() { return _inputScreen; }
    [[nodiscard]] const Screen *InputScreen() const { return _inputScreen; }

    /// @brief Whether any shown screen takes input, which is what the host
    /// answers with the cursor: a UI nothing is being worked in leaves the
    /// pointer to the game.
    [[nodiscard]] bool TakesInput() const;

    /// @brief Whether any shown screen asks for the world to stop. The UI keeps
    /// running either way — it is driven by the application, not by a world.
    [[nodiscard]] bool PausesWorld() const;

    /// @brief Moves focus to @p id on @p screen, or clears it with a null id.
    /// A screen that does not have the keys remembers it for when it does.
    void SetFocus(Screen &screen, NodeId id);

    /// @brief Whether the player has hold of @p id on @p screen right now.
    [[nodiscard]] bool IsHeld(const Screen &screen, NodeId id) const;

    // -------------------------------------------------------------------------
    // The frame
    // -------------------------------------------------------------------------

    /// @brief Moves hover, presses and focus for this frame's input, and says
    /// what of it the UI used. Must precede Sync.
    ///
    /// Runs against the last Sync's layout, since this frame's has not been
    /// made yet: the first frame hits nothing, and the frame after a resize
    /// hits where things were.
    InputResult ProcessInput(const UiInput &input);

    /// @brief Hover, presses and focus as the last ProcessInput left them, on
    /// whichever screen has the keys.
    [[nodiscard]] const Interaction &GetInteraction() const { return _session.interaction; }

    /// @brief Lay every shown screen out against @p viewport and rebuild the
    /// draw list from scratch. Must follow ProcessInput.
    void Sync(Extent viewport);

    /// @brief What the last Sync produced, finalized: every shown screen's
    /// quads, lowest sort key first. Stays valid until the next Sync, so a
    /// redraw between frames shows the same thing without laying out again.
    [[nodiscard]] const DrawList &GetDrawList() const { return _drawList; }

    /// @brief Whether the pointer moving over a node focuses it, so the keys act
    /// on what is pointed at. Off until set; never while keys were used last.
    void SetHoverFocuses(bool focuses) { _hoverFocuses = focuses; }
    [[nodiscard]] bool GetHoverFocuses() const { return _hoverFocuses; }

    /// @brief Whether moving past the last node in a direction comes round to
    /// the first. On until set.
    void SetNavWrap(NavWrap wrap) { _navWrap = wrap; }

    /// @brief The font every text node is set in, and the texture its atlas was
    /// registered as. The font must outlive the Ui or be replaced first. Until
    /// one is set, text takes no space and draws nothing.
    void SetFont(const Font *font, TextureId atlas)
    {
        _font = font;
        _fontAtlas = atlas;
    }

    /// @brief The texture an image falls back to: what the engine registered
    /// for the purpose, so game code can show something known-good where an
    /// asset has not arrived. kWhiteTexture until the host sets one.
    void SetPlaceholderTexture(TextureId texture) { _placeholderTexture = texture; }
    [[nodiscard]] TextureId GetPlaceholderTexture() const { return _placeholderTexture; }

    /// @brief How the UI reaches the system clipboard. Until set, it reads as
    /// empty and writes go nowhere.
    void SetClipboard(Clipboard clipboard) { _clipboard = std::move(clipboard); }
    [[nodiscard]] const Clipboard &GetClipboard() const { return _clipboard; }

    /// @brief The player's UI size, multiplying the scale the viewport gives.
    void SetUserScale(float scale) { _userScale = scale; }
    [[nodiscard]] float GetUserScale() const { return _userScale; }

  private:
    friend class Screen;

    /// Takes @p screen into the set this UI shows and works, and lets it go
    /// again. Called by the screen's own constructor and destructor, so what
    /// owns the screen is what decides how long the UI holds it.
    void Adopt(Screen &screen);
    void Forget(Screen &screen);

    /// Which step the host loop owes next.
    enum class FrameStep : uint8_t
    {
        AwaitingInput,
        AwaitingSync,
        Count
    };

    /// Everything the UI remembers about the screen it is working.
    ///
    /// All of it is meaningless the moment that screen changes: a node id names
    /// a slot in one screen's tree, and the same slot in another screen holds
    /// an unrelated node. Kept in one struct and cleared by assigning a fresh
    /// one, so a field added here later cannot be the one somebody forgets.
    struct InputSession
    {
        Interaction interaction;
        KeyRepeat<UiAction> repeatAction; ///< the direction held down
        KeyRepeat<EditKey> repeatEdit;    ///< the editing key held down
        double lastPressAt = 0.0;         ///< when the last press landed, for counting clicks
        NodeId lastPressed;               ///< what it landed on
        NodeId focusedLast;               ///< what had focus when the last frame ended
        uint32_t clicks = 0;              ///< presses in the run the last one belongs to
    };

    /// Whether @p screen draws over @p other: by sort key, and within a key by
    /// which was shown later.
    [[nodiscard]] static bool Above(const Screen &screen, const Screen &other);

    /// The topmost shown screen whose kind takes input, or null.
    [[nodiscard]] Screen *TopInputScreen();

    /// Hands the keys from the screen that had them to whichever should have
    /// them now, saving the one's focus and restoring the other's. Everything
    /// the old screen's ids meant goes with them.
    void RefreshInputScreen();

    /// Every shown screen, lowest first, from the topmost one that hides what
    /// is beneath it. What is under such a screen is not drawn and not laid
    /// out: it cannot be seen, so placing it would be work for nothing.
    [[nodiscard]] std::vector<Screen *> DrawOrder();

    /// Carries every scrolling node on every shown screen closer to where it is
    /// headed, over @p seconds since the last frame.
    void AdvanceScrolling(double seconds);

    /// Moves hover, presses and focus for @p input on @p screen.
    InputResult Interact(Screen &screen, const UiInput &input);
    /// Pushes the events this frame's interaction calls for, from @p screen.
    void Announce(Screen *screen);
    /// Hands @p event to the control on @p id, if it is one, and remembers a
    /// value that changed so Announce can tell the game.
    WidgetResponse Dispatch(Screen &screen, NodeId id, const WidgetEvent &event);
    /// What Screen::Step does: @p moves arrow-key presses handed to @p target
    /// through Dispatch, so a move is the one a key makes.
    void Step(Screen &screen, NodeId target, int32_t moves);
    /// The wheel, to the control under the pointer or the nearest above it that
    /// takes it. True when one did.
    bool DispatchWheel(Screen &screen, NodeId hit, const UiInput &input);
    /// Which actions act this frame: those pressed, and the held one whose
    /// repeat has come round.
    std::array<bool, kUiActionCount> FiredActions(const UiInput &input);
    /// The same for the editing keys.
    std::array<bool, kEditKeyCount> FiredEdits(const UiInput &input);
    /// Hands the focused control what was typed and which editing keys fired.
    void Write(Screen &screen, const UiInput &input);
    /// How many presses in quick succession this one is, at @p time on @p node.
    uint32_t CountClicks(NodeId node, double time);
    /// Whether what has focus is holding the keyboard for itself — a field
    /// being typed into rather than a button waiting to be pressed. Such a
    /// control lets go when the player presses anywhere else, or presses Back.
    [[nodiscard]] bool Editing(const Screen &screen) const;
    /// What the control @p widget sees of the node @p id this frame.
    [[nodiscard]] WidgetView ViewOf(const Screen &screen, NodeId id, const WidgetType &widget) const;
    /// What the pointer looks like over @p id, which the control decides.
    [[nodiscard]] Core::CursorShape CursorOver(const Screen &screen, NodeId id, Point pointer) const;
    /// Moves focus for @p action, one of the directions or Tab order.
    void Move(Screen &screen, UiAction action);
    /// Moves focus for the directions pressed this frame, and again for one
    /// still held once it has been held long enough.
    void Navigate(Screen &screen, const UiInput &input);
    void DrawFocusRing(const Screen &screen);

    /// Every screen that exists, in the order it was made. Not owned: a screen
    /// puts itself here and takes itself away, and whatever owns it outlives
    /// the entry.
    std::vector<Screen *> _screens;
    Clipboard _clipboard;
    DrawList _drawList;
    /// Nodes on the input screen whose value moved this frame, announced once
    /// each.
    std::vector<NodeId> _changed;
    /// Nodes that finished this frame, announced after the changes.
    std::vector<NodeId> _submitted;
    InputSession _session;
    /// Which screen the session belongs to; null when none takes input.
    Screen *_inputScreen = nullptr;
    Core::EventQueue &_events;
    const Font *_font = nullptr;
    /// How many screens have ever been shown, which orders those sharing a
    /// sort key.
    uint64_t _showSequence = 0;
    double _lastTime = 0.0; ///< the previous frame's clock, for what moves over time
    Point _lastPointer;
    TextureId _fontAtlas = kWhiteTexture;
    TextureId _placeholderTexture = kWhiteTexture;
    float _userScale = 1.f;
    FrameStep _nextStep = FrameStep::AwaitingInput;
    NavWrap _navWrap = NavWrap::Around;
    bool _hoverFocuses = false;
    /// Whether Back was pressed and nothing on the screen wanted it, acted on
    /// once the frame's events are out so a screen announces before it goes.
    bool _backFired = false;
    /// Whether the frame's callbacks are running. A callback may show, hide or
    /// pop a screen; it may not destroy one, because the tree it is being
    /// announced from would go with it.
    bool _announcing = false;
};

} // namespace Assisi::Mondrian
