// Adaptateur : processus invité unique à mémoire identité. Couvre ce que les périphériques nvdrv d'Eden
// utilisent : identifiant de programme, mémoire, verrous de table de pages pour le mappage de périphérique (sans objet
// ici : la mémoire invitée est toujours accessible).
#pragma once

#include <cstddef>

#include "common/common_types.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_typed_address.h"
#include "core/hle/result.h"
#include "core/memory.h"

namespace EdenGpu {
extern u64 g_titre; // src/api.cpp (eden_gpu_titre)
}

namespace Kernel {

enum class KMemoryPermission : u32 {
    None = 0,
};

class KPageTable {
public:
    Result LockForMapDeviceAddressSpace(bool* out_is_io, KProcessAddress, size_t, KMemoryPermission, bool, bool) {
        *out_is_io = false;
        return ResultSuccess;
    }
    Result UnlockForDeviceAddressSpace(KProcessAddress, size_t) { return ResultSuccess; }
};

class KProcess {
public:
    explicit KProcess(Core::Memory::Memory& memoire_) : memoire{memoire_} {}
    [[nodiscard]] u64 GetProgramId() const { return EdenGpu::g_titre; }
    [[nodiscard]] Core::Memory::Memory& GetMemory() { return memoire; }
    [[nodiscard]] KPageTable& GetPageTable() { return table; }
    [[nodiscard]] bool IsApplication() const { return true; }

private:
    Core::Memory::Memory& memoire;
    KPageTable table;
};

} // namespace Kernel
