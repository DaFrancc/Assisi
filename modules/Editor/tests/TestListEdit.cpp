/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestListEdit.cpp
/// @brief What the Inspector's list rows do to a list: a row added, removed or
///        moved, nothing done for a click with nothing to do, each change one
///        undo step, and the edited list saved and loaded back whole.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Editor/ListEdit.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace Assisi;
using Core::Reflect::ContainerOps;
using Core::Reflect::ContainerSpecFor;
using Core::Reflect::FieldMeta;
using Core::Reflect::FieldType;
using Editor::ApplyListEdit;
using Editor::ElementFieldMeta;
using Editor::ListEdit;
using Editor::ListEditKind;

namespace
{

using Ints = std::vector<int32_t>;

bool Apply(Ints &values, ListEditKind kind, std::size_t row)
{
    return ApplyListEdit(*ContainerSpecFor<Ints>()->ops, reinterpret_cast<std::byte *>(&values),
                         ListEdit{.row = row, .kind = kind});
}

const Core::Reflect::ComponentMeta &PoemMeta()
{
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find("Poem");
    REQUIRE(meta != nullptr);
    return *meta;
}

const FieldMeta &VersesField()
{
    for (const FieldMeta &field : PoemMeta().fields)
    {
        if (field.name == "verses")
        {
            return field;
        }
    }
    FAIL("Poem has no verses field");
    return PoemMeta().fields.front();
}

ECS::Verse MakeVerse(ECS::Poem &poem, std::string_view text)
{
    ECS::Verse verse;
    verse.first.text = poem.pool.Add(text);
    return verse;
}

/// The first line of each verse of the poem on @p entity, in order.
std::vector<std::string_view> Firsts(ECS::Scene &scene, ECS::Entity entity)
{
    const ECS::Poem *poem = scene.Get<ECS::Poem>(entity);
    REQUIRE(poem != nullptr);
    std::vector<std::string_view> firsts;
    for (const ECS::Verse &verse : poem->verses)
    {
        firsts.push_back(poem->pool.View(verse.first.text));
    }
    return firsts;
}

/// One click on a row of the poem's verses, as the Inspector makes it: the
/// gesture opened before the write, the edit applied, the frame swept.
bool Click(ECS::Scene &scene, Editor::EditHistory &history, ECS::Entity entity, ListEdit edit)
{
    history.RecordBefore(entity, PoemMeta().id, "Edit Poem", entity);
    std::byte *poem = reinterpret_cast<std::byte *>(scene.GetMut<ECS::Poem>(entity));
    const bool changed = ApplyListEdit(*VersesField().container->ops, poem + VersesField().offset, edit);
    history.EndFrameSweep(false);
    return changed;
}

} // namespace

TEST_CASE("ListEdit: rows are added, removed and moved, and a click with nothing to do changes nothing")
{
    Ints values{1, 2, 3};
    CHECK(Apply(values, ListEditKind::Add, 0));
    CHECK(values == Ints{1, 2, 3, 0});
    CHECK(Apply(values, ListEditKind::Remove, 1));
    CHECK(values == Ints{1, 3, 0});
    CHECK(Apply(values, ListEditKind::MoveUp, 2));
    CHECK(values == Ints{1, 0, 3});
    CHECK(Apply(values, ListEditKind::MoveDown, 0));
    CHECK(values == Ints{0, 1, 3});

    CHECK_FALSE(Apply(values, ListEditKind::MoveUp, 0));
    CHECK_FALSE(Apply(values, ListEditKind::MoveDown, 2));
    CHECK_FALSE(Apply(values, ListEditKind::Remove, 3));
    CHECK_FALSE(Apply(values, ListEditKind::MoveUp, 3));
    CHECK_FALSE(Apply(values, ListEditKind::None, 0));
    CHECK(values == Ints{0, 1, 3});

    // An array keeps its length: nothing can be added, removed or moved.
    std::array<int32_t, 2> fixed{4, 5};
    const ContainerOps &arrayOps = *ContainerSpecFor<std::array<int32_t, 2>>()->ops;
    std::byte *bytes = reinterpret_cast<std::byte *>(&fixed);
    CHECK_FALSE(ApplyListEdit(arrayOps, bytes, ListEdit{.row = 0, .kind = ListEditKind::Add}));
    CHECK_FALSE(ApplyListEdit(arrayOps, bytes, ListEdit{.row = 0, .kind = ListEditKind::Remove}));
    CHECK_FALSE(ApplyListEdit(arrayOps, bytes, ListEdit{.row = 0, .kind = ListEditKind::MoveDown}));
    CHECK(fixed == std::array<int32_t, 2>{4, 5});
}

TEST_CASE("ListEdit: a row of a list of lists is a list, and its rows are the innermost type")
{
    FieldMeta lists;
    lists.name = "grid";
    lists.type = FieldType::Vector;
    lists.container = ContainerSpecFor<std::vector<std::vector<int32_t>>>();
    lists.enumSize = 1;

    const FieldMeta row = ElementFieldMeta(lists, 2);
    CHECK(row.name == "[2]");
    CHECK(row.type == FieldType::Vector);
    CHECK(row.container == lists.container->element);
    CHECK(row.enumSize == 1);

    const FieldMeta cell = ElementFieldMeta(row, 0);
    CHECK(cell.type == FieldType::Int32);
    CHECK(cell.container == nullptr);
    CHECK(cell.enumSize == 1);

    // A struct row keeps the struct's fields.
    const FieldMeta verse = ElementFieldMeta(VersesField(), 0);
    CHECK(verse.type == FieldType::Struct);
    CHECK(verse.structSpec == VersesField().structSpec);
    CHECK(verse.structSpec != nullptr);
}

TEST_CASE("ListEdit: a path through lists and structs finds its field again, and refuses a row that's gone")
{
    ECS::Poem poem;
    poem.verses = {MakeVerse(poem, "one"), MakeVerse(poem, "two")};
    poem.verses[1].rest.resize(2);

    const FieldMeta *rest = nullptr;
    for (const FieldMeta &field : VersesField().structSpec->fields)
    {
        if (field.name == "rest")
        {
            rest = &field;
        }
    }
    REQUIRE(rest != nullptr);
    const std::array<Editor::FieldStep, 2> path{
        Editor::FieldStep{.offset = VersesField().offset, .list = VersesField().container->ops, .row = 1},
        Editor::FieldStep{.offset = rest->offset, .list = rest->container->ops, .row = 1}};
    std::byte *bytes = reinterpret_cast<std::byte *>(&poem);
    CHECK(Editor::ResolveFieldPath(bytes, path) == reinterpret_cast<std::byte *>(&poem.verses[1].rest[1]));

    // The list grew and moved: the path still finds the row, wherever it went.
    poem.verses.resize(64);
    CHECK(Editor::ResolveFieldPath(bytes, path) == reinterpret_cast<std::byte *>(&poem.verses[1].rest[1]));

    poem.verses[1].rest.resize(1);
    CHECK(Editor::ResolveFieldPath(bytes, path) == nullptr);
    poem.verses.resize(1);
    CHECK(Editor::ResolveFieldPath(bytes, path) == nullptr);
}

TEST_CASE("ListEdit: each add, remove and move undoes in one step, and the list saves and loads")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    ECS::Poem poem;
    poem.verses = {MakeVerse(poem, "one"), MakeVerse(poem, "two")};
    REQUIRE(scene.Add(entity, poem) != nullptr);
    Editor::EditHistory history(scene);

    REQUIRE(Click(scene, history, entity, ListEdit{.row = 0, .kind = ListEditKind::MoveDown}));
    CHECK(Firsts(scene, entity) == std::vector<std::string_view>{"two", "one"});
    REQUIRE(Click(scene, history, entity, ListEdit{.row = 2, .kind = ListEditKind::Add}));
    CHECK(Firsts(scene, entity).size() == 3);
    REQUIRE(Click(scene, history, entity, ListEdit{.row = 0, .kind = ListEditKind::Remove}));
    CHECK(Firsts(scene, entity) == std::vector<std::string_view>{"one", ""});

    // Nothing to do is no step: the next undo still takes back the remove.
    CHECK_FALSE(Click(scene, history, entity, ListEdit{.row = 0, .kind = ListEditKind::MoveUp}));
    history.Undo();
    CHECK(Firsts(scene, entity) == std::vector<std::string_view>{"two", "one", ""});
    history.Undo();
    CHECK(Firsts(scene, entity) == std::vector<std::string_view>{"two", "one"});
    history.Undo();
    CHECK(Firsts(scene, entity) == std::vector<std::string_view>{"one", "two"});
    CHECK_FALSE(history.CanUndo());

    history.Redo();
    history.Redo();
    const nlohmann::json level = Runtime::SceneSerializer::Save(scene);
    ECS::Scene loaded;
    REQUIRE(Runtime::SceneSerializer::Load(loaded, level).has_value());
    std::vector<ECS::Entity> entities;
    loaded.ForEachEntity([&entities](ECS::Entity found) { entities.push_back(found); });
    REQUIRE(entities.size() == 1);
    CHECK(Firsts(loaded, entities[0]) == std::vector<std::string_view>{"two", "one", ""});
}
