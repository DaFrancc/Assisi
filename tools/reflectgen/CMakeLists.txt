# reflectgen has no build artifacts — it is a Python codegen script. Its tests
# are golden-file / behavioural checks run through the Python interpreter that
# AssisiReflect.cmake already located (find_package(Python3) at include time).
# Registered as a ctest so `ctest` covers the codegen path alongside the C++
# suites. The test resolves its own fixture/golden paths from __file__, so it is
# independent of the working directory.
add_test(
    NAME reflectgen
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reflectgen.py"
)

# Its own test rather than more cases in the first one: the AEVENT pass reads no
# struct body and shares nothing with the component scanner but namespace
# capture, and what it defends is a name — a catalog key that drifted from the
# type would leave every markup file naming an event that no longer registers.
add_test(
    NAME reflectgen-events
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_events.py"
)

# Its own test rather than more cases in the one above: this covers a different
# input language (.abp files, not C++ headers) and a different failure — a field
# name that is not a real member resolves to NullEntity at runtime instead of
# failing the build.
add_test(
    NAME blueprint-views
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_blueprint_views.py"
)
