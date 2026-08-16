/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintReplication.cpp
/// @brief A real blueprint, spawned on a host, arriving on a client as a
/// blueprint rather than as a pile of entities.
///
/// The NetSync suite drives the same machinery with fakes, which pins the wire
/// format and the id discipline. What it cannot check is the claim the whole
/// design rests on: that the client's *own* expansion of the same file produces
/// the same members, in the same order, at the same places — without the server
/// sending any of it.
///
/// Both sides share an asset root here, which is what a matching content-set
/// hash means in production.

#include <doctest/doctest.h>

#include <ostream>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
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
    std::error_code ec;
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
    Net::NetTransport transport;

    // The worlds a test that does not care where its worlds come from gets:
    // standalone, so `manager` is null — a shape the engine has to keep working
    // (World.hpp) and the one every case below but the services case wants.
    App::World ownHost;
    App::World ownGuest;

    App::World &host;
    App::World &guest;
    std::pair<Net::ConnectionId, Net::ConnectionId> pair;
    NetSync::ReplicationServer server;
    NetSync::ReplicationClient client;
    std::uint64_t tick = 0;

    Fixture()
        : host(ownHost), guest(ownGuest), pair(transport.CreateLoopbackPair()), server(transport, host.scene),
        client(transport, guest.scene, pair.second)
    {
    }

    /// Over worlds the caller owns — a manager's, for the cases that turn on what
    /// a world can reach through its manager. Spelled out rather than delegated:
    /// a delegating constructor would have to name `ownHost` as an argument
    /// before the delegated-to constructor has created it.
    Fixture(App::World &hostWorld, App::World &guestWorld)
        : host(hostWorld), guest(guestWorld), pair(transport.CreateLoopbackPair()),
        server(transport, host.scene), client(transport, guest.scene, pair.second)
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
            // Where a frame flushes: Scene::Destroy is deferred, and until this
            // runs a destroyed entity is still alive to Query and to Get<T>. The
            // server's despawn detection is a set difference over live entities,
            // so without it a destroyed member is never noticed as gone.
            host.scene.FlushDestroyed();
            guest.scene.FlushDestroyed();
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

TEST_CASE("Blueprint over the wire: destroying the instance on the host takes the guest's row with it")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());

    const App::ContentSet content = App::BuildContentSet();

    Fixture fixture;
    fixture.Connect(content.paths);

    const std::optional<ECS::InstanceId> spawned = App::SpawnBlueprint(fixture.host, "car.abp", {});
    REQUIRE(spawned.has_value());

    fixture.Step(12);
    REQUIRE(fixture.guest.instances.Size() == 1);
    REQUIRE(fixture.client.InstanceRecords().size() == 1);

    REQUIRE(App::DestroyInstance(fixture.host, *spawned));
    fixture.Step(12);

    // The members are gone from the mirror, and so is the record NetSync kept
    // for them...
    CHECK(fixture.client.ReplicatedEntityCount() == 0);
    CHECK(fixture.client.InstanceRecords().empty());

    // ...and so is the row the expansion put in the guest's own table, which
    // NetSync does not own and has no way to reach. Without a despawn counterpart
    // to Expand the row outlives every one of its members for the rest of the
    // session: nothing in the entity list, which is member-driven, but a row is
    // what the viewport draws an instance icon from and what PickInstance
    // ray-tests — a ghost you can still click on.
    CHECK(fixture.guest.instances.Size() == 0);
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
/// translation states it in full: the composition is real and the comparison
/// operand has to account for it — these two cases send 153 bytes where a run
/// with no manifest sends 233, the same saving the origin case gets.
///
/// A rotated placement is a strictly harder case and does not belong here: the
/// compose/inverse-compose pair is an exact inverse in arithmetic but not
/// bit-for-bit in float, so whether a *byte* comparison can ever match under one
/// is an open question. Asserting it here would demand something the elision may
/// not be able to deliver.
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
    const std::uint64_t derived = CarBytes(content.paths, origin, /*moveOne=*/ false);
    const std::uint64_t sent    = CarBytes({}, origin, /*moveOne=*/ false);

    // The members are identical to the file, so with a manifest their components
    // are not on the wire at all — the client already has them.
    CHECK(derived < sent);

    // ...and a member that actually differs is not elided, or the mirror would
    // be quietly wrong.
    const std::uint64_t withEdit = CarBytes(content.paths, origin, /*moveOne=*/ true);
    CHECK(withEdit > derived);

    // Spawned at the origin, which is the one placement where comparing a live
    // component against the *authored local* value is sound — composing the origin
    // onto a member changes nothing. So this case says nothing about elision under
    // any other placement; the two below are the ones that measure that.
}

TEST_CASE("Blueprint over the wire: the saving survives a placement that is not the origin")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());
    const App::ContentSet content = App::BuildContentSet();

    const ECS::Transform at      = MovedPlacement();
    const std::uint64_t derived = CarBytes(content.paths, at, /*moveOne=*/ false);
    const std::uint64_t sent    = CarBytes({}, at, /*moveOne=*/ false);

    // Nothing about a placement changes what the client can derive: it receives
    // the placement in the record and composes it onto the same file the host
    // expanded, so every member is still exactly what it would have built anyway.
    //
    // The assertion the origin case above cannot make, since that one passes either
    // way: comparing the authored *local* against a live Compose(P, T) matches
    // nothing away from the origin, and the record is then pure overhead.
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

    // Why the byte count above is not enough on its own: a change that shrinks the
    // snapshot can still leave this mirror wrong. With the authored local as the
    // comparison operand the body now equals it exactly, so the Transform is
    // elided — and the gate is `sinceChangeTick == 0 && !clientHasIt`
    // (ReplicationServerSnapshot.cpp), the empty baseline, so elided once is never
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
    // the code — the founding failure of this whole design, across machines.
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

TEST_CASE("Blueprint over the wire: a guest with no render services expands anyway")
{
    const std::filesystem::path root = FreshRoot();

    // A member with something to resolve: the resolve walks MeshRenderers, so a
    // car of bare Transforms would take the guarded path either way and this case
    // would pass with no guard at all.
    nlohmann::json car = CarFile();
    car["entities"][0]["components"]["MeshRenderer"] = {
        {"mesh", {{"guid", "8c08e9c0-e9fb-4f84-a9ba-7a90223526fd"}}}};
    Write(root, "car.abp", car);

    const App::ContentSet content = App::BuildContentSet();

    // Managed worlds with no services installed — what a dedicated server is, and
    // what every headless process is. The expander resolves its placed members'
    // assets, and this is the shape that resolve has to survive: a manager it can
    // reach, holding a cache and a database that are not there.
    // The standalone world every other case here uses (`manager == nullptr`) is
    // the other half of the same guard.
    App::WorldManager worlds;
    App::World &host  = worlds.Create("Host");
    App::World &guest = worlds.Create("Guest");

    Fixture fixture{host, guest};
    fixture.Connect(content.paths);

    REQUIRE(App::SpawnBlueprint(fixture.host, "car.abp", {}).has_value());
    fixture.Step(12);

    // What the resolve *produces* is a GPU pointer, and a Render::AssetCache has
    // to be Initialize()d against an nvrhi device before it can produce one —
    // which this suite has no way to build. So this case does not claim the
    // meshes came out resolved; it pins the half that is observable without a
    // device, which is that the expansion completes with the services absent
    // rather than dereferencing one of them on the way through.
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
    const auto rows = fixture.guest.instances.All();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].second->source == "car.abp");
    for (const char *name : {"body", "wheel_l", "wheel_r"})
    {
        CAPTURE(name);
        CHECK(App::FindMember(fixture.guest, rows[0].first, name) != ECS::NullEntity);
    }
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

TEST_CASE("Blueprint over the wire: an unsorted manifest is declined rather than misread")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());

    Fixture fixture;
    // Out of order, and specifically an order where the binary search still
    // *finds* car.abp — at index 1, where a sorted list would have put a_other.
    // That is the dangerous shape: not a lookup that misses and falls back to
    // member-by-member, but one that succeeds and names the wrong file, which
    // NetSync cannot catch because it only checks the member count. An order
    // where the search simply misses would pass this test either way.
    fixture.Connect({"b_other.abp", "car.abp", "a_other.abp"});

    REQUIRE(App::SpawnBlueprint(fixture.host, "car.abp", {}).has_value());
    fixture.Step(12);

    // Declined, so this behaves exactly like a blueprint outside the content set:
    // whole and unnamed, rather than named wrongly.
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
    CHECK(fixture.client.InstanceRecords().empty());
    CHECK(fixture.guest.instances.Size() == 0);
}

TEST_CASE("Content set: the scan job delivers the paths it hashed")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());
    Write(root, "b_second.abp", CarFile());

    Core::JobSystem jobs;
    App::ContentSetHashJob job;
    job.Start(jobs);

    // Poll until it lands; the scan is a worker job with no completion callback.
    App::ContentSet delivered;
    for (int i = 0; i < 2000 && !job.Poll(delivered); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // The manifest and the hash have to come from one scan. Rebuilt separately,
    // a file that changed in between would leave the index naming a file the far
    // side never agreed to — which is why the job carries both rather than the
    // caller rescanning for the paths.
    const App::ContentSet direct = App::BuildContentSet();
    CHECK(delivered.hash == direct.hash);
    CHECK(delivered.paths == direct.paths);
    REQUIRE(delivered.paths.size() == 2);
    CHECK(std::is_sorted(delivered.paths.begin(), delivered.paths.end()));

    // Delivered exactly once.
    App::ContentSet again;
    CHECK_FALSE(job.Poll(again));
}

TEST_CASE("Join: every target answers the host's level the same way")
{
    const std::filesystem::path root = FreshRoot();
    Write(root, "car.abp", CarFile());

    // Asked once, in the engine, so a dedicated server and a windowed client
    // cannot disagree about whether a session is joinable.
    SUBCASE("a host running no level")
    {
        NetSync::LevelIdentity level;
        const auto resolved = App::ResolveJoinLevel(level);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error() == App::JoinLevelError::NoLevel);
    }

    SUBCASE("a level this build does not have")
    {
        NetSync::LevelIdentity level;
        level.addressing = NetSync::LevelAddressing::Virtual;
        level.path       = "levels/NotHere.alvl";

        const auto resolved = App::ResolveJoinLevel(level);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error() == App::JoinLevelError::Unresolvable);
    }

    SUBCASE("a level whose bytes differ from the host's")
    {
        NetSync::LevelIdentity level;
        level.addressing = NetSync::LevelAddressing::Virtual;
        level.path       = "car.abp";
        // Whatever this file hashes to, it is not this.
        level.contentHash = 0xDEADBEEFu;

        const auto resolved = App::ResolveJoinLevel(level);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error() == App::JoinLevelError::ContentMismatch);
    }

    SUBCASE("a level that matches resolves to the file to load")
    {
        const std::optional<std::uint64_t> hash = App::HashLevelFile("car.abp");
        REQUIRE(hash.has_value());

        NetSync::LevelIdentity level;
        level.addressing  = NetSync::LevelAddressing::Virtual;
        level.path        = "car.abp";
        level.contentHash = *hash;

        const auto resolved = App::ResolveJoinLevel(level);
        REQUIRE(resolved.has_value());
        CHECK(std::filesystem::exists(*resolved));
    }
}

TEST_CASE("Join: stripping the host's copies takes their bodies out of the physics world")
{
    App::World world;

    // The level's own copy of something the host owns, with a body — which is
    // what a joined client has after loading the same file the host did.
    const ECS::Entity replicated = world.scene.Create();
    ECS::Transform pose;
    pose.position = {0.f, 10.f, 0.f};
    (void)world.scene.Add<ECS::Transform>(replicated, pose);
    (void)world.scene.Add<NetSync::Replicated>(replicated, NetSync::Replicated{});
    (void)world.scene.Add<Physics::RigidBodyDescriptor>(replicated, Physics::RigidBodyDescriptor{});

    // A child of it — a decoration, a light, anything parented to a replicated
    // object in the file.
    const ECS::Entity child = world.scene.Create();
    (void)world.scene.Add<ECS::Transform>(child, ECS::Transform{});
    (void)world.scene.Add<Runtime::Parent>(child, Runtime::Parent{replicated});

    (void)App::BuildSceneBodies(world.scene, world.physics);

    std::vector<Physics::PhysicsWorld::ActiveBodyState> before;
    world.physics.GetActiveBodyStates(before);
    REQUIRE(before.size() == 1);

    const App::StrippedEntities stripped = App::StripReplicatedEntities(world.scene, world.physics);
    CHECK(stripped.entities == 1);
    CHECK(stripped.orphans == 1);

    // The body goes with the entity. Destroying the entity alone leaves a body in
    // the simulation that nothing holds a handle to.
    std::vector<Physics::PhysicsWorld::ActiveBodyState> after;
    world.physics.GetActiveBodyStates(after);
    CHECK(after.empty());

    // ...and the child is not left pointing at a dead parent, which propagation
    // would read as a root and place at its local pose.
    CHECK(world.scene.IsAlive(child));
    CHECK(world.scene.Get<Runtime::Parent>(child) == nullptr);
}
