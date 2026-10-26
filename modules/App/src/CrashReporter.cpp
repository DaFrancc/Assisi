/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/CrashReporter.hpp>

#include "CrashLogFormat.hpp"

#include <Assisi/App/Application.hpp>
#include <Assisi/App/CookedAssets.hpp>
#include <Assisi/App/CrashReport.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Diagnostics.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/PakProvider.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Render/AssetSource.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <system_error>
#include <utility>

#ifndef _WIN32
#    include <elf.h>
#    include <fcntl.h>
#    include <spawn.h>
#    include <sys/wait.h>
#    include <unistd.h>

extern char **environ;
#endif

namespace Assisi::App
{
namespace
{

/// The flag every main() reads as "no window".
constexpr std::string_view kHeadlessFlag = "--headless";

/// The crash window's size, in logical pixels: room for a stack's worth of
/// frames without filling a screen the player was using for something else.
constexpr std::int32_t kWindowWidth = 1120;
constexpr std::int32_t kWindowHeight = 780;

/// How many frames the window lists. The log has them all.
constexpr std::size_t kShownFrames = 16;

/// The window's world holds a screen and nothing else.
constexpr std::uint32_t kWindowWorldBodies = 1;

/// What a frame's address is read as, and a module's start.
constexpr std::int32_t kHexBase = 16;

// ---------------------------------------------------------------------------
// Reading a crash log
// ---------------------------------------------------------------------------

/// One line of /proc/self/maps: a range of addresses and the file it is from.
struct Mapping
{
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t fileOffset = 0;
    std::string path;
};

/// The lines of @p text, without their ends.
std::vector<std::string_view> Lines(std::string_view text)
{
    std::vector<std::string_view> lines;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        lines.push_back(text.substr(0, end));
        if (end == std::string_view::npos)
        {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return lines;
}

std::string_view Trimmed(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

/// A hexadecimal number, with or without its 0x.
std::optional<std::uint64_t> ParseHex(std::string_view text)
{
    if (text.starts_with("0x"))
    {
        text.remove_prefix(2);
    }
    std::uint64_t value = 0;
    const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value, kHexBase);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

/// One line of the module map: "start-end perms offset dev inode path".
std::optional<Mapping> ParseMapping(std::string_view line)
{
    // The five before the path, which is the rest of the line.
    std::array<std::string_view, 5> fields{};
    std::size_t taken = 0;
    std::string_view rest = line;
    while (taken < fields.size() && !rest.empty())
    {
        rest = rest.substr(std::min(rest.find_first_not_of(' '), rest.size()));
        const std::size_t end = rest.find(' ');
        fields[taken++] = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    const std::string_view path = Trimmed(rest);
    if (taken < fields.size() || path.empty() || path.front() != '/')
    {
        return std::nullopt;
    }

    const std::size_t dash = fields[0].find('-');
    if (dash == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> start = ParseHex(fields[0].substr(0, dash));
    const std::optional<std::uint64_t> end = ParseHex(fields[0].substr(dash + 1));
    const std::optional<std::uint64_t> offset = ParseHex(fields[2]);
    if (!start || !end || !offset)
    {
        return std::nullopt;
    }
    return Mapping{.start = *start, .end = *end, .fileOffset = *offset, .path = std::string(path)};
}

/// What a crash log says, as far as naming its frames needs.
struct ReadLog
{
    std::vector<std::uint64_t> frames;
    std::vector<Mapping> mappings;
    std::uint64_t faulting = 0;
};

ReadLog ReadCrashLog(std::string_view log)
{
    enum class Section : std::uint8_t
    {
        Header,
        Frames,
        Modules,
        Count_
    };

    ReadLog read;
    Section section = Section::Header;
    for (const std::string_view line : Lines(log))
    {
        if (line.starts_with(CrashLog::kFaultingInstructionLabel))
        {
            read.faulting = ParseHex(line.substr(CrashLog::kFaultingInstructionLabel.size())).value_or(0);
        }
        else if (line == CrashLog::kFramesHeading)
        {
            section = Section::Frames;
        }
        else if (line == CrashLog::kModulesHeading)
        {
            section = Section::Modules;
        }
        else if (section == Section::Frames)
        {
            if (const std::optional<std::uint64_t> frame = ParseHex(Trimmed(line)))
            {
                read.frames.push_back(*frame);
            }
            else
            {
                section = Section::Header;
            }
        }
        else if (section == Section::Modules)
        {
            if (std::optional<Mapping> mapping = ParseMapping(line))
            {
                read.mappings.push_back(std::move(*mapping));
            }
        }
    }
    return read;
}

// ---------------------------------------------------------------------------
// Naming frames
// ---------------------------------------------------------------------------

#ifndef _WIN32
/// What @p command prints, run to completion; empty when it cannot be started
/// or fails. Its own errors are dropped: a module without symbols is expected.
std::optional<std::string> OutputOf(const std::vector<std::string> &command)
{
    std::array<int, 2> pipeEnds{};
    if (pipe2(pipeEnds.data(), O_CLOEXEC) != 0)
    {
        return std::nullopt;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<std::string> owned = command;
    std::vector<char *> arguments;
    for (std::string &argument : owned)
    {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    pid_t child = 0;
    const int spawned = posix_spawnp(&child, arguments[0], &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeEnds[1]);
    if (spawned != 0)
    {
        close(pipeEnds[0]);
        return std::nullopt;
    }

    std::string output;
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const ssize_t got = read(pipeEnds[0], chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got <= 0)
        {
            break;
        }
        output.append(chunk.data(), static_cast<std::size_t>(got));
    }
    close(pipeEnds[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return std::nullopt;
    }
    return output;
}

/// Names @p frames, all in the module at @p path, with addr2line: three lines
/// a frame, the address, the function and the file:line.
void NameInModule(const std::string &path, std::span<NamedFrame *const> frames, std::span<const std::uint64_t> asked)
{
    std::vector<std::string> command{"addr2line", "-e", path, "-f", "-C", "-a"};
    for (const std::uint64_t address : asked)
    {
        command.push_back(std::format("0x{:x}", address));
    }
    const std::optional<std::string> output = OutputOf(command);
    if (!output)
    {
        return;
    }

    constexpr std::size_t kLinesPerAddress = 3;
    const std::vector<std::string_view> lines = Lines(*output);
    for (std::size_t i = 0; i < frames.size() && (i + 1) * kLinesPerAddress <= lines.size(); ++i)
    {
        const std::string_view function = lines[(i * kLinesPerAddress) + 1];
        std::string_view location = lines[(i * kLinesPerAddress) + 2];
        // "(discriminator 2)" tells the compiler's passes apart, not the reader.
        location = location.substr(0, location.find(" ("));
        if (function != "??")
        {
            frames[i]->function = function;
        }
        // "??:0" and ":?" both mean the module has no line table.
        if (!location.starts_with("??") && !location.starts_with(":"))
        {
            frames[i]->location = location;
        }
    }
}
#endif

/// The lowest address the ELF file at @p path asks to be loaded at, page
/// aligned: 0 for a position-independent file, its fixed address for one that
/// is not. Empty when it is not an ELF file that can be read.
std::optional<std::uint64_t> LinkedAt(const std::string &path)
{
#ifndef _WIN32
    std::ifstream file(path, std::ios::binary);
    Elf64_Ehdr header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)) ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64)
    {
        return std::nullopt;
    }
    constexpr std::uint64_t kPageMask = ~std::uint64_t{0xfff};
    std::optional<std::uint64_t> lowest;
    for (std::uint32_t i = 0; i < header.e_phnum; ++i)
    {
        Elf64_Phdr segment{};
        file.seekg(static_cast<std::streamoff>(header.e_phoff + (std::uint64_t{i} *header.e_phentsize)));
        if (!file.read(reinterpret_cast<char *>(&segment), sizeof(segment)))
        {
            return std::nullopt;
        }
        if (segment.p_type == PT_LOAD)
        {
            lowest = std::min(lowest.value_or(segment.p_vaddr), segment.p_vaddr);
        }
    }
    if (!lowest)
    {
        return std::nullopt;
    }
    return *lowest & kPageMask;
#else
    (void)path;
    return std::nullopt;
#endif
}

/// Whether the ELF file at @p path still has its symbol table. A stripped one
/// keeps only the symbols it exports, and addr2line names every address after
/// the nearest of those, which in a game is some library function that has
/// nothing to do with the crash. Better unnamed than named wrong.
bool HasSymbolTable(const std::string &path)
{
#ifndef _WIN32
    std::ifstream file(path, std::ios::binary);
    Elf64_Ehdr header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)) ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64)
    {
        return false;
    }
    for (std::uint32_t i = 0; i < header.e_shnum; ++i)
    {
        Elf64_Shdr section{};
        file.seekg(static_cast<std::streamoff>(header.e_shoff + (static_cast<std::uint64_t>(i) * header.e_shentsize)));
        if (!file.read(reinterpret_cast<char *>(&section), sizeof(section)))
        {
            return false;
        }
        if (section.sh_type == SHT_SYMTAB)
        {
            return true;
        }
    }
    return false;
#else
    (void)path;
    return false;
#endif
}

/// Where @p address is: the mapping it falls in, and how far the file that
/// mapping is from was moved from where it was linked to run, which is what
/// takes a running address to the one addr2line reads.
std::optional<std::pair<const Mapping *, std::uint64_t>> Locate(const std::vector<Mapping> &mappings,
                                                                std::uint64_t address)
{
    const std::vector<Mapping>::const_iterator hit =
        std::ranges::find_if(mappings, [address](const Mapping &m) { return address >= m.start && address < m.end; });
    if (hit == mappings.end())
    {
        return std::nullopt;
    }
    std::uint64_t loadedAt = hit->start - hit->fileOffset;
    for (const Mapping &mapping : mappings)
    {
        if (mapping.path == hit->path && mapping.fileOffset == 0)
        {
            loadedAt = std::min(loadedAt, mapping.start);
        }
    }
    const std::uint64_t linkedAt = LinkedAt(hit->path).value_or(0);
    return std::pair{&*hit, loadedAt - linkedAt};
}

/// The file name of @p path, which is how the window names a module.
std::string ModuleName(const std::string &path)
{
    return std::filesystem::path(path).filename().string();
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

/// What the window says.
struct CrashSummary
{
    std::string reason;
    std::string stack;
    std::string files;
};

/// The reason a crash log gives: Core::Abort's, else the signal.
std::string ReasonIn(std::string_view log)
{
    std::string_view signal;
    for (const std::string_view line : Lines(log))
    {
        if (line.starts_with(CrashLog::kReasonLabel))
        {
            return std::string(line.substr(CrashLog::kReasonLabel.size()));
        }
        if (line.starts_with(CrashLog::kSignalLabel))
        {
            signal = line.substr(CrashLog::kSignalLabel.size());
        }
    }
    return signal.empty() ? std::string("The crash log could not be read.") : std::string(signal);
}

std::string StackText(const std::vector<NamedFrame> &frames)
{
    std::string text;
    for (std::size_t i = 0; i < std::min(frames.size(), kShownFrames); ++i)
    {
        const NamedFrame &frame = frames[i];
        text += frame.function.empty() ? std::format("{} + 0x{:x}", frame.module, frame.offset) : frame.function;
        if (!frame.location.empty())
        {
            text += std::format("\n    {}", frame.location);
        }
        text += '\n';
    }
    if (frames.size() > kShownFrames)
    {
        text += std::format("... and {} more in the crash log\n", frames.size() - kShownFrames);
    }
    return text;
}

/// Serves the window's shaders from the source asset root. The window draws
/// nothing else, so asking it for anything else is asking for what is not there.
class ShaderFileSource final : public Render::AssetSource
{
public:
    [[nodiscard]] std::expected<Geometry::CookedMesh, Render::AssetLoadError>
    LoadMesh(const Core::AssetId & /*id*/) const override
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }

    [[nodiscard]] std::expected<Geometry::MaterialData, Render::AssetLoadError>
    LoadMaterial(const Core::AssetId & /*id*/) const override
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }

    [[nodiscard]] std::expected<Image::DecodedImage, Render::AssetLoadError>
    LoadTexture(const Core::AssetId & /*id*/, Image::ColorSpace /*space*/, Image::PixelFormat /*format*/) const override
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, Render::AssetLoadError>
    LoadShader(std::string_view vpath) const override
    {
        std::expected<std::vector<std::byte>, Core::AssetError> spirv = Core::AssetSystem::ReadBinary(vpath);
        if (!spirv)
        {
            return std::unexpected(Render::AssetLoadError::Unreadable);
        }
        return std::move(*spirv);
    }

    [[nodiscard]] std::expected<Core::AssetId, Render::AssetLoadError> Resolve(std::string_view /*vpath*/) const override
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }

    [[nodiscard]] std::string Describe(const Core::AssetId &id) const override { return id.ToString(); }
};

/// Opens @p folder in the desktop's file manager.
void OpenFolder(const std::filesystem::path &folder)
{
#ifndef _WIN32
    // Its output goes nowhere rather than to a pipe: the file manager it starts
    // would inherit the pipe and hold it open for as long as it runs.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::string program = "xdg-open";
    std::string where = folder.string();
    std::array<char *, 3> arguments{program.data(), where.data(), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawnp(&child, program.c_str(), &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    if (spawned == 0)
    {
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
    }
    if (spawned != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        Core::Log::Warn("Crash report: could not open {} (xdg-open failed).", folder.string());
    }
#else
    Core::Log::Warn("Crash report: opening {} is not supported here.", folder.string());
#endif
}

class CrashReporterApp final : public Application
{
public:
    CrashReporterApp(std::filesystem::path pak, CrashSummary summary)
        : Application(CrashReporterRole{}), _summary(std::move(summary)), _pakPath(std::move(pak))
    {
        SetWindowSize(kWindowWidth, kWindowHeight);
    }

    ~CrashReporterApp() override
    {
        // Before the sources go, so nothing reaches them through the installed
        // pointer afterwards.
        _world.reset();
        if (Render::GetAssetSource() == &_shaderSource ||
            (_cookedSource && Render::GetAssetSource() == &*_cookedSource))
        {
            (void)Render::SetAssetSource(nullptr);
        }
        if (_pak)
        {
            RemoveCookedReaders();
        }
    }

    CrashReporterApp(const CrashReporterApp &) = delete;
    CrashReporterApp &operator=(const CrashReporterApp &) = delete;

protected:
    [[nodiscard]] bool MountContent() override
    {
        if (_pakPath.empty())
        {
            if (!Application::MountContent())
            {
                return false;
            }
            (void)Render::SetAssetSource(&_shaderSource);
            return true;
        }

        std::expected<Core::PakProvider, Core::AssetError> mounted = Core::PakProvider::Mount(_pakPath);
        if (!mounted)
        {
            Core::Log::Error("Crash report: the content package '{}' cannot be read ({}), so there is no window.",
                             _pakPath.string(), Core::ToString(mounted.error()));
            return false;
        }
        _pak.emplace(std::move(*mounted));
        _cookedSource.emplace(*_pak);
        (void)Render::SetAssetSource(&*_cookedSource);
        LoadAssetsFrom(*_pak);
        InstallCookedReaders(*_pak);
        return true;
    }

    void OnStart() override
    {
        Mondrian::Ui *ui = GetUi();
        if (ui == nullptr)
        {
            RefuseStart();
            return;
        }
        _world = std::make_unique<World>(_collision, kWindowWorldBodies);
        Mondrian::Screen *screen = LoadScreen(*_world, *ui, kCrashScreenPath);
        if (screen == nullptr)
        {
            RefuseStart();
            return;
        }
        Fill(*screen, "title", std::format("{} stopped", GetConfig().title.View()));
        Fill(*screen, "reason", _summary.reason);
        Fill(*screen, "stack", _summary.stack);
        Fill(*screen, "files", _summary.files);
        screen->Show();
    }

    void OnFixedUpdate(float /*dt*/) override {}

    void OnUpdate(float /*dt*/) override
    {
        if (_world == nullptr)
        {
            return;
        }
        if (!_world->events.Read<OpenCrashFolder>().empty())
        {
            OpenFolder(Core::AssetSystem::GetUserRoot());
        }
        _world->events.EndFrame(/*keepPending=*/ false);
    }

private:
    static void Fill(Mondrian::Screen &screen, std::string_view node, std::string_view text)
    {
        if (const Mondrian::NodeId id = screen.Find(node))
        {
            screen.Tree().SetText(id, text);
        }
    }

    // Largest first, so the object carries no interior padding.
    std::optional<Core::PakProvider> _pak;
    std::optional<CookedAssetSource> _cookedSource;
    ShaderFileSource _shaderSource;

    /// The window's world has no bodies, so nothing reads collision from it.
    CookedCollisionSource _collision;
    std::unique_ptr<World> _world;
    CrashSummary _summary;
    std::filesystem::path _pakPath;
};

std::string ReadWhole(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

std::vector<std::string> CrashReporterCommand(bool headless)
{
    const std::optional<std::filesystem::path> executable = Core::AssetSystem::ExecutablePath();
    if (!executable)
    {
        return {};
    }
    std::vector<std::string> command{executable->string(), std::string(kCrashReportFlag), Core::LaunchStamp()};
    if (headless)
    {
        command.emplace_back(kHeadlessFlag);
    }
    return command;
}

std::optional<CrashReportLaunch> CrashReportLaunchFrom(std::span<char *const> arguments)
{
    constexpr std::size_t kFlagAt = 1;
    constexpr std::size_t kStampAt = 2;
    if (arguments.size() <= kStampAt || std::string_view(arguments[kFlagAt]) != kCrashReportFlag)
    {
        return std::nullopt;
    }
    CrashReportLaunch launch;
    launch.stamp = arguments[kStampAt];
    for (std::size_t i = kStampAt + 1; i < arguments.size(); ++i)
    {
        launch.headless = launch.headless || std::string_view(arguments[i]) == kHeadlessFlag;
    }
    return launch;
}

bool ClaimCrashDump(const std::filesystem::path &dumps, const std::filesystem::path &to)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dumps, ec))
    {
        return false;
    }

    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newestTime{};
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(dumps, ec))
    {
        if (!entry.is_regular_file(ec) || entry.path().extension() != kCrashDumpExtension)
        {
            continue;
        }
        const std::filesystem::file_time_type written = entry.last_write_time(ec);
        if (!newest || written > newestTime)
        {
            newest = entry.path();
            newestTime = written;
        }
    }
    if (!newest)
    {
        Core::Log::Warn("Crash report: crashpad_handler left no minidump in {}.", dumps.string());
        return false;
    }

    std::filesystem::rename(*newest, to, ec);
    if (ec)
    {
        // Across file systems, a rename cannot; a copy can.
        std::filesystem::copy_file(*newest, to, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            Core::Log::Warn("Crash report: could not move the minidump {} to {} ({}).", newest->string(), to.string(),
                            ec.message());
            return false;
        }
        std::filesystem::remove(*newest, ec);
    }

    // The handler's own record of the dump, which nothing reads once it is gone.
    std::filesystem::path record = *newest;
    record.replace_extension(".meta");
    std::filesystem::remove(record, ec);
    Core::Log::Info("Crash report: the minidump is {}.", to.string());
    return true;
}

std::vector<NamedFrame> NameFrames(std::string_view log)
{
    const ReadLog read = ReadCrashLog(log);

    // Above the faulting instruction are the handlers that reported it.
    const std::vector<std::uint64_t>::const_iterator fault = std::ranges::find(read.frames, read.faulting);
    const std::size_t first = fault == read.frames.end() ? 0 : static_cast<std::size_t>(fault - read.frames.begin());

    std::vector<NamedFrame> frames;
    std::map<std::string, std::vector<std::size_t>> byModule;
    std::vector<std::uint64_t> asked;
    for (std::size_t i = first; i < read.frames.size(); ++i)
    {
        const std::uint64_t address = read.frames[i];
        NamedFrame frame;
        if (const std::optional<std::pair<const Mapping *, std::uint64_t>> where = Locate(read.mappings, address))
        {
            frame.module = ModuleName(where->first->path);
            frame.offset = address - where->second;
            byModule[where->first->path].push_back(frames.size());
        }
        else
        {
            frame.module = "?";
            frame.offset = address;
        }
        // Every frame but the faulting one is a return address, just past the
        // call; one byte back is the call's own line.
        asked.push_back(i == first ? frame.offset : frame.offset - 1);
        frames.push_back(std::move(frame));
    }

#ifndef _WIN32
    for (const std::pair<const std::string, std::vector<std::size_t>> &module : byModule)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(module.first, ec) || !HasSymbolTable(module.first))
        {
            continue;
        }
        std::vector<NamedFrame *> named;
        std::vector<std::uint64_t> addresses;
        for (const std::size_t index : module.second)
        {
            named.push_back(&frames[index]);
            addresses.push_back(asked[index]);
        }
        NameInModule(module.first, named, addresses);
    }
#endif
    return frames;
}

bool WriteStack(const std::filesystem::path &log, const std::vector<NamedFrame> &frames)
{
    const std::string text = ReadWhole(log);
    std::string stack = std::string(CrashLog::kStackHeading) + "\n";
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const NamedFrame &frame = frames[i];
        stack += std::format("  #{:<2} {}", i, frame.function.empty() ? "??" : frame.function);
        if (!frame.location.empty())
        {
            stack += std::format("  at {}", frame.location);
        }
        stack += std::format("  ({} + 0x{:x})\n", frame.module, frame.offset);
    }
    stack += "\n";

    const std::size_t frameSection = text.find(std::string(CrashLog::kFramesHeading) + "\n");
    std::string written = text;
    written.insert(frameSection == std::string::npos ? written.size() : frameSection, stack);

    std::filesystem::path partial = log;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << written;
        if (!file)
        {
            Core::Log::Warn("Crash report: could not write the named stack into {}.", log.string());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(partial, log, ec);
    if (ec)
    {
        Core::Log::Warn("Crash report: could not write the named stack into {} ({}).", log.string(), ec.message());
        return false;
    }
    return true;
}

std::int32_t RunCrashReporter(const CrashReportLaunch &launch)
{
    const std::filesystem::path &userRoot = Core::AssetSystem::GetUserRoot();
    const std::string stem = CrashFileStem(launch.stamp);
    const std::filesystem::path log = userRoot / (stem + std::string(kCrashLogExtension));
    const std::filesystem::path dump = userRoot / (stem + std::string(kCrashDumpExtension));
    const std::filesystem::path runLog = userRoot / std::format("assisi-{}.log", launch.stamp);

    (void)ClaimCrashDump(userRoot / kCrashDumpFolderName, dump);

    std::error_code ec;
    const std::string text = ReadWhole(log);
    const std::vector<NamedFrame> frames = NameFrames(text);
    if (!frames.empty())
    {
        (void)WriteStack(log, frames);
    }
    if (launch.headless)
    {
        return EXIT_SUCCESS;
    }

    CrashSummary summary{.reason = ReasonIn(text), .stack = StackText(frames), .files = {}};
    // Names only: the whole path is wider than the window, and Open folder goes
    // to where they are.
    for (const std::filesystem::path &file : {log, dump, runLog})
    {
        if (std::filesystem::exists(file, ec))
        {
            summary.files += file.filename().string() + "\n";
        }
    }

    CrashReporterApp app(launch.pak, std::move(summary));
    if (!app.Initialize())
    {
        // The files are in order, which is the part that cannot wait. A machine
        // that cannot open a window has nobody at it to show one to.
        return EXIT_SUCCESS;
    }
    app.Run();
}

} // namespace Assisi::App
