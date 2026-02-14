/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

#include <cstddef>
#include <cstdint>

#include <Assisi/Game/WorldObject.hpp>

namespace Assisi::Game
{

/**
 * @brief Spawns, destroys, and ticks WorldObjects using a page-based pool allocator.
 *
 * - WorldObjects are stored in fixed-size slots inside byte pages.
 * - When full, the pool allocates a new page with double the previous slot count.
 * - Destroy() returns slots to an intrusive free list (no heap churn per object).
 * - Tick() iterates only live objects via an intrusive list.
 */
class SpawnSystem
{
  public:
    using Handle = std::uintptr_t; /* Opaque handle (internally a Node*). */

    /**
     * @brief Creates a SpawnSystem with an initial slot capacity.
     *
     * @param initialSlots Number of objects to fit in the first page.
     */
    explicit SpawnSystem(std::size_t initialSlots = 1024);

    /** @brief Destroys all pages and remaining live objects. */
    ~SpawnSystem() noexcept;

    /** @brief Creates a new WorldObject and returns its handle. */
    Handle Create();

    /** @brief Destroys a WorldObject previously created by Create(). */
    void Destroy(Handle handle);

    /** @brief Access a live WorldObject by handle. */
    WorldObject &Get(Handle handle);

    /** @brief Access a live WorldObject by handle. */
    const WorldObject &Get(Handle handle) const;

    /**
     * @brief Calls Tick() on each live WorldObject.
     *
     * Note: WorldObject currently has no Tick() method in your snippet,
     * so this calls a private hook you can later route to scripting/components.
     */
    void Tick(float deltaSeconds);

    /** @brief Returns the number of live WorldObjects. */
    std::size_t AliveCount() const noexcept;

    /** @brief Returns total slot capacity across all allocated pages. */
    std::size_t CapacitySlots() const noexcept;

  private:
    struct Node
    {
        Node *livePrev = nullptr;
        Node *liveNext = nullptr;
        Node *freeNext = nullptr;

        bool alive = false;
        WorldObject object;
    };

    struct Page
    {
        std::byte *bytes = nullptr;
        std::size_t slotCount = 0;
        Page *next = nullptr;
    };

    void Grow();                          /* Doubles slots by allocating a new page. */
    Node *AllocNode();                    /* Pops from free list (growing if needed). */
    void FreeNode(Node *node) noexcept;   /* Pushes into free list (intrusive). */
    void LinkLive(Node *node) noexcept;   /* Adds to live list. */
    void UnlinkLive(Node *node) noexcept; /* Removes from live list. */

    void TickWorldObject(WorldObject &obj, float dt); /* Hook for per-object logic. */

  private:
    Page *_pages = nullptr;
    Node *_freeList = nullptr;

    Node *_liveHead = nullptr;
    Node *_liveTail = nullptr;

    std::size_t _aliveCount = 0;
    std::size_t _capacitySlots = 0;

    std::size_t _nextPageSlots = 0;
};

} /* namespace Assisi::Scene */
