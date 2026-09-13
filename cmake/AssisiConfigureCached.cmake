# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
#
# Configures a list of presets against one shared copy of the dependency
# sources. Run as
#   cmake -DPRESETS="gcc-debug;gcc-dev;gcc-ship" -P cmake/AssisiConfigureCached.cmake
#
# Why this exists: FETCHCONTENT_BASE_DIR is pinned per build tree in
# CMakeLists.txt, so each preset clones the same ~1.7 GB of git history into its
# own _deps. Configuring the three gcc presets pays for that three times, and on
# a slow link it is the entire cost of the target.
#
# The base dir has to stay pinned — the comment beside it in CMakeLists.txt says
# why — but only the *sources* need sharing to fix this. Every preset keeps its
# own <name>-build under its own _deps, so nothing fights over a build directory.
#
# FETCHCONTENT_SOURCE_DIR_<UPPERCASE_NAME> is what makes it possible: FetchContent
# takes the directory as-is, skipping both the download and update steps, and
# generates no subbuild for that dependency at all. It reaches transitive
# dependencies this tree never declares (absl via protobuf, vulkan_headers via
# GameNetworkingSockets) because the variable is resolved by name wherever the
# declaration lives. The name is the declared one uppercased, which is also the
# <name>-src directory uppercased, so the directory listing is enough to derive
# every flag — and it has to be derived, because which dependencies exist is
# config-dependent (assimp, doctest, protobuf and GameNetworkingSockets are all
# behind options).
#
# Two things that look like they would work and do not:
#   - Copying a configured build tree to another preset. CMakeCache.txt records
#     the absolute binary directory, the generated ninja files are full of
#     absolute paths, and CMAKE_BUILD_TYPE differs between the presets.
#   - Copying only the <name>-src directories into another tree. FetchContent's
#     generated *-gitclone.cmake does a REMOVE_RECURSE on the source directory
#     before it clones and has no way to know the copy is already correct, so
#     the copy is deleted and re-cloned regardless.
#
# The cache seeds itself: the first preset to need a dependency clones it as
# normal, the clone is then moved into the cache, and that preset is configured a
# second time so its build files name the cache. Presets behind it find the
# sources already there. A tree never keeps a second copy of something the cache
# already holds — those clones are deleted rather than moved.
#
# The cost: GIT_TAG pins stop being enforced for anything in the cache, because
# CMake never runs a git operation against a directory handed to it this way.
# After bumping a pin, delete the cache (`make clean-deps`) or the bump silently
# does nothing. The cache is also a shared dependency of every tree pointed at
# it, so deleting it breaks all of them until they are configured again.
#
# Do not run two configures against one build tree at once, with or without this.
# Both clone into the same <name>-src and each one's REMOVE_RECURSE eats the
# other's in-progress pack files, which surfaces as `fatal: fetch-pack: invalid
# index-pack output` and repeats identically on all three retries — so it reads
# as a deterministic failure when it is a race.

if (NOT DEFINED PRESETS)
    message(FATAL_ERROR "AssisiConfigureCached: PRESETS is required")
endif()

get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_cache "${_root}/out/_deps-src")
file(MAKE_DIRECTORY "${_cache}")

# The -D flags naming every source the cache currently holds. Recomputed after
# each harvest: a preset that configures first fills the cache for the ones
# behind it.
function(_assisi_cache_flags out_var)
    set(_flags "")
    file(GLOB _dirs LIST_DIRECTORIES true "${_cache}/*-src")
    foreach(_dir IN LISTS _dirs)
        if (NOT IS_DIRECTORY "${_dir}")
            continue()
        endif()
        get_filename_component(_name "${_dir}" NAME)
        string(REGEX REPLACE "-src$" "" _name "${_name}")
        string(TOUPPER "${_name}" _name)
        list(APPEND _flags "-DFETCHCONTENT_SOURCE_DIR_${_name}=${_dir}")
    endforeach()
    set(${out_var} "${_flags}" PARENT_SCOPE)
endfunction()

# Claims for the cache whatever the tree cloned, and drops what the cache already
# has. Reports whether it touched anything, because the tree's generated build
# files still name the sources where they were until it is configured again.
function(_assisi_harvest build_dir out_var)
    set(_changed FALSE)
    file(GLOB _dirs LIST_DIRECTORIES true "${build_dir}/_deps/*-src")
    foreach(_dir IN LISTS _dirs)
        if (NOT IS_DIRECTORY "${_dir}")
            continue()
        endif()
        get_filename_component(_name "${_dir}" NAME)
        if (IS_DIRECTORY "${_cache}/${_name}")
            file(REMOVE_RECURSE "${_dir}")
        else()
            file(RENAME "${_dir}" "${_cache}/${_name}" RESULT _rc)
            if (NOT _rc STREQUAL "0")
                message(FATAL_ERROR
                    "AssisiConfigureCached: could not move ${_dir} into the cache "
                    "(${_rc}). The cache and the build trees have to share a "
                    "filesystem; both live under out/ for that reason.")
            endif()
            message(STATUS "dep cache: took ${_name}")
        endif()
        set(_changed TRUE)
    endforeach()
    set(${out_var} ${_changed} PARENT_SCOPE)
endfunction()

macro(_assisi_configure preset flags)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --preset "${preset}" ${flags}
        WORKING_DIRECTORY "${_root}"
        RESULT_VARIABLE _configure_rc
    )
    if (NOT _configure_rc EQUAL 0)
        message(FATAL_ERROR "AssisiConfigureCached: preset '${preset}' failed (${_configure_rc})")
    endif()
endmacro()

foreach(_preset IN LISTS PRESETS)
    _assisi_cache_flags(_flags)
    list(LENGTH _flags _count)
    message(STATUS "dep cache: configuring ${_preset} against ${_count} shared source(s)")
    _assisi_configure("${_preset}" "${_flags}")

    _assisi_harvest("${_root}/out/build/${_preset}" _changed)
    if (_changed)
        _assisi_cache_flags(_flags)
        _assisi_configure("${_preset}" "${_flags}")
    endif()
endforeach()
