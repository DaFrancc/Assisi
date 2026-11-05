/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/ScreenLoader.hpp>

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

/// Whether the table is a tree this build can walk in one pass. The blob reader
/// checks the same thing, and this is not a duplicate of it: a document can be
/// built in code and handed straight here, never having been bytes.
bool IsWalkable(const ScreenDocument &document)
{
    if (document.nodes.empty())
    {
        return false;
    }
    if (document.focus != kNoNode && document.focus >= document.nodes.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const uint32_t parent = document.nodes[index].parent;
        const bool rootParent = index == 0 && parent == kNoNode;
        const bool childParent = index > 0 && parent < index;
        if (!rootParent && !childParent)
        {
            return false;
        }
    }
    return true;
}

/// Whether @p node's verb has what it takes: a target it can act on when it
/// takes one and none when it does not, and a count of moves that moves.
bool VerbFits(const ScreenDocument &document, const ScreenNode &node)
{
    if (VerbTakesTarget(node.verb))
    {
        if (node.target >= document.nodes.size() || !VerbActsOn(node.verb, document.nodes[node.target].widget))
        {
            return false;
        }
    }
    else if (node.target != kNoNode)
    {
        return false;
    }
    return !VerbTakesMoves(node.verb) || node.moves != 0;
}

/// Whether @p action is one a context can carry out: a verb that needs no
/// node to act on, or an event @p catalog has.
std::expected<void, ScreenLoadError> CheckContextAction(const ContextAction &action, const Core::EventCatalog &catalog)
{
    switch (action.action)
    {
    case ActionKind::None:
        return {};
    case ActionKind::Verb:
        if (VerbTakesTarget(action.verb) || VerbTakesMoves(action.verb))
        {
            return std::unexpected(ScreenLoadError::BadContext);
        }
        return {};
    case ActionKind::Event:
        if (catalog.Find(action.eventName.View()) == nullptr)
        {
            return std::unexpected(ScreenLoadError::UnknownEvent);
        }
        return {};
    case ActionKind::Count_:
        break;
    }
    return std::unexpected(ScreenLoadError::BadContext);
}

/// Whether every context in @p document has a name of its own and actions it
/// can carry out.
std::expected<void, ScreenLoadError> ResolveContexts(const ScreenDocument &document, const Core::EventCatalog &catalog)
{
    std::unordered_set<Core::InternedString> names;
    for (const ScreenContext &context : document.contexts)
    {
        if (!names.insert(context.name).second)
        {
            return std::unexpected(ScreenLoadError::BadContext);
        }
        for (const ContextAction &action : context.actions)
        {
            if (const std::expected<void, ScreenLoadError> checked = CheckContextAction(action, catalog); !checked)
            {
                return checked;
            }
        }
    }
    return {};
}

/// Whether @p written is one binding naming a field of something @p world has
/// that @p use can take.
bool SoleBindingResolves(std::string_view written, BindingUse use, const ScreenWorld &world)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    return source && ResolveBinding(*source, world.FindOwner(source->component), use).has_value();
}

/// Whether every binding on @p node reads, and names a field of something
/// @p world has that its place can take.
bool BindingsResolve(const ScreenDocument &document, const ScreenNode &node, const ScreenWorld &world)
{
    if (node.textIsBound)
    {
        const std::expected<std::vector<TextRun>, BindingError> runs = SplitBoundText(document.Text(node.text));
        if (!runs)
        {
            return false;
        }
        for (const TextRun &run : *runs)
        {
            if (run.bound && !ResolveBinding(run.binding, world.FindOwner(run.binding.component), BindingUse::Text))
            {
                return false;
            }
        }
    }
    const std::string_view visible = document.Text(node.visibleBinding);
    if (!visible.empty() && !SoleBindingResolves(visible, BindingUse::Bool, world))
    {
        return false;
    }
    const std::string_view value = document.Text(node.valueBinding);
    if (value.empty())
    {
        return true;
    }
    const std::optional<BindingUse> use = ControlUse(node.widget);
    return use && SoleBindingResolves(value, *use, world);
}

/// Whether the event @p node pushes when it changes is one it can push.
std::expected<void, ScreenLoadError> CheckChange(const ScreenNode &node, const Core::EventCatalog &catalog)
{
    if (node.changeEvent.Empty())
    {
        return {};
    }
    const Core::EventDefinition *const event = catalog.Find(node.changeEvent.View());
    if (event == nullptr)
    {
        return std::unexpected(ScreenLoadError::UnknownEvent);
    }
    if (!ChangePushes(node.widget, event->valueKind))
    {
        return std::unexpected(ScreenLoadError::BadChange);
    }
    return {};
}

/// Whether every name in @p document resolves, and every node's contents suit
/// where it sits — and the patterns, compiled, for the caller to apply.
///
/// Answered before anything is built: a screen joins its Ui in its own
/// constructor, so one abandoned part-way would stay in it. Compiling the
/// patterns here rather than while building is what keeps that true, and it
/// leaves the build loop with no failure it would have to unwind from.
///
/// One slot per node, null wherever a node has no pattern.
std::expected<std::vector<std::shared_ptr<const Pattern>>, ScreenLoadError> Resolve(const ScreenDocument &document,
                                                                                    const Core::EventCatalog &catalog,
                                                                                    const ScreenWorld &world)
{
    std::vector<std::shared_ptr<const Pattern>> patterns;
    patterns.resize(document.nodes.size());

    std::unordered_set<Core::InternedString> names;

    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        // Checked here rather than left to the tree, which would refuse the
        // second one only after the first half of the screen was built.
        if (!node.name.Empty() && !names.insert(node.name).second)
        {
            return std::unexpected(ScreenLoadError::DuplicateName);
        }

        // The root is the screen itself. A control there would have nothing to
        // be created under, and an action there would never fire.
        if (index == 0 && (node.widget != BuiltinWidget::None || node.action != ActionKind::None))
        {
            return std::unexpected(ScreenLoadError::MisplacedNode);
        }

        if (static_cast<uint32_t>(node.widget) >= static_cast<uint32_t>(BuiltinWidget::Count_))
        {
            return std::unexpected(ScreenLoadError::UnsupportedWidget);
        }

        if (node.action == ActionKind::Event && catalog.Find(node.eventName.View()) == nullptr)
        {
            return std::unexpected(ScreenLoadError::UnknownEvent);
        }

        if (node.action == ActionKind::Verb && !VerbFits(document, node))
        {
            return std::unexpected(ScreenLoadError::BadTarget);
        }

        if (const std::expected<void, ScreenLoadError> change = CheckChange(node, catalog); !change)
        {
            return std::unexpected(change.error());
        }

        // Only plain text and a button's label are shown as they come: a
        // field's text is the player's.
        const bool bindsText = node.widget == BuiltinWidget::None || node.widget == BuiltinWidget::Button;
        if ((node.textIsBound && !bindsText) || !BindingsResolve(document, node, world))
        {
            return std::unexpected(ScreenLoadError::BadBinding);
        }

        const std::string_view pattern = document.Text(node.pattern);
        if (node.widget == BuiltinWidget::TextField && !pattern.empty())
        {
            std::expected<std::shared_ptr<const Pattern>, PatternError> compiled = CompilePattern(pattern);
            if (!compiled)
            {
                return std::unexpected(ScreenLoadError::BadPattern);
            }
            patterns[index] = *std::move(compiled);
        }
    }
    if (const std::expected<void, ScreenLoadError> contexts = ResolveContexts(document, catalog); !contexts)
    {
        return std::unexpected(contexts.error());
    }
    return patterns;
}

/// The node @p node describes, built by the same call a screen written in C++
/// would make. Every control gives its node a style of its own, which the
/// caller replaces whole with the document's look: what a control's call would
/// have set is already under what the file wrote, so the look is the answer
/// and not an override.
NodeId Create(Screen &screen, NodeId parent, const ScreenDocument &document, const ScreenNode &node)
{
    switch (node.widget)
    {
    case BuiltinWidget::Button:
        return screen.AddButton(parent, document.Text(node.text)).node;
    case BuiltinWidget::Toggle:
        return screen.AddToggle(parent, node.on).node;
    case BuiltinWidget::ContinuousSlider:
        return screen.AddContinuousSlider(parent, node.range, node.value).node;
    case BuiltinWidget::SteppedSlider:
        return screen.AddSteppedSlider(parent, node.range, node.steps, node.step).node;
    case BuiltinWidget::Scroll:
        // Which axes scroll is a style field, so the style the document carries
        // is already the answer this argument wants.
        return screen.AddScroll(parent, node.style, node.style.enabledScrollBars);
    case BuiltinWidget::TextField:
        return screen.AddTextField(parent, node.lines).node;
    case BuiltinWidget::None:
    case BuiltinWidget::Count_:
        break;
    }
    return screen.Add(parent, node.style);
}

/// What a field shows and how it grows: everything it carries that no pattern
/// or text depends on, so it can come first.
void ApplyTextFieldShape(Screen &screen, TextFieldId field, const ScreenDocument &document, const ScreenNode &node)
{
    const std::string_view placeholder = document.Text(node.placeholder);
    if (node.placeholderIsKey)
    {
        screen.SetPlaceholderKey(field, placeholder);
    }
    else
    {
        screen.SetPlaceholder(field, placeholder);
    }
    screen.SetMaxLength(field, node.maxLength);
    // Only where the file asked for one. A field is unbounded already, and
    // saying so again would leave it holding a line count that an unbounded
    // field never had when it was built in code.
    if (node.height != TextHeight::Unbounded)
    {
        screen.SetHeight(field, node.height, node.lineLimit);
    }
}

/// A field's starting text and its mask, once its pattern is set: setting text
/// settles the field against whatever pattern it has by then. @p ui gives what
/// a keyed text says.
void ApplyTextFieldText(Screen &screen, TextFieldId field, const ScreenDocument &document, const ScreenNode &node,
                        const Ui &ui)
{
    // A keyed starting text is looked up once: from then on it is the player's
    // to change, and new string tables leave it alone.
    const std::string_view text = document.Text(node.text);
    if (!text.empty())
    {
        screen.SetText(field, node.textIsKey ? ui.StringFor(text) : text);
    }
    // Last: masking a field also turns copy and cut off, so anything after it
    // that touched the abilities would quietly turn them back on.
    screen.SetMask(field, node.mask);
}

/// How @p node asks to look, as the file wrote it.
NodeLook LookOf(const ScreenNode &node)
{
    return NodeLook{.style = node.style,
                    .tokens = node.tokens,
                    .states = node.states,
                    .styleName = node.styleName,
                    .written = node.written};
}

/// The picture @p node shows, put on @p id: a sprite from a sheet @p ui has,
/// or an image @p ui loads by its path.
void ApplyPicture(Ui &ui, NodeTree &tree, NodeId id, const ScreenNode &node)
{
    if (!node.sprite.Empty())
    {
        const Sprite *const sprite = ui.FindSprite(node.sprite.View());
        if (sprite == nullptr)
        {
            // The cook refuses a screen naming a sprite no listed sheet has, so
            // this is a stale package or a sheet that did not load. A screen
            // with a picture missing is still worth showing.
            Core::Log::Error("Mondrian: no sprite sheet the UI has holds '{}'; it shows the placeholder.",
                             node.sprite.View());
        }
        tree.SetImage(id, sprite != nullptr ? *sprite : WholeTexture(ui.GetPlaceholderTexture(), 0, 0));
        return;
    }
    if (!node.texture.Empty())
    {
        tree.SetImage(id, ui.TextureSprite(node.texture.View()));
    }
}

/// What @p node shows from the screen's contexts, after its flags: a bound
/// visible hides the node until it has read.
void ApplyBindings(Screen &screen, NodeId id, const ScreenDocument &document, const ScreenNode &node)
{
    if (node.textIsBound)
    {
        const std::expected<void, BindingError> bound = screen.BindText(id, document.Text(node.text));
        ASSISI_ASSERT(bound.has_value(), "a binding Resolve passed was refused on the screen it built");
    }
    if (const std::string_view visible = document.Text(node.visibleBinding); !visible.empty())
    {
        const std::expected<void, BindingError> bound = screen.BindVisible(id, visible);
        ASSISI_ASSERT(bound.has_value(), "a binding Resolve passed was refused on the screen it built");
    }
    if (const std::string_view value = document.Text(node.valueBinding); !value.empty())
    {
        const std::expected<void, BindingError> bound = screen.BindControl(id, value);
        ASSISI_ASSERT(bound.has_value(), "a binding Resolve passed was refused on the screen it built");
    }
}

/// Pushes @p value, what a control holds, through @p push.
void PushValue(const std::function<void(const Core::EventRoute &, const Core::EventValue &)> &push,
               const Core::EventRoute &events, const WidgetValue &value)
{
    if (const bool *const on = std::get_if<bool>(&value))
    {
        push(events, *on);
    }
    else if (const float *const rests = std::get_if<float>(&value))
    {
        push(events, *rests);
    }
    else if (const int32_t *const step = std::get_if<int32_t>(&value))
    {
        push(events, *step);
    }
}

/// What @p node pushes when the player moves it.
void ApplyChange(NodeTree &tree, NodeId id, const ScreenNode &node, const Core::EventCatalog &catalog)
{
    if (node.changeEvent.Empty())
    {
        return;
    }
    // Resolved in Resolve, which ran before anything was built.
    const Core::EventDefinition &event = *catalog.Find(node.changeEvent.View());
    if (event.pushValue)
    {
        tree.SetOnChange(id, [push = event.pushValue](const Core::EventRoute &events, const Node &changed)
                         { PushValue(push, events, changed.value); });
        return;
    }
    tree.SetOnChange(id, [push = event.push](const Core::EventRoute &events, const Node &) { push(events); });
}

/// Everything about a node that is neither its style nor its place.
void ApplyFlags(NodeTree &tree, NodeId id, const ScreenNode &node)
{
    tree.SetVisible(id, node.visible);
    tree.SetEnabled(id, node.enabled);
    tree.SetBlocksPointer(id, node.blocksPointer);
    tree.SetTakesKeyboard(id, node.takesKeyboard);
}

/// What node @p index does when it fires. @p ids is every node's id by its
/// place in the document, so a verb's target is one lookup here and none per
/// click.
void ApplyAction(Screen &screen, const std::vector<NodeId> &ids, std::size_t index, const ScreenDocument &document,
                 const Core::EventCatalog &catalog)
{
    const ScreenNode &node = document.nodes[index];
    const NodeId id = ids[index];
    switch (node.action)
    {
    case ActionKind::None:
        return;
    case ActionKind::Verb:
        switch (node.verb)
        {
        case ScreenVerb::Hide:
            // Carried on the node rather than announced: closing a screen
            // touches nothing but the UI, so it works wherever the screen is
            // shown with nothing named anywhere.
            screen.OnActivate(id, [](Screen &self) { self.Hide(); });
            return;
        case ScreenVerb::Step:
            // Resolve checked the target is a slider in the table.
            screen.OnActivate(id, [target = ids[node.target], moves = node.moves](Screen &self)
                              { self.Step(target, moves); });
            return;
        case ScreenVerb::Count_:
            return;
        }
        return;
    case ActionKind::Event:
        // Resolved in Resolve, which ran before anything was built.
        screen.Tree().SetOnActivate(id, catalog.Find(node.eventName.View())->push);
        return;
    case ActionKind::Count_:
        return;
    }
}

/// What the context called @p name does at @p moment, from @p action.
void ApplyContextAction(Screen &screen, std::string_view name, ContextMoment moment, const ContextAction &action,
                        const Core::EventCatalog &catalog)
{
    switch (action.action)
    {
    case ActionKind::None:
    case ActionKind::Count_:
        return;
    case ActionKind::Verb:
        switch (action.verb)
        {
        case ScreenVerb::Hide:
            screen.OnContext(name, moment, [](Screen &self) { self.Hide(); });
            return;
        // Refused by ResolveContexts: a context has no slider of its own to move.
        case ScreenVerb::Step:
        case ScreenVerb::Count_:
            return;
        }
        return;
    case ActionKind::Event:
        // Resolved in ResolveContexts, which ran before anything was built.
        screen.SetOnContext(name, moment, catalog.Find(action.eventName.View())->push);
        return;
    }
}

/// Every context @p document declares, with what it does at each moment. Made
/// whether or not it does anything, so each one the file declares is on the
/// screen from the start.
void ApplyContexts(Screen &screen, const ScreenDocument &document, const Core::EventCatalog &catalog)
{
    for (const ScreenContext &context : document.contexts)
    {
        screen.SetContext(context.name.View(), EntityId{});
        for (std::size_t moment = 0; moment < kContextMomentCount; ++moment)
        {
            ApplyContextAction(screen, context.name.View(), static_cast<ContextMoment>(moment),
                               context.actions[moment], catalog);
        }
    }
}

} // namespace

std::string_view ToString(ScreenLoadError error) noexcept
{
    switch (error)
    {
    case ScreenLoadError::BadDocument:
        return "describes no tree that can be walked";
    case ScreenLoadError::UnknownEvent:
        return "names an event this build does not declare";
    case ScreenLoadError::BadPattern:
        return "holds a pattern this build does not compile";
    case ScreenLoadError::UnsupportedWidget:
        return "names a control this build does not have";
    case ScreenLoadError::MisplacedNode:
        return "puts a control or an action where one cannot go";
    case ScreenLoadError::DuplicateName:
        return "gives two nodes one name";
    case ScreenLoadError::BadTarget:
        return "gives a verb a target it cannot act on";
    case ScreenLoadError::BadContext:
        return "declares a context twice, or gives one a verb it cannot carry out";
    case ScreenLoadError::BadBinding:
        return "binds a field this build's components and structs do not have";
    case ScreenLoadError::BadChange:
        return "gives a control a change event it cannot hand its value to";
    case ScreenLoadError::Count_:
        break;
    }
    return "unknown";
}

std::expected<LoadedScreen, ScreenLoadError> InstantiateScreen(Ui &ui, ScreenWorld &world, std::string_view name,
                                                               const ScreenDocument &document,
                                                               const Core::EventCatalog &catalog)
{
    if (!IsWalkable(document))
    {
        return std::unexpected(ScreenLoadError::BadDocument);
    }

    std::expected<std::vector<std::shared_ptr<const Pattern>>, ScreenLoadError> patterns =
        Resolve(document, catalog, world);
    if (!patterns)
    {
        return std::unexpected(patterns.error());
    }

    LoadedScreen loaded;
    loaded.screen = std::make_unique<Screen>(ui, world, document.traits, document.sortKey, std::string{name});
    for (const Core::InternedString &system : document.systems)
    {
        loaded.systems.emplace_back(system.View());
    }

    Screen &screen = *loaded.screen;
    NodeTree &tree = screen.Tree();

    // Index in the table to id in the tree. The table is preorder with every
    // parent before its children, so a parent's id is always already here.
    std::vector<NodeId> ids;
    ids.reserve(document.nodes.size());

    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        NodeId id;
        if (index == 0)
        {
            // The tree comes with a root, which is never created and never
            // destroyed. The document's first node describes that one.
            id = screen.Root();
            tree.SetLook(id, LookOf(node));
        }
        else
        {
            // Created by the call its control names, then styled — the same
            // order a screen built in C++ uses.
            id = Create(screen, ids[node.parent], document, node);
            tree.SetLook(id, LookOf(node));

            // A control took its text where it was created, from the label or
            // the starting contents its call takes. A keyed label is set again
            // through its key, so new string tables rewrite it.
            if (node.widget == BuiltinWidget::None && !node.textIsKey && !node.textIsBound)
            {
                tree.SetText(id, document.Text(node.text));
            }
            if ((node.widget == BuiltinWidget::None || node.widget == BuiltinWidget::Button) && node.textIsKey)
            {
                screen.SetTextKey(id, document.Text(node.text));
            }
            if (node.widget == BuiltinWidget::TextField)
            {
                const TextFieldId field{.node = id};
                ApplyTextFieldShape(screen, field, document, node);
                screen.SetPattern(field, std::move((*patterns)[index]), node.check);
                ApplyTextFieldText(screen, field, document, node, ui);
            }
        }

        // Resolve refused any name two nodes carry, and every node on this
        // screen came from this document.
        const std::expected<void, NameError> named = tree.SetName(id, node.name.View());
        ASSISI_ASSERT(named.has_value(), "a name Resolve passed was already taken on a screen it built");
        ApplyFlags(tree, id, node);
        ApplyBindings(screen, id, document, node);
        ApplyChange(tree, id, node, catalog);
        ApplyPicture(ui, tree, id, node);
        if (node.selectable)
        {
            screen.SetSelectable(id, true);
        }
        ids.push_back(id);
    }

    // After every node exists: a verb's target may come later in the document
    // than the button naming it.
    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        ApplyAction(screen, ids, index, document, catalog);
    }
    ApplyContexts(screen, document, catalog);

    if (document.focus != kNoNode)
    {
        ui.SetFocus(screen, ids[document.focus]);
    }
    return loaded;
}

} // namespace Assisi::Mondrian
