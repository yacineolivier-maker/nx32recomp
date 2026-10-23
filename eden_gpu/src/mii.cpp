// Mii : construction des Mii par défaut, de base et aléatoires d'après le code d'Eden (tables du firmware), pour le
// service mii du runtime. CharInfo (0x58 octets) identique à nn::mii::CharInfo.
#include <cstring>

#include "core/hle/service/mii/types/char_info.h"
#include "core/hle/service/mii/types/store_data.h"
#include "eden_gpu.h"

using namespace Service::Mii;

namespace {
void copier(const StoreData& sd, uint8_t* sortie) {
    CharInfo ci{};
    ci.SetFromStoreData(sd);
    static_assert(sizeof(CharInfo) == 0x58);
    std::memcpy(sortie, &ci, sizeof ci);
}
} // namespace

extern "C" {

EDEN_API int eden_mii_defaut(uint32_t index, uint8_t* sortie) {
    if (index >= 6 || !sortie) return 0;
    StoreData sd{};
    sd.BuildDefault(index);
    copier(sd, sortie);
    return 1;
}

EDEN_API int eden_mii_base(uint32_t genre, uint8_t* sortie) {
    if (!sortie) return 0;
    StoreData sd{};
    sd.BuildBase(static_cast<Gender>(genre > 1 ? 0 : genre));
    copier(sd, sortie);
    return 1;
}

EDEN_API int eden_mii_aleatoire(uint32_t age, uint32_t genre, uint32_t race, uint8_t* sortie) {
    if (!sortie) return 0;
    StoreData sd{};
    sd.BuildRandom(static_cast<Age>(age), static_cast<Gender>(genre), static_cast<Race>(race));
    copier(sd, sortie);
    return 1;
}

EDEN_API uint32_t eden_mii_verifier(const uint8_t* charinfo) {
    CharInfo ci{};
    std::memcpy(&ci, charinfo, sizeof ci);
    return static_cast<uint32_t>(ci.Verify());
}

} // extern "C"
