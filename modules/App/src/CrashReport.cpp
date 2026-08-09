/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    pragma comment(lib, "dbghelp.lib")
#else
#    include <execinfo.h>
#    include <fcntl.h>
#    include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cerrno>
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

// The report path, as bytes the handler can read with no allocation and no
// std::string involved. A handler that has to build a path is a handler that
// fails on the corrupted heap that is half of why it exists.
std::array<char, 1024> gReportPath{};
bool                   gReportPathSet = false;

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

// MiniDumpNormal alone is thread stacks and nothing else — you get a call stack
// and no ability to see what any object held. These four additions cost a few MB
// instead of a few hundred KB and are the difference between "it crashed in
// this function" and knowing why:
//   DataSegs                    — globals and statics
//   IndirectlyReferencedMemory  — the memory the stacks actually point at
//   ThreadInfo                  — thread times and contexts
//   UnloadedModules             — a DLL unloaded out from under a callback
constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
    MiniDumpWithUnloadedModules);

// Shared by the exception filter and the abort handler. `info` is null on the
// abort path, which MiniDumpWriteDump accepts: the dump then describes every
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
    // Dump first: Log::Fatal formats and allocates, either of which can re-fault
    // on the corrupted heap that may well be why we are here. The dump path
    // below is Win32 only and touches no CRT heap.
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
    // abort() used to leave a single line of text and nothing else — which is
    // where every failed ASSISI_ASSERT and every std::terminate lands. It gets
    // a dump on the same terms as any other crash now.
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
        gReportPathSet = true;
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

// A dedicated stack for the handler. A stack-overflow SIGSEGV arrives with no
// usable stack left, so without this the handler faults on entry and the crash
// that most needs explaining is the one that explains nothing.
alignas(16) std::array<char, 256 * 1024> gSignalStack{};

// Pre-resolved so the handler never calls a formatter. Everything below writes
// only string literals and hand-formatted integers.
constexpr size_t kFrameCapacity = 64;
void            *gFrames[kFrameCapacity]{};

// Only the first thread to take a fatal signal writes a report. Without this,
// two threads faulting at once both O_TRUNC the same path and the second can
// wipe a complete report the first had already written — turning two crashes
// into zero diagnostics. First one wins; the loser falls straight through to
// the re-raise.
std::atomic_flag gReportClaimed = ATOMIC_FLAG_INIT;

// write(2) is async-signal-safe; std::format, iostreams and malloc are not, so
// nothing here uses them.
//
// EINTR is retried rather than treated as failure. A short write or an
// interrupted one is not an error, and returning on it silently truncates the
// report at whatever byte the interruption landed on.
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

// The report is composed here first and written in one syscall, rather than
// dribbled out across a dozen writes as it is built. Two reasons, and the
// second is why this shape was chosen over the obvious one:
//
//   - Every write() is a chance for the process to die between calls, and a
//     file opened but not yet written is a zero-byte report — which looks like
//     the feature is broken rather than like the process died. One write means
//     the file is either absent or complete.
//   - It lets the unwind happen *before* the file is created, so the riskiest
//     work in the handler is no longer sitting between open() and first write.
//
// Bounded and truncating: an overflowing report is clamped, never a buffer
// overrun in the one piece of code that runs when memory is already suspect.
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

// Static, not on the (possibly exhausted) stack. Trivially initialized, so it
// costs no dynamic setup and is ready before main().
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
        // Unwind first, while no file exists yet. backtrace() can allocate on
        // its very first call (it lazily loads libgcc's unwinder), which is why
        // InstallCrashHandlers warms it up while the heap is still healthy —
        // and why anything that might not come back belongs before the open()
        // rather than between the open and the first write.
        const int frames = backtrace(gFrames, kFrameCapacity);

        gReport.used = 0;
        gReport.Str("Assisi crash report\n===================\n\nSignal: ");
        gReport.Str(SignalName(signal));
        gReport.Str(" (");
        gReport.Dec(signal);
        gReport.Str(")\nFault address: ");
        gReport.Hex(reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr));
        gReport.Str("\nsi_code: ");
        gReport.Dec(info != nullptr ? info->si_code : 0);
        gReport.Str("\nFrames: ");
        gReport.Dec(frames);
        gReport.Str("\n\nBacktrace:\n");

        int fd = -1;
        do
        {
            fd = open(gReportPath.data(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        } while (fd < 0 && errno == EINTR);

        if (fd >= 0)
        {
            // One write, so the file goes from absent to substantially complete
            // with nothing observable in between.
            WriteAll(fd, gReport.data.data(), gReport.used);

            // Has to come after, because backtrace_symbols_fd only writes to a
            // descriptor — resolving module+offset into a buffer would mean
            // backtrace_symbols, which mallocs. By now the file already has its
            // header, so a death here costs the frame list, not the report.
            backtrace_symbols_fd(gFrames, frames, fd);

            gReport.used = 0;
            gReport.Str("\nSymbolize with: addr2line -e <binary> -f -C <offset>\n");
            WriteAll(fd, gReport.data.data(), gReport.used);
            close(fd);
        }
    }

    // Restore the default and re-raise, so the process dies of what actually
    // killed it: the exit status stays truthful, and anything the system would
    // normally do with a core still happens.
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
        gReportPathSet = true;
    }
    else
    {
        Core::Log::Warn("Crash report path is too long ({} bytes) — crash reports disabled.", text.size());
    }

    // Warm up the unwinder now, while allocating is still safe. Its first call
    // dlopen()s libgcc and allocates; doing that inside the handler is exactly
    // the deadlock we are trying to avoid.
    void *warmup[4];
    (void)backtrace(warmup, 4);

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
