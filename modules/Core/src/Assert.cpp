/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Assert.cpp

#include <Assisi/Core/Assert.hpp>

#ifndef NDEBUG

#    include <Assisi/Core/Abort.hpp>

#    include <cstdint>
#    include <format>

namespace Assisi::Core
{
namespace
{
// The installed handler, or nullptr for the default (log + abort). Plain global:
// this is debug-only and documented as single-threaded test setup, so it needs
// no synchronization.
ContractHandler g_contractHandler = nullptr;
} // namespace

void SetContractHandler(ContractHandler handler)
{
    g_contractHandler = handler;
}

void ReportContractViolation(const ContractViolation &violation)
{
    if (g_contractHandler != nullptr)
    {
        g_contractHandler(violation);
        // A well-behaved handler does not return (it throws or aborts). If one
        // does return, fall through to the default so a violation is never
        // silently ignored — the checked code must not proceed into the UB.
    }

    // Through Abort, so the failure lands in the log and the crash report quotes
    // it, even in a windowed build with no console.
    AbortAt(std::format("Contract violation: {}  {}", violation.condition,
                        violation.message != nullptr ? violation.message : ""),
            violation.file, static_cast<std::uint32_t>(violation.line));
}

} // namespace Assisi::Core

#endif // !NDEBUG
