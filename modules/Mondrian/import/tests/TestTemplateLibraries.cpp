/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using namespace ScreenTesting;

namespace
{

constexpr std::string_view kControlsPath = "ui/common/Controls.amdt";

/// A library whose second template is built on its first, so a screen can
/// import one without the other and still use what it is built on.
constexpr std::string_view kControls = R"amdt(<templates>
  <template name="menu_button">
    <button padding="28 10 28 10" text_size="28" />
  </template>
  <template name="labelled_slider">
    <row>
      <menu_button name="down" on_click="step(slider, -1)">-</menu_button>
      <slider name="slider" step="5" />
    </row>
  </template>
</templates>
)amdt";

/// The controls library, and nothing else.
Files ControlsOnly()
{
    return Files{{std::string{kControlsPath}, std::string{kControls}}};
}

/// A screen whose line 2 is @p imports and whose line 3 onwards is @p body.
std::string ImportingScreen(std::string_view imports, std::string_view body)
{
    return "<screen>\n" + std::string{imports} + "\n" + std::string{body} + "\n</screen>\n";
}

} // namespace

TEST_CASE("Template libraries: two screens build from one library")
{
    const ScreenDocument pause = Compiled(ImportingScreen(R"(  <import path="ui/common/Controls.amdt" />)",
                                                          R"(  <menu_button name="resume">Resume</menu_button>)"),
                                          ControlsOnly());
    const ScreenDocument options = Compiled(
        ImportingScreen(R"(  <import path="ui/common/Controls.amdt" />)", R"(  <labelled_slider name="music" />)"),
        ControlsOnly());

    CHECK(NodeNamed(pause, "resume").widget == BuiltinWidget::Button);
    CHECK(NodeNamed(pause, "resume").style.textSize == Px(28.f));
    CHECK(NodeNamed(options, "music.down").target == IndexOf(options, "music.slider"));
}

TEST_CASE("Template libraries: an imported template cooks to the bytes a local one does")
{
    const std::string imported =
        ImportingScreen(R"(  <import path="ui/common/Controls.amdt" />)", R"(  <labelled_slider name="music" />)");
    const std::string local = ImportingScreen("", R"amdn(  <labelled_slider name="music" />
  <template name="menu_button">
    <button padding="28 10 28 10" text_size="28" />
  </template>
  <template name="labelled_slider">
    <row>
      <menu_button name="down" on_click="step(slider, -1)">-</menu_button>
      <slider name="slider" step="5" />
    </row>
  </template>)amdn");

    const std::expected<std::vector<std::byte>, MarkupError> one =
        CompileScreenText(imported, OneEvent(), Serving(ControlsOnly()));
    const std::expected<std::vector<std::byte>, MarkupError> other = CompileScreenText(local, OneEvent(), NoFiles());
    REQUIRE_MESSAGE(one.has_value(), Why(one));
    REQUIRE_MESSAGE(other.has_value(), Why(other));
    CHECK(*one == *other);
}

TEST_CASE("Template libraries: names chooses what comes in, and as chooses what it is called")
{
    SUBCASE("names alone: a listed template still brings what it is built on")
    {
        const std::string imports = R"(  <import path="ui/common/Controls.amdt" names="labelled_slider" />)";
        const ScreenDocument document =
            Compiled(ImportingScreen(imports, R"(  <labelled_slider name="music" />)"), ControlsOnly());
        CHECK(IndexOf(document, "music.down") != kNoNode);

        // What it is built on is not the screen's to write.
        const MarkupError error = Refused(ImportingScreen(imports, "  <menu_button />"), ControlsOnly());
        CHECK(error.line == 3);
        CHECK(error.message.find("menu_button") != std::string::npos);
    }

    SUBCASE("as alone: the prefixed name exists and the bare one does not")
    {
        const std::string imports = R"(  <import path="ui/common/Controls.amdt" as="kit" />)";
        const ScreenDocument document =
            Compiled(ImportingScreen(imports, R"(  <kit.labelled_slider name="music" />)"), ControlsOnly());
        // Built on menu_button, which it names bare in its own file.
        CHECK(IndexOf(document, "music.down") != kNoNode);

        CHECK(Refused(ImportingScreen(imports, "  <menu_button />"), ControlsOnly()).line == 3);
    }

    SUBCASE("both together")
    {
        const std::string imports = R"(  <import path="ui/common/Controls.amdt" names="menu_button" as="kit" />)";
        CHECK(Compiled(ImportingScreen(imports, "  <kit.menu_button />"), ControlsOnly()).nodes.size() == 2);
        CHECK(Refused(ImportingScreen(imports, "  <kit.labelled_slider />"), ControlsOnly()).line == 3);
    }
}

TEST_CASE("Template libraries: a library's template takes parameters as a local one does")
{
    const Files files{{"ui/Steppers.amdt", R"amdt(<templates>
  <template name="stepper" params="target moves=1">
    <button on_click="step(@target, @moves)" />
  </template>
</templates>)amdt"}};
    const ScreenDocument document =
        Compiled(ImportingScreen(R"(  <import path="ui/Steppers.amdt" />)",
                                 "  <slider name=\"volume\" step=\"5\" />\n"
                                 "  <stepper name=\"down\" target=\"volume\" moves=\"-1\" />"),
                 files);
    CHECK(NodeNamed(document, "down").target == IndexOf(document, "volume"));
    CHECK(NodeNamed(document, "down").moves == -1);
}

TEST_CASE("Template libraries: an import is refused where it is written")
{
    SUBCASE("a path naming no file")
    {
        const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/Missing.amdt" />)", ""), {});
        CHECK(error.line == 2);
        CHECK(error.file.empty());
        CHECK(error.message.find("ui/Missing.amdt") != std::string::npos);
    }

    SUBCASE("a path that is not a library")
    {
        CHECK(Refused(ImportingScreen(R"(  <import path="ui/notes.txt" />)", ""), {}).line == 2);
    }

    SUBCASE("a screen, whose templates are its own")
    {
        const Files files{{"ui/Pause.amdn", "<screen><template name=\"t\"><row /></template></screen>"}};
        const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/Pause.amdn" />)", ""), files);
        CHECK(error.line == 2);
        CHECK(error.message.find("library") != std::string::npos);
    }

    SUBCASE("a listed name the library does not declare")
    {
        const MarkupError error = Refused(
            ImportingScreen(R"(  <import path="ui/common/Controls.amdt" names="dialog" />)", ""), ControlsOnly());
        CHECK(error.line == 2);
        CHECK(error.message.find("dialog") != std::string::npos);
        CHECK(error.message.find("menu_button") != std::string::npos);
    }

    SUBCASE("a name the file already has")
    {
        const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/common/Controls.amdt" />)",
                                                          R"(  <template name="menu_button"><button /></template>)"),
                                          ControlsOnly());
        CHECK(error.line == 2);
        CHECK(error.message.find("menu_button") != std::string::npos);
    }

    SUBCASE("a name another import already brought, refused at the second")
    {
        const MarkupError error = Refused(ImportingScreen("  <import path=\"ui/common/Controls.amdt\" />\n"
                                                          "  <import path=\"ui/common/Controls.amdt\" />",
                                                          ""),
                                          ControlsOnly());
        CHECK(error.line == 3);
    }

    SUBCASE("a prefix another import already gave")
    {
        const MarkupError error =
            Refused(ImportingScreen("  <import path=\"ui/common/Controls.amdt\" names=\"menu_button\" "
                                    "as=\"kit\" />\n"
                                    "  <import path=\"ui/common/Controls.amdt\" "
                                    "names=\"labelled_slider\" as=\"kit\" />",
                                    ""),
                    ControlsOnly());
        CHECK(error.line == 3);
        CHECK(error.message.find("kit") != std::string::npos);
    }

    SUBCASE("anywhere but directly in the root")
    {
        CHECK(Refused(ImportingScreen("", R"(  <row><import path="ui/common/Controls.amdt" /></row>)"), ControlsOnly())
                  .line == 3);
    }
}

TEST_CASE("Template libraries: a screen is a screen and a library is a library")
{
    SUBCASE("a screen file starting with <templates>")
    {
        const MarkupError error = Refused("<templates>\n  <template name=\"t\"><row /></template>\n</templates>\n");
        CHECK(error.line == 1);
        CHECK(error.message.find("library") != std::string::npos);
    }

    SUBCASE("<templates> inside a screen")
    {
        CHECK(Refused(ImportingScreen("", "  <templates />")).line == 3);
    }

    SUBCASE("a library starting with <screen>, which names the library")
    {
        const Files files{{"ui/Wrong.amdt", "<screen>\n  <row />\n</screen>\n"}};
        const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/Wrong.amdt" />)", ""), files);
        CHECK(error.file == "ui/Wrong.amdt");
        CHECK(error.line == 1);
    }

    SUBCASE("a <screen> inside a library")
    {
        const Files files{{"ui/Wrong.amdt", "<templates>\n  <template name=\"t\"><row /></template>\n  <screen />\n"
                                            "</templates>\n"}};
        const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/Wrong.amdt" />)", ""), files);
        CHECK(error.file == "ui/Wrong.amdt");
        CHECK(error.line == 3);
    }
}

TEST_CASE("Template libraries: a mistake inside a library names the library")
{
    SUBCASE("in a template nobody uses")
    {
        const Files files{{"ui/Broken.amdt", "<templates>\n  <template name=\"t\">\n    <button colour=\"#ff0000\" />\n"
                                             "  </template>\n</templates>\n"}};
        const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/Broken.amdt" />)", ""), files);
        CHECK(error.file == "ui/Broken.amdt");
        CHECK(error.line == 3);
    }

    SUBCASE("found only in an instance the screen writes, which it also names")
    {
        const Files files{{"ui/Steppers.amdt", "<templates>\n  <template name=\"stepper\" params=\"target\">\n"
                                               "    <button on_click=\"step(@target, 1)\" />\n"
                                               "  </template>\n</templates>\n"}};
        const MarkupError error = Refused(
            ImportingScreen(R"(  <import path="ui/Steppers.amdt" />)", R"(  <stepper target="nothing" />)"), files);
        // The target is looked up once the screen is read, where it is
        // reported: the instance's own line in the screen.
        CHECK(error.file.empty());
        CHECK(error.message.find("nothing") != std::string::npos);
    }

    SUBCASE("in an attribute a passed value makes invalid")
    {
        const Files files{{"ui/Steppers.amdt", "<templates>\n  <template name=\"stepper\" params=\"moves\">\n"
                                               "    <row><slider name=\"s\" step=\"1\" />\n"
                                               "    <button on_click=\"step(s, @moves)\" /></row>\n"
                                               "  </template>\n</templates>\n"}};
        const MarkupError error =
            Refused(ImportingScreen(R"(  <import path="ui/Steppers.amdt" />)", R"(  <stepper moves="0" />)"), files);
        CHECK(error.file == "ui/Steppers.amdt");
        CHECK(error.line == 4);
        CHECK(error.message.find("line 3") != std::string::npos);
        CHECK(error.message.find("of the screen") != std::string::npos);
    }
}

TEST_CASE("Template libraries: a library may not import itself, however far down")
{
    const Files files{
        {"ui/A.amdt", "<templates>\n  <import path=\"ui/B.amdt\" />\n  <template name=\"a\"><row /></template>\n"
                      "</templates>\n"},
        {"ui/B.amdt", "<templates>\n  <import path=\"ui/A.amdt\" />\n  <template name=\"b\"><row /></template>\n"
                      "</templates>\n"}};
    const MarkupError error = Refused(ImportingScreen(R"(  <import path="ui/A.amdt" />)", ""), files);
    CHECK(error.file == "ui/B.amdt");
    CHECK(error.line == 2);
    CHECK(error.message.find("ui/A.amdt") != std::string::npos);

    // And a library checked on its own is on the chain from the start.
    const Files self{{"ui/Self.amdt", "<templates>\n  <import path=\"ui/Self.amdt\" />\n"
                                      "  <template name=\"s\"><row /></template>\n</templates>\n"}};
    const std::expected<void, MarkupError> checked = CheckLibrary("ui/Self.amdt", OneEvent(), Serving(self));
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().line == 2);
}

TEST_CASE("Template libraries: a library is checked whole on its own")
{
    CHECK(CheckLibrary(kControlsPath, OneEvent(), Serving(ControlsOnly())).has_value());

    const Files empty{{"ui/Empty.amdt", "<templates>\n</templates>\n"}};
    CHECK_FALSE(CheckLibrary("ui/Empty.amdt", OneEvent(), Serving(empty)).has_value());

    const Files broken{{"ui/Broken.amdt", "<templates>\n  <template name=\"t\">\n    <button colour=\"#ff0000\" />\n"
                                          "  </template>\n</templates>\n"}};
    const std::expected<void, MarkupError> checked = CheckLibrary("ui/Broken.amdt", OneEvent(), Serving(broken));
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().file == "ui/Broken.amdt");
    CHECK(checked.error().line == 3);
}

TEST_CASE("Template libraries: a screen depends on every library it reaches")
{
    const Files files{
        {"ui/A.amdt", "<templates><import path=\"ui/B.amdt\" /><template name=\"a\"><row /></template></templates>"},
        {"ui/B.amdt", "<templates><import path=\"ui/A.amdt\" /><template name=\"b\"><row /></template></templates>"},
        {"ui/C.amdt", "<templates><template name=\"c\"><row /></template></templates>"}};
    const std::string screen =
        "<screen><import path=\"ui/C.amdt\" /><import path=\"ui/A.amdt\" /><import path=\"ui/Gone.amdt\" /></screen>";

    // Through a cycle without following it forever, and naming a file that is
    // not there, which the compile itself reports.
    const std::vector<std::string> expected{"ui/A.amdt", "ui/B.amdt", "ui/C.amdt", "ui/Gone.amdt"};
    CHECK(ImportedLibraries(screen, Serving(files)) == expected);
}
