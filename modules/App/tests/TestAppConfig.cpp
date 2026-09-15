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
