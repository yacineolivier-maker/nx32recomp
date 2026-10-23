/* IPC CMIF : décodage des messages en TLS, sessions, domaines, requêtes de contrôle, sm:. */
#include "ipc.h"
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <mutex>
static const bool g_env_ipc_trace = getenv("NX_IPC_TRACE") != nullptr; /* lu une fois (getenv est lent) */
static thread_local char t_ipc_contexte[48];
extern "C" const char *ipc_contexte(void) { return t_ipc_contexte; }

/* ---- statistiques de durée (NX_IPC_STATS=s) */
static const int g_stats_periode = getenv("NX_IPC_STATS") ? atoi(getenv("NX_IPC_STATS")) : 0;
static std::mutex g_stats_m;
static std::map<std::string, std::pair<uint64_t, uint64_t>> g_stats; /* clé -> (ns cumulées, appels) */

uint64_t stats_horloge_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool stats_actives() {
    static const bool actives = [] {
        if (g_stats_periode <= 0) return false;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(g_stats_periode));
                std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::string>> v;
                {
                    std::lock_guard<std::mutex> lk(g_stats_m);
                    for (auto &[k, d] : g_stats) v.push_back({d, k});
                    g_stats.clear();
                }
                std::sort(v.rbegin(), v.rend());
                fprintf(stderr, "[ipc-stats] sur %d s (temps cumulé, appels, moyenne) :\n", g_stats_periode);
                for (size_t i = 0; i < v.size() && i < 25; i++)
                    fprintf(stderr, "[ipc-stats] %-48s %9.1f ms %7llu appels %8.3f ms\n", v[i].second.c_str(), v[i].first.first / 1e6,
                            (unsigned long long)v[i].first.second, v[i].first.first / 1e6 / (double)v[i].first.second);
                fflush(stderr);
            }
        }).detach();
        return true;
    }();
    return actives;
}

void stats_duree(const std::string &cle, uint64_t ns) {
    std::lock_guard<std::mutex> lk(g_stats_m);
    auto &e = g_stats[cle];
    e.first += ns;
    e.second++;
}

extern "C" void rt_fatal(Cpu *c, const char *fmt, ...);

namespace {

struct Session {
    std::shared_ptr<Service> svc;
    bool domain = false;
    std::vector<std::shared_ptr<Service>> objs; /* id = indice + 1 */
    int refs = 1; /* les clones (CloneCurrentObject) partagent la même session, domaine compris */
};

std::map<std::string, ServiceFactory> &registry() {
    static std::map<std::string, ServiceFactory> r;
    return r;
}

bool g_strict = getenv("NX_IPC_STRICT") != nullptr;
std::mutex g_log_mutex;

uint32_t new_session_for(std::shared_ptr<Service> s) {
    auto *ss = new Session;
    ss->svc = std::move(s);
    return svc_new_session(ss);
}

/* Service inconnu : journalise chaque commande et répond « succès, sortie nulle » (mode exploration). */
struct LogStub : Service {
    using Service::Service;
    bool handle(Request &, Response &) override { return false; }
};

} // namespace

std::string Request::str(Buf bf) const {
    std::string s;
    for (uint32_t i = 0; i < bf.size; i++) {
        char ch = (char)rd8(bf.addr + i);
        if (!ch) break;
        s += ch;
    }
    return s;
}

void ipc_register(const std::string &name, ServiceFactory f) { registry()[name] = std::move(f); }

std::shared_ptr<Service> ipc_open(const std::string &name) {
    auto it = registry().find(name);
    if (it != registry().end()) return it->second();
    return std::make_shared<LogStub>(name);
}

static std::string name_of(uint64_t v) {
    std::string s;
    for (int i = 0; i < 8; i++) {
        char ch = (char)(v >> (8 * i));
        if (!ch) break;
        s += ch;
    }
    return s;
}

/* ------------------------------------------------------------------ sm: */
static void register_sm() {
    ipc_register("sm:", [] {
        auto s = std::make_shared<TableService>("sm:");
        s->cmds[0] = [](Request &, Response &) {}; /* Initialize / RegisterClient */
        s->cmds[1] = [](Request &rq, Response &rs) { /* GetService(name) */
            std::string n = name_of(rq.pop<uint64_t>());
            rs.objects.push_back(ipc_open(n));
            fprintf(stderr, "[sm] GetService(\"%s\")\n", n.c_str());
        };
        return s;
    });
}

/* ------------------------------------------------------------------ cœur IPC */
static void write_response(Cpu *c, Session *ss, bool domain_msg, Response &rs) {
    uint32_t tls = c->tpidruro;
    std::vector<uint32_t> w;
    std::vector<uint32_t> move = rs.move_handles;
    std::vector<uint32_t> out_ids;
    for (auto &o : rs.objects) {
        if (domain_msg) {
            ss->objs.push_back(o);
            out_ids.push_back((uint32_t)ss->objs.size());
        } else {
            move.push_back(new_session_for(o));
        }
    }
    bool hd = !rs.copy_handles.empty() || !move.empty();
    w.push_back(0);
    w.push_back(0);
    if (hd) {
        w.push_back(((uint32_t)rs.copy_handles.size() << 1) | ((uint32_t)move.size() << 5));
        /* copie de handle (comme Horizon) : le jeu reçoit un nouveau handle vers le même objet ; s'il le ferme, celui du
         * service reste valide (sinon un handle statique fermé puis recyclé désignait un autre objet) */
        for (auto h : rs.copy_handles) w.push_back(svc_dupliquer_handle(h));
        for (auto h : move) w.push_back(h);
    }
    size_t raw_start = w.size();
    while ((w.size() * 4) % 16) w.push_back(0); /* alignement 16 octets */
    if (domain_msg) {
        w.push_back((uint32_t)out_ids.size());
        w.push_back(0); w.push_back(0); w.push_back(0);
    }
    w.push_back(0x4F434653); /* 'SFCO' */
    w.push_back(0);
    w.push_back(rs.result);
    w.push_back(0);
    rs.data.resize((rs.data.size() + 3) & ~3u);
    for (size_t i = 0; i < rs.data.size(); i += 4) {
        uint32_t v;
        memcpy(&v, rs.data.data() + i, 4);
        w.push_back(v);
    }
    for (auto id : out_ids) w.push_back(id);
    uint32_t words = (uint32_t)(w.size() - raw_start) + 4;
    w[1] = words | (hd ? 0x80000000u : 0);
    for (size_t i = 0; i < w.size() && i < 64; i++) wr32(tls + 4 * (uint32_t)i, w[i]);
}

extern "C" uint32_t ipc_dispatch(Cpu *c, uint32_t handle) {
    auto *ss = (Session *)svc_session_obj(handle);
    if (!ss) return 0xE401;
    uint32_t tls = c->tpidruro;
    auto W = [&](uint32_t i) { return rd32(tls + 4 * i); };

    Request rq;
    rq.c = c;
    uint32_t w0 = W(0), w1 = W(1);
    rq.type = w0 & 0xFFFF;
    uint32_t nx = (w0 >> 16) & 0xF, na = (w0 >> 20) & 0xF, nb = (w0 >> 24) & 0xF, nw = (w0 >> 28) & 0xF;
    uint32_t data_words = w1 & 0x3FF, recv_flags = (w1 >> 10) & 0xF;
    uint32_t i = 2;
    if (w1 & 0x80000000u) {
        uint32_t hd = W(i++);
        if (hd & 1) { rq.pid = W(i) | ((uint64_t)W(i + 1) << 32); i += 2; }
        for (uint32_t k = 0; k < ((hd >> 1) & 0xF); k++) rq.copy_handles.push_back(W(i++));
        for (uint32_t k = 0; k < ((hd >> 5) & 0xF); k++) rq.move_handles.push_back(W(i++));
    }
    for (uint32_t k = 0; k < nx; k++, i += 2) rq.x.push_back({W(i + 1), W(i) >> 16});
    auto abw = [&](std::vector<Buf> &v, uint32_t n) {
        for (uint32_t k = 0; k < n; k++, i += 3) v.push_back({W(i + 1), W(i)});
    };
    abw(rq.a, na);
    abw(rq.b, nb);
    abw(rq.w, nw);
    uint32_t raw = (i * 4 + 15) & ~15u; /* début des données brutes, en octets */
    uint32_t raw_end = i * 4 + data_words * 4;
    if (recv_flags >= 2) { /* descripteurs C après les données brutes */
        uint32_t ci = (i + data_words);
        uint32_t nc = recv_flags == 2 ? 1 : recv_flags - 2;
        for (uint32_t k = 0; k < nc; k++, ci += 2) rq.cbuf.push_back({W(ci), W(ci + 1) >> 16});
    }

    if (rq.type == 2) { /* Close */
        if (--ss->refs == 0) delete ss;
        return 0;
    }

    std::shared_ptr<Service> target = ss->svc;
    bool domain_msg = ss->domain && (rq.type == 4 || rq.type == 6);
    uint32_t off = raw;
    if (domain_msg) {
        uint32_t dh = rd32(tls + off), obj = rd32(tls + off + 4);
        uint32_t dcmd = dh & 0xFF, nin = (dh >> 8) & 0xFF, plen = dh >> 16;
        for (uint32_t k = 0; k < nin; k++) {
            uint32_t id = rd32(tls + off + 16 + plen + 4 * k);
            rq.in_objects.push_back(id);
            rq.objets_entree.push_back(id >= 1 && id <= ss->objs.size() ? ss->objs[id - 1] : nullptr);
        }
        if (dcmd == 2) { /* fermeture d'objet du domaine */
            if (obj >= 1 && obj <= ss->objs.size()) ss->objs[obj - 1].reset();
            Response rs;
            write_response(c, ss, true, rs);
            return 0;
        }
        if (obj < 1 || obj > ss->objs.size() || !ss->objs[obj - 1]) rt_fatal(c, "objet de domaine %u invalide", obj);
        target = ss->objs[obj - 1];
        off += 16;
    }
    if (rd32(tls + off) != 0x49434653) {
        for (int k = 0; k < 24; k++) fprintf(stderr, "%08X%s", rd32(tls + 4 * k), k % 8 == 7 ? "\n" : " ");
        rt_fatal(c, "en-tête SFCI absent (type %u, %s, domaine %d, off %u)", rq.type, ss->svc->name.c_str(), ss->domain, off);
    }
    rq.cmd = rd32(tls + off + 8);
    uint32_t payload = off + 16;
    if (raw_end > payload) rq.data.assign(g_mem + tls + payload, g_mem + tls + raw_end);

    if (!domain_msg) /* objets passés par handles de session */
        for (uint32_t h : rq.copy_handles)
            if (auto *so = (Session *)svc_session_obj(h)) rq.objets_entree.push_back(so->svc);
    Response rs;
    if (g_env_ipc_trace)
        fprintf(stderr, "[ipc] h=%08X %s type=%u domaine=%d\n", handle, ss->svc->name.c_str(), rq.type, ss->domain);
    if (rq.type == 5 || rq.type == 7) { /* contrôle */
        switch (rq.cmd) {
        case 0: /* ConvertCurrentObjectToDomain */
            ss->domain = true;
            ss->objs.push_back(ss->svc);
            rs.push<uint32_t>(1);
            break;
        case 2: case 4: /* CloneCurrentObject(Ex) */
            ss->refs++;
            rs.move_handles.push_back(svc_new_session(ss));
            break;
        case 3: rs.push<uint16_t>(0x8000); break; /* QueryPointerBufferSize */
        default: rt_fatal(c, "contrôle CMIF %u inconnu", rq.cmd);
        }
        write_response(c, ss, false, rs);
        return 0;
    }

    /* contexte (service#commande) : nom des événements créés pendant la commande (diagnostic des attentes) */
    snprintf(t_ipc_contexte, sizeof t_ipc_contexte, "%s#%u", target->name.c_str(), rq.cmd);
    bool ok = target->handle(rq, rs);
    t_ipc_contexte[0] = 0;
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        if (!ok) {
            fprintf(stderr, "[ipc] %s cmd %u NON IMPLÉMENTÉ (données %zu o, X%u A%u B%u C%zu, handles %zu/%zu)\n",
                    target->name.c_str(), rq.cmd, rq.data.size(), nx, na, nb, rq.cbuf.size(), rq.copy_handles.size(),
                    rq.move_handles.size());
            if (g_strict) rt_fatal(c, "IPC strict");
            rs = Response{};
            for (int k = 0; k < 16; k++) rs.push<uint32_t>(0);
        } else if (g_env_ipc_trace) {
            fprintf(stderr, "[ipc] %s cmd %u -> %08X", target->name.c_str(), rq.cmd, rs.result);
            for (uint32_t h : rs.copy_handles) fprintf(stderr, " copie %08X", h); /* événements donnés au jeu */
            fprintf(stderr, "\n");
        }
    }
    write_response(c, ss, domain_msg, rs);
    return 0;
}

extern "C" void ipc_init() {
    register_sm();
    services_init();
}

extern "C" uint32_t ipc_connect(const char *name) { return new_session_for(ipc_open(name)); }
