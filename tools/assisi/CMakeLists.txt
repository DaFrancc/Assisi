# The build tool (./assisi) has no build artifacts; these are its own tests, run
# through ctest so the whole suite covers it.
#
# Linux only for now: the suite fakes cmake, ninja, git and the rest with
# `#!/bin/sh` scripts, which Windows cannot run, so on Windows nearly every case
# fails on a missing program rather than on the tool. Running it there needs
# fakes Windows can start.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(
      NAME assisi-tool
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_assisi.py"
  )
  # Also run inside the Steam Runtime container, whose Python 3.9 is the oldest the
  # command line has to work on. A syntax check cannot catch library behaviour that
  # changed since: 3.9's argparse, for one, rejects an empty list of positionals
  # that has choices.
  set_tests_properties(assisi-tool PROPERTIES LABELS container)
endif()

# ./assisi steam-sdk, against a fake SDK the test builds: no real one is read.
add_test(
    NAME assisi-steam-sdk
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_steam_sdk.py"
)
set_tests_properties(assisi-steam-sdk PROPERTIES LABELS container)

# The TUI, against a fake terminal; in the container too, since it runs on the
# same Python as the command line.
add_test(
    NAME assisi-tui
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tui.py"
)
set_tests_properties(assisi-tui PROPERTIES LABELS container)
