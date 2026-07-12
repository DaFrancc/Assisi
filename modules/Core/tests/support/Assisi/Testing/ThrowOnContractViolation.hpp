/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ThrowOnContractViolation.hpp
/// @brief Test-only RAII guard that turns a fired ASSISI_ASSERT into a throw.
///
/// Shared by every module test suite that exercises contract guards. Debug-only,
/// like the contract-handler API itself — guard uses with `#ifndef NDEBUG`
/// alongside the test cases that need it.

#include <Assisi/Core/Assert.hpp>

#ifndef NDEBUG

namespace Assisi::Testing
{

/// Installs a contract handler that throws the violation so a fired
/// ASSISI_ASSERT is catchable in-process (CHECK_THROWS_AS) instead of aborting
/// the test runner; restores the default (log + abort) on scope exit.
struct ThrowOnContractViolation
{
    ThrowOnContractViolation()
    {
        Core::SetContractHandler([](const Core::ContractViolation &violation) { throw violation; });
    }
    ~ThrowOnContractViolation() { Core::SetContractHandler(nullptr); }

    ThrowOnContractViolation(const ThrowOnContractViolation &) = delete;
    ThrowOnContractViolation &operator=(const ThrowOnContractViolation &) = delete;
};

} // namespace Assisi::Testing

#endif // !NDEBUG
