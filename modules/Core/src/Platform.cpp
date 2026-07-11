/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Platform.cpp

#include <Assisi/Core/Platform.hpp>

#include <Assisi/Core/Logger.hpp>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    pragma comment(lib, "user32.lib")
#    include <string>
#endif

namespace Assisi::Core
{

void ShowErrorDialog([[maybe_unused]] std::string_view title, std::string_view message)
{
    // Log first, unconditionally: the dialog may be dismissed unseen, and on
    // platforms without a native implementation the log line is the whole story.
    Log::Error("{}", message);

#ifdef _WIN32
    // MessageBoxW takes NUL-terminated UTF-16; widen the UTF-8 inputs. An empty
    // view yields an empty string, which is a valid (if blank) argument.
    const auto widen = [](std::string_view text) -> std::wstring
    {
        if (text.empty())
        {
            return std::wstring{};
        }
        const int wide =
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(wide), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), wide);
        return result;
    };

    const std::wstring wideTitle   = widen(title);
    const std::wstring wideMessage = widen(message);
    ::MessageBoxW(nullptr, wideMessage.c_str(), wideTitle.c_str(), MB_OK | MB_ICONERROR);
#endif
}

} // namespace Assisi::Core
