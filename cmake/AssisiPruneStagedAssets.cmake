# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc").
#
# Removes files from the staged asset tree that no longer exist in the source
# tree. Run via `cmake -P` with -DSRC_DIR=... -DDST_DIR=...
#
# Why this exists: `cmake -E copy_directory` only adds and overwrites, so a
# deleted or renamed source asset lingers in the staged copy forever. A stale
# leftover still resolves through AssetSystem, so the app keeps loading an asset
# that no longer exists in the project — and a renamed shader leaves its old
# .spv behind to be picked up instead of the new one.
#
# Why not just wipe and re-copy: the staged tree is 100+ MB, so a full recopy on
# every asset edit is a real iteration cost. It would also delete the compiled
# .spv files, whose copy steps are guarded by `copy_if_different` stamps that
# would still look up to date — the shaders would silently disappear.
#
# Compiled shaders are therefore skipped here: they are build outputs that live
# only in the staged tree and have no source-tree counterpart to compare against.
# Their own custom commands own their lifetime.

if (NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
    message(FATAL_ERROR "AssisiPruneStagedAssets: SRC_DIR and DST_DIR are required")
endif()

if (NOT IS_DIRECTORY "${DST_DIR}")
    return() # nothing staged yet — first build
endif()

file(GLOB_RECURSE _staged LIST_DIRECTORIES false RELATIVE "${DST_DIR}" "${DST_DIR}/*")

set(_removed 0)
foreach(_rel IN LISTS _staged)
    # A compiled shader has no source-tree counterpart of its own, so it cannot be
    # compared directly — but it is stale exactly when the GLSL it was built from
    # is gone. Compare against that instead of exempting .spv outright: renaming or
    # moving a shader is the case that strands an old .spv next to the new one,
    # where it still resolves through AssetSystem and can be loaded instead.
    #
    # The probe is everything up to and including the shader-stage extension, so a
    # variant built from the same GLSL with different defines
    # (shaders/mesh.frag.masked.spv) resolves to its source the same way the plain
    # build does, rather than looking like an orphan and being deleted.
    set(_probe "${_rel}")
    if (_rel MATCHES "\\.(vert|frag|comp)\\..*spv$")
        string(REGEX REPLACE "\\.(vert|frag|comp)\\..*spv$" ".\\1" _probe "${_rel}")
    elseif (_rel MATCHES "\\.spv$")
        string(REGEX REPLACE "\\.spv$" "" _probe "${_rel}")
    endif()

    if (NOT EXISTS "${SRC_DIR}/${_probe}")
        file(REMOVE "${DST_DIR}/${_rel}")
        math(EXPR _removed "${_removed} + 1")
        message(STATUS "Pruned stale staged asset: ${_rel}")
    endif()
endforeach()

if (_removed GREATER 0)
    message(STATUS "AssisiPruneStagedAssets: removed ${_removed} stale file(s) from ${DST_DIR}")
endif()
