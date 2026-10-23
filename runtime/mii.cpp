/* mii : base de données vide + 6 Mii par défaut. Interfaces : https://switchbrew.org/wiki/Shared_Database_services
 * Les Mii (par défaut, de base, aléatoires) sont construits par le code d'Eden (tables du firmware, eden_gpu.dll) : les
 * anciens Mii maison (couleurs sans conversion « ver3 », identifiant non RFC 4122 v4) n'étaient pas dessinés. */
#include "ipc.h"

#include <cstdio>

#include "eden_gpu.h"

/* Mii personnel du compte (base « utilisateur », un seul Mii) : son identifiant est donné au jeu par le compte
 * (acc UserData.miiId, account.cpp). Le jeu l'utilise directement au lieu de demander « Choisissez un Mii ». */
const char *pseudo(); /* account.cpp */
extern const uint8_t g_mii_perso_id[16] = {0x6D, 0x6B, 0x38, 0x64, 0x78, 0x2D, 0x4A, 0x6F,  /* octet 6 : version 4 */
                                           0x80, 0x75, 0x65, 0x75, 0x72, 0x30, 0x30, 0x31}; /* octet 8 : variante RFC 4122 */

namespace {

#pragma pack(push, 1)
struct CharInfo { /* 0x58 octets */
    uint8_t create_id[16];
    uint16_t nickname[11];
    uint8_t font_region, favorite_color, gender, height, build, type, region_move;
    uint8_t faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    uint8_t hair_type, hair_color, hair_flip;
    uint8_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint8_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint8_t nose_type, nose_scale, nose_y;
    uint8_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint8_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint8_t glasses_type, glasses_color, glasses_scale, glasses_y;
    uint8_t mole_type, mole_scale, mole_x, mole_y;
    uint8_t padding;
};
#pragma pack(pop)
static_assert(sizeof(CharInfo) == 0x58, "CharInfo");

CharInfo mii_maison(uint32_t i) {
    CharInfo c{};
    c.create_id[0] = 0x4D; c.create_id[1] = 0x4B; c.create_id[15] = (uint8_t)(i + 1);
    c.create_id[8] = 0x80; /* variante RFC 4122 */
    const char *nom = "Mii";
    for (int k = 0; nom[k]; k++) c.nickname[k] = (uint16_t)nom[k];
    c.favorite_color = (uint8_t)(i % 12);
    c.gender = (uint8_t)(i % 2);
    c.height = 64; c.build = 64;
    c.faceline_color = (uint8_t)(i % 6);
    c.hair_type = (uint8_t)(i % 2 ? 12 : 33); c.hair_color = 1;
    c.eye_type = 2; c.eye_scale = 4; c.eye_aspect = 3; c.eye_rotate = 4; c.eye_x = 2; c.eye_y = 12;
    c.eyebrow_type = 6; c.eyebrow_color = 1; c.eyebrow_scale = 4; c.eyebrow_aspect = 3; c.eyebrow_rotate = 6;
    c.eyebrow_x = 2; c.eyebrow_y = 10;
    c.nose_type = 1; c.nose_scale = 4; c.nose_y = 9;
    c.mouth_type = 23; c.mouth_scale = 4; c.mouth_aspect = 3; c.mouth_y = 13;
    c.mustache_scale = 4; c.mustache_y = 10;
    c.glasses_scale = 4; c.glasses_y = 10;
    c.mole_scale = 4; c.mole_x = 2; c.mole_y = 20;
    return c;
}
/* Mii par défaut n° i (0 à 5) du firmware, construit comme Eden */
CharInfo mii_defaut(uint32_t i) {
    CharInfo c{};
    if (!eden_mii_defaut(i % 6, reinterpret_cast<uint8_t *>(&c))) c = mii_maison(i);
    static bool verifie = false;
    if (!verifie) { /* diagnostic : validité selon nn::mii (CharInfo::Verify d'Eden) */
        verifie = true;
        CharInfo m = mii_maison(0);
        fprintf(stderr, "[mii] validité : Mii par défaut d'Eden %u, ancien Mii maison %u (0 = valide)\n",
                eden_mii_verifier(reinterpret_cast<const uint8_t *>(&c)), eden_mii_verifier(reinterpret_cast<const uint8_t *>(&m)));
    }
    return c;
}
constexpr uint32_t k_nb_defaut = 6;
constexpr uint32_t SOURCE_DATABASE = 1, SOURCE_DEFAULT = 2;

/* Mii personnel : premier Mii par défaut du firmware, identifiant et pseudonyme du compte */
CharInfo mii_perso() {
    CharInfo c = mii_defaut(0);
    memcpy(c.create_id, g_mii_perso_id, 16);
    memset(c.nickname, 0, sizeof c.nickname);
    for (int k = 0; pseudo()[k] && k < 10; k++) c.nickname[k] = (uint8_t)pseudo()[k];
    return c;
}

void push_charinfo(Response &rs, const CharInfo &c) {
    const uint8_t *p = (const uint8_t *)&c;
    rs.data.insert(rs.data.end(), p, p + sizeof c);
}

std::shared_ptr<Service> database() {
    auto s = std::make_shared<TableService>("mii:IDatabaseService");
    s->cmds[0] = [](Request &, Response &rs) { rs.push<uint8_t>(0); }; /* IsUpdated */
    s->cmds[1] = [](Request &, Response &rs) { rs.push<uint8_t>(0); }; /* IsFullDatabase */
    s->cmds[2] = [](Request &rq, Response &rs) {                         /* GetCount(source) */
        uint32_t src = rq.pop<uint32_t>();
        rs.push<uint32_t>(((src & SOURCE_DATABASE) ? 1 : 0) + ((src & SOURCE_DEFAULT) ? k_nb_defaut : 0));
    };
    auto get = [](bool element) {
        return [element](Request &rq, Response &rs) { /* Get / Get1 (source, B tableau) -> nombre */
            uint32_t src = rq.pop<uint32_t>();
            Buf b = rq.out();
            uint32_t taille = element ? 0x5C : 0x58;
            uint32_t n = 0;
            if ((src & SOURCE_DATABASE) && taille <= b.size) { /* base utilisateur d'abord : le Mii personnel */
                CharInfo c = mii_perso();
                memcpy(g_mem + b.addr, &c, sizeof c);
                if (element) wr32(b.addr + 0x58, 0); /* source : base */
                n = 1;
            }
            if (src & SOURCE_DEFAULT)
                for (uint32_t d = 0; d < k_nb_defaut && (n + 1) * taille <= b.size; d++, n++) {
                    CharInfo c = mii_defaut(d);
                    memcpy(g_mem + b.addr + n * taille, &c, sizeof c);
                    if (element) wr32(b.addr + n * taille + 0x58, 1); /* source : défaut */
                }
            fprintf(stderr, "[mii] Get%s(source %u) -> %u Mii\n", element ? "" : "1", src, n);
            rs.push<uint32_t>(n);
        };
    };
    s->cmds[3] = get(true);
    s->cmds[4] = get(false);
    s->cmds[5] = [](Request &rq, Response &rs) { /* UpdateLatest : renvoie le Mii tel quel */
        CharInfo c;
        for (size_t k = 0; k < sizeof c; k++) ((uint8_t *)&c)[k] = rq.pop<uint8_t>();
        push_charinfo(rs, c);
    };
    s->cmds[6] = [](Request &rq, Response &rs) { /* BuildRandom(âge, genre, origine) */
        uint32_t age = rq.pop<uint32_t>(), genre = rq.pop<uint32_t>(), race = rq.pop<uint32_t>();
        CharInfo c{};
        if (!eden_mii_aleatoire(age, genre, race, reinterpret_cast<uint8_t *>(&c))) c = mii_maison(0);
        push_charinfo(rs, c);
    };
    s->cmds[7] = [](Request &rq, Response &rs) { push_charinfo(rs, mii_defaut(rq.pop<uint32_t>() % k_nb_defaut)); }; /* BuildDefault */
    s->cmds[11] = [](Request &rq, Response &rs) { /* FindIndex(create_id, spécial) : le Mii personnel est à l'indice 0 */
        uint8_t id[16];
        for (auto &o : id) o = rq.pop<uint8_t>();
        rs.push<int32_t>(memcmp(id, g_mii_perso_id, 16) == 0 ? 0 : -1);
    };
    s->cmds[21] = [](Request &rq, Response &rs) { /* GetIndex(CharInfo) */
        CharInfo c;
        for (size_t k = 0; k < sizeof c; k++) ((uint8_t *)&c)[k] = rq.pop<uint8_t>();
        rs.push<int32_t>(memcmp(c.create_id, g_mii_perso_id, 16) == 0 ? 0 : -1);
    };
    s->cmds[22] = [](Request &, Response &) {};                          /* SetInterfaceVersion */
    return s;
}

} // namespace

void mii_register() {
    auto f = [] {
        auto s = std::make_shared<TableService>("mii");
        s->cmds[0] = [](Request &, Response &rs) { rs.objects.push_back(database()); }; /* GetDatabaseService */
        return s;
    };
    ipc_register("mii:e", f);
    ipc_register("mii:u", f);
}
