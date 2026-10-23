#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "cpu.h"
uint32_t svc_new_session(void *obj);
void *svc_session_obj(uint32_t h);
uint32_t svc_new_event(int signaled, uint32_t *readable);
void svc_signal_event(uint32_t h);
void svc_clear_event(uint32_t h);
uint32_t svc_new_shmem(uint32_t size);
uint32_t svc_shmem_addr(uint32_t h);
void svc_shmem_set_init(uint32_t h, const uint8_t *init);
uint32_t svc_dupliquer_handle(uint32_t h); /* nouveau handle vers le même objet */
}

/* Tampon invité décrit par un descripteur IPC */
struct Buf {
    uint32_t addr = 0, size = 0;
};

struct Service;

struct Request {
    Cpu *c = nullptr;
    std::vector<std::shared_ptr<Service>> objets_entree; /* objets passés en entrée (domaine ou handles de session) */
    uint32_t type = 0, cmd = 0;
    std::vector<uint8_t> data; /* charge utile après l'en-tête SFCI */
    std::vector<uint32_t> copy_handles, move_handles, in_objects;
    std::vector<Buf> x, a, b, w, cbuf;
    uint64_t pid = 0;
    size_t rd = 0; /* curseur de lecture dans data */

    template <class T> T pop() {
        T v{};
        rd = (rd + alignof(T) - 1) & ~(alignof(T) - 1);
        if (rd + sizeof(T) <= data.size()) memcpy(&v, data.data() + rd, sizeof(T));
        rd += sizeof(T);
        return v;
    }
    /* Tampon d'entrée : A ou X, selon ce que le client a utilisé */
    Buf in(int i = 0) const {
        if (i < (int)a.size() && a[i].addr) return a[i];
        if (i < (int)x.size()) return x[i];
        return {};
    }
    /* Tampon de sortie : B ou C */
    Buf out(int i = 0) const {
        if (i < (int)b.size() && b[i].addr) return b[i];
        if (i < (int)cbuf.size()) return cbuf[i];
        return {};
    }
    std::string str(Buf bf) const;
};

struct Response {
    uint32_t result = 0;
    std::vector<uint8_t> data;
    std::vector<uint32_t> copy_handles, move_handles;
    std::vector<std::shared_ptr<Service>> objects; /* nouveaux objets (handles hors domaine, ids en domaine) */

    template <class T> void push(T v) {
        size_t o = (data.size() + alignof(T) - 1) & ~(alignof(T) - 1);
        data.resize(o + sizeof(T));
        memcpy(data.data() + o, &v, sizeof(T));
    }
};

struct Service : std::enable_shared_from_this<Service> {
    std::string name;
    explicit Service(std::string n) : name(std::move(n)) {}
    virtual ~Service() = default;
    /* Renvoie false si la commande est inconnue (journalisée par l'appelant). */
    virtual bool handle(Request &rq, Response &rs) = 0;
};

/* Statistiques de durée (NX_IPC_STATS=s : rapport toutes les s secondes) : temps cumulé par clé (service#commande,
 * ioctl nvdrv, transaction binder...) ; stats_actives() est lu une fois. */
bool stats_actives();
void stats_duree(const std::string &cle, uint64_t ns);
uint64_t stats_horloge_ns();

/* Service défini par une table cmd -> fonction */
struct TableService : Service {
    using Fn = std::function<void(Request &, Response &)>;
    std::map<uint32_t, Fn> cmds;
    explicit TableService(std::string n) : Service(std::move(n)) {}
    bool handle(Request &rq, Response &rs) override {
        auto it = cmds.find(rq.cmd);
        if (it == cmds.end()) return false;
        if (!stats_actives()) {
            it->second(rq, rs);
            return true;
        }
        const uint64_t t0 = stats_horloge_ns();
        it->second(rq, rs);
        stats_duree(name + "#" + std::to_string(rq.cmd), stats_horloge_ns() - t0);
        return true;
    }
};

using ServiceFactory = std::function<std::shared_ptr<Service>()>;
void ipc_register(const std::string &name, ServiceFactory f);
std::shared_ptr<Service> ipc_open(const std::string &name);
void services_init();
