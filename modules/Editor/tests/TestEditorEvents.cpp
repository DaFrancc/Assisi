/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventRoute.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Editor/EditorEvents.hpp>

using namespace Assisi;

TEST_CASE("A selection the viewport pushes reaches the system that reads it, whichever world that system runs in")
{
    // The viewport pushes into the program's queue, outside any world; the
    // system that answers runs in the edited world and reads through its route.
    // Unless the event is declared universal, that route looks in the world's
    // queue and the click selects nothing.
    Editor::DeclareEditorEvents();
    Core::EventQueue world;
    Core::EventQueue universal{Core::EventScope::Universal};
    const ECS::Entity picked{7, 0};

    universal.Push(Editor::EntitySelectionChangedEvent{.entity = picked, .additive = true});

    const Core::EventRoute events{world, universal};
    REQUIRE(events.Read<Editor::EntitySelectionChangedEvent>().size() == 1);
    CHECK(events.Read<Editor::EntitySelectionChangedEvent>()[0].entity == picked);
}
