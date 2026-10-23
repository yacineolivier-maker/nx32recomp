// Système adaptateur pour video_core et les périphériques nvdrv d'Eden : horloge, mémoire identité, événements reliés au
// runtime natif, cohérence des écritures du CPU invité vers les caches du GPU (suivi d'écriture Windows).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "common/cpu_features.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_event.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "interne.h"
#include "systeme_impl.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"

namespace EdenGpu {

EdenRappels g_rappels{};

void Journal(const char* format, ...) {
    char texte[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(texte, sizeof texte, format, ap);
    va_end(ap);
    if (g_rappels.journal) g_rappels.journal(g_rappels.ctx, texte);
    else fprintf(stderr, "[eden] %s\n", texte);
}

// ---------------------------------------------------------------- suivi des pages mises en cache par le GPU
namespace {
constexpr uint64_t PAGES = 1ull << 20; // 4 Gio / 4 Kio
std::mutex g_suivi;
std::vector<uint8_t> g_en_cache(PAGES / 8); // une bit par page invitée
uint8_t* g_base = nullptr;
uint64_t g_page_min = PAGES, g_page_max = 0; // étendue des pages mises en cache (jamais réduite)

bool en_cache(uint64_t p) { return (g_en_cache[p >> 3] >> (p & 7)) & 1; }

// statistiques (NX_EDEN_STATS_ECRITURES) : appels, durée cumulée, pages rapportées / retenues
std::atomic<uint64_t> g_nb_collectes{0}, g_ns_collectes{0}, g_pages_vues{0}, g_pages_retenues{0}, g_fautes{0},
    g_fautes_hors_cache{0};

// Mode de suivi : protection de pages (défaut, comme la « fastmem » d'Eden : les pages en cache du GPU sont en lecture
// seule, la première écriture du CPU fait une faute qui les marque écrites) ou GetWriteWatch (NX_EDEN_SUIVI=ww).
const bool g_write_watch = [] {
    const char* m = getenv("NX_EDEN_SUIVI");
    return m && strcmp(m, "ww") == 0;
}();
std::atomic<uint64_t> g_ecrites[PAGES / 64];     // pages écrites par le CPU depuis la dernière collecte
std::atomic<uint64_t> g_a_reproteger[PAGES / 64]; // pages écrites par Eden lui-même (téléchargements) : sans invalidation
std::atomic<bool> g_quelque_chose{false};

/// VirtualProtect échoue en bloc si une page de la plage n'est pas engagée (mémoire libérée par UnmapMemory...) :
/// en cas d'échec, la plage est reprise par sous-plages engagées (VirtualQuery)
std::atomic<uint64_t> g_protections_partielles{0};
void proteger(uint64_t page_debut, uint64_t nb, bool lecture_seule) {
    DWORD ancien;
    const DWORD prot = lecture_seule ? PAGE_READONLY : PAGE_READWRITE;
    uint8_t* debut = g_base + (page_debut << 12);
    uint8_t* const fin = debut + (nb << 12);
    if (VirtualProtect(debut, SIZE_T(fin - debut), prot, &ancien)) return;
    g_protections_partielles++;
    while (debut < fin) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(debut, &mbi, sizeof mbi)) break;
        uint8_t* bout = std::min(fin, static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
        if (mbi.State == MEM_COMMIT) VirtualProtect(debut, SIZE_T(bout - debut), prot, &ancien);
        debut = bout;
    }
}

/// Thread interne d'Eden (thread GPU, barrières asynchrones) : ses écritures dans la mémoire invitée sont des
/// téléchargements de données du GPU, déjà cohérentes (Eden les fait par la mémoire physique, sans faute).
bool thread_eden() {
    thread_local int connu = -1;
    if (connu < 0) {
        connu = 0;
        PWSTR nom = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &nom)) && nom) {
            connu = wcscmp(nom, L"GPU") == 0 || wcscmp(nom, L"GPUFencingThread") == 0;
            LocalFree(nom);
        }
    }
    return connu == 1;
}

std::atomic<uint64_t> g_pages_gpu_hors_memoire{0};

LONG CALLBACK faute_suivi(EXCEPTION_POINTERS* e) {
    const auto* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_base) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* a = reinterpret_cast<uint8_t*>(r->ExceptionInformation[1]);
    if (a < g_base || a >= g_base + (PAGES << 12)) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t p = uint64_t(a - g_base) >> 12;
    if (r->ExceptionInformation[0] != 1) { // lecture : seul cas traité, le GPU hors de la mémoire du jeu (ci-dessous)
        MEMORY_BASIC_INFORMATION mbi;
        if (thread_eden() && VirtualQuery(a, &mbi, sizeof mbi) && mbi.State != MEM_COMMIT) goto hors_memoire;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (en_cache(p)) {
        proteger(p, 1, false);
        (thread_eden() ? g_a_reproteger : g_ecrites)[p >> 6].fetch_or(1ull << (p & 63));
        g_quelque_chose = true;
        g_fautes++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(a, &mbi, sizeof mbi)) return EXCEPTION_CONTINUE_SEARCH;
    if (mbi.State != MEM_COMMIT) {
        if (thread_eden()) goto hors_memoire;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // page rendue inscriptible entre-temps par un autre thread : réessayer l'écriture
    if (mbi.Protect & PAGE_READWRITE) return EXCEPTION_CONTINUE_EXECUTION;
    // filet de sécurité : seule la cohérence GPU met des pages invitées en lecture seule ; une page restée protégée
    // hors cache (protection partielle) est rendue inscriptible et signalée comme écrite
    if (mbi.Protect & PAGE_READONLY) {
        proteger(p, 1, false);
        g_ecrites[p >> 6].fetch_or(1ull << (p & 63));
        g_quelque_chose = true;
        g_fautes_hors_cache++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
    }
hors_memoire:
    // Le GPU (thread interne d'Eden) lit ou écrit hors de la mémoire du jeu (adresse incohérente, tampon libéré) : sur la
    // console l'accès serait perdu ; ici une page vide est engagée pour poursuivre, et l'événement est journalisé.
    if (VirtualAlloc(g_base + (p << 12), 4096, MEM_COMMIT, PAGE_READWRITE)) {
        if (g_pages_gpu_hors_memoire++ < 20)
            Journal("GPU : accès %s hors de la mémoire du jeu à 0x%08llX (page vide engagée)",
                    r->ExceptionInformation[0] == 1 ? "en écriture" : "en lecture", (unsigned long long)(a - g_base));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void MarquerEnCache(uint64_t adresse, uint64_t taille, bool cache) {
    if (!taille) return;
    std::scoped_lock lk{g_suivi};
    const uint64_t debut = adresse >> 12, fin = std::min<uint64_t>((adresse + taille + 0xFFF) >> 12, PAGES);
    if (debut >= fin) return;
    // ordre : une page en lecture seule par notre fait a toujours son bit « en cache » (cf. faute_suivi)
    if (!cache && !g_write_watch) proteger(debut, fin - debut, false);
    for (uint64_t p = debut; p < fin; ++p) {
        if (cache) g_en_cache[p >> 3] |= uint8_t(1u << (p & 7));
        else g_en_cache[p >> 3] &= uint8_t(~(1u << (p & 7)));
    }
    if (cache && !g_write_watch) proteger(debut, fin - debut, true);
    if (cache) {
        g_page_min = std::min(g_page_min, debut);
        g_page_max = std::max(g_page_max, fin - 1);
    }
}

/// Écriture imminente par le noyau (ReadFile...) : les pages en cache de la zone sont rendues inscriptibles et marquées
/// écrites (une écriture du noyau dans une page en lecture seule échouerait sans faute)
void EcritureHote(uint64_t adresse, uint64_t taille) {
    if (!taille || g_write_watch || !g_base) return;
    std::scoped_lock lk{g_suivi};
    const uint64_t debut = adresse >> 12, fin = std::min<uint64_t>((adresse + taille + 0xFFF) >> 12, PAGES);
    for (uint64_t p = debut; p < fin;) {
        if (!en_cache(p)) { ++p; continue; }
        uint64_t q = p;
        while (q < fin && en_cache(q)) {
            g_ecrites[q >> 6].fetch_or(1ull << (q & 63));
            ++q;
        }
        proteger(p, q - p, false);
        p = q;
    }
    g_quelque_chose = true;
}

/// Collecte en mode protection : pages écrites depuis la dernière collecte (remises en lecture seule avant les rappels :
/// une écriture ultérieure sera vue à la collecte suivante)
static void collecter_protection(std::vector<std::pair<uint64_t, uint64_t>>& plages) {
    if (!g_quelque_chose.exchange(false)) return;
    std::scoped_lock lk{g_suivi};
    uint64_t run_debut = 0, run_nb = 0; // pages contiguës à reprotéger
    auto vider = [&] {
        if (run_nb) proteger(run_debut, run_nb, true);
        run_nb = 0;
    };
    for (uint64_t m = 0; m < PAGES / 64; ++m) {
        if (!g_ecrites[m].load(std::memory_order_relaxed) && !g_a_reproteger[m].load(std::memory_order_relaxed)) continue;
        const uint64_t ecrites = g_ecrites[m].exchange(0), rep = g_a_reproteger[m].exchange(0);
        uint64_t bits = ecrites | rep;
        while (bits) {
            const int b = std::countr_zero(bits);
            bits &= bits - 1;
            const uint64_t p = m * 64 + uint64_t(b);
            if (!en_cache(p)) continue;
            if (run_nb && run_debut + run_nb == p) ++run_nb;
            else { vider(); run_debut = p; run_nb = 1; }
            if ((ecrites >> b) & 1) {
                if (!plages.empty() && plages.back().first + plages.back().second == (p << 12)) plages.back().second += 4096;
                else plages.emplace_back(p << 12, 4096);
            }
        }
    }
    vider();
}

/// Pages écrites par le CPU (code du jeu, runtime) depuis le dernier appel, limitées aux zones en cache du GPU.
/// Un seul appel système GetWriteWatch couvre l'étendue des pages en cache (le coût est dominé par le nombre d'appels) ;
/// les pages hors cache rapportées sont écartées par le bitmap (leur suivi est remis à zéro sans conséquence : une page
/// mise en cache plus tard est lue à ce moment-là).
void CollecterEcrituresFn(void (*rappel)(void* ctx, uint64_t adresse, uint64_t taille), void* ctx) {
    if (!g_base) return;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    // plages rapportées hors verrou : les rappels (invalidation des caches du GPU) remettent à jour le suivi
    std::vector<std::pair<uint64_t, uint64_t>> plages;
    uint64_t vues = 0;
    if (!g_write_watch) collecter_protection(plages);
    else {
        std::scoped_lock lk{g_suivi};
        if (g_page_min > g_page_max) return;
        static std::vector<void*> adresses(1u << 18);
        uint8_t* base = g_base + (g_page_min << 12);
        const SIZE_T taille = SIZE_T((g_page_max + 1 - g_page_min) << 12);
        for (;;) {
            ULONG_PTR n = adresses.size();
            DWORD granularite = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, taille, adresses.data(), &n, &granularite) != 0) break;
            vues += n;
            // pages en cache seulement, regroupées par plages contiguës
            for (ULONG_PTR i = 0; i < n;) {
                const uint64_t p = uint64_t((uint8_t*)adresses[i] - g_base) >> 12;
                if (!en_cache(p)) { ++i; continue; }
                uint64_t t = 4096;
                ULONG_PTR j = i + 1;
                while (j < n && (uint8_t*)adresses[j] == (uint8_t*)adresses[j - 1] + 4096 &&
                       en_cache(uint64_t((uint8_t*)adresses[j] - g_base) >> 12)) {
                    t += 4096;
                    ++j;
                }
                plages.emplace_back(p << 12, t);
                i = j;
            }
            if (n < adresses.size()) break; // tout est rapporté
        }
    }
    for (auto& [a, t] : plages) rappel(ctx, a, t);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    g_nb_collectes++;
    g_ns_collectes += uint64_t((t1.QuadPart - t0.QuadPart) * 1000000000.0 / double(freq.QuadPart));
    g_pages_vues += vues;
    uint64_t retenues = 0;
    for (auto& pl : plages) retenues += pl.second >> 12;
    g_pages_retenues += retenues;
    static const bool stats = getenv("NX_EDEN_STATS_ECRITURES") != nullptr;
    static uint64_t t_rapport = GetTickCount64();
    if (stats && GetTickCount64() - t_rapport > 10000) {
        t_rapport = GetTickCount64();
        const uint64_t n = g_nb_collectes.exchange(0), ns = g_ns_collectes.exchange(0);
        Journal("collecte des écritures : %llu appels en 10 s, %.3f ms en moyenne, %llu pages vues, %llu retenues, %llu fautes "
                "(%llu hors cache), %llu protections partielles",
                (unsigned long long)n, n ? ns / 1e6 / double(n) : 0.0, (unsigned long long)g_pages_vues.exchange(0),
                (unsigned long long)g_pages_retenues.exchange(0), (unsigned long long)g_fautes.exchange(0),
                (unsigned long long)g_fautes_hors_cache.exchange(0), (unsigned long long)g_protections_partielles.exchange(0));
    }
}

void DefinirBase(uint8_t* base) {
    g_base = base;
    if (!g_write_watch) AddVectoredExceptionHandler(1, faute_suivi); // avant le gestionnaire du runtime
    Journal("cohérence CPU -> GPU : %s", g_write_watch ? "GetWriteWatch" : "protection de pages");
}

bool SuiviParWriteWatch() { return g_write_watch; }

} // namespace EdenGpu

// ---------------------------------------------------------------- Core::System
namespace Core {

System::System() : impl{std::make_unique<Impl>()} {}
System::~System() = default;

Tegra::GPU& System::GPU() { return *impl->gpu; }
const Tegra::GPU& System::GPU() const { return *impl->gpu; }
Tegra::Host1x::Host1x& System::Host1x() { return *impl->host1x; }
const Tegra::Host1x::Host1x& System::Host1x() const { return *impl->host1x; }
Core::DeviceMemory& System::DeviceMemory() { return impl->device_memory; }
const Core::DeviceMemory& System::DeviceMemory() const { return impl->device_memory; }
Timing::CoreTiming& System::CoreTiming() { return impl->timing; }
const Timing::CoreTiming& System::CoreTiming() const { return impl->timing; }
Kernel::KernelCore& System::Kernel() { return impl->kernel; }
const Kernel::KernelCore& System::Kernel() const { return impl->kernel; }
Core::PerfStats& System::GetPerfStats() { return impl->perf; }
const Core::PerfStats& System::GetPerfStats() const { return impl->perf; }
Core::Memory::Memory& System::ApplicationMemory() { return *impl->memory; }
const Core::Memory::Memory& System::ApplicationMemory() const { return *impl->memory; }

bool System::IsPoweredOn() const { return impl->allume.load(); }
void System::RegisterHostThread() {}

void System::GatherGPUDirtyMemory(std::function<void(PAddr, size_t)>& callback) {
    EdenGpu::CollecterEcrituresFn([](void* ctx, uint64_t a, uint64_t t) { (*static_cast<std::function<void(PAddr, size_t)>*>(ctx))(a, t); },
                                  &callback);
}

std::unique_lock<std::mutex> System::StallApplication() { return std::unique_lock<std::mutex>{impl->arret}; }
void System::UnstallApplication() {}

namespace Timing {
u64 CoreTiming::GetGPUTicks() const { return Common::g_wall_clock.GetGPUTick(); }
u64 CoreTiming::GetClockTicks() const { return Common::g_wall_clock.GetCNTPCT(); }
std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const noexcept { return Common::g_wall_clock.GetTimeNS(); }
std::chrono::microseconds CoreTiming::GetGlobalTimeUs() const { return Common::g_wall_clock.GetTimeUS(); }
} // namespace Timing

namespace Memory {
void Memory::RasterizerMarkRegionCached(Common::ProcessAddress vaddr, u64 size, bool cached) {
    EdenGpu::MarquerEnCache(GetInteger(vaddr), size, cached);
}
bool Memory::ReadBlock(Common::ProcessAddress src_addr, void* dest_buffer, std::size_t size) const {
    std::memcpy(dest_buffer, base + GetInteger(src_addr), size);
    return true;
}
bool Memory::WriteBlock(Common::ProcessAddress dest_addr, const void* src_buffer, std::size_t size) {
    std::memcpy(base + GetInteger(dest_addr), src_buffer, size);
    return true;
}
} // namespace Memory

} // namespace Core

// ---------------------------------------------------------------- événements noyau -> runtime
namespace Kernel {
void KEvent::Signal() {
    if (EdenGpu::g_rappels.evenement_signaler) EdenGpu::g_rappels.evenement_signaler(EdenGpu::g_rappels.ctx, ecriture);
}
void KEvent::Clear() {
    if (EdenGpu::g_rappels.evenement_effacer) EdenGpu::g_rappels.evenement_effacer(EdenGpu::g_rappels.ctx, ecriture);
}
} // namespace Kernel
