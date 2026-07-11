/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Platform.cpp

#include <Assisi/Core/Platform.hpp>

#include <Assisi/Core/Logger.hpp>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    pragma comment(lib, "user32.lib")
#    include <string>
#elif defined(__linux__)
#    include <sys/wait.h>
#    include <unistd.h>

#    include <cstdlib>
#    include <string>
#    include <string_view>
#    include <vector>
#endif
// Any other platform (e.g. macOS) has no native path here and degrades to the
// log line in ShowErrorDialog — see the trailing fallback there.

namespace Assisi::Core
{

#if defined(__linux__)
namespace
{

// Runs a dialog program with the given argv (argv[0] is the executable name,
// resolved via PATH), blocking until it exits. Returns true if the program was
// found and ran — i.e. a dialog was shown — and false only if it could not be
// launched (not installed). Uses fork + execvp with the message passed as a
// single argument, so there is no shell involved and no need to escape the text.
bool RunDialogProcess(const std::vector<std::string> &argv)
{
    // Build the child argv before forking: between fork() and execvp() only
    // async-signal-safe calls are allowed, so no allocation happens there.
    std::vector<char *> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string &arg : argv)
    {
        cargv.push_back(const_cast<char *>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
    {
        return false; // fork failed
    }
    if (pid == 0)
    {
        execvp(cargv[0], cargv.data());
        _exit(127); // exec failed → tool not installed (127 = conventional "not found")
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }
    // A 127 exit is our not-found sentinel; any other exit means the tool ran
    // and showed the dialog (the user may have OK'd or closed it — either way
    // it was displayed, so we stop trying alternatives).
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 127);
}

} // namespace
#endif // __linux__

void ShowErrorDialog([[maybe_unused]] std::string_view title, std::string_view message)
{
    // Log first, unconditionally: the dialog may be dismissed unseen, and if no
    // native dialog mechanism is available the log line is the whole story.
    Log::Error("{}", message);

#if defined(_WIN32)
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
#elif defined(__linux__)
    // No portable native dialog on Linux, so shell out to the desktop's
    // tool: zenity (GTK/GNOME), kdialog (KDE), or xmessage (bare X11 fallback,
    // almost always present). Prefer the one matching the current desktop, then
    // fall through the rest; if none is installed, the log line above stands.
    const std::string titleStr(title);
    const std::string messageStr(message);

    const auto tryZenity = [&]
    { return RunDialogProcess({"zenity", "--error", "--title", titleStr, "--text", messageStr}); };
    const auto tryKdialog = [&]
    { return RunDialogProcess({"kdialog", "--error", messageStr, "--title", titleStr}); };
    const auto tryXmessage = [&]
    { return RunDialogProcess({"xmessage", "-center", titleStr + "\n\n" + messageStr}); };

    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const bool  preferKde =
        desktop != nullptr && std::string_view(desktop).find("KDE") != std::string_view::npos;

    if (preferKde)
    {
        if (tryKdialog() || tryZenity() || tryXmessage())
            return;
    }
    else
    {
        if (tryZenity() || tryKdialog() || tryXmessage())
            return;
    }
#endif
}

} // namespace Assisi::Core
