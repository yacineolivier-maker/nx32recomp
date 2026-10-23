/* pl:u — polices partagées du système (extraites par tools/romfs.py dans <données>/polices).
 * Placement en mémoire partagée identique aux émulateurs : clé dérivée du premier mot, mots inversés,
 * taille re-chiffrée ; la région annoncée commence après l'en-tête de 8 octets. */
#include "ipc.h"

#include <cstdio>
#include <mutex>
#include <vector>

extern std::string g_donnees_dir; /* fs.cpp */

namespace {

constexpr uint32_t TAILLE_SHMEM = 0x1100000;
constexpr uint32_t RESULTAT_ATTENDU = 0x7F9A0218; /* premier mot d'un BFTTF déchiffré */

const char *k_polices[] = {"nintendo_udsg-r_std_003.bfttf",     "nintendo_udsg-r_org_zh-cn_003.bfttf",
                           "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udjxh-db_zh-tw_003.bfttf",
                           "nintendo_udsg-r_ko_003.bfttf",        "nintendo_ext_003.bfttf",
                           "nintendo_ext2_003.bfttf"};
constexpr uint32_t k_nb = sizeof(k_polices) / sizeof(k_polices[0]);

struct Region { uint32_t offset = 0, size = 0; };
uint32_t g_shmem;
Region g_regions[k_nb];
std::once_flag g_once;
bool g_ok = false;

uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }

/* Construit l'image de la mémoire partagée (côté hôte) ; elle est copiée à chaque mappage par le jeu */
std::vector<uint8_t> &image() {
    static std::vector<uint8_t> img(TAILLE_SHMEM, 0);
    return img;
}


void charger() {
    uint32_t off = 0;
    for (uint32_t i = 0; i < k_nb; i++) {
        char chemin[260];
        snprintf(chemin, sizeof chemin, "%s/polices/%s", g_donnees_dir.c_str(), k_polices[i]);
        FILE *f = fopen(chemin, "rb");
        if (!f) { fprintf(stderr, "[pl] police absente : %s\n", chemin); continue; }
        std::vector<uint32_t> mots;
        uint32_t w;
        while (fread(&w, 4, 1, f) == 1) mots.push_back(w);
        fclose(f);
        if (mots.size() < 2 || off + mots.size() * 4 > TAILLE_SHMEM) continue;
        for (auto &m : mots) m = bswap(m); /* les mots du BFTTF se lisent en big-endian */
        uint32_t cle = mots[0] ^ RESULTAT_ATTENDU; /* = 0x49621806 */
        std::vector<uint32_t> t(mots.size());
        for (size_t k = 0; k < mots.size(); k++) t[k] = bswap(mots[k] ^ cle);
        t[1] = bswap(t[1]) ^ cle;
        memcpy(image().data() + off, t.data(), t.size() * 4);
        g_regions[i] = {off + 8, (uint32_t)(mots.size() * 4 - 8)};
        off += (uint32_t)(mots.size() * 4);
    }
    g_ok = true;
    svc_shmem_set_init(g_shmem, image().data());
    fprintf(stderr, "[pl] %u polices chargées (%u octets)\n", k_nb, off);
}

/* La mémoire partagée est remplie dès qu'elle est mappée par le jeu (voir pl_shmem_mapped) */
void remplir_si_mappee() {
    uint32_t a = svc_shmem_addr(g_shmem);
    static uint32_t deja = 0;
    if (a && a != deja) {
        memcpy(g_mem + a, image().data(), TAILLE_SHMEM);
        deja = a;
    }
}

} // namespace

void pl_register() {
    g_shmem = svc_new_shmem(TAILLE_SHMEM);
    ipc_register("pl:u", [] {
        std::call_once(g_once, charger);
        auto s = std::make_shared<TableService>("pl:u");
        s->cmds[0] = [](Request &, Response &) {};                                                  /* RequestLoad */
        s->cmds[1] = [](Request &, Response &rs) { remplir_si_mappee(); rs.push<uint32_t>(1); };    /* GetLoadState : chargé */
        s->cmds[2] = [](Request &rq, Response &rs) {                                                 /* GetSize */
            uint32_t t = rq.pop<uint32_t>();
            rs.push<uint32_t>(t < k_nb ? g_regions[t].size : 0);
        };
        s->cmds[3] = [](Request &rq, Response &rs) {                                                 /* GetSharedMemoryAddressOffset */
            uint32_t t = rq.pop<uint32_t>();
            rs.push<uint32_t>(t < k_nb ? g_regions[t].offset : 0);
        };
        s->cmds[4] = [](Request &, Response &rs) { rs.copy_handles.push_back(g_shmem); };           /* GetSharedMemoryNativeHandle */
        s->cmds[5] = [](Request &rq, Response &rs) {                                                 /* GetSharedFontInOrderOfPriority */
            remplir_si_mappee();
            Buf codes = rq.out(0), offs = rq.b.size() > 1 ? rq.b[1] : Buf{}, tailles = rq.b.size() > 2 ? rq.b[2] : Buf{};
            uint32_t n = k_nb;
            if (codes.size / 4 < n) n = codes.size / 4;
            for (uint32_t i = 0; i < n; i++) {
                if (codes.addr) wr32(codes.addr + 4 * i, i);
                if (offs.addr && 4 * i < offs.size) wr32(offs.addr + 4 * i, g_regions[i].offset);
                if (tailles.addr && 4 * i < tailles.size) wr32(tailles.addr + 4 * i, g_regions[i].size);
            }
            rs.push<uint8_t>(1);
            rs.push<uint32_t>(n);
        };
        return s;
    });
}
