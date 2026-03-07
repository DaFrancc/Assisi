# AssisiReflect.cmake
# Provides the assisi_reflect() and assisi_link_reflections() functions.
#
# Usage in a module's CMakeLists.txt:
#
#   assisi_reflect(
#     TARGET  Assisi-Runtime
#     HEADERS
#       include/Assisi/Runtime/Components.hpp
#       include/Assisi/Runtime/LightComponents.hpp
#   )
#
# For each listed header that contains ACOMP annotations, a .generated.cpp is
# produced in ${CMAKE_CURRENT_BINARY_DIR}/generated/.  The sources are compiled
# into a separate OBJECT library (${TARGET}-Generated) rather than into the
# module's static library.  This avoids the MSVC linker stripping unreferenced
# translation units from static libraries, which would silently discard all
# static-initializer registrations.
#
# In the CMakeLists.txt of every final executable that needs reflection:
#
#   assisi_link_reflections(Assisi-Sandbox)
#
# This adds $<TARGET_OBJECTS:...> for every OBJECT library produced by
# assisi_reflect() so the registration code is always included in the link.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_ASSISI_REFLECTGEN "${CMAKE_SOURCE_DIR}/tools/reflectgen/reflectgen.py"
    CACHE FILEPATH "Path to the reflectgen code-generation script" FORCE)

function(assisi_reflect)
    cmake_parse_arguments(_ARG "" "TARGET" "HEADERS" ${ARGN})

    if(NOT _ARG_TARGET)
        message(FATAL_ERROR "assisi_reflect: TARGET is required")
    endif()
    if(NOT _ARG_HEADERS)
        message(FATAL_ERROR "assisi_reflect: HEADERS is required")
    endif()

    set(_generated_sources "")

    foreach(_header ${_ARG_HEADERS})
        # Resolve to absolute path.
        if(IS_ABSOLUTE "${_header}")
            set(_abs "${_header}")
        else()
            set(_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_header}")
        endif()

        # Output file in binary dir/generated/.
        get_filename_component(_stem "${_header}" NAME_WE)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/generated/${_stem}.generated.cpp")

        add_custom_command(
            OUTPUT  "${_out}"
            COMMAND Python3::Interpreter
                    "${_ASSISI_REFLECTGEN}"
                    "${_abs}"
                    --outdir "${CMAKE_CURRENT_BINARY_DIR}/generated"
            DEPENDS "${_abs}" "${_ASSISI_REFLECTGEN}"
            COMMENT "reflectgen: ${_header}"
            VERBATIM
        )

        list(APPEND _generated_sources "${_out}")
    endforeach()

    # Compile generated sources as a separate OBJECT library.
    # OBJECT libraries are always fully included in the final link — unlike
    # static libraries, the linker never strips their translation units.
    set(_obj_target "${_ARG_TARGET}-Generated")
    add_library("${_obj_target}" OBJECT ${_generated_sources})

    # Inherit the reflected module's include paths and compile settings
    # (Options/Warnings/Perf come through transitively since the module
    # links them PUBLIC via Assisi_apply_defaults).
    target_link_libraries("${_obj_target}" PRIVATE "${_ARG_TARGET}")

    # Register this OBJECT library globally so assisi_link_reflections()
    # can gather all of them.
    set_property(GLOBAL APPEND PROPERTY ASSISI_REFLECT_OBJECT_TARGETS "${_obj_target}")
endfunction()

# Call once on each final executable (or shared library) to force-include
# all reflection registration code produced by assisi_reflect() calls.
function(assisi_link_reflections target)
    get_property(_reflect_targets GLOBAL PROPERTY ASSISI_REFLECT_OBJECT_TARGETS)
    foreach(_rt ${_reflect_targets})
        target_sources("${target}" PRIVATE "$<TARGET_OBJECTS:${_rt}>")
    endforeach()
endfunction()
