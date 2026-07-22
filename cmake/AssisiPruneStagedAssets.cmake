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
    # Build outputs staged alongside the assets; not mirrored from the source tree.
    if (_rel MATCHES "\\.spv$")
        continue()
    endif()

    if (NOT EXISTS "${SRC_DIR}/${_rel}")
        file(REMOVE "${DST_DIR}/${_rel}")
        math(EXPR _removed "${_removed} + 1")
        message(STATUS "Pruned stale staged asset: ${_rel}")
    endif()
endforeach()

if (_removed GREATER 0)
    message(STATUS "AssisiPruneStagedAssets: removed ${_removed} stale file(s) from ${DST_DIR}")
endif()
