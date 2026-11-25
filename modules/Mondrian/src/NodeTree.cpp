/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/NodeTree.hpp>

#include <Assisi/Core/Assert.hpp>

#include <utility>
#include <vector>

namespace Assisi::Mondrian
{

NodeTree::NodeTree()
{
    _slots.emplace_back();
    _slots[0].alive = true;
    _root = IdOf(0);
}

const Node *NodeTree::Get(NodeId id) const
{
    if (id.index >= _slots.size())
    {
        return nullptr;
    }
    const Node &node = _slots[id.index];
    return node.alive && node.generation == id.generation ? &node : nullptr;
}

Node *NodeTree::GetMutable(NodeId id)
{
    // Every change to a node comes through here, so counting here misses none.
    Node *const node = const_cast<Node *>(std::as_const(*this).Get(id));
    if (node != nullptr)
    {
        ++_revision;
    }
    return node;
}

std::expected<NodeId, NameError> NodeTree::Create(NodeId parent, std::string_view name)
{
    // Checked before the node exists, so a refusal leaves no nameless node
    // behind for the caller to find and clean up.
    if (!name.empty() && Find(name))
    {
        return std::unexpected(NameError::Taken);
    }
    const NodeId id = Create(parent);
    if (Node *node = GetMutable(id))
    {
        node->name = name;
    }
    return id;
}

NodeId NodeTree::Create(NodeId parent)
{
    if (!IsAlive(parent))
    {
        return {};
    }

    uint32_t index = 0;
    if (_free.empty())
    {
        index = static_cast<uint32_t>(_slots.size());
        _slots.emplace_back();
    }
    else
    {
        index = _free.back();
        _free.pop_back();
    }

    // Everything but the generation starts over; the generation is what makes
    // the ids of the slot's previous occupants stale.
    Node &node = _slots[index];
    const uint32_t generation = node.generation;
    node = Node{};
    node.generation = generation;
    node.alive = true;
    node.parent = parent;
    const NodeId id = IdOf(index);

    // Last among the siblings, so children lay out in the order they were made.
    Node &parentNode = *GetMutable(parent);
    if (!parentNode.firstChild)
    {
        parentNode.firstChild = id;
    }
    else
    {
        NodeId last = parentNode.firstChild;
        while (_slots[last.index].nextSibling)
        {
            last = _slots[last.index].nextSibling;
        }
        _slots[last.index].nextSibling = id;
    }
    return id;
}

void NodeTree::Unlink(NodeId id)
{
    const Node &node = _slots[id.index];
    Node &parent = _slots[node.parent.index];
    if (parent.firstChild == id)
    {
        parent.firstChild = node.nextSibling;
        return;
    }
    NodeId previous = parent.firstChild;
    while (_slots[previous.index].nextSibling != id)
    {
        previous = _slots[previous.index].nextSibling;
    }
    _slots[previous.index].nextSibling = node.nextSibling;
}

void NodeTree::Destroy(NodeId id)
{
    if (!IsAlive(id))
    {
        return;
    }
    ASSISI_ASSERT(id != _root, "the UI root cannot be destroyed");
    ++_revision;
    Unlink(id);

    // Depth-first over the subtree with an explicit stack, so a deep tree
    // cannot overflow the call stack.
    std::vector<NodeId> pending{id};
    while (!pending.empty())
    {
        const NodeId current = pending.back();
        pending.pop_back();
        Node &node = _slots[current.index];
        for (NodeId child = node.firstChild; child; child = _slots[child.index].nextSibling)
        {
            pending.push_back(child);
        }
        node.alive = false;
        ++node.generation;
        node.text.clear();
        node.name.clear();
        _free.push_back(current.index);
    }
}

void NodeTree::SetText(NodeId id, std::string_view text)
{
    if (Node *node = GetMutable(id))
    {
        node->text = text;
        node->textKey.clear();
    }
}

void NodeTree::SetKeyedText(NodeId id, std::string_view key, std::string_view text)
{
    if (Node *node = GetMutable(id))
    {
        node->text = text;
        node->textKey = key;
    }
}

std::expected<void, NameError> NodeTree::SetName(NodeId id, std::string_view name)
{
    Node *node = GetMutable(id);
    if (node == nullptr)
    {
        return {};
    }
    if (!name.empty())
    {
        const NodeId holder = Find(name);
        if (holder && holder != id)
        {
            return std::unexpected(NameError::Taken);
        }
    }
    node->name = name;
    return {};
}

void NodeTree::SetVisible(NodeId id, bool visible)
{
    // As SetValue: a binding writes this every frame.
    if (const Node *const held = Get(id); held == nullptr || held->visible == visible)
    {
        return;
    }
    if (Node *node = GetMutable(id))
    {
        node->visible = visible;
    }
}

void NodeTree::Relook(Node &node) const
{
    LookBase base;
    if (node.part != kNoPart)
    {
        // A part's look follows its control's kind and the style its control
        // names.
        const Node &control = _slots[node.parent.index];
        const WidgetType *type = _widgets.Get(control.behaviour);
        if (type != nullptr && node.part < type->parts.size())
        {
            const WidgetPart &part = type->parts[node.part];
            base = LookBase{
                .defaults = &part.look, .kind = type->name, .part = part.name, .style = control.look.styleName};
        }
    }
    else if (const WidgetType *type = _widgets.Get(node.behaviour); type != nullptr)
    {
        base = LookBase{.defaults = type->look, .kind = type->name, .part = {}, .style = {}};
    }
    ResolvedLook resolved = ResolveLook(node.look, _theme, base);
    node.resting = std::move(resolved.resting);
    node.states = std::move(resolved.states);
    node.style = node.resting.values;
}

void NodeTree::RelookParts(NodeId id)
{
    const Node *control = Get(id);
    if (control == nullptr)
    {
        return;
    }
    for (NodeId child = control->firstChild; child; child = _slots[child.index].nextSibling)
    {
        if (_slots[child.index].part != kNoPart)
        {
            Relook(_slots[child.index]);
        }
    }
}

NodeId NodeTree::Part(NodeId control, std::string_view name) const
{
    const WidgetType *type = WidgetOf(control);
    if (type == nullptr)
    {
        return {};
    }
    for (NodeId child = Get(control)->firstChild; child; child = _slots[child.index].nextSibling)
    {
        const uint8_t part = _slots[child.index].part;
        if (part < type->parts.size() && type->parts[part].name == name)
        {
            return child;
        }
    }
    return {};
}

NodeId NodeTree::Part(NodeId control, uint8_t part) const
{
    const Node *node = Get(control);
    if (node == nullptr || part == kNoPart)
    {
        return {};
    }
    for (NodeId child = node->firstChild; child; child = _slots[child.index].nextSibling)
    {
        if (_slots[child.index].part == part)
        {
            return child;
        }
    }
    return {};
}

void NodeTree::SetStyle(NodeId id, const Style &style)
{
    if (Node *node = GetMutable(id))
    {
        node->look.style = style;
        node->look.written = kEveryProperty;
        node->look.tokens.clear();
        Relook(*node);
    }
}

void NodeTree::SetOwnProperty(NodeId id, StyleProperty property, const Style &from)
{
    Node *node = GetMutable(id);
    if (node == nullptr)
    {
        return;
    }
    StylePatch own{.values = node->look.style, .written = node->look.written, .state = StyleState::Focused};
    own.Take(property, from);
    node->look.style = own.values;
    node->look.written = own.written;
    std::erase_if(node->look.tokens, [property](const TokenUse &use) { return use.property == property; });
    Relook(*node);
}

void NodeTree::SetStateStyle(NodeId id, const StylePatch &look)
{
    Node *node = GetMutable(id);
    if (node == nullptr)
    {
        return;
    }
    std::erase_if(node->look.states, [&look](const ThemedPatch &held) { return held.patch.state == look.state; });
    if (look.written != 0)
    {
        node->look.states.push_back(ThemedPatch{.patch = look, .tokens = {}});
    }
    Relook(*node);
}

void NodeTree::SetLook(NodeId id, NodeLook look)
{
    if (Node *node = GetMutable(id))
    {
        node->look = std::move(look);
        Relook(*node);
        RelookParts(id);
    }
}

void NodeTree::SetTheme(const ThemeView &theme)
{
    _theme = theme;
    ++_revision;
    for (Node &node : _slots)
    {
        if (node.alive)
        {
            Relook(node);
        }
    }
}

void NodeTree::SetImage(NodeId id, const Sprite &sprite)
{
    if (Node *node = GetMutable(id))
    {
        node->image = sprite;
        node->hasImage = true;
    }
}

void NodeTree::ClearImage(NodeId id)
{
    if (Node *node = GetMutable(id))
    {
        node->hasImage = false;
    }
}

void NodeTree::SetScrollOffset(NodeId id, Point offset)
{
    if (Node *node = GetMutable(id))
    {
        node->scrollOffset = offset;
    }
}

void NodeTree::SetBehaviour(NodeId id, uint32_t behaviour)
{
    const Node *held = Get(id);
    if (held == nullptr || held->behaviour == behaviour)
    {
        return;
    }
    // The old kind's parts go first, so a part never outlives the control it
    // belongs to.
    std::vector<NodeId> parts;
    for (NodeId child = held->firstChild; child; child = _slots[child.index].nextSibling)
    {
        if (_slots[child.index].part != kNoPart)
        {
            parts.push_back(child);
        }
    }
    for (const NodeId part : parts)
    {
        Destroy(part);
    }

    Node *node = GetMutable(id);
    node->behaviour = behaviour;
    Relook(*node);
    const WidgetType *type = _widgets.Get(behaviour);
    if (type == nullptr)
    {
        return;
    }
    ASSISI_ASSERT(type->parts.size() <= kMaxWidgetParts, "a kind of control has more parts than a view carries");
    for (std::size_t index = 0; index < type->parts.size(); ++index)
    {
        Node &part = _slots[Create(id).index];
        part.part = static_cast<uint8_t>(index);
        Relook(part);
    }
}

void NodeTree::SetFocusable(NodeId id, bool focusable)
{
    if (Node *node = GetMutable(id))
    {
        node->focusable = focusable;
    }
}

void NodeTree::SetEnabled(NodeId id, bool enabled)
{
    if (Node *node = GetMutable(id))
    {
        node->enabled = enabled;
    }
}

void NodeTree::SetBlocksPointer(NodeId id, bool blocks)
{
    if (Node *node = GetMutable(id))
    {
        node->blocksPointer = blocks;
    }
}

void NodeTree::SetTakesKeyboard(NodeId id, bool takes)
{
    if (Node *node = GetMutable(id))
    {
        node->takesKeyboard = takes;
    }
}

void NodeTree::SetNavOverride(NodeId id, NavDirection direction, NodeId target)
{
    if (Node *node = GetMutable(id))
    {
        node->navOverride[static_cast<std::size_t>(direction)] = target;
    }
}

void NodeTree::SetOnActivate(NodeId id, std::function<void(const Core::EventRoute &)> push)
{
    if (Node *node = GetMutable(id))
    {
        node->onActivate = std::move(push);
    }
}

void NodeTree::SetOnChange(NodeId id, std::function<void(const Core::EventRoute &, const Node &)> push)
{
    if (Node *node = GetMutable(id))
    {
        node->onChange = std::move(push);
    }
}

void NodeTree::SetOnSubmit(NodeId id, std::function<void(const Core::EventRoute &, const Node &)> push)
{
    if (Node *node = GetMutable(id))
    {
        node->onSubmit = std::move(push);
    }
}

void NodeTree::SetTextEdit(NodeId id, const TextEdit &edit)
{
    if (Node *node = GetMutable(id))
    {
        node->edit = edit;
    }
}

void NodeTree::SetValue(NodeId id, WidgetValue value)
{
    // A binding writes its value every frame; one that has not moved changes
    // nothing, and must not make the screen look changed.
    if (const Node *const held = Get(id); held == nullptr || held->value == value)
    {
        return;
    }
    if (Node *node = GetMutable(id))
    {
        node->value = std::move(value);
    }
}

void NodeTree::SetSteps(NodeId id, int32_t steps)
{
    if (Node *node = GetMutable(id))
    {
        node->steps = steps;
    }
}

void NodeTree::SetRange(NodeId id, SliderRange range)
{
    if (Node *node = GetMutable(id))
    {
        node->range = range;
    }
}

const WidgetType *NodeTree::WidgetOf(NodeId id) const
{
    const Node *node = Get(id);
    return node != nullptr ? _widgets.Get(node->behaviour) : nullptr;
}

NodeId NodeTree::Find(std::string_view name) const
{
    for (uint32_t index = 0; index < _slots.size(); ++index)
    {
        if (_slots[index].alive && _slots[index].name == name)
        {
            return IdOf(index);
        }
    }
    return {};
}

} // namespace Assisi::Mondrian
