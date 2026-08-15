/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Assert.cpp

#include <Assisi/Core/Assert.hpp>

#ifndef NDEBUG

#    include <Assisi/Core/Logger.hpp>

#    include <cstdlib>

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

    // Log through Core::Log so the failure lands in the log/crash report even in
    // a windowed build with no console, then fail fast.
    Log::Fatal("Contract violation: {}  [{}:{}]  {}", violation.condition, violation.file, violation.line,
               violation.message != nullptr ? violation.message : "");
    std::abort();
}

} // namespace Assisi::Core

#endif // !NDEBUG
