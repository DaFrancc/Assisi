/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/GpuTelemetry.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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

// --- Field-value API (nvmlDeviceGetFieldValues) ---------------------------
// A laptop GPU that returns NVML_ERROR_NOT_SUPPORTED from nvmlDeviceGetPowerUsage
// often still reports power through this newer field API. These mirror the nvml.h
// ABI exactly: nvmlFieldValue_t is a 40-byte struct, identical on LP64 and LLP64
// because the value union is 8 bytes on both. timestamp/latencyUsec are opaque to
// us (we never read them), so they are plain 64-bit words rather than nvml.h's
// signed long long.
constexpr unsigned int kNvmlFiDevPowerAverage = 185; // NVML_FI_DEV_POWER_AVERAGE (mW, ~1s window)
constexpr unsigned int kNvmlFiDevPowerInstant = 186; // NVML_FI_DEV_POWER_INSTANT (mW, current)
constexpr unsigned int kNvmlPowerScopeGpu     = 0;   // NVML_POWER_SCOPE_GPU (whole-GPU power)

union NvmlValue
{
    double dVal;         // forces the 8-byte size/alignment of the real nvmlValue_t union
    std::uint64_t u64;   //   "
    std::uint32_t uiVal; // instant/average power is an unsigned int in mW — the member we read
};
struct NvmlFieldValue
{
    std::uint32_t fieldId;
    std::uint32_t scopeId;
    std::uint64_t timestamp;   // nvml.h: signed long long; opaque to us
    std::uint64_t latencyUsec; // nvml.h: signed long long; opaque to us
    std::int32_t valueType;    // nvmlValueType_t
    std::int32_t nvmlReturn;   // nvmlReturn_t
    NvmlValue value;
};
using PFN_GetFieldValues = int (*)(nvmlDevice_t, int, NvmlFieldValue *);

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

// Resolve `base`+"_v2" first, falling back to plain `base`: several NVML calls
// only exist as versioned symbols on modern drivers, while a few are unversioned.
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
    // --- Worker-thread-owned NVML state. Touched only by Run() (Initialize +
    // QueryOnce + Cleanup), never from the main thread, so it needs no locking. ---
    LibHandle lib    = nullptr;
    nvmlDevice_t device = nullptr;
    bool ok     = false;         // NVML up and a device handle acquired

    PFN_Shutdown shutdown  = nullptr;
    PFN_GetClockInfo clockInfo = nullptr;
    PFN_GetUtilizationRates util      = nullptr;
    PFN_GetPowerUsage power     = nullptr;
    PFN_GetPowerLimit powerLim  = nullptr;
    PFN_GetTemperature temp      = nullptr;
    PFN_GetMemoryInfo memInfo   = nullptr;
    PFN_GetFieldValues getFieldValues = nullptr;

    // How to read power draw, decided once in Initialize() and then replayed every
    // query: legacy nvmlDeviceGetPowerUsage is unsupported on many laptop GPUs,
    // where the field-value API often works instead (or neither of them, -> N/A).
    enum class PowerSource : std::uint8_t
    {
        None,
        Legacy,
        Field
    };
    PowerSource powerSource  = PowerSource::None;
    unsigned int powerFieldId = kNvmlFiDevPowerInstant; // which field id worked, when Field

    GpuTelemetrySample working; // the worker's scratch sample (name + accumulating fields + sequence)

    // --- Shared between the worker and the main (Poll) thread, under `mutex`. -----
    std::mutex mutex;
    std::condition_variable wake;          // lets the dtor cut the worker's sleep short
    bool stop   = false;
    GpuTelemetrySample shared;             // latest sample the worker has published

    // --- Main-thread-only state. -------------------------------------------------
    std::thread worker;
    bool started = false;
    GpuTelemetrySample cached;             // what Poll() returns a reference to

    // Open NVML, resolve the entry points, and grab device 0's handle + name.
    // Runs on the worker thread, so its ~100ms cost never lands on a frame.
    void Initialize()
    {
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
        getFieldValues = reinterpret_cast<PFN_GetFieldValues>(resolve(lib, "nvmlDeviceGetFieldValues"));

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
                working.name.assign(buf.data());
        }
        ok = true;

        // Pick the power-draw source once — support is fixed for the device's
        // lifetime, so we never re-probe per query.
        DeterminePowerSource();
    }

    // Read GPU power (milliwatts) via the field-value API into `outMilliwatts`.
    // Worker thread only. Returns false if the call or the per-field status fails.
    bool ReadFieldPower(unsigned int fieldId, unsigned int &outMilliwatts)
    {
        if (!getFieldValues)
            return false;
        NvmlFieldValue fv{};
        fv.fieldId = fieldId;
        fv.scopeId = kNvmlPowerScopeGpu;
        if (getFieldValues(device, 1, &fv) != kNvmlSuccess || fv.nvmlReturn != kNvmlSuccess)
            return false;
        outMilliwatts = fv.value.uiVal;
        return true;
    }

    // Prefer the legacy call, fall back to the field API (instant, then averaged),
    // else leave power unsupported. Called once from Initialize().
    void DeterminePowerSource()
    {
        unsigned int milliwatts = 0;
        if (power && power(device, &milliwatts) == kNvmlSuccess)
        {
            powerSource = PowerSource::Legacy;
        }
        else if (ReadFieldPower(kNvmlFiDevPowerInstant, milliwatts))
        {
            powerSource  = PowerSource::Field;
            powerFieldId = kNvmlFiDevPowerInstant;
        }
        else if (ReadFieldPower(kNvmlFiDevPowerAverage, milliwatts))
        {
            powerSource  = PowerSource::Field;
            powerFieldId = kNvmlFiDevPowerAverage;
        }
        else
        {
            powerSource = PowerSource::None;
        }
        working.powerSupported = (powerSource != PowerSource::None);
    }

    // Query every metric once into `working` and bump its sequence. Worker thread
    // only; these are the blocking driver calls we moved off the render thread.
    void QueryOnce()
    {
        ++working.sequence;

        // Query each metric independently; a driver that doesn't support one
        // (e.g. power on some laptop GPUs) leaves that field at 0 rather than
        // sinking the whole sample.
        unsigned int v = 0;
        if (clockInfo && clockInfo(device, kNvmlClockGraphics, &v) == kNvmlSuccess)
            working.coreClockMhz = v;
        if (clockInfo && clockInfo(device, kNvmlClockMem, &v) == kNvmlSuccess)
            working.memClockMhz = v;

        if (NvmlUtilization u{}; util &&util(device, &u) == kNvmlSuccess)
        {
            working.gpuUtilPct = u.gpu;
            working.memUtilPct = u.memory;
        }
        switch (powerSource)
        {
        case PowerSource::Legacy:
            if (power(device, &v) == kNvmlSuccess)
                working.powerWatts = v / 1000.0;
            break;
        case PowerSource::Field:
            if (ReadFieldPower(powerFieldId, v))
                working.powerWatts = v / 1000.0;
            break;
        case PowerSource::None:
            break;
        }
        if (powerLim && powerLim(device, &v) == kNvmlSuccess)
            working.powerLimitWatts = v / 1000.0;
        if (temp && temp(device, kNvmlTemperatureGpu, &v) == kNvmlSuccess)
            working.temperatureC = v;
        if (NvmlMemory m{}; memInfo &&memInfo(device, &m) == kNvmlSuccess)
        {
            working.memUsedBytes  = m.used;
            working.memTotalBytes = m.total;
        }

        working.valid = true;
    }

    void Cleanup()
    {
        if (ok && shutdown)
            shutdown();
        if (lib)
            libClose(lib);
        lib = nullptr;
    }

    // Background loop: initialise NVML, then re-query every kThrottle and publish
    // the result under `mutex`, until the dtor sets `stop`. If NVML isn't
    // available we clean up and exit; `shared` stays valid=false so Poll() reports
    // "unavailable". The kThrottle sleep is a cv wait so shutdown is prompt rather
    // than blocking on a full interval.
    void Run()
    {
        Initialize();
        if (!ok)
        {
            Cleanup();
            return;
        }

        while (true)
        {
            QueryOnce();
            {
                std::lock_guard<std::mutex> lock(mutex);
                shared = working;
            }

            std::unique_lock<std::mutex> lock(mutex);
            if (wake.wait_for(lock, kThrottle, [this] { return stop; }))
                break; // stop requested
        }

        Cleanup();
    }

    const GpuTelemetrySample &Poll()
    {
        // Spin the worker up on first use, so an app that never opens the overlay
        // never starts the thread or pays the NVML init cost.
        if (!started)
        {
            started = true;
            worker  = std::thread(&Impl::Run, this);
        }

        // Copy the worker's latest sample only when it actually advanced, so the
        // steady state is a lock + one integer compare with no per-frame heap
        // allocation (the sample carries a std::string name). The returned
        // reference stays valid until the next Poll().
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (shared.sequence != cached.sequence)
                cached = shared;
        }
        return cached;
    }

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join(); // the worker runs Cleanup() (nvmlShutdown / FreeLibrary) as it exits
    }
};

GpuTelemetry::GpuTelemetry() : _impl(std::make_unique<Impl>()) {}
GpuTelemetry::~GpuTelemetry() = default;

const GpuTelemetrySample &GpuTelemetry::Poll()
{
    return _impl->Poll();
}

} // namespace Assisi::Render
