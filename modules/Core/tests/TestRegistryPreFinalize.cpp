/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestRegistryPreFinalize.cpp
/// @brief The reflection registries in the window before their first finalize.
///
/// Its own executable, for the same reason TestPreInit.cpp is: the subject is
/// "nothing has queried this registry yet", and a registry is a process
/// singleton whose finalize is one-way. A single query from any other case in
/// the binary — in any order doctest happens to pick — makes every assertion
/// here vacuous. Nothing linked into this binary may query either registry
/// outside the cases below, and the cases themselves are ordered: the first
/// query in the file is the finalize the file is about.
///
/// Registration happens from a static initializer, before main, which is both
/// what generated code does and the only window Register accepts.

#include <doctest/doctest.h>

#include <cstddef>
#include <string_view>
#include <typeindex>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/Reflect/MessageRegistry.hpp>

using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::ComponentRegistry;
using Assisi::Core::Reflect::kInvalidComponentId;
using Assisi::Core::Reflect::kInvalidMessageId;
using Assisi::Core::Reflect::MessageMeta;
using Assisi::Core::Reflect::MessageRegistry;

namespace
{

struct PreFinalizeDupA
{
};
struct PreFinalizeDupB
{
};
struct PreFinalizeOnly
{
};

struct PreFinalizeMsgDupA
{
};
struct PreFinalizeMsgDupB
{
};

ComponentMeta Component(const char *name, std::type_index type)
{
    // Every member listed so -Wmissing-field-initializers stays quiet; the
    // type-erased hooks are unused here. Keep it exhaustive as ComponentMeta
    // gains members.
    return ComponentMeta{.name            = name,
                         .typeIndex       = type,
                         .fields          = {},
                         .serialize       = {},
                         .addToScene      = {},
                         .iterateEntities = {},
                         .getByEntity     = {},
                         .construct       = {},
                         .getMutable      = {},
                         .serializable    = true,
                         .id              = kInvalidComponentId};
}

MessageMeta Message(const char *name, std::type_index type)
{
    // Exhaustive for the same reason Component() is: -Wmissing-field-initializers
    // is on, and the codec hooks are unused by these cases.
    return MessageMeta{.name        = name,
                       .typeIndex   = type,
                       .fields      = {},
                       .direction   = Assisi::Core::Reflect::MessageDirection::Intent,
                       .reliability = Assisi::Core::Reflect::MessageReliability::Unreliable,
                       .independent = true,
                       .id          = kInvalidMessageId,
                       .serialize   = {},
                       .deserialize = {}};
}

const bool s_registered = []
                          {
                              auto &components = ComponentRegistry::Instance();
                              components.Register(Component("PreFinalizeOnly", typeid(PreFinalizeOnly)));
                              components.Register(Component("PreFinalizeDup", typeid(PreFinalizeDupA)));
                              components.Register(Component("PreFinalizeDup", typeid(PreFinalizeDupB)));

                              // Deliberately duplicated too. Nothing in this binary queries the
                              // message registry outside the NDEBUG-only case at the bottom, so
                              // in a debug build this pair is registered and never finalized —
                              // which is what keeps the duplicate-name assert from aborting the
                              // whole executable before doctest reports anything.
                              auto &messages = MessageRegistry::Instance();
                              messages.Register(Message("PreFinalizeMsgDup", typeid(PreFinalizeMsgDupA)));
                              messages.Register(Message("PreFinalizeMsgDup", typeid(PreFinalizeMsgDupB)));
                              return true;
                          }();

} // namespace

// This case must stay first in the file: it is the one that performs the
// registry's first query, and every claim in it is about that query.
TEST_CASE("ComponentRegistry: Count agrees with the table before the first finalize" *
          doctest::should_fail())
{
    // ENG-117, open. ComponentRegistry.cpp:189 is the only accessor that does not
    // call EnsureFinalized() first — All(), Find(), ById(), IdOf(),
    // SerializableComponents() and ReplicableComponents() all do. So before the
    // first finalize, Count() reports the raw registration total, duplicates
    // included, while every other accessor reports the deduplicated table that
    // finalize produces. MessageRegistry::Count() does finalize, so the two
    // registries disagree about what "count" means.
    //
    // Ordering matters and is the reason for this binary: Count() has to run
    // before anything else finalizes, or both sides report the same number and
    // the case proves nothing.
    //
    // should_fail until Count() finalizes like its siblings; the fix removes
    // this decorator.
    auto &registry = ComponentRegistry::Instance();

    const std::size_t reportedCount = registry.Count(); // does not finalize
    const std::size_t actualEntries = registry.All().size(); // finalizes, drops the duplicate

    CAPTURE(reportedCount);
    CAPTURE(actualEntries);
    CHECK(reportedCount == actualEntries);
}

TEST_CASE("ComponentRegistry: after finalize the duplicate is gone and Count agrees")
{
    // The control for the case above: once finalize has run, the two numbers
    // match — which is exactly why the disagreement is only reachable in the
    // window this binary exists to hold open.
    auto &registry = ComponentRegistry::Instance();

    CHECK(registry.Count() == registry.All().size());

    std::size_t duplicates = 0;
    for (const ComponentMeta &meta : registry.All())
        if (meta.name == "PreFinalizeDup")
            ++duplicates;
    CHECK(duplicates == 1);

    // ...and the fixtures really did reach this registry, so the counts above
    // are counting something.
    CHECK(registry.Find("PreFinalizeOnly") != nullptr);
}

#ifdef NDEBUG

TEST_CASE("MessageRegistry: a duplicate message name leaves one entry, not two" *
          doctest::should_fail())
{
    // ENG-117, open. MessageRegistry::EnsureFinalized (MessageRegistry.cpp:52)
    // diagnoses duplicate names and then keeps them: it logs, it asserts, and it
    // never erases. ComponentRegistry handles the same situation with
    // std::unique + erase. The assert is debug-only, so a release build retains
    // both entries, both get dense wire ids, and Find/IdOf(name) resolve to
    // whichever the (unstable) sort happened to place first — which need not
    // agree across builds. The protocol hash cannot catch it: both sides agree
    // on the identical name list.
    //
    // NDEBUG-only, and that is the finding rather than a gap in it. In a debug
    // build the assert aborts inside finalize, so the retention is unobservable
    // in-process; the release build is where it is a silent misdispatch. Run
    // this binary under `make gs` (gcc-ship) to exercise it.
    //
    // should_fail until finalize drops duplicates; the fix removes this decorator.
    auto &registry = MessageRegistry::Instance();

    std::size_t duplicates = 0;
    for (const MessageMeta &meta : registry.All())
        if (meta.name == "PreFinalizeMsgDup")
            ++duplicates;

    CAPTURE(duplicates);
    CHECK(duplicates == 1);
}

#endif // NDEBUG
