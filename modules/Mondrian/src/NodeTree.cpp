/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/NodeTree.hpp>

#include <Assisi/Core/Assert.hpp>

#include <utility>

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
    return const_cast<Node *>(std::as_const(*this).Get(id));
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
    if (Node *node = GetMutable(id))
    {
        node->visible = visible;
    }
}

void NodeTree::SetStyle(NodeId id, const Style &style)
{
    if (Node *node = GetMutable(id))
    {
        node->style = style;
    }
}

void NodeTree::SetImage(NodeId id, TextureId texture, const Rect &uv)
{
    if (Node *node = GetMutable(id))
    {
        node->image = texture;
        node->imageUv = uv;
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
    if (Node *node = GetMutable(id))
    {
        node->behaviour = behaviour;
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

void NodeTree::SetOnActivate(NodeId id, std::function<void(Core::EventQueue &)> push)
{
    if (Node *node = GetMutable(id))
    {
        node->onActivate = std::move(push);
    }
}

void NodeTree::SetOnChange(NodeId id, std::function<void(Core::EventQueue &, const Node &)> push)
{
    if (Node *node = GetMutable(id))
    {
        node->onChange = std::move(push);
    }
}

void NodeTree::SetOnSubmit(NodeId id, std::function<void(Core::EventQueue &, const Node &)> push)
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

void NodeTree::SetSliderButtons(NodeId id, SliderButtons buttons)
{
    if (Node *node = GetMutable(id))
    {
        node->sliderButtons = buttons;
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
