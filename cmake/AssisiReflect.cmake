# AssisiReflect.cmake
# Provides the assisi_reflect() function for wiring reflectgen into a module's build.
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
# produced in ${CMAKE_CURRENT_BINARY_DIR}/generated/ and added as a private
# source to TARGET.  The #include path inside each generated file is
# auto-detected from the 'include/' path segment (e.g.
# modules/Runtime/include/Assisi/Runtime/Components.hpp ->
# Assisi/Runtime/Components.hpp).

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

    target_sources("${_ARG_TARGET}" PRIVATE ${_generated_sources})
    # Generated files include <Assisi/ECS/Scene.hpp> — make sure ECS headers
    # are reachable.  (Modules that call assisi_reflect already link ECS.)
endfunction()