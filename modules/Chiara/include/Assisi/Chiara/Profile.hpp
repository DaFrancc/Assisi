/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Profile.hpp
/// @brief The instrumentation macros — the only Chiara header most call sites
///        need (design: docs/chiara-design-notes.md §3).
///
/// ```cpp
/// void Application::RenderFrame()
/// {
///     ASSISI_PROFILE_FUNCTION();
///     ASSISI_PROFILE_SCOPE("scene");
///     ASSISI_PROFILE_COUNTER("render/draw-calls", double(drawCalls));
/// }
/// ```
///
/// **Names are aggregation keys, so context belongs in an arg, never in the
/// name.** `ASSISI_PROFILE_SCOPE("publish-mesh")` with the asset path attached
/// via ASSISI_PROFILE_ARG_STR aggregates across frames; folding the path into
/// the name shatters that into thousands of one-shot buckets. It is also what
/// makes "click a slice, see which asset" work in the viewer with no tooling of
/// ours.
///
/// In a default build (Chiara off) the macros expand to the
/// `((void)sizeof(...))` pattern Assert.hpp uses: the arguments are parsed and
/// type-checked but never evaluated, and no code is emitted. Instrumentation
/// cannot bit-rot until someone next builds with `-c`, and a variable used only
/// by a profile macro still counts as used.

#include <Assisi/Chiara/Chiara.hpp>

#define ASSISI_CHIARA_CONCAT_INNER(a, b) a ## b
#define ASSISI_CHIARA_CONCAT(a, b) ASSISI_CHIARA_CONCAT_INNER(a, b)
#define ASSISI_CHIARA_UNIQUE(prefix) ASSISI_CHIARA_CONCAT(prefix, __LINE__)

#if defined(ASSISI_CHIARA_ENABLED)

/// @brief Times the enclosing block. `name` must outlive the capture — a string
/// literal, or a pointer from Chiara::InternString.
#    define ASSISI_PROFILE_SCOPE(name)                                                                          \
        const ::Assisi::Chiara::ScopeTimer ASSISI_CHIARA_UNIQUE(chiaraScope_)                                   \
        {                                                                                                       \
            (name)                                                                                              \
        }

/// @brief ASSISI_PROFILE_SCOPE named after the enclosing function.
#    define ASSISI_PROFILE_FUNCTION() ASSISI_PROFILE_SCOPE(__func__)

/// @brief Attaches a string to the innermost open scope. Interns `value`, which
/// takes a lock — fine for the rare, contextful event it is meant for.
#    define ASSISI_PROFILE_ARG_STR(key, value) ::Assisi::Chiara::EmitArgString((key), (value))

/// @brief Attaches a number to the innermost open scope.
#    define ASSISI_PROFILE_ARG_U64(key, value) ::Assisi::Chiara::EmitArgU64((key), (value))

/// @brief Samples a value onto its own track. Name as `group/name` so tracks
/// group in the viewer.
#    define ASSISI_PROFILE_COUNTER(name, value) ::Assisi::Chiara::EmitCounter((name), (value))

/// @brief Marks a cost that will be paid later — see §7. Pair with
/// ASSISI_PROFILE_FLOW_END at the effect, using the same id from NewFlowId().
#    define ASSISI_PROFILE_FLOW_BEGIN(name, flowId) ::Assisi::Chiara::EmitFlowBegin((name), (flowId))

#    define ASSISI_PROFILE_FLOW_END(name, flowId) ::Assisi::Chiara::EmitFlowEnd((name), (flowId))

/// @brief Frame boundary. Main thread only; Chiara owns the frame index.
#    define ASSISI_PROFILE_FRAME() ((void)::Assisi::Chiara::MarkFrame())

#else // !ASSISI_CHIARA_ENABLED

#    define ASSISI_PROFILE_SCOPE(name) ((void)sizeof(name))
#    define ASSISI_PROFILE_FUNCTION() ((void)0)
#    define ASSISI_PROFILE_ARG_STR(key, value) ((void)sizeof(key), (void)sizeof(value))
#    define ASSISI_PROFILE_ARG_U64(key, value) ((void)sizeof(key), (void)sizeof(value))
#    define ASSISI_PROFILE_COUNTER(name, value) ((void)sizeof(name), (void)sizeof(value))
#    define ASSISI_PROFILE_FLOW_BEGIN(name, flowId) ((void)sizeof(name), (void)sizeof(flowId))
#    define ASSISI_PROFILE_FLOW_END(name, flowId) ((void)sizeof(name), (void)sizeof(flowId))
#    define ASSISI_PROFILE_FRAME() ((void)0)

#endif // ASSISI_CHIARA_ENABLED
