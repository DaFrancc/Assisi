/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/App/AppConfig.hpp>

#include <string>

using namespace Assisi::App;

TEST_CASE("AppConfig: a document applies only the fields it names")
{
    // The forward-compatibility contract. A config written before a field
    // existed must not reset that field to zero on load, or every engine update
    // that adds one would silently rewrite everyone's settings.
    const AppConfig cfg = AppConfig::FromJsonText(
        R"({ "version": 1, "type": "AppConfig", "title": "Renamed", "width": 1920 })");

    CHECK(cfg.title.View() == "Renamed");
    CHECK(cfg.width == 1920);
    CHECK(cfg.height == AppConfig{}.height);
    CHECK(cfg.physicsHz == doctest::Approx(AppConfig{}.physicsHz));
    CHECK(cfg.keepLogs == AppConfig{}.keepLogs);
    CHECK(cfg.keepDumps == AppConfig{}.keepDumps);
}

TEST_CASE("AppConfig: the startup scene is what the document says, or nothing")
{
    // The game takes no level argument, so this field is the whole of what tells
    // it what to open. A default of "some level" would be a game that boots
    // something nobody asked it to; empty is refused at startup by name.
    CHECK(AppConfig{}.startupScene.View().empty());

    const AppConfig cfg = AppConfig::FromJsonText(
        R"({ "version": 1, "type": "AppConfig", "startupScene": "levels/Materials.alvl" })");

    CHECK(cfg.startupScene.View() == "levels/Materials.alvl");
}

TEST_CASE("AppConfig: the simulate-from policy is read, and defaults to Begin")
{
    // Begin is what every world did before the policy existed, so a config that
    // says nothing must keep behaving that way.
    CHECK(AppConfig{}.simulateFrom == SimulateFrom::Begin);

    const AppConfig cfg =
        AppConfig::FromJsonText(R"({ "version": 1, "type": "AppConfig", "simulateFrom": 1 })");

    CHECK(cfg.simulateFrom == SimulateFrom::Loaded);
}

TEST_CASE("AppConfig: an enum reads by name as well as by number")
{
    // A config is hand-edited, and "Loaded" says what 1 does not.
    CHECK(AppConfig::FromJsonText(
              R"({ "version": 1, "type": "AppConfig", "simulateFrom": "Loaded" })")
              .simulateFrom == SimulateFrom::Loaded);

    // The integer form still reads, or every file written before this stopped
    // loading.
    CHECK(AppConfig::FromJsonText(R"({ "version": 1, "type": "AppConfig", "simulateFrom": 1 })")
              .simulateFrom == SimulateFrom::Loaded);
}

TEST_CASE("AppConfig: a name the enum does not define is refused, not defaulted")
{
    // A misspelt enumerator is a file that means something other than what it
    // says. Refusing costs the document, which is what every other unreadable
    // field here costs.
    const AppConfig cfg = AppConfig::FromJsonText(
        R"({ "version": 1, "type": "AppConfig", "width": 1920, "simulateFrom": "Lodaed" })");

    CHECK(cfg.width == AppConfig{}.width);
    CHECK(cfg.simulateFrom == SimulateFrom::Begin);
}

TEST_CASE("AppConfig: Count is not a value a field may hold")
{
    // The trailing enumerator counts the others; the generated name table omits
    // it, so it is refused like any other name the enum does not define.
    const AppConfig cfg = AppConfig::FromJsonText(
        R"({ "version": 1, "type": "AppConfig", "width": 1920, "simulateFrom": "Count" })");

    CHECK(cfg.width == AppConfig{}.width);
}

TEST_CASE("AppConfig: a physics rate at or below zero is refused")
{
    // Zero disables fixed update outright (the step becomes infinite) and a
    // negative one makes Application::Run's accumulator loop non-terminating.
    // Neither is something a config file may cause.
    const double fallback = AppConfig{}.physicsHz;

    CHECK(AppConfig::FromJsonText(R"({ "version": 1, "type": "AppConfig", "physicsHz": 0 })").physicsHz ==
          doctest::Approx(fallback));
    CHECK(AppConfig::FromJsonText(R"({ "version": 1, "type": "AppConfig", "physicsHz": -30 })").physicsHz ==
          doctest::Approx(fallback));

    // A positive one still gets through, or the guard would be a constant.
    CHECK(AppConfig::FromJsonText(R"({ "version": 1, "type": "AppConfig", "physicsHz": 120 })").physicsHz ==
          doctest::Approx(120.0));
}

TEST_CASE("AppConfig: a document of another asset type yields defaults entire")
{
    // Field names coincide across asset types, so applying a wrong-type document
    // field by field would produce a config that is part one file and part
    // another — worse than refusing, because it looks like it worked.
    const AppConfig cfg =
        AppConfig::FromJsonText(R"({ "version": 1, "type": "InputBindings", "width": 1920 })");

    CHECK(cfg.width == AppConfig{}.width);
}

TEST_CASE("AppConfig: text that will not parse costs the config, never the launch")
{
    const AppConfig cfg = AppConfig::FromJsonText("{ not json");

    CHECK(cfg.title.View() == AppConfig{}.title.View());
    CHECK(cfg.width == AppConfig{}.width);
    CHECK(cfg.physicsHz == doctest::Approx(AppConfig{}.physicsHz));
}

TEST_CASE("AppConfig: a title of a realistic length survives intact")
{
    // The inline string truncates silently on assignment, so the capacity is
    // what stands between a long title and a game shipping with its own name cut
    // short in the title bar.
    const std::string branded = "Assisi Studios - The Long Subtitle";
    const AppConfig cfg =
        AppConfig::FromJsonText(R"({ "version": 1, "type": "AppConfig", "title": ")" + branded + R"(" })");

    CHECK(cfg.title.View() == branded);
}
