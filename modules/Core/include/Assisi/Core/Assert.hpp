/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Assert.hpp
/// @brief Fail-fast contract assertion for debug-only invariant checks.
///
/// `ASSISI_ASSERT(cond, msg)` checks a programming invariant in debug builds and
/// compiles to nothing in release (the arguments stay type-checked but are never
/// evaluated). A failure is a *bug*, not a recoverable condition, so the default
/// handler fails fast at the point of the violation.
///
/// Why not the standard `assert()`: in a windowed app its stderr output is
/// invisible, so a failure just vanishes the process. This routes the condition,
/// file, line, and message through Core::Log (Fatal) first — so the violation is
/// recorded in the log/crash report that ships with the abort handler in
/// Application — and only then aborts. It also gives real message ergonomics
/// (`ASSISI_ASSERT(cond, "why")`) instead of the `assert(cond && "why")` hack, so
/// it is meant as the engine-wide invariant check, not a one-off for this module.
///
/// The behavior is a swappable handler (the pattern used by GSL fail_fast, the
/// C++26 contracts MVP, and engine `check`/`ensure` macros). One thing that
/// buys us: tests can install a handler that throws ContractViolation instead of
/// aborting, so the fatal path is verifiable in-process with doctest's
/// CHECK_THROWS_AS — no subprocess, no killed runner. The handler is debug-only
/// global state and is not thread-safe; it is for single-threaded test setup.
///
/// Caveat for the throwing test handler: the throw unwinds through the code that
/// fired the assert. An ASSISI_ASSERT placed in a destructor or a `noexcept`
/// function therefore turns the test handler's throw into std::terminate — such
/// a check still fails fast, but it cannot be exercised with CHECK_THROWS_AS.

#include <cstdint>

namespace Assisi::Core
{

#ifndef NDEBUG

/// @brief Payload describing a failed ASSISI_ASSERT. Thrown by the test handler
/// installed via SetContractHandler; not meant to be caught in production code —
/// a contract violation is a bug, and the default handler aborts.
struct ContractViolation
{
    const char *condition; ///< Stringized failing expression.
    const char *message;   ///< Human-readable detail passed to the macro.
    const char *file;      ///< __FILE__ of the check.
    int32_t line;          ///< __LINE__ of the check.
};

/// @brief Handler invoked when an ASSISI_ASSERT fails. Must not return normally
/// (the throwing test handler unwinds; the default aborts) — returning lets the
/// checked code proceed into the very UB the assert was guarding.
using ContractHandler = void (*)(const ContractViolation &violation);

/// @brief Installs the handler for failed asserts; pass nullptr to restore the
/// default (log + std::abort). Debug-only, not thread-safe — for test setup.
void SetContractHandler(ContractHandler handler);

/// @brief Dispatches a failed assertion to the current handler, or the default
/// abort when none is installed. Called by the ASSISI_ASSERT macro.
void ReportContractViolation(const ContractViolation &violation);

#endif // !NDEBUG

} // namespace Assisi::Core

// NOTE: the macro takes exactly two arguments, so a condition containing an
// unparenthesized comma (e.g. `f<A, B>(x)`) splits into three — wrap such
// conditions in an extra pair of parentheses.
#ifndef NDEBUG
#    define ASSISI_ASSERT(cond, msg)                                                                          \
        ((cond) ? (void)0                                                                                     \
                : ::Assisi::Core::ReportContractViolation(                                                    \
             ::Assisi::Core::ContractViolation{#cond, (msg), __FILE__, __LINE__}))
#else
// Both operands sit under sizeof, so nothing is evaluated and no code is
// emitted — but the expressions are still parsed and type-checked. A typo in
// an assert can't bit-rot until the next debug build, and a variable used only
// by asserts still counts as used (no [[maybe_unused]] needed at call sites).
#    define ASSISI_ASSERT(cond, msg) ((void)sizeof(!(cond)), (void)sizeof(msg))
#endif
