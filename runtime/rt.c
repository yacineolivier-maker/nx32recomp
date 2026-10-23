/* Runtime minimal : mémoire invitée, distribution des appels indirects, démarrage. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "funcs.h"
#include "table.h"
#include "entry.h"
#include "svc.h"
#include "jeu.h"

uint8_t *g_mem;
volatile uint32_t rt_excl_seq[4096];
volatile long rt_excl_verrou[4096];
#ifdef RC_TRACE
__declspec(thread) uint32_t rc_trace_buf[64];
__declspec(thread) uint32_t rc_trace_pos;
#endif

/* ------------------------------------------------------------------ remplacements natifs (HLE) */
typedef struct { uint32_t addr; GuestFn fn; } Hle;
#define HLE_MAX 4096
static Hle g_hle[HLE_MAX];
static int g_hle_n;

void rt_hle_register(uint32_t addr, GuestFn fn) {
    if (g_hle_n < HLE_MAX) { g_hle[g_hle_n].addr = addr; g_hle[g_hle_n].fn = fn; g_hle_n++; }
}

static GuestFn find_hle(uint32_t a) {
    for (int i = 0; i < g_hle_n; i++)
        if (g_hle[i].addr == a) return g_hle[i].fn;
    return NULL;
}

static GuestFn find_fn(uint32_t a) {
    uint32_t lo = 0, hi = rc_table_size;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rc_table[mid].addr < a) lo = mid + 1;
        else hi = mid;
    }
    if (lo < rc_table_size && rc_table[lo].addr == a) return rc_table[lo].fn;
    return NULL;
}

GuestFn rt_lookup(uint32_t a) {
    GuestFn f = find_hle(a);
    return f ? f : find_fn(a);
}

void rt_fatal(Cpu *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n[FATAL] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (c) {
        extern uint32_t svc_current_handle(void);
        fprintf(stderr, "thread invité : handle %08X\n", svc_current_handle());
        for (int i = 0; i < 16; i++) fprintf(stderr, "r%-2d=%08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "  ");
        fprintf(stderr, "NZCV=%u%u%u%u\n", c->n, c->z, c->c, c->v);
#ifdef RC_TRACE
        for (uint32_t k = 32; k > 0; k--) /* dernières fonctions entrées, la plus récente en dernier */
            fprintf(stderr, "trace=%08X\n", rc_trace_buf[(rc_trace_pos - k) & 63]);
#endif
        /* pile d'appels invitée par chaînage des cadres (clang ARM : [fp] = fp précédent, [fp+4] = lr) */
        fprintf(stderr, "lr=%08X\n", c->r[14]);
        uint32_t fp = c->r[11];
        for (int i = 0; i < 24 && fp >= 0x1000 && fp < 0xFFFFF000u; i++) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(g_mem + fp, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
            uint32_t lr = rd32(fp + 4), up = rd32(fp);
            fprintf(stderr, "pile=%08X\n", lr);
            if (up <= fp) break;
            fp = up;
        }
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1); /* ExitProcess peut se bloquer dans le détachement des DLL (pilote GPU) */
}

int nvn_thunk_call(Cpu *c, uint32_t addr);

void rt_call_indirect(Cpu *c, uint32_t target) {
    if (nvn_thunk_call(c, target)) return; /* fonction NVN native */
    GuestFn f = rt_lookup(target);
    if (!f) {
        /* fonction absente de la découverte statique : notée pour devenir une graine (tools/recomp/graines.py) */
        if (target == 0 && c->r[0] >= 0x1000) { /* appel virtuel nul probable : montrer l'objet et sa vtable */
            uint32_t vt = rd32(c->r[0]);
            fprintf(stderr, "[diag] objet r0=%08X vtable=%08X :", c->r[0], vt);
            if (vt >= 0x1000 && vt < 0xFFFF0000u)
                for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", rd32(vt + 4 * i));
            fprintf(stderr, "\n[diag] objet :");
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", rd32(c->r[0] + 4 * i));
            fprintf(stderr, "\n");
        }
        FILE *m = fopen("build/manquantes.txt", "a");
        if (m) { fprintf(m, "%08X\n", target); fclose(m); }
        rt_fatal(c, "appel indirect vers 0x%08X : aucune fonction connue (ajoutée à build/manquantes.txt)", target);
    }
    f(c);
}

void rt_bad_jump(Cpu *c, uint32_t at, uint32_t t) { rt_fatal(c, "table de sauts 0x%08X : cible 0x%08X inattendue", at, t); }
void rt_unimpl(Cpu *c, uint32_t addr, const char *what) { rt_fatal(c, "instruction non traduite @0x%08X : %s", addr, what); }
void rt_undefined(Cpu *c, uint32_t addr) { rt_fatal(c, "instruction indéfinie (udf) @0x%08X", addr); }

/* ------------------------------------------------------------------ mémoire */
void *rt_commit(uint32_t addr, uint32_t size) {
    void *p = VirtualAlloc(g_mem + addr, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) fprintf(stderr, "[mem] ERREUR engagement %08X (%X octets) : %lu\n", addr, size, GetLastError());
    return p;
}

/* surveillance d'écriture (diagnostic) : page invitée protégée, l'écrivain est journalisé puis la page rendue */
volatile uint32_t rt_page_surveillee; /* adresse invitée de la page (0 : aucune) */
volatile uint32_t rt_mot_surveille; /* adresse exacte visée (écritures ailleurs sur la page : pas à pas puis reprotection) */
static __declspec(thread) int t_reprotege;
static __declspec(thread) uintptr_t t_rip_ecrivain;
void rt_surveiller(uint32_t adr) {
    if (rt_page_surveillee) return;
    DWORD ancien;
    rt_mot_surveille = adr & ~3u;
    rt_page_surveillee = adr & ~0xFFFu;
    VirtualProtect(g_mem + rt_page_surveillee, 0x1000, PAGE_READONLY, &ancien);
}
/* Faute d'un thread hôte dans la mémoire invitée : nom du thread, état de la page, instruction et pile hôte */
static void pile_faute_hote(EXCEPTION_POINTERS *e, uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi))
        fprintf(stderr, "[FATAL] page : état %lX, protection %lX\n", mbi.State, mbi.Protect);
    typedef HRESULT(WINAPI * DescFn)(HANDLE, PWSTR *);
    DescFn get_desc = (DescFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    PWSTR w = NULL;
    char nom_thread[128] = "";
    if (get_desc && SUCCEEDED(get_desc(GetCurrentThread(), &w)) && w) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, nom_thread, sizeof nom_thread, NULL, NULL);
        LocalFree(w);
    }
    fprintf(stderr, "[FATAL] thread hôte %lu « %s », pile :", GetCurrentThreadId(), nom_thread);
    CONTEXT c = *e->ContextRecord;
    for (int n = 0; n < 16 && c.Rip; n++) {
        HMODULE mod = NULL;
        char chemin[260] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &mod);
        if (mod) GetModuleFileNameA(mod, chemin, sizeof chemin);
        const char *court = strrchr(chemin, 92) ? strrchr(chemin, 92) + 1 : chemin;
        fprintf(stderr, " %s+%llX", court, (unsigned long long)(c.Rip - (uintptr_t)mod));
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!fn) { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; }
        else {
            PVOID donnees = NULL;
            DWORD64 cadre = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &donnees, &cadre, NULL);
        }
    }
    fprintf(stderr, "\n");
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_reprotege) {
        DWORD ancien;
        if (t_reprotege == 2) { /* écriture sur le mot visé exécutée : valeur écrite et auteur */
            static volatile long n_journal;
            uint32_t v = *(uint32_t *)(g_mem + rt_mot_surveille);
            if (v && InterlockedIncrement(&n_journal) < 400)
                fprintf(stderr, "[surveillance] %08X <- %u par l'exécutable+0x%llX\n", rt_mot_surveille, v, (unsigned long long)t_rip_ecrivain);
        }
        t_reprotege = 0;
        if (rt_page_surveillee) VirtualProtect(g_mem + rt_page_surveillee, 0x1000, PAGE_READONLY, &ancien);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rt_page_surveillee && e->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
        if (a >= (uintptr_t)g_mem + rt_page_surveillee && a < (uintptr_t)g_mem + rt_page_surveillee + 0x1000) {
            DWORD ancien;
            uint32_t ga = (uint32_t)(a - (uintptr_t)g_mem);
            VirtualProtect(g_mem + rt_page_surveillee, 0x1000, PAGE_READWRITE, &ancien);
            if (ga >= rt_mot_surveille && ga < rt_mot_surveille + 4) { /* mot visé : pas à pas, valeur lue après */
                t_rip_ecrivain = (uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleA(NULL);
                t_reprotege = 2;
                e->ContextRecord->EFlags |= 0x100;
            } else { /* autre mot : laisser passer une instruction puis reprotéger */
                t_reprotege = 1;
                e->ContextRecord->EFlags |= 0x100;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
        if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x100000000ull) {
            fprintf(stderr, "\n[FATAL] accès %s invité non mappé à 0x%08X\n",
                    e->ExceptionRecord->ExceptionInformation[0] ? "en écriture" : "en lecture",
                    (uint32_t)(a - (uintptr_t)g_mem));
            pile_faute_hote(e, a); /* qui (thread hôte ou invité), où, quelle page */
        } else {
            HMODULE mod = NULL;
            char nom[260] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)e->ExceptionRecord->ExceptionAddress, &mod);
            if (mod) GetModuleFileNameA(mod, nom, sizeof nom);
            fprintf(stderr, "\n[FATAL] violation d'accès hôte à %p (instruction %s+0x%llX)\n", (void *)a, nom,
                    (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)mod));
        }
        fflush(stderr);
        rt_fatal(svc_current(), "faute mémoire (état du thread invité courant)");
    }
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) {
        HMODULE mod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)e->ExceptionRecord->ExceptionAddress, &mod);
        char nom[260] = "?";
        if (mod) GetModuleFileNameA(mod, nom, sizeof nom);
        fprintf(stderr, "\n[FATAL] exception hôte 0x%08lX à %p (module %s, décalage 0x%llX)\n", code, e->ExceptionRecord->ExceptionAddress,
                nom, (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)mod));
        fflush(stderr);
        rt_fatal(svc_current(), "exception hôte");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* abort() (assertion fatale d'Eden, std::terminate...) : pile hôte du thread fautif, puis sortie (sans cela le CRT
 * termine par __fastfail, code 0xC0000409, sans aucune trace) */
static void on_abort(int sig) {
    (void)sig;
    void *pile[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, pile, NULL);
    fprintf(stderr, "\n[FATAL] abort() sur le thread hôte %lu, pile :", GetCurrentThreadId());
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = NULL;
        char nom[260] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pile[i], &mod);
        if (mod) GetModuleFileNameA(mod, nom, sizeof nom);
        const char *court = strrchr(nom, 92) ? strrchr(nom, 92) + 1 : nom;
        fprintf(stderr, " %s+%llX", court, (unsigned long long)((uintptr_t)pile[i] - (uintptr_t)mod));
    }
    fprintf(stderr, "\n");
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 134); /* _exit peut se bloquer (détachement des DLL) */
}

/* bornes de l'image liée (svc.c : QueryMemory) */
const uint32_t rt_image_base = IMAGE_BASE, rt_image_taille = IMAGE_END - IMAGE_BASE;

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal(NULL, "image introuvable : %s", path);
    rt_commit(IMAGE_BASE, IMAGE_END - IMAGE_BASE);
    size_t n = fread(g_mem + IMAGE_BASE, 1, IMAGE_END - IMAGE_BASE, f);
    fclose(f);
    printf("[rt] image chargée : %zu octets à 0x%08X\n", n, IMAGE_BASE);
    for (int i = 0; i < MODULE_COUNT; i++) /* main : point d'entrée à base + 0x24 */
        if (!strcmp(g_modules[i].nom, "main")) jeu_correctifs_image(g_modules[i].init - 0x24);
}

/* ------------------------------------------------------------------ démarrage */
static void call(Cpu *c, uint32_t addr, const char *name) {
    printf("[rt] -> %s (0x%08X)\n", name, addr);
    fflush(stdout);
    c->r[14] = 0; /* adresse de retour sentinelle */
    rt_call_indirect(c, addr);
}

/* ---- chien de garde de diagnostic : NX_DUMP=n -> après n secondes, trace de chaque thread invité ---- */
#define MAX_TRACES 64
static struct { uint32_t handle; uint32_t *buf; uint32_t *pos; } g_traces[MAX_TRACES];
static volatile LONG g_ntraces;
void rt_trace_register(uint32_t handle) {
#ifdef RC_TRACE
    LONG i = InterlockedIncrement(&g_ntraces) - 1;
    if (i < MAX_TRACES) { g_traces[i].handle = handle; g_traces[i].buf = rc_trace_buf; g_traces[i].pos = &rc_trace_pos; }
#endif
}
static DWORD WINAPI watchdog(LPVOID arg) {
    Sleep((DWORD)(uintptr_t)arg * 1000);
    fflush(stdout);
    extern void nvn_stats(void);
    nvn_stats();
    for (LONG i = 0; i < g_ntraces && i < MAX_TRACES; i++) {
        fprintf(stderr, "== thread %08X\n", g_traces[i].handle);
        uint32_t p = *g_traces[i].pos;
        for (uint32_t k = 16; k > 0; k--) fprintf(stderr, "trace=%08X\n", g_traces[i].buf[(p - k) & 63]);
    }
    fflush(stderr);
    ExitProcess(3);
    return 0;
}

/* détecteur de blocage (NX_DETECT_BLOCAGE=s) : plus aucune présentation depuis s secondes => anneaux de trace */
volatile long rt_nb_presentations;
static DWORD WINAPI detecteur_blocage(LPVOID arg) {
    DWORD seuil = (DWORD)(uintptr_t)arg;
    long prec = -1;
    DWORD depuis = GetTickCount();
    for (;;) {
        Sleep(1000);
        long n = rt_nb_presentations;
        if (n != prec) { prec = n; depuis = GetTickCount(); continue; }
        if (GetTickCount() - depuis < seuil * 1000) continue;
        fprintf(stderr, "%c[blocage] aucune présentation depuis %u s (n=%ld)%c", 10, seuil, n, 10);
        { extern void svc_vider_threads(void); svc_vider_threads(); }
        { extern void svc_piles_hotes(void); svc_piles_hotes(); }
        for (LONG i = 0; i < g_ntraces && i < MAX_TRACES; i++) {
            fprintf(stderr, "[blocage] thread %08X :", g_traces[i].handle);
            uint32_t p = *g_traces[i].pos;
            for (uint32_t k = 64; k > 0; k--) fprintf(stderr, " %X", g_traces[i].buf[(p - k) & 63]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
        Sleep(500);
        for (LONG i = 0; i < g_ntraces && i < MAX_TRACES; i++) { /* second relevé : boucle ou progrès ? */
            fprintf(stderr, "[blocage2] thread %08X :", g_traces[i].handle);
            uint32_t p = *g_traces[i].pos;
            for (uint32_t k = 16; k > 0; k--) fprintf(stderr, " %X", g_traces[i].buf[(p - k) & 63]);
            fprintf(stderr, " (pos %u)\n", p);
        }
        fflush(stderr);
        ExitProcess(4);
    }
    return 0;
}

static DWORD WINAPI main_thread(LPVOID arg) {
    Cpu *c = svc_adopt_main((Cpu *)arg);
    rt_trace_register(svc_current_handle());
    jeu_hle_init();
    call(c, SYM___nnDetailInitLibc0, "__nnDetailInitLibc0");
    extern uint32_t svc_current_handle(void);
    c->r[0] = svc_current_handle(); /* handle réel du thread principal */
    c->r[1] = 0;
    call(c, SYM_nnosInitialize, "nnosInitialize");
    call(c, SYM___nnDetailInitLibc1, "__nnDetailInitLibc1");
    call(c, SYM_nndiagStartup, "nndiagStartup");
#ifdef SYM_nninitInitializeSdkModule
    call(c, SYM_nninitInitializeSdkModule, "nninitInitializeSdkModule");
#endif
    call(c, SYM_nninitStartup, "nninitStartup");
    call(c, SYM___nnDetailInitLibc2, "__nnDetailInitLibc2");
    for (int i = 0; i < MODULE_COUNT; i++) /* comme rtld : DT_INIT de chaque module (qui parcourt .init_array) */
        if (g_modules[i].init) call(c, g_modules[i].init, g_modules[i].nom);
    call(c, SYM_nnMain, "nnMain");
    printf("[rt] nnMain est revenu\n");
    return 0;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    /* suivi d'écriture Windows pour la cohérence des écritures du CPU vers les caches du GPU émulé (NX_EDEN_SUIVI=ww) */
    const int write_watch = getenv("NX_EDEN_SUIVI") && !strcmp(getenv("NX_EDEN_SUIVI"), "ww");
    g_mem = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE | (write_watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
    if (!g_mem) rt_fatal(NULL, "réservation de 4 Gio impossible");
    AddVectoredExceptionHandler(1, on_fault);
    signal(SIGABRT, on_abort);
    extern const char *chemins_init(const char *image_arg); /* fs.cpp : installation ou développement */
    load_image(chemins_init(argc > 1 ? argv[1] : NULL));
    svc_init();

    static Cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    rt_commit(MAIN_STACK_BASE, MAIN_STACK_SIZE);
    cpu.r[13] = MAIN_STACK_BASE + MAIN_STACK_SIZE;
    cpu.tpidruro = svc_alloc_tls(&cpu);

    /* pile hôte large : la profondeur d'appel invitée se traduit en profondeur C */
    if (getenv("NX_DUMP")) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(getenv("NX_DUMP")), 0, NULL);
    { extern void profil_demarrer(void); profil_demarrer(); } /* NX_PROFIL_TOUS=s */
    if (getenv("NX_DETECT_BLOCAGE")) CreateThread(NULL, 0, detecteur_blocage, (LPVOID)(uintptr_t)atoi(getenv("NX_DETECT_BLOCAGE")), 0, NULL);
    extern int gfx_demarrer(void);
    extern void gfx_boucle(void);
    int fenetre = !getenv("NX_SANS_FENETRE") && gfx_demarrer(); /* fenêtre + Vulkan avant le jeu */
    HANDLE h = CreateThread(NULL, 256u << 20, main_thread, &cpu, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (fenetre) gfx_boucle(); /* jusqu'à la fermeture de la fenêtre */
    else WaitForSingleObject(h, INFINITE);
    /* GPU d'Eden arrêté proprement, puis fin sans destructeurs statiques (verrous des threads tués) */
    extern void eden_arret_propre(void);
    eden_arret_propre();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
