/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    pragma comment(lib, "dbghelp.lib")
#else
#    include <dlfcn.h>
#    include <execinfo.h>
#    include <fcntl.h>
#    include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <Assisi/App/CrashReport.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::App
{
namespace
{

// The report path as plain bytes: the handler must not allocate to name its
// own output file.
std::array<char, 1024> gReportPath{};
bool                   gReportPathSet = false;

// A handler cannot report that it failed to open its own output file — it runs
// in signal context, where warning is not an option, so the failure would be
// indistinguishable from never having crashed. Find out now, while saying
// something is still possible. Probes by creating a file rather than reading
// directory permissions, which lie on network and container mounts.
bool ProbeWritable(const std::filesystem::path &path) noexcept
{
    std::error_code ec;
    const bool      existed = std::filesystem::exists(path, ec);

    const bool writable = std::ofstream(path, std::ios::app).is_open();
    if (!existed)
    {
        std::filesystem::remove(path, ec);
    }

    if (!writable)
    {
        Core::Log::Warn("Crash reports cannot be written to {} — this run will produce none.", path.string());
    }
    return writable;
}

} // namespace

// ---------------------------------------------------------------------------
// Windows: minidump
// ---------------------------------------------------------------------------
#ifdef _WIN32

std::string_view CrashReportExtension() noexcept
{
    return ".dmp";
}

namespace
{

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
    // Every failed ASSISI_ASSERT and std::terminate lands here, so it gets a
    // dump on the same terms as any other crash.
    const bool dumpWritten = WriteDump(nullptr);

    Core::Log::Fatal("Crash: abort() called (assertion failure or std::terminate).");
    if (dumpWritten)
    {
        Core::Log::Fatal("Crash: minidump written to {}", gReportPath.data());
    }
}

} // namespace

void InstallCrashHandlers(const std::filesystem::path &path) noexcept
{
    const std::string text = path.string();
    if (text.size() < gReportPath.size())
    {
        std::memcpy(gReportPath.data(), text.c_str(), text.size() + 1);
        gReportPathSet = ProbeWritable(path);
    }
    else
    {
        Core::Log::Warn("Crash report path is too long ({} bytes) — dumps disabled.", text.size());
    }

    SetUnhandledExceptionFilter(ExceptionFilter);
    std::signal(SIGABRT, AbortHandler);
}

// ---------------------------------------------------------------------------
// POSIX: text report
// ---------------------------------------------------------------------------
#else

std::string_view CrashReportExtension() noexcept
{
    return ".txt";
}

namespace
{

// A stack-overflow SIGSEGV arrives with no usable stack, so the handler needs
// its own or it faults on entry.
alignas(16) std::array<char, 256 * 1024> gSignalStack{};

constexpr size_t kFrameCapacity = 64;
void            *gFrames[kFrameCapacity]{};

// Load address of this executable, resolved at install time while calling into
// the loader is still safe. Frame address minus this is what addr2line wants;
// without it the raw addresses are meaningless under ASLR.
void *gModuleBase = nullptr;

// Ceiling on the symbolization step. Long enough that an ordinary loader
// contention resolves, short enough that a wedged one does not outlive the
// crash it is describing.
constexpr unsigned int kSymbolizeTimeoutSeconds = 5;

// First thread to take a fatal signal wins. Otherwise two concurrent faults
// both O_TRUNC the path and the second wipes the first's complete report.
std::atomic_flag gReportClaimed = ATOMIC_FLAG_INIT;

// write(2) is async-signal-safe; std::format, iostreams and malloc are not.
// EINTR is retried — returning on it silently truncates the report.
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
    size_t                 used = 0;

    void Str(const char *text) noexcept
    {
        const size_t length = std::strlen(text);
        const size_t room   = data.size() - used;
        const size_t take   = length < room ? length : room;
        std::memcpy(data.data() + used, text, take);
        used += take;
    }

    // Hand-rolled because snprintf is not async-signal-safe.
    void Hex(uintptr_t value) noexcept
    {
        char        buffer[2 + (sizeof(uintptr_t) * 2)];
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
        char     buffer[24];
        size_t   index     = sizeof(buffer);
        bool     negative  = value < 0;
        uint64_t magnitude = negative ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);
        do
        {
            buffer[--index] = static_cast<char>('0' + (magnitude % 10));
            magnitude /= 10;
        } while (magnitude != 0 && index > 0);
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
    default:      return "unknown signal";
    }
}

void SignalHandler(int signal, siginfo_t *info, void *) noexcept
{
    if (gReportPathSet && !gReportClaimed.test_and_set())
    {
        // Unwind before the file exists, so the riskiest call is not sitting
        // between open() and first write. backtrace() allocates on its first
        // call, which is why InstallCrashHandlers warms it up.
        const int frames = backtrace(gFrames, kFrameCapacity);

        gReport.used = 0;
        gReport.Str("Assisi crash report\n===================\n\nSignal: ");
        gReport.Str(SignalName(signal));
        gReport.Str(" (");
        gReport.Dec(signal);
        gReport.Str(")\n");

        // si_addr only means a fault address for a hardware-raised signal. For
        // a kill/abort (si_code <= 0) that union member carries si_pid/si_uid,
        // and printing it produces a plausible-looking pointer that is really
        // two spliced integers — worst on SIGABRT, which is where every failed
        // ASSISI_ASSERT lands.
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
        gReport.Str("\nFrames: ");
        gReport.Dec(frames);
        gReport.Str("\nModule base: ");
        gReport.Hex(reinterpret_cast<uintptr_t>(gModuleBase));

        // Raw addresses first, because producing them takes no lock. Everything
        // needed to symbolize is here: subtract the module base, feed the result
        // to addr2line. The pretty form below is a convenience that may not
        // survive.
        gReport.Str("\n\nFrame addresses (subtract module base for addr2line):\n");
        for (int i = 0; i < frames; ++i)
        {
            gReport.Str("  ");
            gReport.Hex(reinterpret_cast<uintptr_t>(gFrames[i]));
            gReport.Str("\n");
        }
        gReport.Str("\nSymbolized (best effort):\n");

        int fd = -1;
        do
        {
            fd = open(gReportPath.data(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        } while (fd < 0 && errno == EINTR);

        if (fd >= 0)
        {
            WriteAll(fd, gReport.data.data(), gReport.used);

            // backtrace_symbols_fd resolves module+offset through _dl_addr,
            // which takes the dynamic-loader lock — it is not async-signal-safe,
            // and a thread stuck mid-dlopen blocks it. Measured at 5s behind a
            // slow library constructor; a wedged loader (a stalled network
            // mount, a driver deadlock) blocks it forever, which in a Vulkan
            // process that dlopens ICDs and layers lazily is reachable. A hung
            // process with no exit status is worse than a report without pretty
            // names, so bound it: SIGALRM's default action terminates, and by
            // this point the addresses above are already on disk.
            alarm(kSymbolizeTimeoutSeconds);
            backtrace_symbols_fd(gFrames, frames, fd);
            alarm(0);

            gReport.used = 0;
            gReport.Str("\nSymbolize with: addr2line -e <binary> -f -C <frame - module base>\n");
            WriteAll(fd, gReport.data.data(), gReport.used);
            close(fd);
        }
    }

    // Restore the default and re-raise, so the exit status stays truthful and
    // whatever the system does with a core still happens.
    std::signal(signal, SIG_DFL);
    raise(signal);
}

} // namespace

void InstallCrashHandlers(const std::filesystem::path &path) noexcept
{
    const std::string text = path.string();
    if (text.size() < gReportPath.size())
    {
        std::memcpy(gReportPath.data(), text.c_str(), text.size() + 1);
        gReportPathSet = ProbeWritable(path);
    }
    else
    {
        Core::Log::Warn("Crash report path is too long ({} bytes) — crash reports disabled.", text.size());
    }

    // Warm the unwinder while allocating is still safe: its first call dlopens
    // libgcc and allocates, which inside the handler is the deadlock we are
    // avoiding.
    void *warmup[4];
    (void)backtrace(warmup, 4);

    // Same reasoning for the load address: dladdr takes the loader lock, so ask
    // now rather than from the handler.
    Dl_info self{};
    if (dladdr(reinterpret_cast<void *>(&SignalHandler), &self) != 0)
    {
        gModuleBase = self.dli_fbase;
    }

    stack_t altStack{};
    altStack.ss_sp    = gSignalStack.data();
    altStack.ss_size  = gSignalStack.size();
    altStack.ss_flags = 0;
    if (sigaltstack(&altStack, nullptr) != 0)
    {
        Core::Log::Warn("sigaltstack failed — a stack-overflow crash will not be reported.");
    }

    struct sigaction action{};
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

#endif

} // namespace Assisi::App
