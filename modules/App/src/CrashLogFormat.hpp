/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CrashLogFormat.hpp
/// @brief The headings of a crash log's sections, shared by the crash handler
///        that writes them and the reporter that reads them back.

#include <string_view>

namespace Assisi::App::CrashLog
{

/// Opens the frames: one raw address a line, innermost first.
inline constexpr std::string_view kFramesHeading = "Frames:";

/// Opens the module map, /proc/self/maps as the crash found it.
inline constexpr std::string_view kModulesHeading = "Modules:";

/// Opens the frames the reporter named, which it writes above the raw ones.
inline constexpr std::string_view kStackHeading = "Stack:";

/// The line holding the instruction that faulted, which the frames above it in
/// a backtrace are the handlers reporting.
inline constexpr std::string_view kFaultingInstructionLabel = "Faulting instruction: ";

/// The line holding Core::Abort's reason, when it was an Abort.
inline constexpr std::string_view kReasonLabel = "Reason: ";

/// The line holding the signal's name.
inline constexpr std::string_view kSignalLabel = "Signal: ";

} // namespace Assisi::App::CrashLog
