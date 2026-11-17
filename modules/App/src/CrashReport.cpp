/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    pragma comment(lib, "dbghelp.lib")
#else
#    include <execinfo.h>
#    include <fcntl.h>
#    include <sys/syscall.h>
#    include <ucontext.h>
#    include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "CrashLogFormat.hpp"

#include <Assisi/App/CrashReport.hpp>
#include <Assisi/Core/Abort.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#if defined(ASSISI_CRASHPAD)
#    include <client/crashpad_client.h>
#endif

namespace Assisi::App
{
namespace
{

// A handler cannot report that it failed to open its own output file — it runs
// in signal context, where warning is not an option, so the failure would be
// indistinguishable from never having crashed. Find out now, while saying
// something is still possible. Probes by creating a file rather than reading
// directory permissions, which lie on network and container mounts.
bool ProbeWritable(const std::filesystem::path &path) noexcept
{
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);

    const bool writable = std::ofstream(path, std::ios::app).is_open();
    if (!existed)
    {
        std::filesystem::remove(path, ec);
    }

    if (!writable)
    {
        Core::Log::Warn("Crash reports cannot be written to {} - this run will produce none.", path.string());
    }
    return writable;
}

} // namespace

std::string CrashFileStem(std::string_view launchStamp)
{
    return std::string("crash-").append(launchStamp);
}

// ---------------------------------------------------------------------------
// Windows: minidump
// ---------------------------------------------------------------------------
#ifdef _WIN32

namespace
{

// The dump path as plain bytes: the handler must not allocate to name its own
// output file.
std::array<char, 1024> gReportPath{};
bool gReportPathSet = false;

// MiniDumpNormal alone is thread stacks: a call stack and no way to see what
// anything held. These four cost a few MB instead of a few hundred KB.
//   DataSegs                    — globals and statics
//   IndirectlyReferencedMemory  — the memory the stacks point at
//   ThreadInfo                  — thread times and contexts
//   UnloadedModules             — a DLL unloaded under a callback
constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
    MiniDumpWithUnloadedModules);

// `info` is null on the abort path; MiniDumpWriteDump then describes every
// thread without singling one out as faulting.
bool WriteDump(EXCEPTION_POINTERS *info) noexcept
{
    if (!gReportPathSet)
    {
        return false;
    }

    const HANDLE file =
        CreateFileA(gReportPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    MINIDUMP_EXCEPTION_INFORMATION *meiPtr = nullptr;
    if (info != nullptr)
    {
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;
        meiPtr                = &mei;
    }

    const bool ok =
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kDumpType, meiPtr, nullptr, nullptr) != FALSE;
    CloseHandle(file);
    return ok;
}

LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS *info)
{
    // Dump first: Log::Fatal formats and allocates, which can re-fault on the
    // corrupted heap that may be why we are here. WriteDump is Win32 only and
    // touches no CRT heap.
    const bool dumpWritten = WriteDump(info);

    const DWORD code = info->ExceptionRecord->ExceptionCode;

    const char *name = "UNKNOWN";
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:    name = "ACCESS_VIOLATION";    break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: name = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW:      name = "STACK_OVERFLOW";      break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  name = "INT_DIVIDE_BY_ZERO";  break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:  name = "FLT_DIVIDE_BY_ZERO";  break;
    case EXCEPTION_IN_PAGE_ERROR:       name = "IN_PAGE_ERROR";       break;
    default:                            break;
    }

    Core::Log::Fatal("Crash: unhandled exception 0x{:08X} ({})", static_cast<uint32_t>(code), name);
    if (dumpWritten)
    {
        Core::Log::Fatal("Crash: minidump written to {}", gReportPath.data());
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

void AbortHandler(int)
{
    // Every failed ASSISI_ASSERT, Core::Abort and std::terminate lands here, so
    // it gets a dump on the same terms as any other crash.
    const bool dumpWritten = WriteDump(nullptr);

    Core::Log::Fatal("Crash: abort() called (Core::Abort, assertion failure or std::terminate).");
    if (dumpWritten)
    {
        Core::Log::Fatal("Crash: minidump written to {}", gReportPath.data());
    }
}

} // namespace

void InstallCrashHandlers(const std::filesystem::path &base, const std::filesystem::path & /*dumps*/) noexcept
{
    std::filesystem::path dump = base;
    dump += kCrashDumpExtension;
    const std::string text = dump.string();
    if (text.size() < gReportPath.size())
    {
        std::memcpy(gReportPath.data(), text.c_str(), text.size() + 1);
        gReportPathSet = ProbeWritable(dump);
    }
    else
    {
        Core::Log::Warn("Crash report path is too long ({} bytes) - dumps disabled.", text.size());
    }

    SetUnhandledExceptionFilter(ExceptionFilter);
    std::signal(SIGABRT, AbortHandler);
}

void SetCrashReporter(std::span<const std::string> /*command*/)
{
}

// ---------------------------------------------------------------------------
// Linux: crash log, minidump through crashpad_handler, and the reporter
// ---------------------------------------------------------------------------
#else

namespace
{

// The crash log's path as plain bytes: the handler must not allocate to name its
// own output file.
std::array<char, 1024> gLogPath{};
bool gLogPathSet = false;

// A stack-overflow SIGSEGV arrives with no usable stack, so the handler needs
// its own or it faults on entry. Crashpad sets up its own when it is running.
alignas(16) std::array<char, 256 * 1024> gSignalStack{};

constexpr std::size_t kFrameCapacity = 64;
std::array<void *, kFrameCapacity> gFrames{};

// Read in /proc/self/maps-sized pieces. On the handler's stack, which is the
// alternate one above or Crashpad's, both far larger.
constexpr std::size_t kCopyChunk = 4096;

// First thread to take a fatal signal wins. Otherwise two concurrent faults
// both O_TRUNC the path and the second wipes the first's complete log.
// Crashpad makes the same guarantee when it is running; this covers the
// handler below for when it is not.
std::atomic_flag gLogClaimed = ATOMIC_FLAG_INIT;

// The reporter's command as SetCrashReporter laid it out: the strings, and the
// null-terminated array of pointers into them that execv takes. Set before
// anything could crash and never changed while something could.
std::vector<std::string> gReporterStrings;
std::vector<char *> gReporterArgv;

// write(2) is async-signal-safe; std::format, iostreams and malloc are not.
// EINTR is retried — returning on it silently truncates the log.
void WriteAll(int fileDesc, const char *data, size_t size) noexcept
{
    while (size > 0)
    {
        const ssize_t written = write(fileDesc, data, size);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return;
        }
        if (written == 0)
        {
            return;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
}

// Composed here, then written in one syscall. Every write() between open() and
// the last byte is a chance to die holding a zero-byte file, which reads as a
// broken feature rather than a dead process. It also lets the unwind happen
// before the file exists. Bounded and truncating — this code runs when memory
// is already suspect.
struct Report
{
    std::array<char, 8192> data{};
    size_t used = 0;

    void Str(const char *text) noexcept
    {
        Raw(text, std::strlen(text));
    }

    void Str(std::string_view text) noexcept
    {
        Raw(text.data(), text.size());
    }

    // Hand-rolled because snprintf is not async-signal-safe.
    void Hex(uintptr_t value) noexcept
    {
        char buffer[2 + (sizeof(uintptr_t) * 2)];
        const char *digits = "0123456789abcdef";
        buffer[0]          = '0';
        buffer[1]          = 'x';
        for (size_t i = 0; i < sizeof(uintptr_t) * 2; ++i)
        {
            const size_t shift = ((sizeof(uintptr_t) * 2) - 1 - i) * 4;
            buffer[2 + i]      = digits[(value >> shift) & 0xF];
        }
        Raw(buffer, sizeof(buffer));
    }

    void Dec(int64_t value) noexcept
    {
        char buffer[24];
        size_t index     = sizeof(buffer);
        bool negative  = value < 0;
        uint64_t magnitude = negative ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);
        do
        {
            buffer[--index] = static_cast<char>('0' + (magnitude % 10));
            magnitude /= 10;
        }
        while (magnitude != 0 && index > 0);
        if (negative && index > 0)
        {
            buffer[--index] = '-';
        }
        Raw(buffer + index, sizeof(buffer) - index);
    }

    void Raw(const char *bytes, size_t size) noexcept
    {
        const size_t room = data.size() - used;
        const size_t take = size < room ? size : room;
        std::memcpy(data.data() + used, bytes, take);
        used += take;
    }
};

// Static, not on the possibly-exhausted stack. Trivially initialized, so it is
// ready before main().
Report gReport;

const char *SignalName(int signal) noexcept
{
    switch (signal)
    {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS:  return "SIGBUS (bus error)";
    case SIGFPE:  return "SIGFPE (arithmetic exception)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort / assertion failure / terminate)";
    case SIGTRAP: return "SIGTRAP (trap)";
    case SIGSYS:  return "SIGSYS (bad system call)";
    case SIGQUIT: return "SIGQUIT (quit)";
    default:      return "unknown signal";
    }
}

// The instruction that faulted, from the registers the kernel saved. Every frame
// a backtrace finds above it belongs to the handlers reporting the fault.
uintptr_t FaultingInstruction(const ucontext_t *context) noexcept
{
    if (context == nullptr)
    {
        return 0;
    }
#if defined(__x86_64__)
    return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<uintptr_t>(context->uc_mcontext.pc);
#else
    return 0;
#endif
}

// Appends /proc/self/maps: which file each address is in and where that file
// was loaded, without which the frames mean nothing under ASLR. Only open, read
// and write, all three async-signal-safe.
void CopyModuleMap(int fileDesc) noexcept
{
    int maps = -1;
    do
    {
        maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    }
    while (maps < 0 && errno == EINTR);
    if (maps < 0)
    {
        return;
    }

    std::array<char, kCopyChunk> chunk;
    for (;;)
    {
        const ssize_t got = read(maps, chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got <= 0)
        {
            break;
        }
        WriteAll(fileDesc, chunk.data(), static_cast<size_t>(got));
    }
    close(maps);
}

// Writes the crash log for @p signal: why, the frames, and the module map.
void WriteCrashLog(int signal, const siginfo_t *info, const ucontext_t *context) noexcept
{
    // Unwind before the file exists, so the riskiest call is not sitting
    // between open() and first write. backtrace() allocates on its first call,
    // which is why InstallCrashHandlers warms it up.
    const int frames = backtrace(gFrames.data(), static_cast<int>(gFrames.size()));

    gReport.used = 0;
    gReport.Str("Assisi crash report\n===================\n\n");

    const char *reason = Core::AbortReason();
    if (reason[0] != '\0')
    {
        gReport.Str(CrashLog::kReasonLabel);
        gReport.Str(reason);
        gReport.Str("\nCalled from: ");
        gReport.Str(Core::AbortLocation());
        gReport.Str("\n");
    }

    gReport.Str(CrashLog::kSignalLabel);
    gReport.Str(SignalName(signal));
    gReport.Str(" (");
    gReport.Dec(signal);
    gReport.Str(")\n");

    // si_addr only means a fault address for a hardware-raised signal. For a
    // kill/abort (si_code <= 0) that union member carries si_pid/si_uid, and
    // printing it produces a plausible-looking pointer that is really two
    // spliced integers — worst on SIGABRT, which is where every Core::Abort and
    // failed ASSISI_ASSERT lands.
    if (info != nullptr && info->si_code > 0)
    {
        gReport.Str("Fault address: ");
        gReport.Hex(reinterpret_cast<uintptr_t>(info->si_addr));
        gReport.Str("\n");
    }
    else
    {
        gReport.Str("Fault address: n/a (signal was raised, not a hardware fault)\n");
    }

    gReport.Str("si_code: ");
    gReport.Dec(info != nullptr ? info->si_code : 0);
    gReport.Str("\n");
    gReport.Str(CrashLog::kFaultingInstructionLabel);
    gReport.Hex(FaultingInstruction(context));

    // Raw addresses, because producing them takes no lock. To name one by hand,
    // find the module it falls in below and give addr2line its distance from
    // where that module starts: addr2line -e <module> -f -C <offset>.
    gReport.Str("\n\n");
    gReport.Str(CrashLog::kFramesHeading);
    gReport.Str("\n");
    for (int i = 0; i < frames; ++i)
    {
        gReport.Str("  ");
        gReport.Hex(reinterpret_cast<uintptr_t>(gFrames[static_cast<size_t>(i)]));
        gReport.Str("\n");
    }
    gReport.Str("\n");
    gReport.Str(CrashLog::kModulesHeading);
    gReport.Str("\n");

    int fd = -1;
    do
    {
        fd = open(gLogPath.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    while (fd < 0 && errno == EINTR);

    if (fd >= 0)
    {
        WriteAll(fd, gReport.data.data(), gReport.used);
        CopyModuleMap(fd);
        close(fd);
    }
}

// Starts the reporter, and leaves it running when this process dies.
void StartReporter() noexcept
{
    if (gReporterArgv.empty())
    {
        return;
    }

    // Not fork(): its atfork handlers take malloc's locks, which the thread that
    // crashed may be holding. A bare clone copies the process as fork does and
    // runs none of them, and all the copy does is replace itself.
    // Every argument a long: syscall() reads its arguments as longs.
    const long child = syscall(SYS_clone, static_cast<long>(SIGCHLD), 0L, 0L, 0L, 0L);
    if (child != 0)
    {
        return;
    }

#if defined(SYS_close_range)
    // Nothing this process had open reaches the reporter: a socket it still held
    // would keep the game's port taken while the window is up.
    constexpr unsigned long kFirstUnsharedDescriptor = 3; // after stdin, stdout and stderr
    constexpr unsigned long kLastDescriptor = ~0U;
    (void)syscall(SYS_close_range, kFirstUnsharedDescriptor, kLastDescriptor, 0UL);
#endif
    execv(gReporterArgv[0], gReporterArgv.data());
    _exit(EXIT_FAILURE);
}

// The whole handler when crashpad_handler is not running: the log and the
// reporter, then the signal's own action.
void SignalHandler(int signal, siginfo_t *info, void *context) noexcept
{
    if (gLogPathSet && !gLogClaimed.test_and_set())
    {
        WriteCrashLog(signal, info, static_cast<const ucontext_t *>(context));
        StartReporter();
    }

    // Restore the default and re-raise, so the exit status stays truthful and
    // whatever the system does with a core still happens.
    std::signal(signal, SIG_DFL);
    raise(signal);
}

void InstallSignalHandlers() noexcept
{
    stack_t altStack{};
    altStack.ss_sp    = gSignalStack.data();
    altStack.ss_size  = gSignalStack.size();
    altStack.ss_flags = 0;
    if (sigaltstack(&altStack, nullptr) != 0)
    {
        Core::Log::Warn("sigaltstack failed - a stack-overflow crash will not be reported.");
    }

    struct sigaction action {};
    action.sa_sigaction = SignalHandler;
    action.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);

    for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
    {
        if (sigaction(signal, &action, nullptr) != 0)
        {
            Core::Log::Warn("Could not install handler for signal {}.", signal);
        }
    }
}

#if defined(ASSISI_CRASHPAD)

// Runs once crashpad_handler has written the dump, on the thread that crashed.
// False lets Crashpad re-raise the signal under the action from before it was
// installed, the default, so the exit status stays truthful.
bool AfterDump(int signal, siginfo_t *info, ucontext_t *context)
{
    if (gLogPathSet)
    {
        WriteCrashLog(signal, info, context);
    }
    StartReporter();
    return false;
}

// Has a crash start crashpad_handler, from beside the executable, to dump this
// process into @p dumps. False, said why, when it cannot, which leaves the crash
// log and the reporter to SignalHandler.
bool StartDumpHandler(const std::filesystem::path &dumps)
{
    const std::optional<std::filesystem::path> executable = Core::AssetSystem::ExecutablePath();
    if (!executable)
    {
        Core::Log::Warn("Crash dumps: this executable cannot find itself, so a crash will leave no minidump.");
        return false;
    }
    const std::filesystem::path handler = executable->parent_path() / kCrashDumpHandlerName;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(handler, ec))
    {
        Core::Log::Warn("Crash dumps: {} is missing, so a crash will leave no minidump.", handler.string());
        return false;
    }

    crashpad::CrashpadClient client;
    if (!client.StartHandlerAtCrash(base::FilePath(handler.string()), base::FilePath(dumps.string()), base::FilePath(),
                                    std::string(), std::string(), {}, {}))
    {
        Core::Log::Warn("Crash dumps: {} could not be set up, so a crash will leave no minidump.", handler.string());
        return false;
    }
    crashpad::CrashpadClient::SetLastChanceExceptionHandler(&AfterDump);
    return true;
}

#endif

} // namespace

void InstallCrashHandlers(const std::filesystem::path &base, [[maybe_unused]] const std::filesystem::path &dumps) noexcept
{
    std::filesystem::path log = base;
    log += kCrashLogExtension;
    const std::string text = log.string();
    if (text.size() < gLogPath.size())
    {
        std::memcpy(gLogPath.data(), text.c_str(), text.size() + 1);
        gLogPathSet = ProbeWritable(log);
    }
    else
    {
        Core::Log::Warn("Crash report path is too long ({} bytes) - crash reports disabled.", text.size());
    }

    // Warm the unwinder while allocating is still safe: its first call dlopens
    // libgcc and allocates, which inside the handler is the deadlock we are
    // avoiding.
    std::array<void *, 4> warmup{};
    (void)backtrace(warmup.data(), static_cast<int>(warmup.size()));

#if defined(ASSISI_CRASHPAD)
    if (StartDumpHandler(dumps))
    {
        return;
    }
#endif
    InstallSignalHandlers();
}

void SetCrashReporter(std::span<const std::string> command)
{
    gReporterArgv.clear();
    gReporterStrings.assign(command.begin(), command.end());
    for (std::string &part : gReporterStrings)
    {
        gReporterArgv.push_back(part.data());
    }
    if (!gReporterArgv.empty())
    {
        gReporterArgv.push_back(nullptr);
    }
}

#endif

} // namespace Assisi::App
