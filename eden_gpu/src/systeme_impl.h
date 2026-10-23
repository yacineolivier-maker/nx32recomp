// État du système adaptateur (partagé entre systeme.cpp et api.cpp).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_event.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"

namespace Core {

struct System::Impl {
    Core::DeviceMemory device_memory;
    std::optional<Core::Memory::Memory> memory;
    Timing::CoreTiming timing;
    Kernel::KernelCore kernel;
    PerfStats perf;
    std::optional<Tegra::Host1x::Host1x> host1x;
    std::optional<Tegra::GPU> gpu;
    std::mutex arret;
    std::atomic<bool> allume{true};
};

} // namespace Core

namespace EdenGpu {
void DefinirBase(uint8_t* base);
}
