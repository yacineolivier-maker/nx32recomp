// Adaptateur : mémoire « physique » d'Eden = espace invité du portage (identité). La base est le pointeur hôte g_mem du
// runtime : adresse physique brute (PAddr) = adresse invitée 32 bits, toujours < 4 Gio (mode Memory_4Gb du SMMU).
#pragma once

#include <cstdint>

#include "common/common_types.h"
#include "common/typed_address.h"

namespace Core {

namespace DramMemoryMap {
enum : u64 {
    Base = 0x80000000ULL,
    KernelReserveBase = Base + 0x60000,
    SlabHeapBase = KernelReserveBase + 0x85000,
};
} // namespace DramMemoryMap

struct BaseIdentite {
    u8* base = nullptr;
    [[nodiscard]] u8* BackingBasePointer() noexcept { return base; }
    [[nodiscard]] const u8* BackingBasePointer() const noexcept { return base; }
};

class DeviceMemory {
public:
    template <typename T>
    PAddr GetRawPhysicalAddr(const T* ptr) const {
        return static_cast<PAddr>(reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(buffer.BackingBasePointer()));
    }
    template <typename T>
    T* GetPointerFromRaw(PAddr addr) {
        return reinterpret_cast<T*>(buffer.BackingBasePointer() + addr);
    }
    template <typename T>
    const T* GetPointerFromRaw(PAddr addr) const {
        return reinterpret_cast<const T*>(buffer.BackingBasePointer() + addr);
    }
    BaseIdentite buffer;
};

} // namespace Core
