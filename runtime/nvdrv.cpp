/* Service nvdrv (pilote GPU de la Switch) pour le pilote NVN d'origine du jeu (NX_PILOTE_ORIGINE=1).
 * Le pilote NVN (subsdk0) ouvre des périphériques (/dev/nvmap, /dev/nvhost-as-gpu, /dev/nvhost-gpu, /dev/nvhost-ctrl,
 * /dev/nvhost-ctrl-gpu...) et leur adresse des ioctl de style Linux. Commandes IPC et codes de retour d'après Eden
 * (src/core/hle/service/nvdrv/nvdrv_interface.cpp, nvdata.h). Les périphériques sont dans nvdrv_dev*.cpp. */
#include "nvdrv.h"

#include <cstdio>
#include <map>
#include <mutex>

namespace {

std::mutex g_m;
std::map<int32_t, std::shared_ptr<NvDev>> g_fds;
int32_t g_prochain_fd = 1;
bool g_journal = getenv("NX_NVDRV_TRACE") != nullptr;

std::map<std::string, NvFabrique> &fabriques() {
    static std::map<std::string, NvFabrique> f;
    return f;
}

/* Périphérique pas encore implémenté : journalise chaque ioctl et répond « non implémenté » */
struct DevJournal : NvDev {
    std::string chemin;
    explicit DevJournal(std::string c) : chemin(std::move(c)) {}
    uint32_t ioctl(NvIoctl cmd, const std::vector<uint8_t> &in, std::vector<uint8_t> &, const std::vector<uint8_t> &,
                   std::vector<uint8_t> &) override {
        fprintf(stderr, "[nvdrv] %s : ioctl %08X (groupe %02X, n° %02X, %u octets%s%s) NON IMPLÉMENTÉ, entrée %zu octets\n", chemin.c_str(),
                cmd.raw, cmd.groupe(), cmd.numero(), cmd.taille(), cmd.entree() ? ", entrée" : "", cmd.sortie() ? ", sortie" : "",
                in.size());
        return NV_NON_IMPLEMENTE;
    }
};

std::shared_ptr<NvDev> dev_de(int32_t fd) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second;
}

std::vector<uint8_t> lire(Buf b) {
    std::vector<uint8_t> v(b.size);
    if (b.addr && b.size) memcpy(v.data(), g_mem + b.addr, b.size);
    return v;
}
void ecrire(Buf b, const std::vector<uint8_t> &v) {
    if (b.addr && b.size) memcpy(g_mem + b.addr, v.data(), std::min<size_t>(b.size, v.size()));
}

/* Ioctl / Ioctl2 / Ioctl3 : (fd, commande) + tampons d'entrée et de sortie (descripteurs « auto » A/X et B/C) */
void faire_ioctl(Request &rq, Response &rs, int variante) {
    int32_t fd = rq.pop<int32_t>();
    NvIoctl cmd{rq.pop<uint32_t>()};
    auto dev = dev_de(fd);
    if (!dev) { rs.push<uint32_t>(NV_PARAMETRE_INVALIDE); return; }
    std::vector<uint8_t> in = lire(rq.in(0)), in2, out(rq.out(0).size), out2;
    if (variante == 2) in2 = lire(rq.in(1));
    if (variante == 3) out2.resize(rq.out(1).size);
    if (out.size() < in.size() && cmd.sortie()) out.resize(in.size());
    const uint64_t t0 = stats_actives() ? stats_horloge_ns() : 0;
    uint32_t r = dev->ioctl(cmd, in, out, in2, out2);
    if (t0) {
        char cle[48];
        snprintf(cle, sizeof cle, "nvdrv fd %d ioctl %08X", fd, cmd.raw);
        stats_duree(cle, stats_horloge_ns() - t0);
    }
    if (g_journal)
        fprintf(stderr, "[nvdrv] fd %d ioctl%d %08X (groupe %02X n° %02X, %u octets) -> %X\n", fd, variante, cmd.raw, cmd.groupe(), cmd.numero(),
                cmd.taille(), r);
    if (cmd.sortie()) {
        ecrire(rq.out(0), out);
        if (variante == 3) ecrire(rq.out(1), out2);
    }
    rs.push<uint32_t>(r);
}

} // namespace
extern "C" int eden_actif(void);
std::shared_ptr<NvDev> eden_ouvrir(const std::string &chemin);
namespace {

std::shared_ptr<Service> service_nvdrv(const std::string &nom) {
    auto s = std::make_shared<TableService>(nom);
    s->cmds[0] = [](Request &rq, Response &rs) { /* Open(chemin) -> fd, NvResult */
        std::string chemin = rq.str(rq.in(0));
        std::shared_ptr<NvDev> dev;
        if (eden_actif()) { /* émulation GPU d'Eden : périphérique tenu par eden_gpu.dll */
            dev = eden_ouvrir(chemin);
            if (!dev) {
                fprintf(stderr, "[nvdrv] Open(\"%s\") : inconnu de l'émulation GPU\n", chemin.c_str());
                rs.push<int32_t>(-1);
                rs.push<uint32_t>(0x30003); /* FileOperationFailed */
                return;
            }
        } else {
            auto it = fabriques().find(chemin);
            dev = it != fabriques().end() ? it->second() : std::make_shared<DevJournal>(chemin);
        }
        int32_t fd;
        {
            std::lock_guard<std::mutex> lk(g_m);
            fd = dev->fd_impose();
            if (fd < 0) fd = g_prochain_fd++;
            g_fds[fd] = dev;
        }
        const bool journal_seul = !eden_actif() && !fabriques().count(chemin);
        fprintf(stderr, "[nvdrv] Open(\"%s\") -> fd %d%s\n", chemin.c_str(), fd, journal_seul ? " (journal seulement)" : "");
        rs.push<int32_t>(fd);
        rs.push<uint32_t>(NV_SUCCES);
    };
    s->cmds[1] = [](Request &rq, Response &rs) { faire_ioctl(rq, rs, 1); };
    s->cmds[2] = [](Request &rq, Response &rs) { /* Close(fd) */
        int32_t fd = rq.pop<int32_t>();
        std::lock_guard<std::mutex> lk(g_m);
        rs.push<uint32_t>(g_fds.erase(fd) ? NV_SUCCES : NV_PARAMETRE_INVALIDE);
    };
    s->cmds[3] = [](Request &rq, Response &rs) { /* Initialize(taille mémoire de transfert, processus, mémoire de transfert) */
        uint32_t taille = rq.pop<uint32_t>();
        fprintf(stderr, "[nvdrv] Initialize : mémoire de transfert %X octets (handles %zu)\n", taille, rq.copy_handles.size());
        rs.push<uint32_t>(NV_SUCCES);
    };
    s->cmds[4] = [](Request &rq, Response &rs) { /* QueryEvent(fd, id) -> NvResult + événement (copie) */
        int32_t fd = rq.pop<int32_t>();
        uint32_t id = rq.pop<uint32_t>();
        auto dev = dev_de(fd);
        uint32_t h = 0, r = dev ? dev->evenement(id, &h) : NV_PARAMETRE_INVALIDE;
        fprintf(stderr, "[nvdrv] QueryEvent(fd %d, id %X) -> %X\n", fd, id, r);
        if (r == NV_SUCCES) rs.copy_handles.push_back(h);
        rs.push<uint32_t>(r);
    };
    s->cmds[6] = [](Request &, Response &rs) { rs.push<uint32_t>(NV_SUCCES); };   /* GetStatus */
    s->cmds[8] = [](Request &, Response &rs) { rs.push<uint32_t>(NV_SUCCES); };   /* SetAruid(pid) */
    s->cmds[9] = [](Request &, Response &) {};                                    /* DumpGraphicsMemoryInfo */
    s->cmds[11] = [](Request &rq, Response &rs) { faire_ioctl(rq, rs, 2); };
    s->cmds[12] = [](Request &rq, Response &rs) { faire_ioctl(rq, rs, 3); };
    s->cmds[13] = [](Request &, Response &) {};                                   /* SetGraphicsFirmwareMemoryMarginEnabled */
    return s;
}

} // namespace

void nvdrv_enregistrer_peripherique(const std::string &chemin, NvFabrique f) { fabriques()[chemin] = std::move(f); }

void nvdrv_register() {
    for (const char *n : {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"}) {
        std::string nom = n;
        ipc_register(nom, [nom] { return service_nvdrv(nom); });
    }
}
