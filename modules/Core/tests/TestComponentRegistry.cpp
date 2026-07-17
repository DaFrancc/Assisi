/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <doctest/doctest.h>

#include <typeindex>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

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
} // namespace

TEST_CASE("ComponentRegistry assigns dense alphabetical ids")
{
    auto &registry = ComponentRegistry::Instance();

    // doctest re-runs this body once per SUBCASE, but the registry is a process
    // singleton — register the fixtures exactly once (out of alphabetical order
    // on purpose) via a function-local static, or duplicate metas accumulate.
    static const bool registered = [&registry]()
    {
        registry.Register(Meta("RegZeta", typeid(RegZeta)));
        registry.Register(Meta("RegAlpha", typeid(RegAlpha)));
        registry.Register(Meta("RegMu", typeid(RegMu)));
        registry.Register(Meta("RegHidden", typeid(RegHidden), /*serializable=*/false));
        return true;
    }();
    (void)registered;

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

// Round-6 review M4 (a): the registry is documented as immutable after startup, but a
// Register that arrives AFTER a query clears _finalized and re-sorts + RENUMBERS on
// the next query. An id handed out once must stay stable — ComponentIdOf<T> caches
// its id in a function-local static computed exactly once, so any renumbering
// silently mis-maps every cached id and stored ComponentId. Registering an
// alphabetically-earlier name after an id has been observed shifts that id.
//
// DEFERRED (should_fail): confirmed-real bug kept as a live reproduction. The fix is
// an open design decision — assert !_finalized in Register (abort late registration),
// an explicit Freeze(), or decouple id assignment from sort order so ids never move.
// Remove the should_fail decorator when that decision is made. See
// docs/code-review-2026-07-round6.md.
TEST_CASE("ComponentRegistry: a late Register must not renumber an already-issued id" *
          doctest::should_fail())
{
    auto &registry = ComponentRegistry::Instance();

    registry.Register(Meta("ZzzM4_Late", typeid(ZzzM4Late)));
    const ComponentId id1 = registry.IdOf(std::type_index(typeid(ZzzM4Late))); // finalizes now
    REQUIRE(id1 != kInvalidComponentId);
    REQUIRE(registry.ById(id1)->name == "ZzzM4_Late"); // id1 maps to Zzz right now

    // A later Register of an alphabetically-earlier name re-sorts and renumbers.
    registry.Register(Meta("AaaM4_Early", typeid(AaaM4Early)));
    const ComponentId id2 = registry.IdOf(std::type_index(typeid(ZzzM4Late)));

    CHECK(id1 == id2); // stability contract: "AaaM4_Early" sorting first shifts Zzz's id up
}

// Round-6 review M4: Register performs no name-uniqueness check, so two metas with
// the same name (different types) both survive into All() and every save/Find path.
// A correct registry rejects or dedups the duplicate.
TEST_CASE("ComponentRegistry: duplicate component names are rejected, not both kept")
{
    auto &registry = ComponentRegistry::Instance();

    registry.Register(Meta("M4_DupName", typeid(M4DupA)));
    registry.Register(Meta("M4_DupName", typeid(M4DupB)));

    size_t count = 0;
    for (const auto &meta : registry.All()) // finalizes
        if (meta.name == "M4_DupName")
            ++count;

    CHECK(count == 1); // no uniqueness check → both duplicates persist
}
