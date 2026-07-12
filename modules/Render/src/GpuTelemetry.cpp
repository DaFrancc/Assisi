/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/GpuTelemetry.hpp>

#include <array>
#include <chrono>
#include <string>

// We load NVML at runtime rather than linking it, so we declare only the
// handful of entry points we use. These signatures and enum values are part of
// NVML's stable ABI (the driver ships forever-compatible _v2 symbols), so the
// local declarations stay valid across driver versions without the CUDA headers.
namespace
{
using nvmlDevice_t = void *;
constexpr int kNvmlSuccess = 0;

// nvmlClockType_t
constexpr int kNvmlClockGraphics = 0;
constexpr int kNvmlClockMem      = 2;
// nvmlTemperatureSensors_t
constexpr int kNvmlTemperatureGpu = 0;

struct NvmlUtilization
{
    unsigned int gpu;
    unsigned int memory;
};
struct NvmlMemory
{
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

using PFN_Init                = int (*)();
using PFN_Shutdown            = int (*)();
using PFN_GetHandleByIndex    = int (*)(unsigned int, nvmlDevice_t *);
using PFN_GetName             = int (*)(nvmlDevice_t, char *, unsigned int);
using PFN_GetClockInfo        = int (*)(nvmlDevice_t, int, unsigned int *);
using PFN_GetUtilizationRates = int (*)(nvmlDevice_t, NvmlUtilization *);
using PFN_GetPowerUsage       = int (*)(nvmlDevice_t, unsigned int *);
using PFN_GetPowerLimit       = int (*)(nvmlDevice_t, unsigned int *);
using PFN_GetTemperature      = int (*)(nvmlDevice_t, int, unsigned int *);
using PFN_GetMemoryInfo       = int (*)(nvmlDevice_t, NvmlMemory *);

// --- Tiny platform shim for the dynamic library ---------------------------
#ifdef _WIN32
#include <windows.h>
using LibHandle = HMODULE;
LibHandle libOpen()
{
    return LoadLibraryA("nvml.dll"); // ships under System32 with the driver
}
void *libSym(LibHandle h, const char *name)
{
    return reinterpret_cast<void *>(GetProcAddress(h, name));
}
void libClose(LibHandle h)
{
    FreeLibrary(h);
}
#else
#include <dlfcn.h>
using LibHandle = void *;
LibHandle libOpen()
{
    // The versioned .so.1 is the stable SONAME; fall back to the dev symlink.
    if (LibHandle h = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL))
        return h;
    return dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
}
void *libSym(LibHandle h, const char *name)
{
    return dlsym(h, name);
}
void libClose(LibHandle h)
{
    dlclose(h);
}
#endif

// Resolve `name`, then `name`+"_v2" preference: several NVML calls only exist as
// versioned symbols on modern drivers, while a few are unversioned. Try the
// versioned form first, then the plain one.
void *resolve(LibHandle lib, const char *base)
{
    const std::string versioned = std::string(base) + "_v2";
    if (void *fn = libSym(lib, versioned.c_str()))
        return fn;
    return libSym(lib, base);
}

constexpr auto kThrottle = std::chrono::milliseconds(200);
} // namespace

namespace Assisi::Render
{

struct GpuTelemetry::Impl
{
    bool         initialized = false; // NVML init attempted (success or not)
    LibHandle    lib         = nullptr;
    nvmlDevice_t device      = nullptr;
    bool         ok          = false; // NVML up and a device handle acquired

    PFN_Shutdown            shutdown  = nullptr;
    PFN_GetClockInfo        clockInfo = nullptr;
    PFN_GetUtilizationRates util      = nullptr;
    PFN_GetPowerUsage       power     = nullptr;
    PFN_GetPowerLimit       powerLim  = nullptr;
    PFN_GetTemperature      temp      = nullptr;
    PFN_GetMemoryInfo       memInfo   = nullptr;

    GpuTelemetrySample                    sample;
    std::chrono::steady_clock::time_point lastQuery{};
    bool                                  queried = false;

    void Initialize()
    {
        initialized = true;

        lib = libOpen();
        if (!lib)
            return;

        auto init   = reinterpret_cast<PFN_Init>(resolve(lib, "nvmlInit"));
        auto handle = reinterpret_cast<PFN_GetHandleByIndex>(resolve(lib, "nvmlDeviceGetHandleByIndex"));
        auto name   = reinterpret_cast<PFN_GetName>(resolve(lib, "nvmlDeviceGetName"));
        shutdown    = reinterpret_cast<PFN_Shutdown>(resolve(lib, "nvmlShutdown"));
        clockInfo   = reinterpret_cast<PFN_GetClockInfo>(resolve(lib, "nvmlDeviceGetClockInfo"));
        util        = reinterpret_cast<PFN_GetUtilizationRates>(resolve(lib, "nvmlDeviceGetUtilizationRates"));
        power       = reinterpret_cast<PFN_GetPowerUsage>(resolve(lib, "nvmlDeviceGetPowerUsage"));
        powerLim    = reinterpret_cast<PFN_GetPowerLimit>(resolve(lib, "nvmlDeviceGetEnforcedPowerLimit"));
        temp        = reinterpret_cast<PFN_GetTemperature>(resolve(lib, "nvmlDeviceGetTemperature"));
        memInfo     = reinterpret_cast<PFN_GetMemoryInfo>(resolve(lib, "nvmlDeviceGetMemoryInfo"));

        if (!init || !handle || init() != kNvmlSuccess)
            return;

        // Device 0. Multi-GPU would need to match the Vulkan device by PCI/UUID;
        // for a single-GPU box (the common case) this is correct.
        if (handle(0, &device) != kNvmlSuccess)
            return;

        if (name)
        {
            std::array<char, 96> buf{}; // NVML_DEVICE_NAME_V2_BUFFER_SIZE
            if (name(device, buf.data(), static_cast<unsigned>(buf.size())) == kNvmlSuccess)
                sample.name.assign(buf.data());
        }
        ok = true;
    }

    const GpuTelemetrySample &Poll()
    {
        if (!initialized)
            Initialize();
        if (!ok)
            return sample; // stays valid=false

        const auto now = std::chrono::steady_clock::now();
        if (queried && now - lastQuery < kThrottle)
            return sample;
        lastQuery = now;
        queried   = true;
        ++sample.sequence;

        // Query each metric independently; a driver that doesn't support one
        // (e.g. power on some laptop GPUs) leaves that field at 0 rather than
        // sinking the whole sample.
        unsigned int v = 0;
        if (clockInfo && clockInfo(device, kNvmlClockGraphics, &v) == kNvmlSuccess)
            sample.coreClockMhz = v;
        if (clockInfo && clockInfo(device, kNvmlClockMem, &v) == kNvmlSuccess)
            sample.memClockMhz = v;

        if (NvmlUtilization u{}; util && util(device, &u) == kNvmlSuccess)
        {
            sample.gpuUtilPct = u.gpu;
            sample.memUtilPct = u.memory;
        }
        if (power && power(device, &v) == kNvmlSuccess)
            sample.powerWatts = v / 1000.0;
        if (powerLim && powerLim(device, &v) == kNvmlSuccess)
            sample.powerLimitWatts = v / 1000.0;
        if (temp && temp(device, kNvmlTemperatureGpu, &v) == kNvmlSuccess)
            sample.temperatureC = v;
        if (NvmlMemory m{}; memInfo && memInfo(device, &m) == kNvmlSuccess)
        {
            sample.memUsedBytes  = m.used;
            sample.memTotalBytes = m.total;
        }

        sample.valid = true;
        return sample;
    }

    ~Impl()
    {
        if (ok && shutdown)
            shutdown();
        if (lib)
            libClose(lib);
    }
};

GpuTelemetry::GpuTelemetry() : _impl(std::make_unique<Impl>()) {}
GpuTelemetry::~GpuTelemetry() = default;

const GpuTelemetrySample &GpuTelemetry::Poll()
{
    return _impl->Poll();
}

} // namespace Assisi::Render
