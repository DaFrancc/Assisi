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
// linked into the test binary's registry. The registry is a process singleton,
// so everything shares one TEST_CASE to register exactly once.
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

// Round-6 review M4 fixtures. Distinctive names/types so the assertions hold
// regardless of what else is linked into this binary's shared registry.
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
    // type-erased hooks are unused by these tests.
    return ComponentMeta{
        .name = name, .typeIndex = type, .fields = {}, .serialize = {}, .addToScene = {},
        .iterateEntities = {}, .getByEntity = {}, .serializable = serializable, .id = kInvalidComponentId};
}

// Registered from a static initializer, before main and therefore before any
// test can query (and finalize) the registry — exactly how generated component
// registrations behave. Registering lazily inside a TEST_CASE only worked while
// a late Register silently renumbered; it is now refused (round-6 M4a), and the
// order these arrive in is deliberately not alphabetical so the id-assignment
// tests still prove the sort.
const bool s_fixturesRegistered = []
{
    auto &registry = ComponentRegistry::Instance();
    registry.Register(Meta("RegZeta", typeid(RegZeta)));
    registry.Register(Meta("RegAlpha", typeid(RegAlpha)));
    registry.Register(Meta("RegMu", typeid(RegMu)));
    registry.Register(Meta("RegHidden", typeid(RegHidden), /*serializable=*/false));
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
        for (ComponentId i = 0; i < all.size(); ++i)
        {
            CHECK(all[i].id == i);
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

// Round-6 review M4 (a), FIXED: the registry is immutable once an id has been
// issued. Ids are positions in the name-sorted list (the property that makes them
// reproducible across builds), so honouring a late Register would renumber ids
// that ComponentIdOf<T> has already memoised and that saved scenes already store
// — and would reallocate _metas, dangling every pointer ById()/All() handed out.
// Register therefore refuses: it asserts in debug and drops the component with an
// error in release. Both are better than silent renumbering.
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

// Round-6 review M4: Register performs no name-uniqueness check, so two metas with
// the same name (different types) both survive into All() and every save/Find path.
// A correct registry rejects or dedups the duplicate.
TEST_CASE("ComponentRegistry: duplicate component names are rejected, not both kept")
{
    auto &registry = ComponentRegistry::Instance();


    size_t count = 0;
    for (const auto &meta : registry.All()) // finalizes
        if (meta.name == "M4_DupName")
            ++count;

    CHECK(count == 1); // no uniqueness check → both duplicates persist
}
