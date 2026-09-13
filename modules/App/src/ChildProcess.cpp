/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/ChildProcess.hpp>

#include <Assisi/Core/Logger.hpp>

#include <chrono>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace Assisi::App
{

ChildProcess::ChildProcess(ChildProcess &&other) noexcept : _pid(std::exchange(other._pid, 0)) {}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept
{
    if (this != &other)
    {
        Terminate();
        _pid = std::exchange(other._pid, 0);
    }
    return *this;
}

ChildProcess::~ChildProcess() { Terminate(); }

#if defined(_WIN32)

bool ChildProcess::Spawn(const std::filesystem::path &executable, const std::vector<std::string> &,
                         const std::vector<std::string> &, const std::filesystem::path &)
{
    // Deliberately unimplemented rather than approximated: the networking chain
    // this exists to serve has never built here,
    // so a Windows path would be untested code guarding an untested feature.
    Core::Log::Error("ChildProcess: spawning '{}' is not implemented on Windows.", executable.string());
    return false;
}

bool ChildProcess::IsRunning() { return false; }

void ChildProcess::Terminate(double) { _pid = 0; }

#else

bool ChildProcess::Spawn(const std::filesystem::path &executable, const std::vector<std::string> &args,
                         const std::vector<std::string> &environment,
                         const std::filesystem::path &workingDirectory)
{
    Terminate();

    // Check before forking. After a fork the child's exec failure is invisible
    // to us — it exits 127 and Spawn has already reported success — so an editor
    // that mistyped a path would believe it launched three clients and watch
    // none appear. The residual race (the file vanishing between here and the
    // exec) is not worth closing; the case this catches is a wrong path, which
    // does not move.
    if (::access(executable.c_str(), X_OK) != 0)
    {
        Core::Log::Error("ChildProcess: '{}' is not an executable file: {}", executable.string(),
                         std::strerror(errno));
        return false;
    }

    // Everything the child needs is built *before* the fork. After it, only
    // async-signal-safe calls are legal in the child, which rules out every
    // allocation — and building an argv there is nothing but allocation.
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(executable.string());
    owned.insert(owned.end(), args.begin(), args.end());

    std::vector<char *> argv;
    argv.reserve(owned.size() + 1);
    for (std::string &arg : owned)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    std::vector<std::string> ownedEnv = environment;
    std::vector<char *>      envp;
    envp.reserve(ownedEnv.size() + 1);
    for (std::string &entry : ownedEnv)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    const std::string cwd = workingDirectory.empty() ? std::string{} : workingDirectory.string();

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        Core::Log::Error("ChildProcess: fork failed for '{}': {}", executable.string(), std::strerror(errno));
        return false;
    }

    if (pid == 0)
    {
        // --- child ---------------------------------------------------------
        // First, before anything that can take time. Until execv replaces the
        // image the child still carries the *parent's* signal handlers, and a
        // signal arriving in that window runs the parent's handler in the child
        // — with the parent's atexit chain behind it. Terminate() sends SIGTERM
        // and can easily beat exec: the window is short, but a sanitized build
        // widens it enough for the test suite to hit it every run, where doctest's
        // own handler reports a crash from a process that was never a test runner.
        // An editor spawning and immediately closing a play-in-editor client is
        // the same shape.
        for (int sig = 1; sig < NSIG; ++sig)
            ::signal(sig, SIG_DFL); // EINVAL on the unmaskable ones; nothing to do about those

#if defined(__linux__)
        // The one case the parent cannot clean up from its own side. Without
        // this, an editor that crashes (or is killed with -9) leaves its viewer
        // windows on screen, connected to a server that no longer exists.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        // The parent could already be gone between the fork and the prctl, in
        // which case the signal we just asked for will never be sent.
        if (::getppid() == 1)
            ::_exit(127);
#endif
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
            ::_exit(127);

        for (char *entry : envp)
        {
            if (entry != nullptr)
                ::putenv(entry); // inherits the rest of the environment
        }

        ::execv(executable.c_str(), argv.data());
        ::_exit(127); // execv only returns on failure
    }

    // --- parent -------------------------------------------------------------
    _pid = pid;
    Core::Log::Info("ChildProcess: spawned '{}' (pid {}).", executable.filename().string(), _pid);
    return true;
}

bool ChildProcess::IsRunning()
{
    if (_pid <= 0)
        return false;

    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(_pid), &status, WNOHANG);
    if (result == 0)
        return true; // still going

    // Either it exited (and we just reaped it) or it was never ours to wait on.
    // Both mean the same thing to a caller, and both must clear the handle so
    // Terminate does not later signal a pid the system has recycled.
    _pid = 0;
    return false;
}

void ChildProcess::Terminate(double graceSeconds)
{
    if (_pid <= 0)
        return;

    const pid_t pid = static_cast<pid_t>(_pid);

    // Ask first: a Vulkan client that is killed outright leaves its device in
    // whatever state the driver felt like.
    ::kill(pid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(graceSeconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) != 0)
        {
            _pid = 0;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    // Out of patience. Insist, then collect — a SIGKILLed child is still a
    // zombie until someone waits on it.
    Core::Log::Warn("ChildProcess: pid {} did not exit within {:.1f}s; killing.", _pid, graceSeconds);
    ::kill(pid, SIGKILL);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    _pid = 0;
}

#endif

} // namespace Assisi::App
