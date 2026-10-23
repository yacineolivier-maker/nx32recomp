/* Service nvdrv et interface des périphériques /dev/nv* (pilote NVN d'origine, cf. nvdrv.cpp) */
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ipc.h"

/* NvResult (Eden nvdata.h) */
enum : uint32_t {
    NV_SUCCES = 0x0,
    NV_NON_IMPLEMENTE = 0x1,
    NV_NON_PRIS_EN_CHARGE = 0x2,
    NV_NON_INITIALISE = 0x3,
    NV_PARAMETRE_INVALIDE = 0x4,
    NV_DELAI = 0x5,
    NV_MEMOIRE_INSUFFISANTE = 0x6,
    NV_ETAT_INVALIDE = 0x8,
    NV_ADRESSE_INVALIDE = 0x9,
    NV_TAILLE_INVALIDE = 0xA,
    NV_VALEUR_INVALIDE = 0xB,
    NV_DEJA_ALLOUE = 0xD,
    NV_OCCUPE = 0xE,
};

/* Numéro d'ioctl : n° (bits 0-7), groupe (8-15), taille (16-29), entrée (30), sortie (31) */
struct NvIoctl {
    uint32_t raw;
    uint32_t numero() const { return raw & 0xFF; }
    uint32_t groupe() const { return (raw >> 8) & 0xFF; }
    uint32_t taille() const { return (raw >> 16) & 0x3FFF; }
    bool entree() const { return (raw >> 30) & 1; }
    bool sortie() const { return (raw >> 31) & 1; }
};

struct NvDev {
    virtual ~NvDev() = default;
    /* in/out : tampons principaux ; in2 (Ioctl2) / out2 (Ioctl3) : tampons « en ligne » supplémentaires */
    virtual uint32_t ioctl(NvIoctl cmd, const std::vector<uint8_t> &in, std::vector<uint8_t> &out, const std::vector<uint8_t> &in2,
                           std::vector<uint8_t> &out2) = 0;
    virtual uint32_t evenement(uint32_t id, uint32_t *handle) { (void)id; (void)handle; return NV_VALEUR_INVALIDE; }
    /* numéro de fd imposé (>= 0) : celui de l'émulation GPU d'Eden, que le jeu repasse dans certains ioctl (BindChannel) */
    virtual int32_t fd_impose() const { return -1; }
};

using NvFabrique = std::function<std::shared_ptr<NvDev>()>;
void nvdrv_enregistrer_peripherique(const std::string &chemin, NvFabrique f);
void nvdrv_register();
