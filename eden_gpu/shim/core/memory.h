// Adaptateur : mémoire du processus invité (identité : pointeur hôte = g_mem + adresse invitée). Les zones mises en cache
// par le GPU sont suivies pour la cohérence CPU -> GPU (implémentation : eden_gpu/src/systeme.cpp).
#pragma once

#include <cstddef>

#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/typed_address.h"

namespace Core::Memory {

constexpr std::size_t YUZU_PAGEBITS = 12;
constexpr u64 YUZU_PAGESIZE = 1ULL << YUZU_PAGEBITS;
constexpr u64 YUZU_PAGEMASK = YUZU_PAGESIZE - 1;

class Memory {
public:
    explicit Memory(u8* base_) : base{base_} {}
    [[nodiscard]] u8* GetPointerSilent(Common::ProcessAddress vaddr) const { return base + GetInteger(vaddr); }
    [[nodiscard]] u8* GetPointer(Common::ProcessAddress vaddr) const { return base + GetInteger(vaddr); }
    /// Le GPU met en cache [vaddr, vaddr+size) (cached) ou cesse de le faire
    void RasterizerMarkRegionCached(Common::ProcessAddress vaddr, u64 size, bool cached);
    bool ReadBlock(Common::ProcessAddress src_addr, void* dest_buffer, std::size_t size) const;
    bool WriteBlock(Common::ProcessAddress dest_addr, const void* src_buffer, std::size_t size);
    [[nodiscard]] u8* Base() const { return base; }

private:
    u8* base;
};

} // namespace Core::Memory
