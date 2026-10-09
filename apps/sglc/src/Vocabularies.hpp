/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Vocabularies.hpp
/// @brief The vocabularies sglc can compile files against.

#include <Assisi/Sigil/Compile/Vocabulary.hpp>

#include <vector>

namespace Assisi::Sglc
{

/// @brief Every vocabulary a `use` line may name.
[[nodiscard]] std::vector<Sigil::Compile::Vocabulary> Vocabularies();

} // namespace Assisi::Sglc
