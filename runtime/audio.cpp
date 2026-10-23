/* audren:u — moteur de rendu audio. Étape actuelle : silencieux mais structurellement exact
 * (format de RequestUpdate repris d'InfoUpdater, cf. audio_core d'Eden). Le mixage SDL3 viendra ensuite. */
#include "ipc.h"
#include "mixeur.h"

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

struct Params { /* AudioRendererParameterInternal (0x34) */
    uint32_t sample_rate, sample_count, mixes, sub_mixes, voices, sinks, effects, perf_frames;
    uint8_t voice_drop, unk21, device, exec_mode;
    uint32_t splitter_infos;
    int32_t splitter_destinations;
    uint32_t external_context_size, revision;
};
static_assert(sizeof(Params) == 0x34, "Params");

uint32_t rev_num(uint32_t magic) { return ((magic >> 24) & 0xFF) - '0'; } /* 'REVn' */

struct Renderer : Service {
    Params p{};
    uint32_t event_w = 0, event_r = 0;
    uint64_t frames = 0;
    std::atomic<bool> actif{false};
    std::thread horloge;
    Mixeur *mixeur = nullptr;

    explicit Renderer(const Params &pp) : Service("audren:IAudioRenderer"), p(pp) {
        event_w = svc_new_event(0, &event_r);
        actif = true;
        mixeur = mixeur_creer(p.sample_rate, p.sample_count, p.voices, p.sub_mixes + 1, p.mixes, p.sinks);
        uint32_t ew = event_w;
        uint32_t periode_us = p.sample_rate ? (uint32_t)(1000000ull * p.sample_count / p.sample_rate) : 5000;
        horloge = std::thread([this, ew, periode_us] { /* une trame audio mixée puis l'événement système (5 ms) */
            auto prochaine = std::chrono::steady_clock::now();
            while (actif) {
                prochaine += std::chrono::microseconds(periode_us);
                std::this_thread::sleep_until(prochaine);
                if (std::chrono::steady_clock::now() - prochaine > std::chrono::milliseconds(50))
                    prochaine = std::chrono::steady_clock::now(); /* retard important : on se recale */
                mixeur_trame(mixeur);
                svc_signal_event(ew);
            }
        });
    }
    ~Renderer() override { actif = false; if (horloge.joinable()) horloge.join(); }

    void request_update(Request &rq) {
        Buf in = rq.in(), out = rq.out(0);
        if (!in.addr || !out.addr) return;
        uint32_t rev = rev_num(rd32(in.addr));
        uint32_t mempools = p.effects + p.voices * 4;
        uint32_t in_pool_size = rd32(in.addr + 0x08);
        /* entrée : en-tête 0x40, comportement (behaviour_size), puis pools mémoire (0x20 chacun) */
        uint32_t in_pools = in.addr + 0x40 + rd32(in.addr + 0x04);
        uint32_t o = out.addr;
        memset(g_mem + o, 0, out.size);
        uint32_t cur = 0x40;
        auto section = [&](uint32_t off_header, uint32_t taille) {
            wr32(o + off_header, taille);
            cur += taille;
        };
        /* pools mémoire : RequestAttach (4) -> Attached (5), RequestDetach (2) -> Detached (3) */
        uint32_t n_pools = in_pool_size / 0x20;
        if (n_pools > mempools) n_pools = mempools;
        for (uint32_t i = 0; i < mempools; i++) {
            uint32_t etat = i < n_pools ? rd32(in_pools + i * 0x20 + 0x10) : 0;
            uint32_t sortie = etat == 4 ? 5 : etat == 2 ? 3 : etat;
            wr32(o + cur + i * 0x10, sortie);
        }
        section(0x08, mempools * 0x10);
        mixeur_mise_a_jour(mixeur, in.addr, o + cur); /* voix : état des tampons d'onde, sortie juste après les pools */
        section(0x0C, p.voices * 0x10);
        section(0x14, p.effects * (rev >= 9 ? 0x90 : 0x10));
        section(0x1C, p.sinks * 0x20);
        section(0x20, 0x10); /* performances : toujours présente (vérifié sur le SDK REV1) */
        section(0x04, 0xB0); /* erreurs : aucune */
        if (rev >= 5) { /* informations de rendu : nombre de trames écoulées */
            wr64(o + cur, frames);
            section(0x28, 0x10);
        }
        if (frames == 0)
            fprintf(stderr, "[audio] RequestUpdate : sortie calculée 0x%X, tampon 0x%X, pools en entrée %u (attendu %u)\n", cur,
                    out.size, n_pools, mempools);
        frames++;
        wr32(o, rd32(in.addr)); /* révision */
        wr32(o + 0x3C, cur);    /* taille totale */
    }

    bool handle(Request &rq, Response &rs) override {
        switch (rq.cmd) {
        case 0: rs.push<uint32_t>(p.sample_rate); return true;
        case 1: rs.push<uint32_t>(p.sample_count); return true;
        case 2: rs.push<uint32_t>(p.mixes); return true;
        case 3: rs.push<uint32_t>(0); return true; /* GetState : démarré */
        case 4: case 10: request_update(rq); return true;
        case 5: case 6: return true;              /* Start / Stop */
        case 7: rs.copy_handles.push_back(event_r); return true;
        case 8: return true;
        case 9: rs.push<uint32_t>(100); return true;
        case 11: return true;
        }
        return false;
    }
};

uint64_t work_buffer_size(const Params &p) { /* estimation généreuse : la mémoire est fournie par le jeu */
    uint64_t s = 0x10000 + (uint64_t)p.voices * 0x400 + (uint64_t)p.effects * 0x2000 + (uint64_t)(p.mixes + p.sub_mixes) * 0x1000 +
                 (uint64_t)p.sinks * 0x200 + (uint64_t)p.sample_count * (p.mixes + 6) * 4 * 2;
    return (s + 0xFFF) & ~0xFFFull;
}

std::shared_ptr<Service> audio_device() {
    auto s = std::make_shared<TableService>("audren:IAudioDevice");
    auto noms = [](Request &rq, Response &rs) {
        Buf b = rq.out();
        if (b.addr && b.size >= 0x100) { memset(g_mem + b.addr, 0, 0x100); memcpy(g_mem + b.addr, "AudioTvOutput", 13); }
        rs.push<uint32_t>(1);
    };
    s->cmds[0] = noms; s->cmds[6] = noms;
    s->cmds[1] = [](Request &, Response &) {};
    s->cmds[2] = [](Request &, Response &rs) { rs.push<float>(1.0f); };
    s->cmds[3] = [](Request &rq, Response &) {
        Buf b = rq.out();
        if (b.addr && b.size >= 0x100) { memset(g_mem + b.addr, 0, 0x100); memcpy(g_mem + b.addr, "AudioTvOutput", 13); }
    };
    s->cmds[7] = s->cmds[3];
    auto ev = [](Request &, Response &rs) { uint32_t r; svc_new_event(1, &r); rs.copy_handles.push_back(r); };
    s->cmds[4] = ev; s->cmds[10] = ev; s->cmds[11] = ev;
    s->cmds[5] = [](Request &, Response &rs) { rs.push<uint32_t>(2); };
    return s;
}

} // namespace

void audio_register() {
    ipc_register("audren:u", [] {
        auto s = std::make_shared<TableService>("audren:u");
        s->cmds[0] = [](Request &rq, Response &rs) { /* OpenAudioRenderer(params, taille, aruid, tmem, process) */
            Params p = rq.pop<Params>();
            fprintf(stderr, "[audio] OpenAudioRenderer %u Hz, %u échantillons, %u voix, %u effets, %u mix, %u sorties, REV%u\n",
                    p.sample_rate, p.sample_count, p.voices, p.effects, p.mixes, p.sinks, rev_num(p.revision));
            rs.objects.push_back(std::make_shared<Renderer>(p));
        };
        s->cmds[1] = [](Request &rq, Response &rs) { rs.push<uint64_t>(work_buffer_size(rq.pop<Params>())); };
        s->cmds[2] = [](Request &, Response &rs) { rs.objects.push_back(audio_device()); };
        s->cmds[4] = [](Request &, Response &rs) { rs.objects.push_back(audio_device()); };
        return s;
    });
}

/* ------------------------------------------------------------------ amis (friend:a) : aucun ami, pas de notification */
void friend_register() {
    auto f = [] {
        auto s = std::make_shared<TableService>("friend");
        s->cmds[0] = [](Request &, Response &rs) { rs.objects.push_back(std::make_shared<TableService>("friend:IFriendService")); };
        s->cmds[1] = [](Request &, Response &rs) {
            auto n = std::make_shared<TableService>("friend:INotificationService");
            n->cmds[0] = [](Request &, Response &r2) { uint32_t r; svc_new_event(0, &r); r2.copy_handles.push_back(r); };
            n->cmds[1] = [](Request &, Response &) {};
            n->cmds[2] = [](Request &, Response &r2) { r2.result = 0x6E3; }; /* Pop : aucune notification */
            rs.objects.push_back(n);
        };
        return s;
    };
    for (auto n : {"friend:a", "friend:u", "friend:v", "friend:m"}) ipc_register(n, f);
}
