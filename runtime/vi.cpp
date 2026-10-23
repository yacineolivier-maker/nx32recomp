/* vi (affichage) : l'image est présentée par NVN natif (Vulkan) ; ce service ne fait que répondre au SDK.
 * En mode pilote d'origine (NX_PILOTE_ORIGINE), les files de tampons (binder IGraphicBufferProducer) et la composition
 * sont celles d'Eden (eden_gpu.dll), cadencées par l'horloge vsync ci-dessous.
 * Interfaces : https://switchbrew.org/wiki/Display_services */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ipc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "eden_gpu.h"

extern "C" int eden_actif(void);
extern "C" volatile long rt_nb_presentations; /* images présentées (détecteur de blocage, scripts de manette) */

#include "jeu.h" /* jeu_largeur_station : 1920, ou plus en ultra-large */

namespace {

using Fn = TableService::Fn;
void ok(Request &, Response &) {}

/* Parcel Android sérialisé : en-tête (taille/offset données, taille/offset objets) + données + objets */
struct ParcelOut {
    std::vector<uint8_t> data;
    template <class T> void put(T v) {
        size_t o = data.size();
        data.resize(o + ((sizeof(T) + 3) & ~3u));
        memcpy(data.data() + o, &v, sizeof(T));
    }
    void write(Buf b) const {
        if (!b.addr) return;
        uint32_t hdr[4] = {(uint32_t)data.size(), 0x10, 0, 0x10 + (uint32_t)data.size()};
        std::vector<uint8_t> out(0x10 + data.size());
        memcpy(out.data(), hdr, 16);
        memcpy(out.data() + 16, data.data(), data.size());
        memset(g_mem + b.addr, 0, b.size);
        memcpy(g_mem + b.addr, out.data(), out.size() < b.size ? out.size() : b.size);
    }
};

/* Lecture d'un argument s32 après le jeton d'interface (u32, longueur, chaîne UTF-16 alignée) */
int32_t parcel_arg0(Buf in) {
    if (!in.addr || in.size < 0x18) return 0;
    uint32_t off = rd32(in.addr + 4);
    uint32_t p = in.addr + off;
    uint32_t len = rd32(p + 4);
    p += 8 + ((len + 1) * 2 + 3 & ~3u);
    return (int32_t)rd32(p);
}

/* Horloge vsync (60 Hz) : compose l'écran (mode Eden) puis signale l'événement vsync du jeu */
std::once_flag g_vsync_once;
std::atomic<uint32_t> g_vsync_ev{0};
uint32_t g_vsync_lisible = 0;

void vsync_lancer() {
    std::call_once(g_vsync_once, [] {
        g_vsync_ev = svc_new_event(0, &g_vsync_lisible);
        std::thread([] {
            auto prochaine = std::chrono::steady_clock::now();
            for (;;) {
                prochaine += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(prochaine);
                if (eden_actif()) eden_vi_composer();
                svc_signal_event(g_vsync_ev);
            }
        }).detach();
    });
}

/* Image présentée par le jeu (QUEUE_BUFFER) : compteur (détecteur de blocage, scripts de manette), statistiques
 * toutes les 600 images, captures NX_CAPTURE (liste de numéros d'image) en mode Eden */
void presentation() {
    const long n = InterlockedIncrement(&rt_nb_presentations);
    static DWORD t0 = GetTickCount(), t_prec = t0;
    if (n % 600 == 1) {
        DWORD t = GetTickCount();
        fprintf(stderr, "[vi] présentation n°%ld (t=%.1f s, %.1f im/s)\n", n, (t - t0) / 1000.0, n > 1 ? 600000.0 / (t - t_prec + 1) : 0.0);
        t_prec = t;
    }
    static const char *cap = getenv("NX_CAPTURE");
    if (cap && eden_actif()) {
        char cle[16];
        snprintf(cle, sizeof cle, ",%ld,", n);
        if ((std::string(",") + cap + ",").find(cle) != std::string::npos) {
            CreateDirectoryA("build", nullptr);
            CreateDirectoryA("build/captures", nullptr);
            char chemin[64];
            snprintf(chemin, sizeof chemin, "build/captures/image_%05ld.bmp", n);
            if (eden_gpu_capture(chemin, jeu_largeur_station(), 1080)) fprintf(stderr, "[capture] %s\n", chemin);
        }
    }
}

std::shared_ptr<Service> binder() {
    auto s = std::make_shared<TableService>("vi:IHOSBinderDriver");
    auto transact = [](Request &rq, Response &) {
        uint32_t id = rq.pop<uint32_t>(), code = rq.pop<uint32_t>(), flags = rq.pop<uint32_t>();
        static int n = 0;
        if (n++ < 40) fprintf(stderr, "[vi] binder transact id=%u code=%u\n", id, code);
        Buf in = rq.in(), out = rq.out();
        if (code == 7) { /* QUEUE_BUFFER */
            presentation();
            if (in.addr && in.size >= 0x80) { /* intervalle de présentation demandé (1 = 60 im/s, 2 = 30 im/s) : journal aux changements */
                uint32_t p = in.addr + rd32(in.addr + 4); /* données : jeton d'interface, emplacement, QueueBufferInput aplati */
                uint32_t len = rd32(p + 4);
                p += 8 + ((len + 1) * 2 + 3 & ~3u);
                const int32_t intervalle = (int32_t)rd32(p + 4 + 8 + 44);
                static int32_t prec = -1000;
                if (intervalle != prec) fprintf(stderr, "[vi] intervalle de présentation : %d (image %ld)\n", intervalle, rt_nb_presentations);
                prec = intervalle;
            }
        }
        const uint64_t t0 = stats_actives() ? stats_horloge_ns() : 0;
        if (eden_actif() &&
            eden_vi_transact((int32_t)id, code, in.addr ? g_mem + in.addr : nullptr, in.addr ? in.size : 0,
                             out.addr ? g_mem + out.addr : nullptr, out.addr ? out.size : 0, flags)) {
            if (t0) stats_duree("binder code " + std::to_string(code), stats_horloge_ns() - t0);
            return;
        }
        ParcelOut po;
        switch (code) {
        case 10: /* CONNECT -> QueueBufferOutput(largeur, hauteur, transform_hint, num_pending) + statut */
            po.put<uint32_t>(jeu_largeur_station()); po.put<uint32_t>(1080); po.put<uint32_t>(0); po.put<uint32_t>(0);
            po.put<int32_t>(0);
            break;
        case 9: { /* QUERY(what) -> valeur + statut */
            int32_t what = parcel_arg0(rq.in());
            int32_t v = what == 0 ? jeu_largeur_station() : what == 1 ? 1080 : what == 2 ? 1 : what == 3 ? 1 : 0;
            po.put<int32_t>(v); po.put<int32_t>(0);
            break;
        }
        default:
            po.put<int32_t>(0); /* statut OK */
            break;
        }
        po.write(rq.out());
    };
    s->cmds[0] = transact; /* TransactParcel */
    s->cmds[3] = transact; /* TransactParcelAuto */
    s->cmds[1] = ok;       /* AdjustRefcount */
    s->cmds[2] = [](Request &rq, Response &rs) { /* GetNativeHandle(binder, type) -> événement (tampon rendu) */
        int32_t id = rq.pop<int32_t>();
        uint32_t type = rq.pop<uint32_t>(), r = 0;
        if (eden_actif() && eden_vi_evenement(id, type, &r)) { rs.copy_handles.push_back(r); return; }
        svc_new_event(1, &r);
        rs.copy_handles.push_back(r);
    };
    return s;
}

std::shared_ptr<Service> display_service() {
    auto s = std::make_shared<TableService>("vi:IApplicationDisplayService");
    s->cmds[100] = [](Request &, Response &rs) { rs.objects.push_back(binder()); };
    s->cmds[101] = [](Request &, Response &rs) {
        auto sys = std::make_shared<TableService>("vi:ISystemDisplayService");
        sys->cmds[2205] = ok; sys->cmds[2207] = ok; sys->cmds[2312] = ok; /* SetLayerZ / visibilité / CreateStrayLayer */
        rs.objects.push_back(sys);
    };
    s->cmds[102] = [](Request &, Response &rs) {
        auto man = std::make_shared<TableService>("vi:IManagerDisplayService");
        man->cmds[2010] = [](Request &, Response &rs2) { rs2.push<uint64_t>(1); }; /* CreateManagedLayer */
        man->cmds[2011] = ok;
        man->cmds[6000] = ok; man->cmds[6002] = ok;                               /* AddToLayerStack */
        rs.objects.push_back(man);
    };
    s->cmds[103] = [](Request &, Response &rs) { rs.objects.push_back(binder()); };
    s->cmds[1000] = [](Request &rq, Response &rs) { /* ListDisplays */
        Buf b = rq.out();
        if (b.addr && b.size >= 0x60) {
            memset(g_mem + b.addr, 0, 0x60);
            memcpy(g_mem + b.addr, "Default", 7);
            g_mem[b.addr + 0x40] = 1; /* has_limited_layers */
            wr64(b.addr + 0x48, 1);   /* max_layers */
            wr64(b.addr + 0x50, jeu_largeur_station());
            wr64(b.addr + 0x58, 1080);
        }
        rs.push<uint64_t>(1);
    };
    s->cmds[1010] = [](Request &, Response &rs) { rs.push<uint64_t>(0); }; /* OpenDisplay -> id */
    s->cmds[1011] = [](Request &, Response &rs) { rs.push<uint64_t>(0); }; /* OpenDefaultDisplay */
    s->cmds[1020] = ok;
    s->cmds[1101] = ok;
    s->cmds[1102] = [](Request &, Response &rs) { rs.push<uint64_t>(jeu_largeur_station()); rs.push<uint64_t>(1080); };
    s->cmds[2020] = [](Request &rq, Response &rs) { /* OpenLayer(nom, layer_id, aruid, B parcel) -> taille */
        Buf b = rq.out();
        /* parcel : en-tête (taille données, offset données, taille objets, offset objets) + objet binder « dispdrv » */
        uint8_t parcel[0x3C] = {}; /* en-tête + NativeWindow (0x28) + tampon d'objets (u32 0) */
        uint32_t data_size = 0x28;
        uint32_t hdr[4] = {data_size, 0x10, 4, 0x10 + data_size};
        memcpy(parcel, hdr, 16);
        uint32_t magic = 2, pid = 1, id = 1;
        if (eden_actif()) { /* file de tampons d'Eden : le parcel désigne son producteur */
            int32_t p = eden_vi_creer_couche();
            if (p > 0) id = (uint32_t)p;
            vsync_lancer();
        }
        memcpy(parcel + 0x10, &magic, 4);
        memcpy(parcel + 0x14, &pid, 4);
        memcpy(parcel + 0x18, &id, 4);
        memcpy(parcel + 0x28, "dispdrv", 8);
        if (b.addr) {
            memset(g_mem + b.addr, 0, b.size);
            memcpy(g_mem + b.addr, parcel, b.size < sizeof parcel ? b.size : sizeof parcel);
        }
        rs.push<uint64_t>(sizeof parcel);
    };
    s->cmds[2021] = ok;
    s->cmds[2030] = s->cmds[2020];
    s->cmds[2031] = ok;
    s->cmds[2101] = ok;
    s->cmds[2102] = [](Request &, Response &rs) { rs.push<uint64_t>(2); };
    s->cmds[5202] = [](Request &, Response &rs) { /* GetDisplayVsyncEvent : signalé à 60 Hz par l'horloge vsync */
        vsync_lancer();
        rs.copy_handles.push_back(g_vsync_lisible);
    };
    return s;
}

} // namespace

void vi_register() {
    auto f = [] {
        auto s = std::make_shared<TableService>("vi");
        for (uint32_t c : {0u, 1u, 2u}) s->cmds[c] = [](Request &, Response &rs) { rs.objects.push_back(display_service()); };
        return s;
    };
    for (auto n : {"vi:m", "vi:s", "vi:u"}) ipc_register(n, f);
}
