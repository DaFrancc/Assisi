# Configure-time assertion that a target's link closure excludes named libraries.
#
# The companion to the symbol scan, and the half that works everywhere. A scan
# reads a built binary, so it needs a symbol table: a Release link strips one
# away entirely, and MSVC's nm equivalent is a different tool with a different
# output. This reads what CMake already knows instead, so it holds on every
# preset and every platform, and it fails at configure time rather than after a
# build.
#
# Neither check replaces the other. This one proves the link graph is right; the
# scan proves the binary that came out of it is. A link that is correct while the
# binary still carries the symbols means something upstream is pulling them in by
# another route, which is exactly the case worth catching.

# Resolves what `target` links, transitively, into `out_var`.
#
# Walks LINK_LIBRARIES on the target itself and INTERFACE_LINK_LIBRARIES on
# everything it reaches, following ALIASED_TARGET so that `Assisi::Editor` and
# `Assisi-Editor` are recognised as one thing. Visited names accumulate in
# `out_var`, which doubles as the cycle guard.
#
# Generator expressions this cannot evaluate are collected verbatim into
# `unresolved_var` rather than dropped, so the caller can decide whether any of
# them could have hidden what it is looking for.
function(_assisi_collect_link_closure target out_var unresolved_var)
    set(_seen "${${out_var}}")
    set(_unresolved "${${unresolved_var}}")
    if (target IN_LIST _seen)
        return()
    endif()
    list(APPEND _seen "${target}")

    if (NOT TARGET "${target}")
        # A plain library name (`m`, `dl`) or a path. It links nothing further of
        # ours, so the name itself is the whole contribution.
        set("${out_var}" "${_seen}" PARENT_SCOPE)
        return()
    endif()

    get_target_property(_alias "${target}" ALIASED_TARGET)
    if (_alias)
        set(_closure "${_seen}")
        _assisi_collect_link_closure("${_alias}" _closure _unresolved)
        set("${out_var}" "${_closure}" PARENT_SCOPE)
        set("${unresolved_var}" "${_unresolved}" PARENT_SCOPE)
        return()
    endif()

    set(_deps "")
    get_target_property(_link "${target}" LINK_LIBRARIES)
    if (_link)
        list(APPEND _deps ${_link})
    endif()
    get_target_property(_interface "${target}" INTERFACE_LINK_LIBRARIES)
    if (_interface)
        list(APPEND _deps ${_interface})
    endif()

    set(_closure "${_seen}")
    foreach(_dep IN LISTS _deps)
        # $<LINK_ONLY:X> is what PRIVATE linkage looks like on an INTERFACE
        # property; the payload still links, so unwrap rather than skip.
        if (_dep MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_dep "${CMAKE_MATCH_1}")
        endif()
        # Any other generator expression cannot be evaluated at configure time.
        # Kept rather than dropped, so the caller can say whether it mattered: a
        # silent gap in this walk would be a guard reporting a result it never
        # established. In practice these are third-party linker flags, which name
        # no target at all.
        if (_dep MATCHES "\\$<")
            list(APPEND _unresolved "${_dep}")
            continue()
        endif()
        _assisi_collect_link_closure("${_dep}" _closure _unresolved)
    endforeach()

    set("${out_var}" "${_closure}" PARENT_SCOPE)
    set("${unresolved_var}" "${_unresolved}" PARENT_SCOPE)
endfunction()

# Fails the configure if `target` links any of the remaining arguments,
# transitively.
#
# Names are matched as written, so pass whichever spelling the link uses; an
# alias and the target behind it both resolve, because the walk follows
# ALIASED_TARGET.
function(assisi_assert_link_excludes target)
    set(_closure "")
    set(_unresolved "")
    _assisi_collect_link_closure("${target}" _closure _unresolved)

    set(_found "")
    foreach(_forbidden IN LISTS ARGN)
        if (_forbidden IN_LIST _closure)
            list(APPEND _found "${_forbidden}")
        endif()
        # A generator expression the walk could not evaluate only matters if the
        # thing being excluded could be inside it. Warning about every one of them
        # would bury this in third-party linker flags, and a warning nobody reads
        # is the same as no warning.
        foreach(_genex IN LISTS _unresolved)
            if (_genex MATCHES "${_forbidden}")
                message(WARNING
                    "assisi_assert_link_excludes: '${target}' links through "
                    "${_genex}, which this check cannot evaluate and which names "
                    "'${_forbidden}'. That exclusion is unverified.")
            endif()
        endforeach()
    endforeach()

    if (_found)
        string(REPLACE ";" ", " _found_text "${_found}")
        message(FATAL_ERROR
            "${target} must not link: ${_found_text}.\n"
            "Something in its dependencies pulls one of those in. Follow the "
            "target_link_libraries chain from ${target} and cut the edge rather "
            "than excluding the symbol downstream.")
    endif()
endfunction()
