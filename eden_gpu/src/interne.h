// Partagé entre les fichiers de eden_gpu.dll : rappels du runtime, système unique.
#pragma once

#include <cstdarg>
#include <cstdint>

#include "eden_gpu.h"

namespace Core {
class System;
}

namespace Service::Nvidia {
class Module;
}

namespace EdenGpu {

extern EdenRappels g_rappels; ///< fournis par le runtime à eden_gpu_init
Core::System& Systeme();
void Journal(const char* format, ...);

/// Cohérence CPU -> GPU : pages invitées dont le GPU garde une copie (textures, tampons), surveillées par
/// GetWriteWatch (g_mem réservé avec MEM_WRITE_WATCH par le runtime).
void MarquerEnCache(uint64_t adresse, uint64_t taille, bool en_cache);
template <class F> void CollecterEcritures(F&& rappel);
void CollecterEcrituresFn(void (*rappel)(void* ctx, uint64_t adresse, uint64_t taille), void* ctx);
void EcritureHote(uint64_t adresse, uint64_t taille);
bool SuiviParWriteWatch();

/// Écran et files de tampons (vi) : affichage.cpp
void AffichageInit(Core::System& systeme, Service::Nvidia::Module& module);
void AffichageArreter();

} // namespace EdenGpu
