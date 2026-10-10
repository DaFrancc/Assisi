# The build tool (./assisi) has no build artifacts; these are its own tests, run
# through ctest so the whole suite covers it.
#
# The command line's tests run on any Python. The TUI's tests also drive the
# Textual app, so they run under the TUI's own interpreter when that environment
# has been created (the first time somebody opens the TUI); under any other
# Python the app half skips itself and the rest still runs.
add_test(
    NAME assisi-tool
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_assisi.py"
)
# Also run inside the Steam Runtime container, whose Python 3.9 is the oldest the
# command line has to work on. A syntax check cannot catch library behaviour that
# changed since: 3.9's argparse, for one, rejects an empty list of positionals
# that has choices.
set_tests_properties(assisi-tool PROPERTIES LABELS container)

# ./assisi steam-sdk, against a fake SDK the test builds: no real one is read.
add_test(
    NAME assisi-steam-sdk
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_steam_sdk.py"
)
set_tests_properties(assisi-steam-sdk PROPERTIES LABELS container)

if (WIN32)
  set(_assisi_tui_python "${CMAKE_SOURCE_DIR}/out/tool-env/Scripts/python.exe")
else()
  set(_assisi_tui_python "${CMAKE_SOURCE_DIR}/out/tool-env/bin/python")
endif()

if (EXISTS "${_assisi_tui_python}")
  add_test(NAME assisi-tui COMMAND "${_assisi_tui_python}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tui.py")
else()
  add_test(NAME assisi-tui COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tui.py")
endif()
