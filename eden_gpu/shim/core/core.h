// Adaptateur : Core::System réduit à ce que video_core et les périphériques nvdrv d'Eden utilisent.
// Le portage natif fournit la mémoire (identité : adresse invitée = adresse « physique »), les événements et la fenêtre ;
// l'implémentation est dans eden_gpu/src/systeme.cpp. Remplace src/core/core.h d'Eden (dossier shim placé avant).
#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "common/common_types.h"

namespace Tegra {
class GPU;
namespace Host1x {
class Host1x;
} // namespace Host1x
} // namespace Tegra

namespace Kernel {
class KernelCore;
} // namespace Kernel

namespace Core::Memory {
class Memory;
} // namespace Core::Memory

namespace Core::Timing {
class CoreTiming;
} // namespace Core::Timing

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Core {

class DeviceMemory;
class PerfStats;

/// Limiteur de vitesse : la cadence est imposée par la présentation du portage
class SpeedLimiter {
public:
    template <class D> void DoSpeedLimiting(D) {}
};

class System {
public:
    System();
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    [[nodiscard]] Tegra::GPU& GPU();
    [[nodiscard]] const Tegra::GPU& GPU() const;
    [[nodiscard]] Tegra::Host1x::Host1x& Host1x();
    [[nodiscard]] const Tegra::Host1x::Host1x& Host1x() const;
    [[nodiscard]] Core::DeviceMemory& DeviceMemory();
    [[nodiscard]] const Core::DeviceMemory& DeviceMemory() const;
    [[nodiscard]] Timing::CoreTiming& CoreTiming();
    [[nodiscard]] const Timing::CoreTiming& CoreTiming() const;
    [[nodiscard]] Kernel::KernelCore& Kernel();
    [[nodiscard]] const Kernel::KernelCore& Kernel() const;
    [[nodiscard]] Core::PerfStats& GetPerfStats();
    [[nodiscard]] const Core::PerfStats& GetPerfStats() const;
    [[nodiscard]] Core::SpeedLimiter& SpeedLimiter() { return limiteur; }
    [[nodiscard]] Core::Memory::Memory& ApplicationMemory();
    [[nodiscard]] const Core::Memory::Memory& ApplicationMemory() const;

    [[nodiscard]] bool IsPoweredOn() const;
    void RegisterHostThread();
    /// Écritures du CPU invité dans des zones mises en cache par le GPU (suivi d'écriture Windows)
    void GatherGPUDirtyMemory(std::function<void(PAddr, size_t)>& callback);
    std::unique_lock<std::mutex> StallApplication();
    void UnstallApplication();

    // --- propre au portage natif ---
    struct Impl;
    std::unique_ptr<Impl> impl;
    Core::SpeedLimiter limiteur;
};

} // namespace Core
