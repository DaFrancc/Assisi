/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Game/SpawnSystem.hpp>

#include <new>     /* ::operator new, std::align_val_t */
#include <utility> /* std::exchange */

namespace Assisi::Game
{

SpawnSystem::SpawnSystem(std::size_t initialSlots) : _nextPageSlots(initialSlots == 0 ? 1 : initialSlots)
{
    Grow();
}

SpawnSystem::~SpawnSystem() noexcept
{
    /* Destroy any remaining live objects. */
    for (Node *n = _liveHead; n; n = n->liveNext)
    {
        n->object.~WorldObject();
        n->alive = false;
    }

    /* Free pages. */
    while (_pages)
    {
        Page *p = std::exchange(_pages, _pages->next);
        ::operator delete(p->bytes, std::align_val_t{alignof(Node)});
        delete p;
    }
}

void SpawnSystem::Grow()
{
    Page *page = new Page{};
    page->slotCount = _nextPageSlots;
    page->next = _pages;
    _pages = page;

    const std::size_t bytes = sizeof(Node) * page->slotCount;
    page->bytes = static_cast<std::byte *>(::operator new(bytes, std::align_val_t{alignof(Node)}));

    /* Build free list from the newly allocated slots. */
    for (std::size_t i = 0; i < page->slotCount; ++i)
    {
        auto *node = reinterpret_cast<Node *>(page->bytes + i * sizeof(Node));
        node->freeNext = _freeList;
        _freeList = node;
    }

    _capacitySlots += page->slotCount;
    _nextPageSlots *= 2;
}

SpawnSystem::Node *SpawnSystem::AllocNode()
{
    if (!_freeList)
        Grow();

    Node *node = std::exchange(_freeList, _freeList->freeNext);
    node->freeNext = nullptr;
    node->alive = true;

    /* Construct WorldObject in-place. */
    ::new (&node->object) WorldObject{};
    return node;
}

void SpawnSystem::FreeNode(Node *node) noexcept
{
    node->freeNext = _freeList;
    _freeList = node;
}

void SpawnSystem::LinkLive(Node *node) noexcept
{
    node->livePrev = _liveTail;
    node->liveNext = nullptr;

    if (_liveTail)
        _liveTail->liveNext = node;
    else
        _liveHead = node;

    _liveTail = node;
}

void SpawnSystem::UnlinkLive(Node *node) noexcept
{
    if (node->livePrev)
        node->livePrev->liveNext = node->liveNext;
    else
        _liveHead = node->liveNext;

    if (node->liveNext)
        node->liveNext->livePrev = node->livePrev;
    else
        _liveTail = node->livePrev;

    node->livePrev = nullptr;
    node->liveNext = nullptr;
}

SpawnSystem::Handle SpawnSystem::Create()
{
    Node *node = AllocNode();
    LinkLive(node);
    ++_aliveCount;
    return reinterpret_cast<Handle>(node);
}

void SpawnSystem::Destroy(Handle handle)
{
    auto *node = reinterpret_cast<Node *>(handle);
    if (!node || !node->alive)
        return;

    UnlinkLive(node);

    node->object.~WorldObject(); /* Run object destructor explicitly. */
    node->alive = false;

    FreeNode(node);
    --_aliveCount;
}

WorldObject &SpawnSystem::Get(Handle handle)
{
    return reinterpret_cast<Node *>(handle)->object;
}

const WorldObject &SpawnSystem::Get(Handle handle) const
{
    return reinterpret_cast<const Node *>(handle)->object;
}

void SpawnSystem::Tick(float deltaSeconds)
{
    for (Node *n = _liveHead; n; n = n->liveNext)
    {
        TickWorldObject(n->object, deltaSeconds);
    }
}

void SpawnSystem::TickWorldObject(WorldObject &obj, float dt)
{
    (void)obj;
    (void)dt;

    /* Hook point:
     * - Later: dispatch to script, components, animation, etc.
     * - For now: intentionally empty.
     */
}

std::size_t SpawnSystem::AliveCount() const noexcept
{
    return _aliveCount;
}

std::size_t SpawnSystem::CapacitySlots() const noexcept
{
    return _capacitySlots;
}

} /* namespace Assisi::Game */
