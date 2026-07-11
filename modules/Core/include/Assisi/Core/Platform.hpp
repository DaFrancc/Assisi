/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Platform.hpp
/// @brief Small OS-specific helpers that don't warrant a module of their own.

#include <string_view>

namespace Assisi::Core
{

/// @brief Shows a native, modal error dialog with an OK button and blocks until
/// it is dismissed.
///
/// For fatal, user-facing failures — typically at startup, where the process is
/// about to exit and the log alone would go unseen (e.g. no Vulkan-capable GPU).
/// The message is always logged at error level too, so it is captured even when
/// the dialog is dismissed or a platform has no native implementation (there it
/// degrades to just the log line). Text is UTF-8.
void ShowErrorDialog(std::string_view title, std::string_view message);

} // namespace Assisi::Core
