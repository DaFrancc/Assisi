/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <doctest/doctest.h>

#include <string_view>
#include <typeindex>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

using Assisi::Core::Reflect::ComponentId;
using Assisi::Core::Reflect::ComponentIdOf;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::ComponentRegistry;
using Assisi::Core::Reflect::kInvalidComponentId;

namespace
{
// Distinctive names/types so the assertions hold regardless of whatever else is
// linked into the test binary's registry, which is a process singleton shared
// with every other suite in this executable.
struct RegAlpha
{
};
struct RegMu
{
};
struct RegZeta
{
};
struct RegHidden
{
};

// Fixtures for the late-registration and duplicate-name cases below.
struct ZzzM4Late
{
};
struct AaaM4Early
{
};
struct M4DupA
{
};
struct M4DupB
{
};

ComponentMeta Meta(const char *name, std::type_index type, bool serializable = true)
{
    // Every member listed so -Wmissing-field-initializers stays quiet; the
    // type-erased hooks are unused by these tests. Keep it exhaustive as
    // ComponentMeta gains members.
    return ComponentMeta{.name            = name,
                         .typeIndex       = type,
                         .fields          = {},
                         .serialize       = {},
                         .addToScene      = {},
                         .iterateEntities = {},
                         .getByEntity     = {},
                         .construct       = {},
                         .getMutable      = {},
                         .serializable    = serializable,
                         .id              = kInvalidComponentId};
}

// Registered from a static initializer, before main and therefore before any
// test can query (and finalize) the registry — exactly how generated component
// registrations behave, and the only window a Register is accepted in. The
// arrival order is deliberately not alphabetical so the id-assignment tests
// still prove the sort.
const bool s_fixturesRegistered = []
                                  {
                                      auto &registry = ComponentRegistry::Instance();
                                      registry.Register(Meta("RegZeta", typeid(RegZeta)));
                                      registry.Register(Meta("RegAlpha", typeid(RegAlpha)));
                                      registry.Register(Meta("RegMu", typeid(RegMu)));
                                      registry.Register(Meta("RegHidden", typeid(RegHidden), /*serializable=*/ false));
                                      registry.Register(Meta("ZzzM4_Late", typeid(ZzzM4Late)));
                                      registry.Register(Meta("M4_DupName", typeid(M4DupA)));
                                      registry.Register(Meta("M4_DupName", typeid(M4DupB))); // duplicate on purpose
                                      return true;
                                  }();
} // namespace

TEST_CASE("ComponentRegistry assigns dense alphabetical ids")
{
    auto &registry = ComponentRegistry::Instance();

    // Fixtures are registered at static-init time (see s_fixturesRegistered);
    // this only queries, which is all a test may do once the registry is live.

    const ComponentId alpha = registry.IdOf(std::type_index(typeid(RegAlpha)));
    const ComponentId mu    = registry.IdOf(std::type_index(typeid(RegMu)));
    const ComponentId zeta  = registry.IdOf(std::type_index(typeid(RegZeta)));

    SUBCASE("ids order alphabetically by name")
    {
        CHECK(alpha != kInvalidComponentId);
        CHECK(alpha < mu);
        CHECK(mu < zeta);
    }

    SUBCASE("ById round-trips to the same component")
    {
        CHECK(registry.ById(alpha)->name == "RegAlpha");
        CHECK(registry.ById(mu)->name == "RegMu");
        CHECK(registry.ById(zeta)->name == "RegZeta");
    }

    SUBCASE("Find and name-based IdOf agree with type-based IdOf")
    {
        CHECK(registry.Find("RegMu")->id == mu);
        CHECK(registry.IdOf("RegMu") == mu);
    }

    SUBCASE("the id field on the meta matches its id")
    {
        CHECK(registry.ById(zeta)->id == zeta);
    }

    SUBCASE("ComponentIdOf<T> matches the registry lookup")
    {
        CHECK(ComponentIdOf<RegAlpha>() == alpha);
        CHECK(ComponentIdOf<RegZeta>() == zeta);
    }

    SUBCASE("unknown lookups return the sentinel")
    {
        struct Unregistered
        {
        };
        CHECK(registry.IdOf(std::type_index(typeid(Unregistered))) == kInvalidComponentId);
        CHECK(registry.IdOf("NoSuchComponent") == kInvalidComponentId);
        CHECK(registry.ById(kInvalidComponentId) == nullptr);
        CHECK(registry.Find("NoSuchComponent") == nullptr);
    }

    SUBCASE("All() is sorted and its ids are dense and ascending")
    {
        const auto all = registry.All();
        // <= not < : other test files may register their own fixtures into this
        // shared registry, and duplicate names would still be a valid sort.
        //
        // Raw counter, not a ComponentId — this walks the table by array index
        // (into `all`) and does arithmetic (`i - 1`), neither of which a
        // ComponentId supports on purpose. See ComponentRegistry::EnsureFinalized
        // for the one place a counter like this becomes an id.
        for (std::uint32_t i = 0; i < all.size(); ++i)
        {
            CHECK(all[i].id == ComponentId{i});
            if (i > 0)
                CHECK(all[i - 1].name <= all[i].name);
        }
    }

    SUBCASE("SerializableComponents() excludes non-serializable registrations")
    {
        // RegHidden was registered with serializable = false: it still gets an id
        // and appears in All(), but SerializableComponents() must omit it.
        CHECK(registry.IdOf("RegHidden") != kInvalidComponentId);
        CHECK(registry.Find("RegHidden") != nullptr);

        bool hiddenInAll = false;
        for (const auto &meta : registry.All())
            if (meta.name == "RegHidden")
                hiddenInAll = true;
        CHECK(hiddenInAll);

        bool hiddenInSerializable = false;
        bool sawAlpha             = false;
        for (const auto *meta : registry.SerializableComponents())
        {
            CHECK(meta->serializable); // everything yielded here is serializable
            if (meta->name == "RegHidden")
                hiddenInSerializable = true;
            if (meta->name == "RegAlpha")
                sawAlpha = true;
        }
        CHECK_FALSE(hiddenInSerializable);
        CHECK(sawAlpha); // a normal component still comes through
    }
}

// The registry is immutable once an id has been issued. Ids are positions in the
// name-sorted list, so honouring a late Register would renumber ids that
// ComponentIdOf<T> has memoised and that saved scenes store — and would
// reallocate _metas, dangling every pointer ById()/All() handed out. Register
// refuses instead: assert in debug, log and drop in release.
TEST_CASE("ComponentRegistry: a late Register is refused and leaves issued ids stable")
{
    auto &registry = ComponentRegistry::Instance();

    const ComponentId id1 = registry.IdOf(std::type_index(typeid(ZzzM4Late))); // finalizes if not already
    REQUIRE(id1 != kInvalidComponentId);
    REQUIRE(registry.ById(id1)->name == "ZzzM4_Late");

#ifndef NDEBUG
    {
        // The contract has teeth: registering now is a programming error, not a
        // silently-tolerated one.
        Assisi::Testing::ThrowOnContractViolation guard;
        CHECK_THROWS_AS(registry.Register(Meta("AaaM4_Early", typeid(AaaM4Early))),
                        Assisi::Core::ContractViolation);
    }
#else
    registry.Register(Meta("AaaM4_Early", typeid(AaaM4Early))); // logged and dropped
#endif

    // The whole point: an alphabetically-earlier late arrival must not shift it.
    const ComponentId id2 = registry.IdOf(std::type_index(typeid(ZzzM4Late)));
    CHECK(id1 == id2);
    CHECK(registry.ById(id1)->name == "ZzzM4_Late");
    // ...and the refused component is genuinely absent rather than half-registered.
    CHECK(registry.IdOf(std::string_view{"AaaM4_Early"}) == kInvalidComponentId);
}

// Two metas registered under one name would collide in every save/Find path, so
// finalize keeps the first and drops the rest.
TEST_CASE("ComponentRegistry: duplicate component names are rejected, not both kept")
{
    auto &registry = ComponentRegistry::Instance();


    size_t count = 0;
    for (const auto &meta : registry.All()) // finalizes
        if (meta.name == "M4_DupName")
            ++count;

    CHECK(count == 1); // the duplicate was dropped, not kept alongside
}

// ---------------------------------------------------------------------------
// EnsureFinalized's thread safety — verified, but not compilable as it stands
// ---------------------------------------------------------------------------
//
// Finalization is lazy: the first ask sorts _metas, drops duplicates, assigns
// every id and fills _serializable/_replicable/_replicableOrdinal. That ask can
// come from any thread — async travel deserializes on a worker and walks this
// registry there while the main thread does too — so it is guarded by an atomic
// flag with acquire/release plus a double-checked lock.
//
// **This case cannot be compiled here.** It needs a registry that has *never*
// been queried, and the only one this file can reach is Instance(), which
// earlier cases in this binary have already finalized — every thread would take
// the fast path and the case would pass whether or not the lock exists. A fresh
// registry is what makes it real, and ComponentRegistry() is private on purpose
// (one registry per process).
//
// It was run, by making that constructor public temporarily:
//   * with the lock — clean under `make gcc-tsan`;
//   * with the `lock_guard` in EnsureFinalized removed — 393 data races,
//     naming ComponentRegistry.cpp's sort comparator and ComponentMeta's move
//     constructor/assignment, i.e. two threads sorting the same vector.
//
// To re-run it: make ComponentRegistry() public, uncomment below, add <atomic>,
// <cstdint>, <string>, <thread> and <vector>, then `make test-gcc-tsan`. To keep
// it permanently instead, it needs its own test binary — nothing else linked
// into it may query the registry — following the Assisi-Chiara-PreInit-Tests
// pattern in modules/Chiara/tests/CMakeLists.txt, which exists for the same
// reason ("Initialize has not been called yet" cannot hold in a shared process).
//
// TEST_CASE("ComponentRegistry: racing the first finalize is safe")
// {
//     ComponentRegistry registry;
//
//     // Enough entries that the sort is not a single compare — the wider the
//     // finalize window, the more reliably two threads are inside it at once.
//     // Deliberately not in name order, so the sort actually moves things.
//     std::vector<std::string> names;
//     names.reserve(64);
//     for (std::int32_t i = 63; i >= 0; --i)
//         names.push_back("Race" + std::string(i < 10 ? "0" : "") + std::to_string(i));
//     for (const std::string &name : names)
//         registry.Register(Meta(name.c_str(), typeid(RegAlpha))); // typeIndex is not under test
//
//     constexpr std::int32_t kThreads = 8;
//
//     // Released together, so they arrive at EnsureFinalized simultaneously
//     // rather than one winning outright and the rest taking the fast path.
//     std::atomic<bool>         go{false};
//     std::atomic<std::int32_t> ready{0};
//     std::atomic<std::int32_t> disagreements{0};
//     std::vector<std::thread>  workers;
//     workers.reserve(kThreads);
//
//     for (std::int32_t index = 0; index < kThreads; ++index)
//     {
//         workers.emplace_back(
//             [index, &registry, &go, &ready, &disagreements]
//             {
//                 ready.fetch_add(1, std::memory_order_release);
//                 while (!go.load(std::memory_order_acquire))
//                 {
//                 }
//
//                 // A different finalizing entry point per thread, so whichever
//                 // one wins, the others are inside a different accessor.
//                 switch (index % 3)
//                 {
//                 case 0:
//                     if (registry.All().size() != 64)
//                         disagreements.fetch_add(1, std::memory_order_relaxed);
//                     break;
//                 case 1:
//                     if (registry.SerializableComponents().size() != 64)
//                         disagreements.fetch_add(1, std::memory_order_relaxed);
//                     break;
//                 default:
//                     if (registry.IdOf("Race00") == kInvalidComponentId)
//                         disagreements.fetch_add(1, std::memory_order_relaxed);
//                     break;
//                 }
//
//                 // Ids are handed out during finalization, so a half-numbered
//                 // or re-sorted table shows up here.
//                 for (const auto &meta : registry.All())
//                 {
//                     if (registry.ById(meta.id) != &meta)
//                         disagreements.fetch_add(1, std::memory_order_relaxed);
//                 }
//             });
//     }
//
//     while (ready.load(std::memory_order_acquire) < kThreads)
//     {
//     }
//     go.store(true, std::memory_order_release);
//
//     for (std::thread &worker : workers)
//         worker.join();
//
//     CHECK(disagreements.load(std::memory_order_relaxed) == 0);
//     CHECK(registry.All().size() == 64);
// }
