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

# Fail the build if two AMSG_HANDLER declarations claim the same message type.
#
# The bindings themselves are emitted per header, into the same OBJECT library
# as that header's component registrations — which is where they belong, because
# a handler must ship with the module that declares it and with nobody else. An
# OBJECT library is never stripped, so linkage is already guaranteed by the same
# mechanism component registration relies on.
#
# What per-header codegen cannot see is the *other* headers, so uniqueness gets
# its own whole-tree pass. Call once, from the top level, after every
# add_subdirectory() — same contract as assisi_generate_replicable_limits(), and
# for the same reason: the header list is only complete then.
# Emit the generated header that gives every AMSG type its compile-time facts —
# direction, reliability, independence — so a send call can refuse the wrong
# direction at compile time rather than dropping a packet at the far end.
#
# Same contract and same copy_if_different reasoning as the replicable count:
# every header edit re-runs the scan, but the output only *changes* when a
# message is added, renamed, moved, or reclassified.
function(assisi_generate_message_traits)
    get_property(_headers GLOBAL PROPERTY ASSISI_REFLECTED_HEADERS)
    if (NOT _headers)
        message(FATAL_ERROR
            "assisi_generate_message_traits: no reflected headers registered. "
            "Call this after the add_subdirectory() calls that invoke assisi_reflect().")
    endif()

    set(_dir "${CMAKE_BINARY_DIR}/generated/Assisi/Core/Reflect")
    set(_out "${_dir}/MessageTraits.hpp")
    set(_tmp "${_out}.tmp")

    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND Python3::Interpreter "${_ASSISI_REFLECTGEN}" ${_headers} --message-traits "${_tmp}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_tmp}" "${_out}"
        DEPENDS ${_headers} "${_ASSISI_REFLECTGEN}" ${_ASSISI_REFLECTGEN_SOURCES}
        COMMENT "reflectgen: message traits"
        VERBATIM
    )

    add_custom_target(Assisi-MessageTraits DEPENDS "${_out}")

    # NetSync's MessageDispatch.hpp includes it, so NetSync is what must wait.
    # The include directory is already published by Assisi-Core (same generated
    # root as ReplicableLimits.hpp).
    add_dependencies(Assisi-NetSync Assisi-MessageTraits)
endfunction()

# Emit the generated InstanceView<T> specializations for the blueprints a project
# opts in, so `SpawnBlueprint<Car>(world, at)` names its members as fields and a
# typo stops being a NullEntity at runtime.
#
#   assisi_generate_instance_views(
#       ASSET_ROOT "${CMAKE_SOURCE_DIR}/assets"
#       BLUEPRINTS "Car=blueprints/car.abp"
#                  "Lot=blueprints/parking_lot.abp")
#
# **An explicit list, not a glob.** This is the one place content reaches into
# the build graph — editing a .abp recompiles every call site — so which files
# have that power is stated rather than inferred from where they happen to sit.
# Adding a blueprint to a project does not slow anyone's build until somebody
# writes it down here.
#
# Unlike the passes above this takes no header list: its inputs are .abp files.
# It also cannot state its own dependencies up front, because a blueprint that
# instances another is only discovered by reading it — hence the depfile, which
# is what makes editing a *nested* file regenerate the view.
function(assisi_generate_instance_views)
    cmake_parse_arguments(ARG "" "ASSET_ROOT" "BLUEPRINTS" ${ARGN})

    if (NOT ARG_ASSET_ROOT)
        message(FATAL_ERROR "assisi_generate_instance_views: ASSET_ROOT is required")
    endif()
    if (NOT ARG_BLUEPRINTS)
        message(FATAL_ERROR
            "assisi_generate_instance_views: BLUEPRINTS is required. Call this only "
            "when a project actually opts a blueprint in.")
    endif()

    set(_args "")
    foreach(_spec ${ARG_BLUEPRINTS})
        if (NOT _spec MATCHES "^[^=]+=[^=]+$")
            message(FATAL_ERROR
                "assisi_generate_instance_views: BLUEPRINTS entries are TypeName=path, got '${_spec}'")
        endif()
        list(APPEND _args --blueprint "${_spec}")
    endforeach()

    set(_dir "${CMAKE_BINARY_DIR}/generated/Assisi/Runtime")
    set(_out "${_dir}/InstanceViews.hpp")
    set(_tmp "${_out}.tmp")
    set(_dep "${CMAKE_BINARY_DIR}/generated/instance_views.d")

    # Same copy_if_different reasoning as the passes above: a blueprint edit that
    # only moves a member re-runs the generator and rebuilds nothing, because the
    # *names* did not change. Only adding, removing or renaming a member touches
    # the file, which is exactly when call sites do need recompiling.
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND Python3::Interpreter "${_ASSISI_REFLECTGEN}"
                --instance-views "${_tmp}" --asset-root "${ARG_ASSET_ROOT}"
                --depfile "${_dep}" ${_args}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_tmp}" "${_out}"
        DEPENDS "${_ASSISI_REFLECTGEN}" ${_ASSISI_REFLECTGEN_SOURCES}
        DEPFILE "${_dep}"
        COMMENT "reflectgen: instance views"
        VERBATIM
    )

    add_custom_target(Assisi-InstanceViews DEPENDS "${_out}")

    # App is what publishes the typed verbs, so it is what must wait. The include
    # directory is already published by Assisi-Core (same generated root as
    # ReplicableLimits.hpp), so nothing else needs to know where this lands.
    add_dependencies(Assisi-App Assisi-InstanceViews)
endfunction()

function(assisi_check_message_handlers)
    get_property(_headers GLOBAL PROPERTY ASSISI_REFLECTED_HEADERS)
    if (NOT _headers)
        message(FATAL_ERROR
            "assisi_check_message_handlers: no reflected headers registered. "
            "Call this after the add_subdirectory() calls that invoke assisi_reflect().")
    endif()

    set(_dir "${CMAKE_BINARY_DIR}/generated/messages")
    set(_out "${_dir}/HandlerMap.txt")
    set(_tmp "${_out}.tmp")

    # The map is written as a file rather than only printed so it is inspectable
    # after the fact: "which handler is bound to this message, and is anything
    # unhandled" is a question worth being able to answer without re-running the
    # build. copy_if_different keeps an unchanged map from touching mtimes.
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND Python3::Interpreter "${_ASSISI_REFLECTGEN}" ${_headers} --check-handlers "${_tmp}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_tmp}" "${_out}"
        DEPENDS ${_headers} "${_ASSISI_REFLECTGEN}" ${_ASSISI_REFLECTGEN_SOURCES}
        COMMENT "reflectgen: checking message handlers"
        VERBATIM
    )

    add_custom_target(Assisi-MessageHandlerCheck ALL DEPENDS "${_out}")
endfunction()

# Call once on each final executable (or shared library) to force-include
# all reflection registration code produced by assisi_reflect() calls.
function(assisi_link_reflections target)
    get_property(_reflect_targets GLOBAL PROPERTY ASSISI_REFLECT_OBJECT_TARGETS)
    foreach(_rt ${_reflect_targets})
        target_sources("${target}" PRIVATE "$<TARGET_OBJECTS:${_rt}>")
    endforeach()
endfunction()
