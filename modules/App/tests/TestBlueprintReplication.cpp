/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintReplication.cpp
/// @brief A real blueprint, spawned on a host, arriving on a client as a
/// blueprint rather than as a pile of entities.
///
/// The NetSync suite drives the same machinery with fakes, which pins the wire
/// format and the id discipline. What it cannot check is the claim the whole
/// design rests on: that the client's *own* expansion of the same file produces
/// the same members, in the same order, at the same places — without the server
/// sending any of it (docs/blueprint-system-concept.md §9).
///
/// Both sides share an asset root here, which is what a matching content-set
/// hash means in production.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

using namespace Assisi;

namespace
{

std::filesystem::path FreshRoot()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi_bpnet";
    std::error_code             ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

void Write(const std::filesystem::path &root, const std::string &name, const nlohmann::json &doc)
{
    std::ofstream out(root / name, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

nlohmann::json At(float x, float y, float z)
{
    return {{"Transform",
             {{"position", {x, y, z}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}}};
}

/// A body and two wheels, every member replicated so the server hands the whole
/// instance a block.
nlohmann::json CarFile()
{
    auto body     = At(0.f, 0.f, 0.f);
    body["Replicated"] = nlohmann::json::object();
    auto left     = At(-1.f, 0.f, 0.f);
    left["Replicated"] = nlohmann::json::object();
    auto right    = At(1.f, 0.f, 0.f);
    right["Replicated"] = nlohmann::json::object();

    return {{"version", 2},
            {"entities", nlohmann::json::array({{{"name", "body"}, {"components", body}},
                                                {{"name", "wheel_l"}, {"components", left}},
                                                {{"name", "wheel_r"}, {"components", right}}})}};
}

struct Fixture
{
    Net::NetTransport                               transport;
    App::World                                      host;
    App::World                                      guest;
    std::pair<Net::ConnectionId, Net::ConnectionId> pair;
    NetSync::ReplicationServer                      server;
    NetSync::ReplicationClient                      client;
    std::uint64_t                                   tick = 0;

    Fixture()
        : pair(transport.CreateLoopbackPair()), server(transport, host.scene),
          client(transport, guest.scene, pair.second)
    {
    }

    void Connect(const std::vector<std::string> &manifest)
    {
        App::InstallInstanceInfoProvider(server, host, manifest);
        App::InstallInstanceExpander(client, guest, manifest);
        server.SetContentSetHash(0);
        client.SetContentSetHash(0);
        server.AddConnection(pair.first);
    }

    void Step(int times)
    {
        for (int i = 0; i < times; ++i)
        {
            std::vector<Net::NetEvent> events;
            transport.Poll(events);
            for (const Net::NetEvent &event : events)
            {
                if (event.type != Net::NetEvent::Type::Message)
                    continue;
                if (event.connection == pair.first)
                    server.HandleMessage(pair.first, event.payload);
                else
                    client.HandleMessage(event.payload);
            }
            server.Tick(++tick);
        }
    }
};

} // namespace

TEST_CASE("Blueprint over the wire: the client expands the same file the host spawned")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());

    const App::ContentSet content = App::BuildContentSet();
    REQUIRE(content.paths.size() == 1);

    Fixture fixture;
    fixture.Connect(content.paths);

    ECS::Transform at;
    at.position = {12.f, 0.f, 3.f};
    const std::optional<ECS::InstanceId> spawned = App::SpawnBlueprint(fixture.host, "car.abp", at);
    REQUIRE(spawned.has_value());

    fixture.Step(12);

    // Three members here, three members there — built locally from the file, not
    // received one by one.
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
    REQUIRE(fixture.client.InstanceRecords().size() == 1);

    // The guest has a real instance in its own table, of the right blueprint.
    REQUIRE(fixture.guest.instances.Size() == 1);
    const auto rows = fixture.guest.instances.All();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].second->source == "car.abp");
    // Not authored: it exists because the server said so, and saving this level
    // must not turn it into content.
    CHECK_FALSE(rows[0].second->authored);

    // Named members resolve on the guest, which is what says the expansion — not
    // just the entity count — matched.
    const ECS::InstanceId guestInstance = rows[0].first;
    for (const char *name : {"body", "wheel_l", "wheel_r"})
    {
        const ECS::Entity member = App::FindMember(fixture.guest, guestInstance, name);
        CAPTURE(name);
        CHECK(member != ECS::NullEntity);
    }

    // ...and it landed where the host put it. The placement crossed once, in the
    // record; every member's pose was composed locally from it.
    const ECS::Entity guestBody = App::FindMember(fixture.guest, guestInstance, "wheel_r");
    const ECS::Transform *pose  = fixture.guest.scene.Get<ECS::Transform>(guestBody);
    REQUIRE(pose != nullptr);
    CHECK(pose->position.x == doctest::Approx(13.f)); // 12 placement + 1 local
    CHECK(pose->position.z == doctest::Approx(3.f));
}

TEST_CASE("Blueprint over the wire: the guest's tag names the guest's instance")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());

    const App::ContentSet content = App::BuildContentSet();

    Fixture fixture;
    fixture.Connect(content.paths);

    // Burn a few instance ids on the host so its numbering cannot coincide with
    // the guest's, which starts at 1.
    for (int i = 0; i < 3; ++i)
        (void)App::SpawnBlueprint(fixture.host, "car.abp", {});
    const std::optional<ECS::InstanceId> spawned = App::SpawnBlueprint(fixture.host, "car.abp", {});
    REQUIRE(spawned.has_value());

    fixture.Step(14);

    const auto rows = fixture.guest.instances.All();
    REQUIRE(!rows.empty());

    // Every mirrored member's tag names an instance in the *guest's* table.
    for (auto [entity, tag] : fixture.guest.scene.Query<ECS::BlueprintMember>())
    {
        (void)entity;
        CAPTURE(tag.instanceId.value);
        CHECK(fixture.guest.instances.Find(tag.instanceId) != nullptr);
    }
}

namespace
{

/// Bytes the server put on the wire for one spawn of `car.abp` at @p at.
///
/// The same world twice — once with the manifest, once without — is what turns
/// "the client derives it" into a number rather than into an assertion about
/// internals. @p moveOne edits a member away from the file's value first, so that
/// member must still travel.
std::uint64_t CarBytes(const std::vector<std::string> &manifest, const ECS::Transform &at, bool moveOne)
{
    Fixture fixture;
    fixture.Connect(manifest);

    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(fixture.host, "car.abp", at);
    REQUIRE(id.has_value());

    if (moveOne)
    {
        const ECS::Entity body = App::FindMember(fixture.host, *id, "body");
        REQUIRE(body != ECS::NullEntity);
        ECS::Transform *pose = fixture.host.scene.GetMut<ECS::Transform>(body);
        REQUIRE(pose != nullptr);
        pose->position.y = 42.f;
        // GetMut already stamps the change tick.
    }

    fixture.Step(14);
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
    const NetSync::ConnectionDiagnostics *stats = fixture.server.Diagnostics(fixture.pair.first);
    REQUIRE(stats != nullptr);
    return stats->bytesSent;
}

/// The placement the two cases below mean by "not the origin".
///
/// A translation, deliberately without a rotation. The claim being defended is
/// that a placement does not stop the client deriving its own members, and a
/// translation states it in full: the composition is real, the comparison operand
/// has to account for it, and a fix is *measurably* enough — with the placement
/// composed onto the comparison operand these two cases go to 153 bytes against
/// 233, the same saving the origin case gets.
///
/// A rotated placement is a strictly harder case and does not belong here: the
/// compose/inverse-compose pair is an exact inverse in arithmetic but not
/// bit-for-bit in float, so whether a *byte* comparison can ever match under one
/// is a question about B9's fix rather than about B9. Naming it here would assert
/// something a correct fix might not be able to deliver.
ECS::Transform MovedPlacement()
{
    ECS::Transform at;
    at.position = {40.f, 0.f, 0.f};
    return at;
}

} // namespace

TEST_CASE("Blueprint over the wire: an untouched member costs no component bytes")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());
    const App::ContentSet content = App::BuildContentSet();

    const ECS::Transform origin;
    const std::uint64_t  derived = CarBytes(content.paths, origin, /*moveOne=*/false);
    const std::uint64_t  sent    = CarBytes({}, origin, /*moveOne=*/false);

    // The members are identical to the file, so with a manifest their components
    // are not on the wire at all — the client already has them.
    CHECK(derived < sent);

    // ...and a member that actually differs is not elided, or the mirror would
    // be quietly wrong.
    const std::uint64_t withEdit = CarBytes(content.paths, origin, /*moveOne=*/true);
    CHECK(withEdit > derived);

    // Spawned at the origin, which is the one placement where comparing a live
    // component against the *authored local* value is sound — composing the
    // origin onto a member changes nothing. So this case cannot see B9 at all,
    // and it passes whether or not B9 is fixed. The two below are the ones that
    // measure it.
}

TEST_CASE("Blueprint over the wire: the saving survives a placement that is not the origin")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());
    const App::ContentSet content = App::BuildContentSet();

    const ECS::Transform at      = MovedPlacement();
    const std::uint64_t  derived = CarBytes(content.paths, at, /*moveOne=*/false);
    const std::uint64_t  sent    = CarBytes({}, at, /*moveOne=*/false);

    // Nothing about a placement changes what the client can derive: it receives
    // the placement in the record and composes it onto the same file the host
    // expanded, so every member is still exactly what it would have built anyway.
    //
    // The assertion that proves B9's fix did anything, since the origin case above
    // passes either way: before it, the comparison operand was the authored *local*
    // value against a live Compose(P, T), nothing matched, and the record was pure
    // overhead — 277 bytes against 233 with no manifest at all.
    CHECK(derived < sent);
}

TEST_CASE("Blueprint over the wire: a member reset to its authored value away from the origin still arrives")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());
    const App::ContentSet content = App::BuildContentSet();

    Fixture fixture;
    fixture.Connect(content.paths);

    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(fixture.host, "car.abp", MovedPlacement());
    REQUIRE(id.has_value());

    const ECS::Entity body = App::FindMember(fixture.host, *id, "body");
    REQUIRE(body != ECS::NullEntity);
    ECS::Transform *pose = fixture.host.scene.GetMut<ECS::Transform>(body);
    REQUIRE(pose != nullptr);
    REQUIRE(pose->position.x == doctest::Approx(40.f)); // the placement, composed in

    // The body is dragged back to the origin before the first snapshot — a pose
    // that now coincides with the file's authored local value while the placement
    // does not. Nothing exotic: an edit, a spawn-time snap, a reset.
    pose->position = {0.f, 0.f, 0.f};

    fixture.Step(14);

    const auto rows = fixture.guest.instances.All();
    REQUIRE(rows.size() == 1);
    const ECS::Entity mirror = App::FindMember(fixture.guest, rows[0].first, "body");
    REQUIRE(mirror != ECS::NullEntity);
    const ECS::Transform *mirrored = fixture.guest.scene.Get<ECS::Transform>(mirror);
    REQUIRE(mirrored != nullptr);

    // B9's silent half, and why the byte count above is not enough on its own: a
    // fix could shrink the snapshot and still leave this mirror wrong. With the
    // authored local as the operand the body now equals it exactly, so the
    // Transform was elided — and the gate is `sinceChangeTick == 0 && !clientHasIt`
    // (Replication.cpp:1951), the empty baseline, so elided once meant never
    // resent: guest x = 40 forever against a host at 0.
    CHECK(mirrored->position.x == doctest::Approx(0.f));
}

TEST_CASE("Blueprint over the wire: an edited member still arrives edited")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());
    const App::ContentSet content = App::BuildContentSet();

    Fixture fixture;
    fixture.Connect(content.paths);

    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(fixture.host, "car.abp", {});
    REQUIRE(id.has_value());

    const ECS::Entity wheel = App::FindMember(fixture.host, *id, "wheel_l");
    REQUIRE(wheel != ECS::NullEntity);
    ECS::Transform *pose = fixture.host.scene.GetMut<ECS::Transform>(wheel);
    REQUIRE(pose != nullptr);
    pose->position.y = 7.5f;
    // GetMut already stamps the change tick.

    fixture.Step(14);

    const auto rows = fixture.guest.instances.All();
    REQUIRE(rows.size() == 1);
    const ECS::Entity mirror = App::FindMember(fixture.guest, rows[0].first, "wheel_l");
    REQUIRE(mirror != ECS::NullEntity);

    // The elision is by value, so a member that stopped matching the file is
    // sent — this is the assertion that would fail if the skip were driven by a
    // change tick that happened to predate the spawn.
    const ECS::Transform *mirrored = fixture.guest.scene.Get<ECS::Transform>(mirror);
    REQUIRE(mirrored != nullptr);
    CHECK(mirrored->position.y == doctest::Approx(7.5f));
}

TEST_CASE("Blueprint over the wire: the guest installs the systems the blueprint names")
{
    const std::filesystem::path root = FreshRoot();

    // A car that needs a system to behave. The guest never ran SpawnBlueprint,
    // so without the install hook it would hold the components and run none of
    // the code — the founding failure of this whole design, across machines
    // (docs/blueprint-concept-review.md W1).
    nlohmann::json car   = CarFile();
    car["systems"]       = nlohmann::json::array({"Counter"});
    Write(root, "car.abp", car);

    const App::ContentSet content = App::BuildContentSet();

    Fixture fixture;
    fixture.Connect(content.paths);

    REQUIRE_FALSE(fixture.guest.systems.Has("Counter"));

    REQUIRE(App::SpawnBlueprint(fixture.host, "car.abp", {}).has_value());
    fixture.Step(12);

    // Queued, so it lands at the next safe point rather than mid-walk. The frame
    // loop drains every resident world at DrainMain; there is no frame loop here,
    // so this names the two worlds the fixture has.
    App::DrainSystemInstalls(fixture.host);
    App::DrainSystemInstalls(fixture.guest);

    CHECK(fixture.guest.systems.Has("Counter"));
    CHECK(fixture.host.systems.Has("Counter"));
}

TEST_CASE("Blueprint over the wire: a blueprint outside the content set still replicates")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());

    Fixture fixture;
    // An empty manifest: nothing can be named by index, so the provider refuses
    // every instance and the members fall back to ordinary entities.
    fixture.Connect({});

    REQUIRE(App::SpawnBlueprint(fixture.host, "car.abp", {}).has_value());
    fixture.Step(12);

    // Still mirrored — correct, merely larger — and with no record to expand.
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
    CHECK(fixture.client.InstanceRecords().empty());
    CHECK(fixture.guest.instances.Size() == 0);
}
