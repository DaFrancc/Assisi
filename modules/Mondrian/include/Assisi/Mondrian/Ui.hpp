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
#include <Assisi/Mondrian/Sprite.hpp>
#include <Assisi/Mondrian/StringTable.hpp>
#include <Assisi/Mondrian/Theme.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{

class Ui
{
  public:
    /// @brief A UI holding no screens, pushing the universal events its screens
    /// announce into @p universal. A game makes the screens it needs, and each
    /// pushes every other event into the queue it was built with.
    ///
    /// The queue is taken here rather than set afterwards so that there is no
    /// moment where the UI has nowhere to push: a callback firing then would
    /// have had to be skipped, and skipping one that only works the UI is a
    /// button that quietly does nothing. @p universal must outlive the UI.
    explicit Ui(Core::EventQueue &universal);
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
    // Targets
    // -------------------------------------------------------------------------

    /// @brief A new target of @p size pixels for screens to draw into instead of
    /// the window, made shown by whatever the host's TargetMaker makes. Its
    /// screens are laid out against @p size and scaled by it alone, its shorter
    /// side 1080 UI pixels: the player's UI size and scale match are about
    /// their display, not a monitor in the world.
    TargetId AddTarget(Extent size);

    /// @brief Takes @p target away, letting the host release what showed it.
    /// Screens still on it draw nowhere until given another target.
    void RemoveTarget(TargetId target);

    /// @brief How the host makes and releases what shows a target. Targets made
    /// before it is set have nothing to show them.
    void SetTargetHost(TargetMaker make, TargetReleaser release);

    /// @brief What the host made to show @p target, or nullopt when it made
    /// nothing or there is no such target.
    [[nodiscard]] std::optional<TargetTexture> GetTargetTexture(TargetId target) const;

    /// @brief @p target's size, or nullopt when there is no such target.
    [[nodiscard]] std::optional<Extent> TargetSize(TargetId target) const;

    /// @brief What the last Sync drew into @p target, or null when there is no
    /// such target. Kept until the target is drawn again.
    [[nodiscard]] const DrawList *TargetDrawList(TargetId target) const;

    /// @brief Whether the last Sync drew @p target again. A target whose screens
    /// did not change since it was last drawn is not drawn, so what shows it
    /// keeps the picture it has: a room of idle monitors costs no layout, no
    /// drawing and no GPU pass.
    [[nodiscard]] bool TargetRedrawn(TargetId target) const;

    /// @brief Where the window pointer lands on @p target, for when one of its
    /// screens is used: the engine casts it into the world. Without one, a
    /// used screen is worked by the keys alone.
    void SetTargetPointer(TargetId target, PointerMap map);

    /// @brief Gives the keys and the pointer to @p screen, a screen on a target
    /// other than the window, while no window screen takes input: a monitor the
    /// player walked up to. The pointer reaches it through its target's
    /// PointerMap. Back, a hidden screen or StopUsing ends it. A screen on the
    /// window is not used: it takes input by its kind.
    void Use(Screen &screen);

    /// @brief Ends the use of whichever screen is used.
    void StopUsing();

    /// @brief The screen being used, or null.
    [[nodiscard]] Screen *InUse() { return _inUse; }
    [[nodiscard]] const Screen *InUse() const { return _inUse; }

    // -------------------------------------------------------------------------
    // Sources
    // -------------------------------------------------------------------------

    /// @brief Makes @p source, an ASTRUCT on no entity, what bindings naming
    /// its struct read and write, on every screen, replacing any registered
    /// before it. It must outlive the UI or be unregistered first.
    ///
    /// Looked up at each sync, so a screen may be made first, and a source
    /// taken away leaves its bindings as they last were.
    template <typename T> void RegisterSource(T &source)
    {
        static_assert(Core::Reflect::StructTraits<T>::reflected,
                      "a source is a non-const ASTRUCT, and a file using it as one says so at global scope with "
                      "ASSISI_REFLECTED_STRUCT(Type)");
        RegisterSource(*Core::Reflect::StructTraits<T>::Spec(), &source);
    }

    /// @brief Takes away the source of struct @p T, if there is one.
    template <typename T> void UnregisterSource()
    {
        static_assert(Core::Reflect::StructTraits<T>::reflected,
                      "a source is an ASTRUCT, and a file using it as one says so at global scope with "
                      "ASSISI_REFLECTED_STRUCT(Type)");
        UnregisterSource(*Core::Reflect::StructTraits<T>::Spec());
    }

    /// @brief The same given the struct's table rather than its type.
    void RegisterSource(const Core::Reflect::StructSpec &spec, void *data);
    void UnregisterSource(const Core::Reflect::StructSpec &spec);

    /// @brief What is registered as a @p spec, or null when nothing is. Found
    /// by the table rather than its name, so a binding that looked up some
    /// other struct of that name can never read this one as that.
    [[nodiscard]] void *FindSource(const Core::Reflect::StructSpec &spec) const;

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

    /// @brief Lay every shown screen on the window out against @p viewport and
    /// rebuild the draw list from scratch, and draw again every target whose
    /// screens changed. Must follow ProcessInput.
    void Sync(Extent viewport);

    /// @brief The viewport the last Sync laid screens out against, in device
    /// pixels; empty before the first. What the engine places a screen that
    /// follows something in the world against, a frame ahead of the Sync that
    /// draws it, so a resize lands a frame late.
    [[nodiscard]] Extent Viewport() const { return _viewport; }

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
        ++_lookRevision;
    }

    /// @brief The texture an image falls back to: what the engine registered
    /// for the purpose, so game code can show something known-good where an
    /// asset has not arrived. kWhiteTexture until the host sets one.
    void SetPlaceholderTexture(TextureId texture) { _placeholderTexture = texture; }
    [[nodiscard]] TextureId GetPlaceholderTexture() const { return _placeholderTexture; }

    /// @brief Makes every sprite of @p sheet, the sheet loaded from @p vpath,
    /// one a screen can name as `sheet:name`, drawn from @p texture, which the
    /// engine registered holding the sheet's image. Add sheets before loading
    /// screens that name their sprites.
    void AddSpriteSheet(std::string_view vpath, const SpriteSheetDocument &sheet, TextureId texture);

    /// @brief The sprite called @p qualified, `sheet:name`, or null when no
    /// sheet added has it.
    [[nodiscard]] const Sprite *FindSprite(std::string_view qualified) const;

    /// @brief Uploads the image at an asset path and registers it, giving all
    /// of it as one picture of its size; nullopt, logged, when it cannot.
    using TextureLoader = std::function<std::optional<Sprite>(std::string_view vpath)>;

    /// @brief What a picture named by its texture's path is loaded through:
    /// the engine's, which can reach the GPU and the UI cannot. Until set,
    /// every such picture is the placeholder.
    void SetTextureLoader(TextureLoader loader) { _textureLoader = std::move(loader); }

    /// @brief All of the image at @p vpath as one picture: loaded the first
    /// time it is asked for, and kept until the UI goes. The placeholder, of
    /// no known size, when it cannot be loaded.
    [[nodiscard]] Sprite TextureSprite(std::string_view vpath);

    /// @brief Uploads the image an asset id names and registers it, as the
    /// loader above does for a path; nullopt, logged, when it cannot.
    using AssetTextureLoader = std::function<std::optional<Sprite>(Core::AssetId id)>;

    /// @brief What a picture named by its asset id, as a bound texture is, is
    /// loaded through. Until set, every such picture is the placeholder.
    void SetAssetTextureLoader(AssetTextureLoader loader) { _assetTextureLoader = std::move(loader); }

    /// @brief All of the image @p id names as one picture: loaded the first time
    /// it is asked for, and kept until the UI goes. The placeholder, of no known
    /// size, when it cannot be loaded.
    [[nodiscard]] Sprite TextureSprite(Core::AssetId id);

    /// @brief How the UI reaches the system clipboard. Until set, it reads as
    /// empty and writes go nowhere.
    void SetClipboard(Clipboard clipboard) { _clipboard = std::move(clipboard); }
    [[nodiscard]] const Clipboard &GetClipboard() const { return _clipboard; }

    /// @brief The player's UI size, multiplying the scale the viewport gives.
    void SetUserScale(float scale) { _userScale = scale; }
    [[nodiscard]] float GetUserScale() const { return _userScale; }

    /// @brief Which side of the viewport a UI pixel is measured against. The
    /// shorter side until set.
    void SetScaleMatch(ScaleMatch match)
    {
        _scaleMatch = match;
        ++_lookRevision;
    }
    [[nodiscard]] ScaleMatch GetScaleMatch() const { return _scaleMatch; }

    /// @brief The string tables keyed text is looked up in, and the text every
    /// keyed node on every screen shows, rewritten from them now. The tables
    /// must outlive the Ui or be replaced first; null takes them away.
    void SetStringTables(const StringTables *tables);
    [[nodiscard]] const StringTables *GetStringTables() const { return _tables; }

    /// @brief What the string tables give @p qualified, `table:key`. The key
    /// itself behind a #, when no table gives it anything: a missing string is
    /// something to see and fix, not a reason for a screen not to load.
    [[nodiscard]] std::string StringFor(std::string_view qualified) const;

    /// @brief The themes a node naming a style or a token takes them from, and
    /// the first of them, in its first mode, put on every screen now. They must
    /// outlive the Ui or be replaced first; null or none takes them away, and
    /// every node wears only its own look.
    void SetThemes(const Themes *themes);

    /// @brief Puts the theme loaded from @p vpath on every screen, in the mode
    /// called what the current one is when it has one, and its first otherwise.
    std::expected<void, ThemeError> SetTheme(std::string_view vpath);

    /// @brief Puts the current theme's mode called @p mode on every screen.
    std::expected<void, ThemeError> SetThemeMode(std::string_view mode);

    /// @brief Where the theme every screen wears was loaded from, or empty for
    /// none.
    [[nodiscard]] std::string_view GetTheme() const;

    /// @brief The mode it is in, or empty for no theme.
    [[nodiscard]] std::string_view GetThemeMode() const;

  private:
    friend class Screen;

    /// Takes @p screen into the set this UI shows and works, and lets it go
    /// again. Called by the screen's own constructor and destructor, so what
    /// owns the screen is what decides how long the UI holds it.
    void Adopt(Screen &screen);
    void Forget(Screen &screen);

    /// Puts @p theme on every screen.
    void PutOn(const ThemeView &theme);

    /// What Screen::SetTarget does: moves @p screen onto @p target, handing the
    /// keys on if it can no longer have them.
    void Retarget(Screen &screen, TargetId target);

    /// What a target's screens were when it was last drawn, one per screen, to
    /// tell whether it needs drawing again.
    struct DrawnScreen
    {
        std::optional<ScreenAnchor> anchor;
        const Screen *screen = nullptr;
        uint64_t revision = 0; ///< its tree's, after drawing
        uint64_t shownAt = 0;
        float scale = 0.f;
        float opacity = 0.f;

        bool operator==(const DrawnScreen &) const = default;
    };

    /// A surface other than the window that screens draw into.
    struct Target
    {
        DrawList drawList;
        std::vector<DrawnScreen> drawn;
        PointerMap pointerMap;
        /// The look revision it was last drawn at.
        uint64_t drawnLook = 0;
        Extent size;
        TargetId id;
        /// What the host made to show it, if anything.
        std::optional<TargetTexture> texture;
        /// Whether it has been drawn at all; the first Sync after it is made
        /// always draws it.
        bool everDrawn = false;
        /// Whether a screen on it was being worked when it was last synced.
        bool drawnWorked = false;
        /// Whether the last Sync drew it.
        bool redrawn = false;
    };

    [[nodiscard]] Target *FindTarget(TargetId target);
    [[nodiscard]] const Target *FindTarget(TargetId target) const;

    /// What @p screens look like now, for comparing with what was drawn.
    [[nodiscard]] static std::vector<DrawnScreen> Snapshot(const std::vector<Screen *> &screens);

    /// Draws @p target's shown screens again, if any of them changed since it
    /// was last drawn.
    void SyncTarget(Target &target);

    /// Restyles, lays out and draws @p screen into @p list, against @p area at
    /// @p scale. Its bindings have been refreshed already.
    void DrawScreen(Screen &screen, Extent area, float scale, DrawList &list);

    /// Which step the host loop owes next.
    enum class FrameStep : uint8_t
    {
        AwaitingInput,
        AwaitingSync,
        Count_
    };

    /// Everything the UI remembers about the screen it is working.
    ///
    /// All of it is meaningless the moment that screen changes: a node id names
    /// a slot in one screen's tree, and the same slot in another screen holds
    /// an unrelated node. Kept in one struct and cleared by assigning a fresh
    /// one, so a field added here later cannot be the one somebody forgets.
    /// Where a caret is: its field, the caret and the other end of any
    /// selection, and how much text there is. Any change restarts the blink.
    struct CaretSeen
    {
        std::size_t length = 0;
        NodeId field;
        uint32_t caret = 0;
        uint32_t anchor = 0;

        [[nodiscard]] friend bool operator==(const CaretSeen &, const CaretSeen &) = default;
    };

    struct InputSession
    {
        Interaction interaction;
        KeyRepeat<UiAction> repeatAction; ///< the direction held down
        KeyRepeat<EditKey> repeatEdit;    ///< the editing key held down
        double lastPressAt = 0.0;         ///< when the last press landed, for counting clicks
        NodeId lastPressed;               ///< what it landed on
        NodeId focusedLast;               ///< what had focus when the last frame ended
        uint32_t clicks = 0;              ///< presses in the run the last one belongs to
        /// Where the focused node's caret was last frame, and since when it
        /// has been there, which its blink is counted from.
        CaretSeen caretSeen;
        double caretSince = 0.0;
    };

    /// A struct on no entity that bindings can reach. Not owned.
    struct Source
    {
        const Core::Reflect::StructSpec *spec = nullptr;
        void *data = nullptr;
    };

    /// Whether @p screen draws over @p other: by sort key, and within a key by
    /// which was shown later.
    [[nodiscard]] static bool Above(const Screen &screen, const Screen &other);

    /// The topmost shown screen on the window whose kind takes input, or the
    /// screen in use when there is none, or null.
    [[nodiscard]] Screen *TopInputScreen();

    /// Hands the keys from the screen that had them to whichever should have
    /// them now, saving the one's focus and restoring the other's. Everything
    /// the old screen's ids meant goes with them.
    void RefreshInputScreen();

    /// Every shown screen on @p target, lowest first, from the topmost one that
    /// hides what is beneath it. What is under such a screen is not drawn and
    /// not laid out: it cannot be seen, so placing it would be work for nothing.
    [[nodiscard]] std::vector<Screen *> DrawOrder(TargetId target);

    /// Carries every scrolling node on every shown screen closer to where it is
    /// headed, over @p seconds since the last frame.
    void AdvanceScrolling(double seconds);

    /// Moves hover, presses and focus for @p input on @p screen.
    InputResult Interact(Screen &screen, const UiInput &input);
    /// @p input as @p screen sees it: with the pointer passed through its
    /// target's PointerMap when it is on one, and claimed where that misses.
    [[nodiscard]] UiInput OnTarget(const Screen &screen, const UiInput &input) const;
    /// Answers what happened to every screen's contexts since the last input
    /// step. Here rather than in Sync, which runs after the frame's systems:
    /// an event pushed there would be dropped before any of them read it.
    void AnnounceContexts();
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
    /// Starts the caret's blink again when its field or where it stands
    /// changed since last frame, and says whether it shows at @p now.
    void Blink(const Screen *screen, double now);
    /// Lays @p screen out against @p viewport at @p scale: across the whole of
    /// it, or at its own size where its anchor puts it.
    void LayOut(Screen &screen, Extent viewport, float scale);

    /// Every screen that exists, in the order it was made. Not owned: a screen
    /// puts itself here and takes itself away, and whatever owns it outlives
    /// the entry.
    std::vector<Screen *> _screens;
    /// Few, so a scan finds one.
    std::vector<Source> _sources;
    /// Every target but the window. Few, so a scan finds one.
    std::vector<Target> _targets;
    TargetMaker _makeTarget;
    TargetReleaser _releaseTarget;
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
    /// The screen on a target the player is using, if any.
    Screen *_inUse = nullptr;
    Core::EventQueue &_universal;
    const Font *_font = nullptr;
    const StringTables *_tables = nullptr;
    const Themes *_themes = nullptr;
    /// The theme every screen wears, which a screen made later is given too.
    ThemeView _theme;
    /// Every sheet's sprites, by `sheet:name`.
    std::unordered_map<Core::InternedString, Sprite> _sprites;
    /// Every picture named by its texture's path that a screen has asked for,
    /// by that path, the placeholder standing in for one that did not load.
    std::unordered_map<Core::InternedString, Sprite> _textures;
    TextureLoader _textureLoader;
    /// The same for pictures named by asset id.
    std::unordered_map<Core::AssetId, Sprite> _assetTextures;
    AssetTextureLoader _assetTextureLoader;
    /// How many screens have ever been shown, which orders those sharing a
    /// sort key.
    uint64_t _showSequence = 0;
    /// Moves when something every screen is drawn with changes — the font, the
    /// sprite sheets, how a UI pixel is measured — so every target draws again.
    uint64_t _lookRevision = 0;
    /// The id the next target is given; the window's is zero.
    uint32_t _nextTarget = 1;
    double _lastTime = 0.0; ///< the previous frame's clock, for what moves over time
    Point _lastPointer;
    TextureId _fontAtlas = kWhiteTexture;
    TextureId _placeholderTexture = kWhiteTexture;
    Extent _viewport;
    float _userScale = 1.f;
    FrameStep _nextStep = FrameStep::AwaitingInput;
    NavWrap _navWrap = NavWrap::Around;
    ScaleMatch _scaleMatch = ScaleMatch::ShorterSide;
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
