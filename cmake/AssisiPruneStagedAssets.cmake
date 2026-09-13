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
#
# So is anything the engine writes beside a staged file — an .aast sidecar for a
# compiled shader is generated on first load and has no source of its own. Those
# are judged by the file they describe rather than by themselves, because asking
# the direct question deletes them all on every build for the app to rebuild on
# every launch, and a prune that removes only files it regenerates is pure churn.
#
# What this still deletes, and should not: assets the importer generates into the
# staged tree with no source counterpart at all, such as the materials a glTF
# import derives. Those are a question about where importer output belongs rather
# than about this script, and they are left alone here deliberately.

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

    # A sidecar describes the file beside it, so it is stale exactly when that
    # file is — including when that file is itself a build output with no source
    # of its own. Strip the suffix first and let the rest of the rule judge what
    # is left, rather than asking whether the sidecar has a source counterpart:
    # one generated next to a compiled shader never does, so that question
    # deleted every one of them on every build and the app rebuilt them on every
    # launch.
    if (_probe MATCHES "\\.aast$")
        string(REGEX REPLACE "\\.aast$" "" _probe "${_probe}")
    endif()

    if (_probe MATCHES "\\.(vert|frag|comp)\\..*spv$")
        string(REGEX REPLACE "\\.(vert|frag|comp)\\..*spv$" ".\\1" _probe "${_probe}")
    elseif (_probe MATCHES "\\.spv$")
        string(REGEX REPLACE "\\.spv$" "" _probe "${_probe}")
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
