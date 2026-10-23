/* Vignettes : fonctions natives appelables par le code invité. Chacune reçoit une adresse invitée dans
 * [THUNK_BASE, THUNK_BASE + 4*THUNK_MAX) ; un appel à cette adresse arrive dans nvn_thunk_call().
 * Sert aux remplacements natifs (natif_vignette : vtables de remplacement...) et à la trace du pilote NVN d'origine
 * (nvn_vignette_origine). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "cpu.h"
#include "table.h"
}

namespace {

constexpr uint32_t THUNK_BASE = 0x02F00000u;
constexpr uint32_t THUNK_MAX = 0x4000u;

using Native = void (*)(Cpu *);

struct Proc {
    std::string name;
    Native fn = nullptr;
    uint32_t orig = 0; /* pilote d'origine : fonction relayée (trace NX_NVN_TRACE_ORIGINE) */
    bool signale = false;
    volatile long appels = 0;
};

std::mutex g_m;
std::vector<Proc> g_procs;
std::unordered_map<std::string, uint32_t> g_index;
std::unordered_map<std::string, Native> &impl() { /* implémentations natives par nom (singleton : ordre d'init) */
    static std::unordered_map<std::string, Native> m;
    return m;
}

std::string guest_str(uint32_t a) {
    std::string s;
    for (char ch; a && (ch = (char)rd8(a)) != 0; a++) s += ch;
    return s;
}

uint32_t thunk_for(const std::string &name) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_index.find(name);
    if (it != g_index.end()) return THUNK_BASE + 4 * it->second;
    uint32_t i = (uint32_t)g_procs.size();
    if (i >= THUNK_MAX) rt_fatal(nullptr, "trop de fonctions NVN");
    Proc p;
    p.name = name;
    auto im = impl().find(name);
    if (im != impl().end()) p.fn = im->second;
    g_procs.push_back(p);
    g_index[name] = i;
    static const bool noms = getenv("NX_NVN_NOMS") != nullptr;
    if (noms) fprintf(stderr, "[nvn-nom] %s -> %08X\n", name.c_str(), THUNK_BASE + 4 * i);
    return THUNK_BASE + 4 * i;
}

bool ends_with(const std::string &s, const char *suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

void reg(const char *n, Native f) { impl()[n] = f; }

} // namespace

void nvn_reg(const char *name, Native f) { reg(name, f); }

extern "C" uint32_t nvn_vignette_origine(const std::string &nom, uint32_t orig);
/* Vignette native générique : adresse invitée appelable qui exécute f (vtables de remplacement, etc.) */
extern "C" uint32_t natif_vignette(const char *nom, Native f) {
    reg(nom, f);
    return thunk_for(nom);
}

/* Vignette de trace : relaie la fonction d'origine `orig` du pilote NVN (NX_PILOTE_ORIGINE + NX_NVN_TRACE_ORIGINE) */
extern "C" uint32_t nvn_vignette_origine(const std::string &nom, uint32_t orig) {
    std::string cle = "origine:" + nom;
    uint32_t v = thunk_for(cle);
    std::lock_guard<std::mutex> lk(g_m);
    Proc &p = g_procs[(v - THUNK_BASE) / 4];
    p.orig = orig;
    p.name = nom;
    return v;
}

/* Appel d'une vignette (depuis rt_call_indirect). Renvoie 0 si l'adresse n'est pas une vignette. */
extern "C" int nvn_thunk_call(Cpu *c, uint32_t addr) {
    if (addr < THUNK_BASE || addr >= THUNK_BASE + 4 * THUNK_MAX) return 0;
    uint32_t i = (addr - THUNK_BASE) / 4;
    Proc *p;
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (i >= g_procs.size()) rt_fatal(c, "vignette NVN %u inconnue", i);
        p = &g_procs[i];
    }
    InterlockedIncrement(&p->appels);
    static const uint32_t lr_min = getenv("NX_NVN_LR") ? strtoul(getenv("NX_NVN_LR"), nullptr, 16) : 0;
    if (lr_min && c->r[14] >= lr_min && c->r[14] < lr_min + 0x100 && rand() % 2000 == 0)
        fprintf(stderr, "[nvn-lr] %08X %s(%08X, %08X, %08X, %08X)\n", c->r[14], p->name.c_str(), c->r[0], c->r[1], c->r[2], c->r[3]);
    static const bool cb_pile = getenv("NX_NVN_CB_PILE") != nullptr; /* diagnostic : commandes sur tampons alloués sur la pile */
    if (cb_pile && c->r[0] >= c->r[13] && c->r[0] < c->r[13] + 0x2000 && p->name.compare(0, 16, "nvnCommandBuffer") == 0) {
        static std::mutex mj;
        static std::unordered_map<std::string, long> vus;
        std::lock_guard<std::mutex> lk(mj);
        if (vus[p->name]++ < 3)
            fprintf(stderr, "[nvn-pile] %s(%08X, %08X, %08X, %08X) lr=%08X%c", p->name.c_str(), c->r[0], c->r[1], c->r[2], c->r[3], c->r[14], 10);
    }
    if (p->orig) { /* relais de trace vers le pilote NVN d'origine */
        static const int n_max = getenv("NX_NVN_TRACE_ORIGINE") ? atoi(getenv("NX_NVN_TRACE_ORIGINE")) : 0;
        const uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2], a3 = c->r[3], lr = c->r[14];
        rt_call_indirect(c, p->orig);
        const bool init = p->name.size() > 10 && p->name.compare(p->name.size() - 10, 10, "Initialize") == 0;
        if (p->appels <= n_max || (init && c->r[0] == 0))
            fprintf(stderr, "[nvn-origine] %s(%08X, %08X, %08X, %08X) -> %08X lr=%08X%s\n", p->name.c_str(), a0, a1, a2, a3, c->r[0], lr,
                    init && c->r[0] == 0 ? "  <-- ÉCHEC" : "");
        if (p->name == "nvnDeviceGetProcAddress" && c->r[0]) { /* les fonctions obtenues sont relayées elles aussi */
            std::string n = guest_str(a1);
            uint32_t orig = c->r[0];
            c->r[0] = nvn_vignette_origine(n, orig);
        }
        return 1;
    }
    if (p->fn) { p->fn(c); return 1; }
    static const int n_journal = getenv("NX_NVN_JOURNAL") ? atoi(getenv("NX_NVN_JOURNAL")) : 1;
    if (p->appels <= n_journal) {
        p->signale = true;
        uint32_t sp = c->r[13];
        fprintf(stderr, "[nvn] non implémenté : %s(%08X, %08X, %08X, %08X | pile %08X %08X %08X %08X | s0 %g s1 %g) lr=%08X\n",
                p->name.c_str(), c->r[0], c->r[1], c->r[2], c->r[3], rd32(sp), rd32(sp + 4), rd32(sp + 8), rd32(sp + 12),
                c->vr.f[0], c->vr.f[1], c->r[14]);
    }
    c->r[0] = ends_with(p->name, "Initialize") ? 1 : 0; /* NVNboolean vrai pour les initialisations */
    return 1;
}

/* pilote d'origine : résultat de nvnBootstrapLoader relayé pour la trace (NX_NVN_TRACE_ORIGINE) */
extern "C" uint32_t nvn_bootstrap_origine(uint32_t nom, uint32_t orig) {
    if (!orig || !getenv("NX_NVN_TRACE_ORIGINE")) return orig;
    return nvn_vignette_origine(guest_str(nom), orig);
}

/* Statistiques : nombre d'appels par fonction NVN (affichées par le chien de garde NX_DUMP) */
extern "C" void nvn_stats(void) {
    std::vector<std::pair<long, std::string>> v;
    {
        std::lock_guard<std::mutex> lk(g_m);
        for (auto &p : g_procs) if (p.appels) v.push_back({p.appels, p.name + (p.fn ? "" : " (non implémenté)")});
    }
    std::sort(v.rbegin(), v.rend());
    fprintf(stderr, "== appels NVN ==\n");
    for (auto &x : v) fprintf(stderr, "%9ld %s\n", x.first, x.second.c_str());
}
