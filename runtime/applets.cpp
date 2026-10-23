/* Applets système (am:ILibraryAppletCreator) : il n'y a pas d'applet réelle ; chaque applet lancée se termine
 * aussitôt avec un résultat plausible (sélecteur de manettes : joueurs connectés, etc.). */
#include "ipc.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>

extern uint32_t g_hid_shmem;

void applet_focus(uint8_t etat); /* services.cpp */

namespace {

/* IStorage dont le contenu reste accessible côté hôte (entrées/sorties des applets) */
struct Stockage : TableService {
    std::shared_ptr<std::vector<uint8_t>> d = std::make_shared<std::vector<uint8_t>>();
    Stockage() : TableService("am:IStorage") {
        auto donnees = d;
        cmds[0] = [donnees](Request &, Response &rs) { /* Open -> IStorageAccessor */
            auto acc = std::make_shared<TableService>("am:IStorageAccessor");
            acc->cmds[0] = [donnees](Request &, Response &r) { r.push<int64_t>((int64_t)donnees->size()); }; /* GetSize */
            acc->cmds[10] = [donnees](Request &rq, Response &) {                                         /* Write */
                int64_t off = rq.pop<int64_t>();
                Buf b = rq.in();
                if (off >= 0 && b.addr && (size_t)off < donnees->size())
                    memcpy(donnees->data() + off, g_mem + b.addr, std::min<size_t>(b.size, donnees->size() - (size_t)off));
            };
            acc->cmds[11] = [donnees](Request &rq, Response &) { /* Read */
                int64_t off = rq.pop<int64_t>();
                Buf b = rq.out();
                if (off >= 0 && b.addr && (size_t)off < donnees->size())
                    memcpy(g_mem + b.addr, donnees->data() + off, std::min<size_t>(b.size, donnees->size() - (size_t)off));
            };
            rs.objects.push_back(acc);
        };
    }
};
std::shared_ptr<Stockage> stockage(std::vector<uint8_t> v) {
    auto s = std::make_shared<Stockage>();
    *s->d = std::move(v);
    return s;
}

int nb_joueurs_connectes() { /* d'après la mémoire partagée HID (style de chaque Npad) */
    uint32_t base = svc_shmem_addr(g_hid_shmem);
    int n = 0;
    if (base)
        for (int i = 0; i < 8; i++) n += rd32(base + 0x9A00 + 0x5000 * i) != 0;
    return n;
}

struct Accesseur : TableService {
    uint32_t id, ev_etat_w = 0, ev_etat_r = 0, ev_sortie_w = 0, ev_sortie_r = 0;
    bool termine = false;
    std::mutex m;
    std::deque<std::shared_ptr<Stockage>> entrees, sorties;

    explicit Accesseur(uint32_t applet) : TableService("am:ILibraryAppletAccessor"), id(applet) {
        ev_etat_w = svc_new_event(0, &ev_etat_r);
        ev_sortie_w = svc_new_event(0, &ev_sortie_r);
        cmds[0] = [this](Request &, Response &rs) { rs.copy_handles.push_back(ev_etat_r); }; /* GetAppletStateChangedEvent */
        cmds[1] = [this](Request &, Response &rs) { rs.push<uint8_t>(termine); };           /* IsCompleted */
        cmds[10] = [this](Request &, Response &) { demarrer(); };                           /* Start */
        cmds[20] = [](Request &, Response &) {};                                            /* RequestExit */
        cmds[25] = [](Request &, Response &) {};                                            /* Terminate */
        cmds[30] = [](Request &, Response &) {};                                            /* GetResult : succès */
        cmds[50] = [](Request &, Response &) {};                                            /* SetOutOfFocusApplicationSuspendingEnabled */
        cmds[100] = [this](Request &rq, Response &) {                                       /* PushInStorage */
            for (auto &o : rq.objets_entree)
                if (auto s = std::dynamic_pointer_cast<Stockage>(o)) entrees.push_back(s);
        };
        cmds[103] = cmds[100]; /* PushInteractiveInStorage */
        cmds[101] = [this](Request &, Response &rs) { /* PopOutStorage */
            std::lock_guard<std::mutex> lk(m);
            if (sorties.empty()) { rs.result = 0x680; return; }
            rs.objects.push_back(sorties.front());
            sorties.pop_front();
        };
        cmds[104] = [](Request &, Response &rs) { rs.result = 0x680; };                          /* PopInteractiveOutStorage */
        cmds[105] = [this](Request &, Response &rs) { rs.copy_handles.push_back(ev_sortie_r); }; /* GetPopOutDataEvent */
        cmds[106] = [](Request &, Response &rs) { uint32_t r; svc_new_event(0, &r); rs.copy_handles.push_back(r); };
        cmds[120] = [this](Request &, Response &rs) { rs.push<uint32_t>(id); rs.push<uint32_t>(0); }; /* GetLibraryAppletInfo */
    }

    void demarrer() {
        applet_focus(2); /* l'application perd le focus pendant l'applet (message FocusStateChanged) */
        std::vector<uint8_t> sortie;
        switch (id) {
        case 0x0C: /* Controller : ControllerSupportResultInfo {s8 joueurs, pad[3], u32 id sélectionné, u32 résultat} */
            sortie.assign(12, 0);
            sortie[0] = (uint8_t)std::max(1, nb_joueurs_connectes());
            break;
        case 0x0E: break; /* Error : rien en retour */
        default: fprintf(stderr, "[applet] applet 0x%X : terminée sans données\n", id); break;
        }
        fprintf(stderr, "[applet] applet 0x%X : %zu stockage(s) en entrée, terminée\n", id, entrees.size());
        {
            std::lock_guard<std::mutex> lk(m);
            if (!sortie.empty()) sorties.push_back(stockage(std::move(sortie)));
        }
        termine = true;
        svc_signal_event(ev_sortie_w);
        svc_signal_event(ev_etat_w);
        applet_focus(1); /* puis le retrouve */
    }
};

} // namespace

std::shared_ptr<Service> library_applet_creator() {
    auto s = std::make_shared<TableService>("am:ILibraryAppletCreator");
    s->cmds[0] = [](Request &rq, Response &rs) { /* CreateLibraryApplet(id, mode) */
        uint32_t id = rq.pop<uint32_t>(), mode = rq.pop<uint32_t>();
        fprintf(stderr, "[applet] création : applet 0x%X mode %u\n", id, mode);
        rs.objects.push_back(std::make_shared<Accesseur>(id));
    };
    s->cmds[10] = [](Request &rq, Response &rs) { /* CreateStorage(taille) */
        int64_t n = rq.pop<int64_t>();
        rs.objects.push_back(stockage(std::vector<uint8_t>(n > 0 && n < (1 << 26) ? (size_t)n : 0)));
    };
    s->cmds[11] = [](Request &rq, Response &rs) { /* CreateTransferMemoryStorage(inscriptible, taille, handle) */
        rq.pop<uint8_t>();
        int64_t n = rq.pop<int64_t>();
        rs.objects.push_back(stockage(std::vector<uint8_t>(n > 0 && n < (1 << 26) ? (size_t)n : 0)));
    };
    return s;
}
