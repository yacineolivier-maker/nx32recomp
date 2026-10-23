// Adaptateur : Core::Timing::CoreTiming réduit à l'horloge (ticks GPU à 614,4 MHz, temps global).
#pragma once

#include <chrono>

#include "common/common_types.h"

namespace Core::Timing {

class CoreTiming {
public:
    /// Ticks du GPU de la console (614,4 MHz) depuis le démarrage
    [[nodiscard]] u64 GetGPUTicks() const;
    /// Ticks du CPU (19,2 MHz) depuis le démarrage
    [[nodiscard]] u64 GetClockTicks() const;
    [[nodiscard]] std::chrono::nanoseconds GetGlobalTimeNs() const noexcept;
    [[nodiscard]] std::chrono::microseconds GetGlobalTimeUs() const;
};

} // namespace Core::Timing
