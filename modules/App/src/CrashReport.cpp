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

// write(2) is async-signal-safe; std::format, iostreams and malloc are not, so
// nothing here uses them.
void WriteAll(int fd, const char *data, size_t size) noexcept
{
    while (size > 0)
    {
        const ssize_t written = write(fd, data, size);
        if (written <= 0)
        {
            return;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
}

void WriteStr(int fd, const char *text) noexcept
{
    WriteAll(fd, text, std::strlen(text));
}

// Hand-rolled because snprintf is not async-signal-safe. Hex for addresses,
// decimal for counts.
void WriteHex(int fd, uintptr_t value) noexcept
{
    char        buffer[2 + (sizeof(uintptr_t) * 2)];
    const char *digits = "0123456789abcdef";
    buffer[0]          = '0';
    buffer[1]          = 'x';
    for (size_t i = 0; i < sizeof(uintptr_t) * 2; ++i)
    {
        const size_t shift              = (sizeof(uintptr_t) * 2 - 1 - i) * 4;
        buffer[2 + i]                   = digits[(value >> shift) & 0xF];
    }
    WriteAll(fd, buffer, sizeof(buffer));
}

void WriteDec(int fd, int64_t value) noexcept
{
    char   buffer[24];
    size_t index = sizeof(buffer);
    bool   negative = value < 0;
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
    WriteAll(fd, buffer + index, sizeof(buffer) - index);
}

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
    if (gReportPathSet)
    {
        const int fd = open(gReportPath.data(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            WriteStr(fd, "Assisi crash report\n===================\n\nSignal: ");
            WriteStr(fd, SignalName(signal));
            WriteStr(fd, " (");
            WriteDec(fd, signal);
            WriteStr(fd, ")\nFault address: ");
            WriteHex(fd, reinterpret_cast<uintptr_t>(info != nullptr ? info->si_addr : nullptr));
            WriteStr(fd, "\nsi_code: ");
            WriteDec(fd, info != nullptr ? info->si_code : 0);
            WriteStr(fd, "\n\nBacktrace:\n");

            // backtrace() can allocate on its very first call (it lazily loads
            // libgcc's unwinder), which is why InstallCrashHandlers warms it up
            // while the heap is still healthy. backtrace_symbols_fd writes
            // straight to the fd and, unlike backtrace_symbols, never mallocs.
            const int frames = backtrace(gFrames, kFrameCapacity);
            backtrace_symbols_fd(gFrames, frames, fd);

            WriteStr(fd, "\nSymbolize with: addr2line -e <binary> -f -C <address>\n");
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
