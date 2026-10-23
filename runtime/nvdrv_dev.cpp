/* Périphériques nvdrv simples (d'après Eden, src/core/hle/service/nvdrv/devices) :
 * /dev/nvhost-ctrl-gpu (caractéristiques du GPU GM20B, ZCull, ZBC, masques) et /dev/nvhost-ctrl (configuration ;
 * points de synchronisation et événements à venir avec l'émulation GPU). */
#include "nvdrv.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

template <class T> T lire_struct(const std::vector<uint8_t> &in) {
    T v{};
    memcpy(&v, in.data(), std::min(sizeof(T), in.size()));
    return v;
}
template <class T> void ecrire_struct(std::vector<uint8_t> &out, const T &v) {
    if (out.size() < sizeof(T)) out.resize(sizeof(T));
    memcpy(out.data(), &v, sizeof(T));
}

#pragma pack(push, 4)
struct CaracteristiquesGpu { /* IoctlGpuCharacteristics (160 octets) */
    uint32_t arch, impl, rev, num_gpc;
    uint64_t l2_cache_size, on_board_video_memory_size;
    uint32_t num_tpc_per_gpc, bus_type, big_page_size, compression_page_size, pde_coverage_bit_count, available_big_page_sizes, gpc_mask,
        sm_arch_sm_version, sm_arch_spa_version, sm_arch_warp_count, gpu_va_bit_count, reserved;
    uint64_t flags;
    uint32_t twod_class, threed_class, compute_class, gpfifo_class, inline_to_memory_class, dma_copy_class, max_fbps_count, fbp_en_mask,
        max_ltc_per_fbp, max_lts_per_ltc, max_tex_per_tpc, max_gpc_count, rop_l2_en_mask_0, rop_l2_en_mask_1;
    uint64_t chipname, gr_compbit_store_base_hw;
};
static_assert(sizeof(CaracteristiquesGpu) == 160, "IoctlGpuCharacteristics");
struct IoctlCaracteristiques {
    uint64_t taille_tampon, adresse_tampon;
    CaracteristiquesGpu gc;
};
static_assert(sizeof(IoctlCaracteristiques) == 176, "IoctlCharacteristics");
#pragma pack(pop)

CaracteristiquesGpu gm20b() {
    CaracteristiquesGpu g{};
    g.arch = 0x120; g.impl = 0xB; g.rev = 0xA1; g.num_gpc = 1;
    g.l2_cache_size = 0x40000; g.num_tpc_per_gpc = 2; g.bus_type = 0x20;
    g.big_page_size = 0x20000; g.compression_page_size = 0x20000; g.pde_coverage_bit_count = 0x1B; g.available_big_page_sizes = 0x30000;
    g.gpc_mask = 1; g.sm_arch_sm_version = 0x503; g.sm_arch_spa_version = 0x503; g.sm_arch_warp_count = 0x80; g.gpu_va_bit_count = 0x28;
    g.flags = 0x55;
    g.twod_class = 0x902D; g.threed_class = 0xB197; g.compute_class = 0xB1C0; g.gpfifo_class = 0xB06F; g.inline_to_memory_class = 0xA140;
    g.dma_copy_class = 0xB0B5;
    g.max_fbps_count = 1; g.max_ltc_per_fbp = 2; g.max_lts_per_ltc = 1; g.max_gpc_count = 1; g.rop_l2_en_mask_0 = 0x21D70;
    g.chipname = 0x6230326D67ull; /* « gm20b » */
    return g;
}

struct DevCtrlGpu : NvDev {
    uint32_t ev_erreur_w = 0, ev_erreur_r = 0, ev_inconnu_w = 0, ev_inconnu_r = 0;
    DevCtrlGpu() {
        ev_erreur_w = svc_new_event(0, &ev_erreur_r);
        ev_inconnu_w = svc_new_event(0, &ev_inconnu_r);
    }
    uint32_t ioctl(NvIoctl cmd, const std::vector<uint8_t> &in, std::vector<uint8_t> &out, const std::vector<uint8_t> &,
                   std::vector<uint8_t> &out2) override {
        if (cmd.groupe() != 'G') return NV_NON_IMPLEMENTE;
        switch (cmd.numero()) {
        case 0x01: { uint32_t v = 1; ecrire_struct(out, v); return NV_SUCCES; } /* ZCullGetCtxSize */
        case 0x02: { /* ZCullGetInfo */
            uint32_t v[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            ecrire_struct(out, v);
            return NV_SUCCES;
        }
        case 0x03: return NV_SUCCES; /* ZBCSetTable : couleurs/profondeurs d'effacement rapide (sans effet ici) */
        case 0x04: return NV_PARAMETRE_INVALIDE; /* ZBCQueryTable : table vide */
        case 0x05: { /* GetCharacteristics (Ioctl1, ou Ioctl3 avec copie en ligne) */
            IoctlCaracteristiques p = lire_struct<IoctlCaracteristiques>(in);
            p.gc = gm20b();
            p.taille_tampon = sizeof(CaracteristiquesGpu);
            p.adresse_tampon = 0xDEADBEEF; /* ne doit pas être nul */
            ecrire_struct(out, p);
            if (out2.size() >= sizeof(CaracteristiquesGpu)) memcpy(out2.data(), &p.gc, sizeof p.gc);
            return NV_SUCCES;
        }
        case 0x06: { /* GetTPCMasks : {u32 taille, u32 pad, u64 adresse, u32 masque, u32 pad} */
            uint32_t p[6] = {};
            memcpy(p, in.data(), std::min(sizeof p, in.size()));
            if (p[0]) p[4] = 3;
            ecrire_struct(out, p);
            if (out2.size() >= 4) memcpy(out2.data(), &p[4], 4);
            return NV_SUCCES;
        }
        case 0x07: return NV_SUCCES; /* FlushL2 */
        case 0x14: { uint32_t v[2] = {7, 1}; ecrire_struct(out, v); return NV_SUCCES; } /* ZBC GetActiveSlotMask */
        case 0x15: { uint32_t v = 100; ecrire_struct(out, v); return NV_SUCCES; }       /* PmuGetGpuLoad */
        case 0x1C: { /* GetGpuTime (ns) */
            uint64_t v[2] = {(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), 0};
            ecrire_struct(out, v);
            return NV_SUCCES;
        }
        }
        fprintf(stderr, "[nvdrv] nvhost-ctrl-gpu : ioctl G-%02X non implémenté\n", cmd.numero());
        return NV_NON_IMPLEMENTE;
    }
    uint32_t evenement(uint32_t id, uint32_t *h) override {
        if (id == 1) { *h = ev_erreur_r; return NV_SUCCES; }
        if (id == 2) { *h = ev_inconnu_r; return NV_SUCCES; }
        return NV_VALEUR_INVALIDE;
    }
};

struct DevCtrl : NvDev {
    uint32_t ioctl(NvIoctl cmd, const std::vector<uint8_t> &in, std::vector<uint8_t> &, const std::vector<uint8_t> &,
                   std::vector<uint8_t> &) override {
        if (cmd.groupe() == 0 && cmd.numero() == 0x1B) { /* NvOsGetConfigU32 : absent en production */
            static int n = 0;
            if (n++ < 12 && in.size() >= 0x82)
                fprintf(stderr, "[nvdrv] nvhost-ctrl GetConfig(\"%.65s\", \"%.65s\") -> absent\n", (const char *)in.data(), (const char *)in.data() + 0x41);
            return 0x30006; /* ConfigVarNotFound */
        }
        fprintf(stderr, "[nvdrv] nvhost-ctrl : ioctl %02X-%02X non implémenté (%u octets)\n", cmd.groupe(), cmd.numero(), cmd.taille());
        return NV_NON_IMPLEMENTE;
    }
};

struct Enregistrement {
    Enregistrement() {
        nvdrv_enregistrer_peripherique("/dev/nvhost-ctrl-gpu", [] { return std::make_shared<DevCtrlGpu>(); });
        nvdrv_enregistrer_peripherique("/dev/nvhost-ctrl", [] { return std::make_shared<DevCtrl>(); });
    }
} g_enregistrement;

} // namespace
