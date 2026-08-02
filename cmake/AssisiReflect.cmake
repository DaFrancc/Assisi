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

# reflectgen.py is a thin CLI over sibling modules (reflect_parser/_types/
# _codegen); depend on all of them so editing any one regenerates the output.
file(GLOB _ASSISI_REFLECTGEN_SOURCES "${CMAKE_SOURCE_DIR}/tools/reflectgen/*.py")
set(_ASSISI_REFLECTGEN_SOURCES "${_ASSISI_REFLECTGEN_SOURCES}"
    CACHE INTERNAL "reflectgen implementation files the generated sources depend on" FORCE)

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
            DEPENDS "${_abs}" "${_ASSISI_REFLECTGEN}" ${_ASSISI_REFLECTGEN_SOURCES}
            COMMENT "reflectgen: ${_header}"
            VERBATIM
        )

        list(APPEND _generated_sources "${_out}")

        # Accumulate for the whole-tree replicable count (see
        # assisi_generate_replicable_limits). Every reflected header in the tree
        # routes through this function, so coverage is automatic rather than a
        # list someone has to remember to maintain.
        set_property(GLOBAL APPEND PROPERTY ASSISI_REFLECTED_HEADERS "${_abs}")
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

    # FieldMeta initializers are emitted positionally and stop at the last member
    # a field actually needs; everything after that is left to FieldMeta's default
    # member initializers, which is deliberate and correct. GCC/Clang still flag it
    # under -Wextra, once per omitted member per field — ~125 warnings across the
    # generated sources, none of them actionable. Suppressed narrowly (not -w) so
    # every other warning still applies to generated code, which is exactly where a
    # codegen bug would surface.
    #
    # Applied per-source, not via target_compile_options: the warning flags arrive
    # through Assisi::Warnings as INTERFACE options, which land *after* a target's
    # own options on the command line, so a target-level -Wno- would be re-enabled
    # by the later -Wextra (clang does exactly that; gcc happened not to). Source
    # file properties are appended last, so they win.
    if (NOT MSVC)
        set_source_files_properties(${_generated_sources}
            TARGET_DIRECTORY "${_obj_target}"
            PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()

    # Register this OBJECT library globally so assisi_link_reflections()
    # can gather all of them.
    set_property(GLOBAL APPEND PROPERTY ASSISI_REFLECT_OBJECT_TARGETS "${_obj_target}")
endfunction()

# Emit the generated header that fixes ComponentMask's width, counted from every
# header that has passed through assisi_reflect().
#
# Call once, from the top level, *after* every add_subdirectory() — the property
# it reads is only complete once every module has registered its headers.
#
# The scan is text-only, so nothing here waits on a compile and no target cycle
# exists even though the scanned headers live in modules that link Core: the
# dependency is on header *files*.
#
# copy_if_different is load-bearing rather than tidiness. Every header edit
# re-runs the scan, but the output only *changes* when the replicable count
# crosses a byte boundary; below that the file is byte-identical and nothing
# downstream rebuilds. Without the guard, editing any reflected header anywhere
# would rebuild everything that includes ComponentMask.
function(assisi_generate_replicable_limits)
    get_property(_headers GLOBAL PROPERTY ASSISI_REFLECTED_HEADERS)
    if (NOT _headers)
        message(FATAL_ERROR
            "assisi_generate_replicable_limits: no reflected headers registered. "
            "Call this after the add_subdirectory() calls that invoke assisi_reflect().")
    endif()

    set(_dir "${CMAKE_BINARY_DIR}/generated/Assisi/Core/Reflect")
    set(_out "${_dir}/ReplicableLimits.hpp")
    set(_tmp "${_out}.tmp")

    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND Python3::Interpreter "${_ASSISI_REFLECTGEN}" ${_headers} --count-replicable "${_tmp}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_tmp}" "${_out}"
        DEPENDS ${_headers} "${_ASSISI_REFLECTGEN}" ${_ASSISI_REFLECTGEN_SOURCES}
        COMMENT "reflectgen: counting replicable components"
        VERBATIM
    )

    add_custom_target(Assisi-ReplicableLimits DEPENDS "${_out}")

    # Core owns the ComponentMask declaration, so it is what must wait for the
    # header and what publishes the include path onward.
    add_dependencies(Assisi-Core Assisi-ReplicableLimits)
    target_include_directories(Assisi-Core PUBLIC "${CMAKE_BINARY_DIR}/generated")
endfunction()

# Call once on each final executable (or shared library) to force-include
# all reflection registration code produced by assisi_reflect() calls.
function(assisi_link_reflections target)
    get_property(_reflect_targets GLOBAL PROPERTY ASSISI_REFLECT_OBJECT_TARGETS)
    foreach(_rt ${_reflect_targets})
        target_sources("${target}" PRIVATE "$<TARGET_OBJECTS:${_rt}>")
    endforeach()
endfunction()
