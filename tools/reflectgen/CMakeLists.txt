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
